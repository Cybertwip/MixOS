package main

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

/*
 * ── PHONE BOOT DIRS ──
 *
 * build/flash is one CLI for every boot dir: the j36 per-mode dirs and the
 * phone LK dirs (build/mt67xx/<device>/boot, build/mt68xx/<device>/boot),
 * selected with -root. The j36 paths below speak for j36 hardware only --
 * the BROM feed jumps an MT6592 DA, the scatter offsets are the j36's -- so
 * pointing them at a phone would write a stranger's bootloader layout onto
 * it. This file is the guard: it recognizes a phone boot dir by the
 * build-info.txt the family build.sh stages next to lk.bin, and refuses
 * every device path except the two that are honest for phones today:
 *
 *   fastboot, with the partition named explicitly by the operator
 *   (./flash -root <phone-dir> -upload lk -backend fastboot -partition <LK
 *   name from the stock scatter>), on an unlocked bootloader; and
 *   BROM raw exec, with payload bytes and address supplied explicitly by
 *   the operator (./flash -root <phone-dir> -address 0x... [payload.bin]
 *   -device /dev/cu.usbmodem...), which jumps only what it is given.
 *
 * Everything else -- the bundled-feed BROM flow, raw block, the live
 * console, the MTK read verbs -- stays j36-only until LK-BRINGUP step 6
 * wires phone backends (phone DA + scatter). The refusal messages say
 * exactly that, plus the working alternative (SP Flash Tool / mtkclient
 * per LK-BRINGUP step 2).
 */

// phoneRoot describes a phone LK boot dir.
type phoneRoot struct {
	device string // devices.sh codename, e.g. cph2381
	soc    string // e.g. mt6833
	family string // e.g. mt68xx, derived from soc for doc pointers
}

// detectPhoneRoot recognizes a phone LK boot dir by its build-info.txt
// (device= + soc=mt.... lines). j36 boot dirs carry no such file and
// never match, so all behavior below is additive: j36 runs cannot tell
// this file exists.
func detectPhoneRoot(root string) (*phoneRoot, bool) {
	raw, err := os.ReadFile(filepath.Join(root, "build-info.txt"))
	if err != nil {
		return nil, false
	}
	var device, soc string
	for _, line := range strings.Split(string(raw), "\n") {
		line = strings.TrimSpace(line)
		if rest, ok := strings.CutPrefix(line, "device="); ok {
			device = strings.TrimSpace(rest)
		} else if rest, ok := strings.CutPrefix(line, "soc="); ok {
			soc = strings.TrimSpace(rest)
		}
	}
	if device == "" || !strings.HasPrefix(strings.ToLower(soc), "mt") {
		return nil, false
	}
	phone := &phoneRoot{device: device, soc: strings.ToLower(soc)}
	phone.family = phoneFamilyForSoc(phone.soc)
	return phone, true
}

// phoneFamilyForSoc derives the LK tree family (mt67xx, mt68xx, ...) from a
// soc codename; shared by root detection and rootless phone flows.
func phoneFamilyForSoc(soc string) string {
	digits := strings.TrimPrefix(strings.ToLower(strings.TrimSpace(soc)), "mt")
	if len(digits) >= 2 && isASCIIDigits(digits[:2]) {
		return "mt" + digits[:2] + "xx"
	}
	return ""
}

func isASCIIDigits(s string) bool {
	if s == "" {
		return false
	}
	for i := 0; i < len(s); i++ {
		if s[i] < '0' || s[i] > '9' {
			return false
		}
	}
	return true
}

// bringupPointer names the playbook that owns this phone's flashing story.
func (p *phoneRoot) bringupPointer() string {
	if p.family != "" {
		return "tools/mediatek/" + p.family + "/LK-BRINGUP.md"
	}
	return "the phone LK tree's LK-BRINGUP.md"
}

// confirmWord is what the operator must type to confirm a flash: the j36
// sentence on j36 roots, the device codename on phone roots. Typing
// 'FLASH J36 ULTRA' at a phone would be muscle memory, not consent.
func confirmWord(cfg config) string {
	if phone, ok := detectPhoneRoot(cfg.root); ok {
		return "FLASH " + strings.ToUpper(phone.device)
	}
	return "FLASH J36 ULTRA"
}

