package main

// -unlock: OPPO MTK fastboot unlock through a preloader patch.
//
// The patch is pattern-anchored (magic + BRLYT offsets + ROMINFO search),
// following path_preloader.py: relocate the 0x800 code blob to 0x2000,
// repoint the BRLYT layout bytes at it, plant the ROMINFO flag block at
// 0x1000, and clear the fastboot lock byte (0x22 -> 0x00).
//
//   ./flash -unlock -preloader boot1.bin [-device /dev/cu.usbmodemXXXX] [-yes]
//
// Without -device this only writes the patched image next to the input and
// prints the mtkclient commands to run by hand. With -device it backs up
// boot1/boot2 via mtkclient (the proven transport on secured phones),
// patches the dump when its flag data differs from the input file, and
// writes the patched image back. The write needs -yes or typed consent.

import (
	"bufio"
	"bytes"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
)

var unlockBRLYT = [][2]int{
	{0x20D, 0x20},
	{0x21D, 0x20},
	{0x211, 0x10},
	{0x212, 0x10},
	{0x221, 0x10},
	{0x222, 0x10},
}

const (
	unlockFlagLen     = 0x78
	unlockFlagLockOff = 0x4C
	unlockNewCodeOff  = 0x2000
	unlockTailDrop    = 0x3000
	unlockROMINFO     = "AND_ROMINFO_v"
	unlockConfirmWord = "UNLOCK BOOT1"
)

type unlockPatchReport struct {
	CodeOffset int
	FlagOffset int
	LockBefore byte
}

func patchUnlockPreloader(data []byte) ([]byte, unlockPatchReport, error) {
	var report unlockPatchReport
	if bytes.HasPrefix(data, []byte("MMM\x018\x00\x00\x00FILE_INF")) {
		return nil, report, errors.New("RAW preloader (no offset header); need a full boot1 dump")
	}
	if !bytes.HasPrefix(data, []byte("EMMC_BOOT")) &&
		!bytes.HasPrefix(data, []byte("UFS_BOOT")) &&
		!bytes.HasPrefix(data, []byte("COMBO_BOOT")) {
		fmt.Printf("warn: unknown magic %q; continuing anyway\n", data[:min(len(data), 16)])
	}
	if len(data) < unlockNewCodeOff+1 {
		return nil, report, fmt.Errorf("image too small (%d bytes) to relocate code", len(data))
	}
	out := bytes.Clone(data)
	for _, p := range unlockBRLYT {
		if out[p[0]] == byte(p[1]) {
			return nil, report, fmt.Errorf("already patched (BRLYT %#x=%#x); refusing", p[0], p[1])
		}
	}
	codeOffset := int(out[0x20D]) * 256
	if codeOffset <= 0 || codeOffset > unlockNewCodeOff {
		return nil, report, fmt.Errorf("unexpected code offset %#x", codeOffset)
	}
	flagAt := bytes.Index(out, []byte(unlockROMINFO))
	if flagAt < 0 {
		return nil, report, errors.New("AND_ROMINFO_v flag block not found")
	}
	if flagAt+unlockFlagLen > len(out) {
		return nil, report, errors.New("AND_ROMINFO_v flag block truncated")
	}
	report = unlockPatchReport{CodeOffset: codeOffset, FlagOffset: flagAt, LockBefore: out[flagAt+unlockFlagLockOff]}
	rawEnd := len(out) - unlockTailDrop
	if rawEnd <= codeOffset {
		return nil, report, fmt.Errorf("image too small (%d bytes) for code at %#x", len(data), codeOffset)
	}
	// Snapshot both blobs before zeroing: the flag block usually sits
	// inside the zeroed range (0xaec in the stock image).
	raw := bytes.Clone(out[codeOffset:rawEnd])
	flag := bytes.Clone(out[flagAt : flagAt+unlockFlagLen])
	for i := codeOffset; i < len(out); i++ {
		out[i] = 0
	}
	copy(out[unlockNewCodeOff:], raw)
	for _, p := range unlockBRLYT {
		out[p[0]] = byte(p[1])
	}
	copy(out[0x1000:], flag)
	out[0x1000+unlockFlagLockOff] = 0x00
	return out, report, nil
}

