package main

import (
	"fmt"
	"os"
	"strconv"
	"strings"
)

// ── PHONE BROM STAGING (step 6 groundwork) ──
//
// A phone -root with a serial -device and -upload selects the J36 BROM feed
// today, which is refused in phone_target.go because it would jump an MT6592
// DA at J36 offsets onto the phone. The staged shape below is the honest
// first half of native phone BROM: it resolves and validates every host file
// the wire write will need (stock scatter, phone DA, preloader, SLA auth,
// our lk.bin against the scatter slot) and prints the SP Flash Tool
// equivalent, but performs no device I/O at all -- opening the serial port,
// the SLA exchange and the DA eMMC protocol land with LK-BRINGUP step 6.
// Until then the run ends with an error after a valid plan, so a staged run
// can never report a flash it did not perform.

type phoneBROMPlan struct {
	device    string
	soc       string
	hwCode    uint16
	partition string
	offset    uint64
	slotSize  uint64
	lkPath    string
	lkSize    uint64
	scatter   string
	daPath    string
	daRegions int
	preloader string
	auth      string
}

// phoneHWCode derives the BROM DA hw code from the soc in build-info.txt.
// MediaTek hw codes track the model number (mt6765 -> 0x6765).
func phoneHWCode(soc string) (uint16, error) {
	digits := strings.TrimPrefix(strings.ToLower(strings.TrimSpace(soc)), "mt")
	if digits == "" {
		return 0, fmt.Errorf("soc %q names no MediaTek model", soc)
	}
	n, err := strconv.ParseUint(digits, 16, 16)
	if err != nil || n == 0 {
		return 0, fmt.Errorf("soc %q is not a MediaTek model number", soc)
	}
	return uint16(n), nil
}

// isPhoneBROMShape reports whether this invocation stages a phone BROM
// write: phone root, serial VCOM (the BROM selector -- without it the same
// flags are the working fastboot path), an explicit LK partition, and every
// staging file named. Raw exec (operator -address, no -upload) and any
// j36-only verb keep their existing meaning and never match here.
func isPhoneBROMShape(cfg config) bool {
	if !isSerialDevicePath(cfg.device) {
		return false
	}
	if !cfg.partitionExplicit {
		return false
	}
	if !strings.EqualFold(effectiveUploadTarget(cfg), uploadLK) {
		return false
	}
	if hasRawAddress(cfg) {
		return false
	}
	if j36OnlyVerb(cfg) != "" {
		return false
	}
	return strings.TrimSpace(cfg.mtkScatter) != "" &&
		strings.TrimSpace(cfg.daLoader) != "" &&
		strings.TrimSpace(cfg.preloader) != "" &&
		strings.TrimSpace(cfg.authFile) != ""
}