// refusePhoneWrite gates every device-touching run with a phone -root.
// It returns nil only for the honest phone paths (fastboot with an
// explicit partition and no -device; BROM raw exec with explicit address
// and no feed/-upload flags; -unlock and -mtk-phone-write-boot1 with
// their own native phone-DA flow and consent); anything else fails with
// the reason and the
// alternative. Pure over cfg + the root dir, so the Go suite pins the
// whole matrix.
func refusePhoneWrite(cfg config, phone *phoneRoot) error {
	if cfg.listOnly {
		return nil // listing devices touches nothing
	}
	if dev := strings.TrimSpace(cfg.device); dev != "" {
		if !isSerialDevicePath(dev) {
			return fmt.Errorf("phone target %s: -device %s is a block device; raw-block writes take j36 offsets. "+
				"Flash via fastboot (-backend fastboot -partition <LK name from the stock scatter>, no -device), "+
				"BROM raw exec (-address 0x... [payload]), or SP Flash Tool / mtkclient per %s step 2",
				phone.device, dev, phone.bringupPointer())
		}
		// Serial VCOM: raw exec iff fully operator-addressed (address set,
		// no -upload, no feed/j36 flags -- mirroring the dispatch order in
		// run(), where this shape returns before the auto-feed). Without
		// -address the same -device would select the j36 BROM feed.
		if j36OnlyVerb(cfg) == "" && hasRawAddress(cfg) && cfg.upload == "" {
			return nil
		}
		// -unlock and -mtk-phone-write-boot1 are phone-explicit (native
		// phone-DA boot1 backup/patch/write with their own consent); they
		// never select the j36 feed.
		if cfg.unlock || cfg.mtkPhoneWriteBoot1 != "" {
			return nil
		}
		// Staged phone BROM (step 6 groundwork): every staging file is
		// named, so run() validates the plan host-side instead of feeding
		// the j36 flow. No device I/O happens on that path yet.
		if isPhoneBROMShape(cfg) {
			return nil
		}
		return fmt.Errorf("phone target %s: serial VCOM without a raw-exec shape selects the j36 BROM feed "+
			"(MT6592 DA, j36 scatter). For BROM add -address 0x... [payload] with no -upload and no feed flags "+
			"(raw exec jumps only what it is given); for flashing use -backend fastboot -partition <LK name "+
			"from the stock scatter>, or SP Flash Tool / mtkclient per %s step 2",
			phone.device, phone.bringupPointer())
	}
	if backend := strings.ToLower(strings.TrimSpace(cfg.backend)); backend != "" && backend != "auto" && backend != "fastboot" {
		return fmt.Errorf("phone target %s: -backend=%s speaks j36 hardware (MT6592 DA, j36 scatter); "+
			"use -backend fastboot -partition <LK name from the stock scatter>, or SP Flash Tool / mtkclient per %s step 2",
			phone.device, cfg.backend, phone.bringupPointer())
	}
	if verb := j36OnlyVerb(cfg); verb != "" {
		return fmt.Errorf("phone target %s: %s is j36-only (MT6592 DA / live console); "+
			"phone BROM support lands at %s step 6 -- until then SP Flash Tool / mtkclient per step 2",
			phone.device, verb, phone.bringupPointer())
	}
	if cfg.upload != "" {
		target := effectiveUploadTarget(cfg)
		if !validUploadTarget(target) {
			return invalidUploadTargetError(cfg.upload)
		}
		for _, want := range uploadRequiredImages(cfg, target) {
			if !fileExists(want) {
				return fmt.Errorf("-upload %s needs %s, which this %s boot dir does not ship "+
					"(phone LKs ship lk.bin only until LK-BRINGUP step 6 grows a second policy); drop -upload or use -upload lk",
					target, want, phone.device)
			}
		}
	}
	return nil
}

// hasRawAddress reports whether the run names its own BROM address, the
// shape rawExecutePayload demands (it refuses to run without one, so there
// is no default address to leak across SoCs).
func hasRawAddress(cfg config) bool {
	return strings.TrimSpace(cfg.mtkPayloadAddr) != "" || strings.TrimSpace(cfg.mtkPayloadEntry) != ""
}

// j36OnlyVerb names the first j36-mechanism flag on this command line, or
// "" when none is present. Host-side analysis of operator-named files
// (-mtk-analyze-preloader, -mtk-decode-lk, -scatter alone) is deliberately
// absent: it touches no device.
func j36OnlyVerb(cfg config) string {
	switch {
	case cfg.mtkFeedPayload != "":
		return "-mtk-feed-payload"
	case cfg.mtkDumpPreloader != "":
		return "-mtk-dump-preloader"
	case cfg.mtkWritePreloader != "":
		return "-mtk-write-preloader"
	case cfg.mtkReadBootStatus:
		return "-mtk-read-boot-status"
	case cfg.mtkSelftestWrite:
		return "-mtk-selftest-write"
	case cfg.mtkRunStage1:
		return "-mtk-run-stage1"
	case cfg.mtkReadPartitions:
		return "-mtk-read-partitions"
	case cfg.bootFlag != "":
		return "-flag"
	case cfg.dbgConsole:
		return "-dbg"
	case cfg.dbgRun != "":
		return "-dbg-run"
	default:
		return ""
	}
}