// unlockFlagFingerprint returns the ROMINFO flag block with the lock byte
// zeroed, so a dump and a file can be compared for per-device sameness.
func unlockFlagFingerprint(data []byte) ([]byte, error) {
	at := bytes.Index(data, []byte(unlockROMINFO))
	if at < 0 || at+unlockFlagLen > len(data) {
		return nil, errors.New("AND_ROMINFO_v flag block not found")
	}
	flag := bytes.Clone(data[at : at+unlockFlagLen])
	flag[unlockFlagLockOff] = 0x00
	return flag, nil
}

func defaultUnlockOutput(src string) string {
	dir := filepath.Dir(src)
	base := filepath.Base(src)
	stem := strings.TrimSuffix(base, filepath.Ext(base))
	return filepath.Join(dir, stem+"-patched.bin")
}

func resolveUnlockMTK(cfg config) (script, python string, err error) {
	if env := strings.TrimSpace(os.Getenv("MTKCLIENT_DIR")); env != "" {
		cand := filepath.Join(env, "mtk.py")
		if !fileExists(cand) {
			return "", "", fmt.Errorf("MTKCLIENT_DIR=%s has no mtk.py", env)
		}
		script = cand
	} else if s, ok := resolveBundledMTKClient(cfg); ok {
		script = s
	} else {
		return "", "", errors.New("no mtkclient checkout found; set MTKCLIENT_DIR or pass -mtkclient-root <dir>")
	}
	if env := strings.TrimSpace(os.Getenv("MTK_PYTHON")); env != "" {
		if resolved, lerr := exec.LookPath(env); lerr == nil {
			return script, resolved, nil
		} else if fileExists(env) {
			return script, env, nil
		}
		return "", "", fmt.Errorf("MTK_PYTHON=%s not found", env)
	}
	python, err = pythonForMTKScript(script)
	if err != nil {
		return "", "", err
	}
	return script, python, nil
}

func runUnlockMTK(dir, python, script string, args ...string) error {
	cmd := exec.Command(python, append([]string{script}, args...)...)
	cmd.Dir = dir
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	cmd.Stdin = os.Stdin
	return cmd.Run()
}

func confirmUnlockWrite(destination, image string, yes bool) error {
	fmt.Println()
	fmt.Printf("About to write %s to %s.\n", image, destination)
	fmt.Println("A bad boot1 image stops the phone booting until the backup is written back.")
	if yes {
		return nil
	}
	fmt.Printf("Type exactly '%s' to continue: ", unlockConfirmWord)
	line, err := bufio.NewReader(os.Stdin).ReadString('\n')
	if err != nil {
		return err
	}
	if strings.TrimSpace(line) != unlockConfirmWord {
		return errors.New("confirmation did not match; leaving the device untouched")
	}
	return nil
}

func printUnlockManual(python, script, dir, patched, backup1, backup2 string) {
	fmt.Println()
	fmt.Println("Manual equivalent (one phone connection per command; replug between them):")
	fmt.Printf("  cd %q && %q %q r boot1 %q\n", dir, python, script, backup1)
	fmt.Printf("  cd %q && %q %q r boot2 %q\n", dir, python, script, backup2)
	fmt.Printf("  cd %q && %q %q w boot1 %q\n", dir, python, script, patched)
	fmt.Println("Append --preloader <stock> --auth <auth_sv5.auth> if your recipe needs them.")
}

func printUnlockNextSteps(backup1 string) {
	fmt.Println()
	fmt.Println("Next: enable OEM unlocking in developer settings, then:")
	fmt.Println("  adb reboot bootloader")
	fmt.Println("  fastboot flashing unlock   (confirm with a volume key on the phone)")
	if backup1 != "" {
		fmt.Printf("Recovery if it fails to boot: write the backup back over boot1 (%s).\n", backup1)
	}
}

