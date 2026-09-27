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
 * every device path except the one that is honest for phones today:
 *
 *   fastboot, with the partition named explicitly by the operator
 *   (./flash -root <phone-dir> -upload lk -backend fastboot -partition <LK
 *   name from the stock scatter>), on an unlocked bootloader.
 *
 * Everything else -- BROM feed, raw block, the live console, the MTK read
 * verbs -- stays j36-only until LK-BRINGUP step 6 wires phone backends
 * (phone DA + scatter). The refusal messages say exactly that, plus the
 * working alternative (SP Flash Tool / mtkclient per LK-BRINGUP step 2).
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
	digits := strings.TrimPrefix(phone.soc, "mt")
	if len(digits) >= 2 && isASCIIDigits(digits[:2]) {
		phone.family = "mt" + digits[:2] + "xx"
	}
	return phone, true
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
		return "tools/" + p.family + "/LK-BRINGUP.md"
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
// It returns nil only for the one honest phone path (explicit fastboot,
// no -device, no j36-only verbs, -upload resolving to files present in
// this root); anything else fails with the reason and the alternative.
// Pure over cfg + the root dir, so the Go suite pins the whole matrix.
func refusePhoneWrite(cfg config, phone *phoneRoot) error {
	if cfg.listOnly {
		return nil // listing devices touches nothing
	}
	if dev := strings.TrimSpace(cfg.device); dev != "" {
		return fmt.Errorf("phone target %s: -device %s selects the j36 BROM feed / j36 block offsets; "+
			"./flash reaches phones through fastboot only (-backend fastboot -partition <LK name from the stock scatter>, "+
			"no -device), or SP Flash Tool / mtkclient per %s step 2",
			phone.device, dev, phone.bringupPointer())
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