// planPhoneBROM validates the staging files for a phone BROM write. It
// reads host files only and never touches a device.
func planPhoneBROM(cfg config, phone *phoneRoot) (*phoneBROMPlan, error) {
	if !cfg.partitionExplicit {
		return nil, fmt.Errorf("phone target: pass -partition explicitly (the LK slot name from the stock scatter); "+
			"the %q default is the J36's, and staging lk.bin at it would miss the slot", cfg.partition)
	}
	if !strings.EqualFold(effectiveUploadTarget(cfg), uploadLK) {
		return nil, fmt.Errorf("-upload %s: phone BROM stages lk.bin only until LK-BRINGUP step 6 grows a second policy; use -upload lk",
			effectiveUploadTarget(cfg))
	}
	hwCode, err := phoneHWCode(phone.soc)
	if err != nil {
		return nil, err
	}
	lkPath := strings.TrimSpace(cfg.image)
	if lkPath == "" {
		lkPath = defaultMVIILKImagePath(cfg)
	}
	lkInfo, err := os.Stat(lkPath)
	if err != nil || lkInfo.IsDir() {
		return nil, fmt.Errorf("phone target %s: LK image not found: %s (want the lk.bin in %s)",
			phone.device, lkPath, cfg.root)
	}
	entries, err := parseMTKScatterFile(cfg.mtkScatter)
	if err != nil {
		return nil, fmt.Errorf("phone target %s: scatter %s: %w", phone.device, cfg.mtkScatter, err)
	}
	var slot *mtkScatterEntry
	for i := range entries {
		if strings.EqualFold(entries[i].PartitionName, strings.TrimSpace(cfg.partition)) {
			slot = &entries[i]
			break
		}
	}
	if slot == nil {
		return nil, fmt.Errorf("phone target %s: scatter %s names no partition %q",
			phone.device, cfg.mtkScatter, cfg.partition)
	}
	if !strings.EqualFold(slot.Region, "EMMC_USER") {
		return nil, fmt.Errorf("phone target %s: scatter %s lists %s in region %q, want EMMC_USER",
			phone.device, cfg.mtkScatter, slot.PartitionName, slot.Region)
	}
	if !slot.IsDownload {
		return nil, fmt.Errorf("phone target %s: scatter %s lists %s with is_download false",
			phone.device, cfg.mtkScatter, slot.PartitionName)
	}
	if slot.PartitionSize != 0 && uint64(lkInfo.Size()) > slot.PartitionSize {
		return nil, fmt.Errorf("phone target %s: lk.bin is 0x%x but scatter %s sizes %s at 0x%x",
			phone.device, uint64(lkInfo.Size()), cfg.mtkScatter, slot.PartitionName, slot.PartitionSize)
	}
	loader, err := parseMTKDALoader(cfg.daLoader, hwCode, 0, 0)
	if err != nil {
		return nil, fmt.Errorf("phone target %s: DA %s: %w", phone.device, cfg.daLoader, err)
	}
	if _, err := readDARegion(loader, 0); err != nil {
		return nil, fmt.Errorf("phone target %s: DA %s: %w", phone.device, cfg.daLoader, err)
	}
	if !fileExists(cfg.preloader) {
		return nil, fmt.Errorf("phone target %s: preloader not found: %s (DRAM EMI comes from the stock preloader)",
			phone.device, cfg.preloader)
	}
	if !fileExists(cfg.authFile) {
		return nil, fmt.Errorf("phone target %s: auth file not found: %s (retail BROM demands SLA authentication; the stock package ships auth_sv5.auth)",
			phone.device, cfg.authFile)
	}
	return &phoneBROMPlan{
		device:    phone.device,
		soc:       phone.soc,
		hwCode:    hwCode,
		partition: slot.PartitionName,
		offset:    slot.LinearStart,
		slotSize:  slot.PartitionSize,
		lkPath:    lkPath,
		lkSize:    uint64(lkInfo.Size()),
		scatter:   cfg.mtkScatter,
		daPath:    cfg.daLoader,
		daRegions: len(loader.Regions),
		preloader: cfg.preloader,
		auth:      cfg.authFile,
	}, nil
}

// runPhoneBROMPlan prints a valid staging plan and stops: the wire write
// (SLA exchange + DA eMMC protocol) is LK-BRINGUP step 6, so today the
// actual flash is SP Flash Tool with the files below. Nothing is written to
// any device by this path, and the non-nil return keeps the exit non-zero.
func runPhoneBROMPlan(cfg config, phone *phoneRoot) error {
	plan, err := planPhoneBROM(cfg, phone)
	if err != nil {
		return err
	}
	fmt.Printf("Phone BROM staged for %s (%s, hw code 0x%04x):\n", plan.device, plan.soc, plan.hwCode)
	fmt.Printf("  LK image:  %s (0x%x) -> %s at eMMC offset 0x%x (slot 0x%x, EMMC_USER)\n",
		plan.lkPath, plan.lkSize, plan.partition, plan.offset, plan.slotSize)
	fmt.Printf("  Scatter:   %s\n", plan.scatter)
	fmt.Printf("  DA:        %s (%d regions)\n", plan.daPath, plan.daRegions)
	fmt.Printf("  Preloader: %s (DRAM EMI)\n", plan.preloader)
	fmt.Printf("  Auth:      %s (SLA)\n", plan.auth)
	return fmt.Errorf("phone target %s: BROM wire write is not implemented (LK-BRINGUP step 6: SLA exchange + DA eMMC protocol); "+
		"nothing was written -- flash today with SP Flash Tool, Download Only, %s <- lk.bin, using the scatter, DA, preloader and auth above",
		plan.device, plan.partition)
}