func runUnlockCommand(cfg config) error {
	src := strings.TrimSpace(cfg.preloader)
	if src == "" {
		return errors.New("-unlock requires -preloader /path/to/boot1-dump-or-preloader image")
	}
	data, err := os.ReadFile(src)
	if err != nil {
		return err
	}
	patched, report, err := patchUnlockPreloader(data)
	if err != nil {
		return fmt.Errorf("%s: %w", src, err)
	}
	outPath := defaultUnlockOutput(src)
	if err := os.WriteFile(outPath, patched, 0644); err != nil {
		return err
	}
	differ := 0
	for i := range data {
		if data[i] != patched[i] {
			differ++
		}
	}
	fmt.Printf("code offset: %#x -> %#x; flag block at %#x; lock state: %#04x -> 0x00\n",
		report.CodeOffset, unlockNewCodeOff, report.FlagOffset, report.LockBefore)
	fmt.Printf("wrote %s (%d bytes, %d bytes differ)\n", outPath, len(patched), differ)

	dev := strings.TrimSpace(cfg.device)
	python, script := "python3", "mtk.py"
	if resolved, py, rerr := resolveUnlockMTK(cfg); rerr == nil {
		script, python = resolved, py
	}
	dir := filepath.Dir(outPath)
	backup1 := filepath.Join(dir, "boot1-stock.bin")
	backup2 := filepath.Join(dir, "boot2-stock.bin")
	if dev == "" {
		printUnlockManual(python, script, dir, outPath, backup1, backup2)
		printUnlockNextSteps("")
		return nil
	}
	if !isSerialDevicePath(dev) {
		return fmt.Errorf("-unlock -device %s is not a serial VCOM path", dev)
	}
	if _, err := os.Stat(dev); err != nil {
		return fmt.Errorf("-device %s not present: power the phone off, plug it in download mode, then retry", dev)
	}
	script, python, err = resolveUnlockMTK(cfg)
	if err != nil {
		printUnlockManual(python, script, dir, outPath, backup1, backup2)
		return err
	}
	authArgs := []string{}
	if strings.TrimSpace(cfg.authFile) != "" {
		authArgs = append(authArgs, "--auth", cfg.authFile)
	}
	fmt.Println("mtkclient waits for the phone itself: plug the powered-off phone in")
	fmt.Println("download mode now, and replug it between stages if it disconnects.")
	fmt.Printf("Backup: reading boot1 -> %s\n", backup1)
	rArgs := append([]string{"r", "boot1", backup1}, authArgs...)
	if err := runUnlockMTK(dir, python, script, rArgs...); err != nil {
		printUnlockManual(python, script, dir, outPath, backup1, backup2)
		return fmt.Errorf("boot1 backup failed (refusing to write without one): %w", err)
	}
	fmt.Printf("Backup: reading boot2 -> %s (best effort)\n", backup2)
	if err := runUnlockMTK(dir, python, script, append([]string{"r", "boot2", backup2}, authArgs...)...); err != nil {
		fmt.Printf("warn: boot2 backup failed: %v; continuing (boot2 is untouched)\n", err)
	}

	chosen := outPath
	dump, err := os.ReadFile(backup1)
	if err != nil {
		return fmt.Errorf("read back %s: %w", backup1, err)
	}
	dumpFlag, derr := unlockFlagFingerprint(dump)
	fileFlag, ferr := unlockFlagFingerprint(data)
	switch {
	case derr != nil:
		return fmt.Errorf("%s has no ROMINFO flag block; refusing to write a stranger layout", backup1)
	case ferr != nil:
		return fmt.Errorf("%s lost its ROMINFO flag block; refusing to continue", src)
	case !bytes.Equal(dumpFlag, fileFlag):
		redone, _, err := patchUnlockPreloader(dump)
		if err != nil {
			return fmt.Errorf("patch %s: %w", backup1, err)
		}
		chosen = filepath.Join(dir, "boot1-patched.bin")
		if err := os.WriteFile(chosen, redone, 0644); err != nil {
			return err
		}
		fmt.Printf("dump flag data differs from %s; patched the dump instead -> %s\n", src, chosen)
	default:
		fmt.Printf("dump matches %s; writing %s\n", src, chosen)
	}

	if err := confirmUnlockWrite("boot1 on "+dev, chosen, cfg.yes); err != nil {
		return err
	}
	wArgs := append([]string{"w", "boot1", chosen}, authArgs...)
	if err := runUnlockMTK(dir, python, script, wArgs...); err != nil {
		printUnlockManual(python, script, dir, chosen, backup1, backup2)
		return fmt.Errorf("boot1 write failed: %w", err)
	}
	fmt.Printf("wrote %s to boot1 on %s\n", chosen, dev)
	printUnlockNextSteps(backup1)
	return nil
}
