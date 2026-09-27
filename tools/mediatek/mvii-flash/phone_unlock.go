package main

// -unlock: OPPO MTK fastboot unlock through a preloader patch, and
// -mtk-phone-write-boot1: raw boot1 restore. Both run the native phone
// BROM/DA transport in this tool; no external flasher is involved.
//
// The patch is pattern-anchored (magic + BRLYT offsets + ROMINFO search),
// following path_preloader.py: relocate the 0x800 code blob to 0x2000,
// repoint the BRLYT layout bytes at it, plant the ROMINFO flag block at
// 0x1000, and clear the fastboot lock byte (0x22 -> 0x00).
//
//   ./flash -unlock -preloader boot1.bin -device /dev/cu.usbmodemXXXX \
//       -da-loader DA.bin [-auth auth_sv5.auth] [-yes]
//
// Without -device this only writes the patched image next to the input and
// prints the device command to re-run. With -device it backs up boot1/boot2
// over the DA, patches the dump when its flag data differs from the input
// file, and writes the patched image back. The write needs -yes or typed
// consent, and never runs without a fresh boot1 backup in hand.

import (
	"bufio"
	"bytes"
	"errors"
	"fmt"
	"os"
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
	unlockConfirmWord = "WRITE BOOT1"
	// unlockBootDumpLength is the full boot1/boot2 hardware area. Backups
	// stay restorable byte-for-byte and dumps patch without resizing.
	unlockBootDumpLength = 0x400000
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

func requireUnlockFile(kind, path string) error {
	if strings.TrimSpace(path) == "" {
		return fmt.Errorf("phone unlock needs %s (DRAM + eMMC come from the download agent)", kind)
	}
	if !fileExists(path) {
		return fmt.Errorf("%s not found: %s", kind, path)
	}
	return nil
}

func defaultUnlockOutput(src string) string {
	dir := filepath.Dir(src)
	base := filepath.Base(src)
	stem := strings.TrimSuffix(base, filepath.Ext(base))
	return filepath.Join(dir, stem+"-patched.bin")
}

func padToSerialBlock(data []byte) []byte {
	out := make([]byte, alignUp(uint64(len(data)), mtkSerialBlockSize))
	copy(out, data)
	return out
}

type phoneUnlockPlan struct {
	device    string
	soc       string
	hwCode    uint16
	daCode    uint16
	source    string
	patched   string
	daPath    string
	daRegions int
	emiPath   string
	auth      string
}

// planPhoneUnlock validates the staging files for a phone boot1 operation.
// It reads host files only and never touches a device.
func planPhoneUnlock(cfg config, phone *phoneRoot, srcPath, patchedPath, emiPath string) (*phoneUnlockPlan, error) {
	hwCode, err := phoneBROMCode(phone.soc)
	if err != nil {
		return nil, err
	}
	daCode, err := phoneDACode(phone.soc)
	if err != nil {
		return nil, err
	}
	info, err := os.Stat(srcPath)
	if err != nil || info.IsDir() {
		return nil, fmt.Errorf("phone target %s: image not found: %s", phone.device, srcPath)
	}
	if strings.TrimSpace(cfg.daLoader) == "" {
		return nil, fmt.Errorf("phone target %s: boot1 access needs -da-loader /path/to/DA.bin (DRAM + eMMC come from the download agent)", phone.device)
	}
	loader, err := parseMTKDALoader(cfg.daLoader, daCode, 0, 0)
	if err != nil {
		return nil, fmt.Errorf("phone target %s: DA %s: %w", phone.device, cfg.daLoader, err)
	}
	if len(loader.Regions) <= 2 {
		return nil, fmt.Errorf("DA loader %s has %d regions; the legacy DA stack needs stage 1 and stage 2",
			loader.Path, len(loader.Regions))
	}
	if !fileExists(emiPath) {
		return nil, fmt.Errorf("phone target %s: EMI source not found: %s (DRAM EMI comes from the stock preloader)", phone.device, emiPath)
	}
	auth := ""
	if a := strings.TrimSpace(cfg.authFile); a != "" {
		if !fileExists(a) {
			return nil, fmt.Errorf("phone target %s: auth file not found: %s", phone.device, a)
		}
		auth = a
	}
	return &phoneUnlockPlan{
		device: phone.device, soc: phone.soc, hwCode: hwCode, daCode: daCode,
		source: srcPath, patched: patchedPath,
		daPath: cfg.daLoader, daRegions: len(loader.Regions),
		emiPath: emiPath, auth: auth,
	}, nil
}

func printPhoneUnlockPlan(plan *phoneUnlockPlan) {
	fmt.Printf("Phone unlock for %s (%s, BROM hw code 0x%04x, DA entry 0x%04x):\n", plan.device, plan.soc, plan.hwCode, plan.daCode)
	if plan.patched == "" {
		fmt.Printf("  Image:     %s (verbatim boot1 write, no patch)\n", plan.source)
	} else {
		fmt.Printf("  Source:    %s -> %s\n", plan.source, plan.patched)
	}
	fmt.Printf("  DA:        %s (%d regions)\n", plan.daPath, plan.daRegions)
	fmt.Printf("  Preloader: %s (DRAM EMI)\n", plan.emiPath)
	if plan.auth != "" {
		fmt.Printf("  Auth:      %s (sent via SEND_AUTH when BROM enforces DAA)\n", plan.auth)
	} else {
		fmt.Printf("  Auth:      none provided (DAA targets are attempted without it)\n")
	}
}

// resolveUnlockPhone names the target: a phone -root when one is given,
// else one handshake to read the hw code off the wire.
func resolveUnlockPhone(cfg config) (*phoneRoot, phoneFacts, error) {
	if phone, ok := detectPhoneRoot(cfg.root); ok {
		facts, err := phoneFactsFor(phone.soc)
		if err != nil {
			return nil, phoneFacts{}, err
		}
		return phone, facts, nil
	}
	client, err := connectMTKSerialWithOptions(cfg.device, mtkSerialConnectOptions{handshakeWake: true})
	if err != nil {
		return nil, phoneFacts{}, err
	}
	hw, _, hwErr := client.getHWCode()
	_ = client.port.Close()
	if hwErr != nil {
		return nil, phoneFacts{}, hwErr
	}
	soc, facts, err := phoneFactsForHWCode(hw)
	if err != nil {
		return nil, phoneFacts{}, err
	}
	fmt.Printf("Target identifies as %s (hw code 0x%04x).\n", soc, hw)
	return &phoneRoot{device: soc, soc: soc, family: phoneFamilyForSoc(soc)}, facts, nil
}

func confirmUnlockWrite(destination, image string, yes bool) error {
	fmt.Println()
	fmt.Printf("About to write %s to %s.\n", image, destination)
	fmt.Println("A bad boot1 image stops the phone booting until a good image is written back.")
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

func printUnlockOfflineSteps(src string) {
	fmt.Println()
	fmt.Println("No -device: patched image only. To back up and write on a phone, re-run:")
	fmt.Printf("  ./flash -unlock -preloader %q -device /dev/cu.usbmodemXXXX -da-loader <DA.bin> [-auth <auth>] [-yes]\n", src)
	fmt.Println("Re-running re-patches deterministically; the write needs -yes or typed consent.")
}

func printUnlockNextSteps(restoreImage, dev, daLoader, preloader, auth string) {
	fmt.Println()
	fmt.Println("Next: enable OEM unlocking in developer settings, then:")
	fmt.Println("  adb reboot bootloader")
	fmt.Println("  fastboot flashing unlock   (confirm with a volume key on the phone)")
	if restoreImage == "" {
		return
	}
	if dev == "" {
		dev = "/dev/cu.usbmodemXXXX"
	}
	if daLoader == "" {
		daLoader = "<DA.bin>"
	}
	authFlag := ""
	if strings.TrimSpace(auth) != "" {
		authFlag = fmt.Sprintf(" -auth %q", auth)
	}
	fmt.Printf("Recovery: ./flash -mtk-phone-write-boot1 %q -device %q -da-loader %q -preloader %q%s -yes\n",
		restoreImage, dev, daLoader, preloader, authFlag)
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
	if dev == "" {
		printUnlockOfflineSteps(src)
		printUnlockNextSteps(src, "", cfg.daLoader, src, cfg.authFile)
		return nil
	}
	if !isSerialDevicePath(dev) {
		return fmt.Errorf("-unlock -device %s is not a serial VCOM path", dev)
	}
	if err := requireUnlockFile("-da-loader /path/to/DA.bin", cfg.daLoader); err != nil {
		return err
	}
	if a := strings.TrimSpace(cfg.authFile); a != "" {
		if err := requireUnlockFile("auth file", a); err != nil {
			return err
		}
	}
	phone, facts, err := resolveUnlockPhone(cfg)
	if err != nil {
		return err
	}
	emi, err := resolvePhoneEMI(cfg)
	if err != nil {
		return err
	}
	plan, err := planPhoneUnlock(cfg, phone, src, outPath, cfg.preloader)
	if err != nil {
		return err
	}
	printPhoneUnlockPlan(plan)
	packetSize, err := mtkSerialPacketSize(cfg)
	if err != nil {
		return err
	}
	loader, err := parseMTKDALoader(plan.daPath, plan.daCode, 0, 0)
	if err != nil {
		return fmt.Errorf("phone target %s: DA %s: %w", phone.device, plan.daPath, err)
	}
	fmt.Printf("Using DA loader: %s (hw code 0x%04x)\n", loader.Path, loader.HWCode)
	fmt.Printf("Using device preloader EMI: %s (version 0x%x, length 0x%x)\n", emi.Path, emi.Version, len(emi.Data))
	fmt.Printf("Using MTK serial packet size: 0x%x\n", packetSize)
	client, _, err := startPhoneDA(cfg, phone, facts, emi, loader, plan.auth)
	if err != nil {
		return err
	}
	defer func() {
		if client != nil && client.port != nil {
			_ = client.port.Close()
		}
	}()

	dir := filepath.Dir(outPath)
	backup1 := filepath.Join(dir, "boot1-stock.bin")
	backup2 := filepath.Join(dir, "boot2-stock.bin")
	fmt.Printf("Backup: reading boot1 (0x%x bytes) -> %s\n", unlockBootDumpLength, backup1)
	dump, err := client.readLegacyEMMC(mtkLegacyEMMCPartBoot1, 0, unlockBootDumpLength, packetSize)
	if err != nil {
		return fmt.Errorf("boot1 backup failed (refusing to write without one): %w", err)
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return err
	}
	if err := os.WriteFile(backup1, dump, 0o644); err != nil {
		return err
	}
	fmt.Printf("Backup: reading boot2 (best effort) -> %s\n", backup2)
	if dump2, err := client.readLegacyEMMC(mtkLegacyEMMCPartBoot2, 0, unlockBootDumpLength, packetSize); err != nil {
		fmt.Printf("warn: boot2 backup failed: %v; continuing (boot2 is untouched)\n", err)
	} else if err := os.WriteFile(backup2, dump2, 0o644); err != nil {
		fmt.Printf("warn: write %s: %v; continuing (boot2 is untouched)\n", backup2, err)
	}

	chosenPath, chosenBytes := outPath, patched
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
		chosenPath = filepath.Join(dir, "boot1-patched.bin")
		if err := os.WriteFile(chosenPath, redone, 0o644); err != nil {
			return err
		}
		chosenBytes = redone
		fmt.Printf("dump flag data differs from %s; patched the dump instead -> %s\n", src, chosenPath)
	default:
		fmt.Printf("dump matches %s; writing %s\n", src, chosenPath)
	}

	destination := fmt.Sprintf("boot1 on %s", dev)
	if err := confirmUnlockWrite(destination, chosenPath, cfg.yes); err != nil {
		return err
	}
	padded := padToSerialBlock(chosenBytes)
	if err := client.writeLegacyEMMCPartition(mtkLegacyEMMCPartBoot1, 0, padded, packetSize); err != nil {
		return fmt.Errorf("boot1 write failed (restore with %s): %w", backup1, err)
	}
	fmt.Printf("wrote %s to %s\n", chosenPath, destination)
	printUnlockNextSteps(backup1, dev, plan.daPath, plan.emiPath, plan.auth)
	return nil
}

// runPhoneWriteBoot1 writes an image verbatim to phone eMMC BOOT1: the
// native restore path for -unlock backups (and stock preloaders). DRAM EMI
// comes from -preloader when given, else from the image itself, which for
// boot1 dumps carries the same EMI record.
func runPhoneWriteBoot1(cfg config, file string) error {
	dev := strings.TrimSpace(cfg.device)
	if dev == "" {
		return errors.New("-mtk-phone-write-boot1 requires -device /dev/cu.usbmodem... or another MTK VCOM serial device")
	}
	if !isSerialDevicePath(dev) {
		return errors.New("-mtk-phone-write-boot1 -device is not a serial VCOM path")
	}
	data, err := os.ReadFile(file)
	if err != nil {
		return err
	}
	if err := requireUnlockFile("-da-loader /path/to/DA.bin", cfg.daLoader); err != nil {
		return err
	}
	if a := strings.TrimSpace(cfg.authFile); a != "" {
		if err := requireUnlockFile("auth file", a); err != nil {
			return err
		}
	}
	emiPath := strings.TrimSpace(cfg.preloader)
	var emi *mtkPreloaderEMI
	if emiPath == "" {
		emiPath = file
		emi, err = readMTKPreloaderEMI(file)
	} else {
		emi, err = resolvePhoneEMI(cfg)
	}
	if err != nil {
		return err
	}
	phone, facts, err := resolveUnlockPhone(cfg)
	if err != nil {
		return err
	}
	plan, err := planPhoneUnlock(cfg, phone, file, "", emiPath)
	if err != nil {
		return err
	}
	printPhoneUnlockPlan(plan)
	packetSize, err := mtkSerialPacketSize(cfg)
	if err != nil {
		return err
	}
	loader, err := parseMTKDALoader(plan.daPath, plan.daCode, 0, 0)
	if err != nil {
		return fmt.Errorf("phone target %s: DA %s: %w", phone.device, plan.daPath, err)
	}
	client, _, err := startPhoneDA(cfg, phone, facts, emi, loader, plan.auth)
	if err != nil {
		return err
	}
	defer func() {
		if client != nil && client.port != nil {
			_ = client.port.Close()
		}
	}()

	destination := fmt.Sprintf("boot1 on %s", dev)
	if err := confirmUnlockWrite(destination, file, cfg.yes); err != nil {
		return err
	}
	if err := client.writeLegacyEMMCPartition(mtkLegacyEMMCPartBoot1, 0, padToSerialBlock(data), packetSize); err != nil {
		return fmt.Errorf("boot1 write failed: %w", err)
	}
	fmt.Printf("wrote %s to %s\n", file, destination)
	return nil
}
