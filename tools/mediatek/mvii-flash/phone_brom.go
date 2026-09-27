package main

import (
	"errors"
	"fmt"
	"os"
	"strconv"
	"strings"
)

// ── PHONE BROM FLASH (Oppo / LG, no fastboot) ──
//
// The same legacy DA stack the J36 Ultra flashes through, parameterized for
// phone roots: the DA loader entry is selected by the soc in build-info.txt,
// DRAM/EMI comes from the operator's stock preloader (never the mt6592
// built-ins), and the eMMC offset comes from the validated stock scatter
// slot -- never a J36 default. The wire itself reuses the exercised J36
// functions unchanged (handshake, SEND_DA, stage-1 sync, stage-2 upload, EMI
// config, sdmmc_write_data); this file adds no new protocol bytes. What it
// cannot do is authenticate: a BROM that enforces SLA/DAA is refused at
// probe time, before anything moves, because the tool speaks no auth
// exchange and the stock auth file has no consumer here.

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

// isPhoneBROMShape reports whether this invocation flashes a phone over
// BROM: phone root, serial VCOM (the BROM selector -- without it the same
// flags are the working fastboot path), an explicit LK partition, and the
// staging files named. Raw exec (operator -address, no -upload) and any
// j36-only verb keep their existing meaning and never match here. The auth
// file is deliberately not required: the tool cannot consume it, so demanding
// it would promise an SLA exchange that does not exist; SLA/DAA targets are
// refused at probe time instead.
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
	hasPlace := strings.TrimSpace(cfg.mtkScatter) != "" || strings.TrimSpace(cfg.rawOffset) != ""
	return hasPlace &&
		strings.TrimSpace(cfg.daLoader) != "" &&
		strings.TrimSpace(cfg.preloader) != ""
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
	// Placement is the scatter slot when -scatter is given, else the
	// operator's explicit -raw-offset (J36 legacy style). The raw form
	// carries no slot size, so the fit check is skipped and the plan says
	// so; the DA-reported eMMC bounds check still guards the write.
	partition := strings.TrimSpace(cfg.partition)
	var offset, slotSize uint64
	if s := strings.TrimSpace(cfg.mtkScatter); s != "" {
		entries, err := parseMTKScatterFile(s)
		if err != nil {
			return nil, fmt.Errorf("phone target %s: scatter %s: %w", phone.device, s, err)
		}
		var slot *mtkScatterEntry
		for i := range entries {
			if strings.EqualFold(entries[i].PartitionName, partition) {
				slot = &entries[i]
				break
			}
		}
		if slot == nil {
			return nil, fmt.Errorf("phone target %s: scatter %s names no partition %q",
				phone.device, s, cfg.partition)
		}
		if !strings.EqualFold(slot.Region, "EMMC_USER") {
			return nil, fmt.Errorf("phone target %s: scatter %s lists %s in region %q, want EMMC_USER",
				phone.device, s, slot.PartitionName, slot.Region)
		}
		if !slot.IsDownload {
			return nil, fmt.Errorf("phone target %s: scatter %s lists %s with is_download false",
				phone.device, s, slot.PartitionName)
		}
		if slot.PartitionSize != 0 && uint64(lkInfo.Size()) > slot.PartitionSize {
			return nil, fmt.Errorf("phone target %s: lk.bin is 0x%x but scatter %s sizes %s at 0x%x",
				phone.device, uint64(lkInfo.Size()), s, slot.PartitionName, slot.PartitionSize)
		}
		partition, offset, slotSize = slot.PartitionName, slot.LinearStart, slot.PartitionSize
	} else {
		off, err := parseMTKNumber(strings.TrimSpace(cfg.rawOffset), "-raw-offset")
		if err != nil {
			return nil, err
		}
		offset = off
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
	auth := ""
	if a := strings.TrimSpace(cfg.authFile); a != "" {
		if !fileExists(a) {
			return nil, fmt.Errorf("phone target %s: auth file not found: %s", phone.device, a)
		}
		auth = a
	}
	return &phoneBROMPlan{
		device:    phone.device,
		soc:       phone.soc,
		hwCode:    hwCode,
		partition: partition,
		offset:    offset,
		slotSize:  slotSize,
		lkPath:    lkPath,
		lkSize:    uint64(lkInfo.Size()),
		scatter:   cfg.mtkScatter,
		daPath:    cfg.daLoader,
		daRegions: len(loader.Regions),
		preloader: cfg.preloader,
		auth:      auth,
	}, nil
}

func printPhoneBROMPlan(plan *phoneBROMPlan) {
	fmt.Printf("Phone BROM flash for %s (%s, hw code 0x%04x):\n", plan.device, plan.soc, plan.hwCode)
	slot := fmt.Sprintf("0x%x", plan.slotSize)
	if plan.scatter == "" {
		slot = "unknown (-raw-offset carries no slot size)"
	}
	fmt.Printf("  LK image:  %s (0x%x) -> %s at eMMC offset 0x%x (slot %s, EMMC_USER)\n",
		plan.lkPath, plan.lkSize, plan.partition, plan.offset, slot)
	scatter := plan.scatter
	if scatter == "" {
		scatter = "none (explicit -raw-offset)"
	}
	fmt.Printf("  Scatter:   %s\n", scatter)
	fmt.Printf("  DA:        %s (%d regions)\n", plan.daPath, plan.daRegions)
	fmt.Printf("  Preloader: %s (DRAM EMI)\n", plan.preloader)
	if plan.auth != "" {
		fmt.Printf("  Auth:      %s (recorded; the tool speaks no SLA exchange)\n", plan.auth)
	} else {
		fmt.Printf("  Auth:      none provided (proceeds only if BROM enforces no SLA/DAA)\n")
	}
}

// resolvePhoneEMI loads DRAM/EMI config from the operator's stock preloader.
// The mt6592 built-in profiles are never consulted: phone DRAM calibration
// comes from the device's own preloader or nothing does.
func resolvePhoneEMI(cfg config) (*mtkPreloaderEMI, error) {
	if strings.TrimSpace(cfg.preloader) == "" {
		return nil, errors.New("phone BROM needs -preloader /path/to/preloader_*.bin (DRAM EMI comes from the stock preloader)")
	}
	if !fileExists(cfg.preloader) {
		return nil, fmt.Errorf("-preloader %s does not exist", cfg.preloader)
	}
	return readMTKPreloaderEMI(cfg.preloader)
}

// refusePhoneSLA fails fast when the target enforces authentication: the
// tool speaks no SLA/DAA exchange, so proceeding would only die at SEND_DA
// with 0x1D0D. Checked at probe time, before anything moves.
func refusePhoneSLA(phone *phoneRoot, target mtkTargetConfig) error {
	if !target.SLA && !target.DAA {
		return nil
	}
	which := "SLA"
	switch {
	case target.SLA && target.DAA:
		which = "SLA/DAA"
	case target.DAA:
		which = "DAA"
	}
	return fmt.Errorf("phone target %s: BROM enforces %s authentication and this tool speaks no auth exchange; "+
		"flash this unit with SP Flash Tool (or mtkclient) and the stock auth_sv5.auth per LK-BRINGUP step 2", phone.device, which)
}

// probePhoneBROM is the phone sibling of probeMT6592: same reconnect loop,
// but the HW gate is the phone's own code and the watchdog poke is skipped --
// TOPRGU is ungrounded on phones, and a blind write32 to a guessed base
// risks worse than the watchdog itself. A mid-session BROM reset fails the
// run loudly; the LK slot stays reflashable because the preloader is never
// touched.
func probePhoneBROM(c *mtkSerialClient, phone *phoneRoot, hwCode uint16) error {
	const maxReconnect = 8
	for attempt := 0; ; attempt++ {
		err := probePhoneBROMOnce(c, phone, hwCode)
		if err == nil {
			return nil
		}
		if !isDeviceGoneError(err) || attempt >= maxReconnect {
			return err
		}
		fmt.Printf("MTK device dropped during probe (%v); the BROM re-enumerated USB. Reconnecting...\n", err)
		if rerr := c.reopenBROMHandshake(); rerr != nil {
			return fmt.Errorf("%w; reconnect after device drop failed: %v", err, rerr)
		}
	}
}

func probePhoneBROMOnce(c *mtkSerialClient, phone *phoneRoot, hwCode uint16) error {
	got, hwVer, err := c.getHWCode()
	if err != nil {
		return err
	}
	fmt.Printf("MTK HW code: 0x%04x, HW version: 0x%04x\n", got, hwVer)
	if got != hwCode {
		return fmt.Errorf("connected MediaTek target is 0x%04x, expected %s (%s, hw code 0x%04x)",
			got, phone.device, phone.soc, hwCode)
	}
	fmt.Println("Phone BROM: leaving the watchdog alone (no grounded TOPRGU base on phones).")
	target, err := c.getTargetConfig()
	if err != nil {
		return err
	}
	fmt.Printf("Target config: 0x%08x (SBC=%t SLA=%t DAA=%t)\n", target.Raw, target.SBC, target.SLA, target.DAA)
	if err := refusePhoneSLA(phone, target); err != nil {
		return err
	}
	blver, isBROM, err := c.getBLVersion()
	if err != nil {
		return err
	}
	mode := "preloader"
	if isBROM {
		mode = "BROM"
	}
	bromver, err := c.getBROMVersion()
	if err != nil {
		return err
	}
	c.blVersion = blver
	c.bromVersion = bromver
	c.isBROM = isBROM
	fmt.Printf("MTK mode: %s, BL version: 0x%02x, BROM version: 0x%02x\n", mode, blver, bromver)
	if _, _, _, err := c.getHWSWVersion(); err == nil {
		// Best-effort metadata, as on the J36 path.
	} else {
		fmt.Printf("Warning: could not read HW/SW version tuple: %v\n", err)
	}
	return nil
}

// flashPhoneBROM validates the staging plan, then runs the legacy DA stack
// against the phone: probe, stage-1/2 DA upload, DRAM init from the stock
// preloader EMI, and the sdmmc write of lk.bin at the scatter slot offset.
// Consent uses the phone confirm word (confirmFlash), never the J36's.
func flashPhoneBROM(cfg config, phone *phoneRoot) error {
	plan, err := planPhoneBROM(cfg, phone)
	if err != nil {
		return err
	}
	printPhoneBROMPlan(plan)
	loader, err := parseMTKDALoader(plan.daPath, plan.hwCode, 0, 0)
	if err != nil {
		return fmt.Errorf("phone target %s: DA %s: %w", phone.device, plan.daPath, err)
	}
	if len(loader.Regions) <= 2 {
		return fmt.Errorf("DA loader %s has %d regions; the legacy writer needs stage 1 and stage 2",
			loader.Path, len(loader.Regions))
	}
	emi, err := resolvePhoneEMI(cfg)
	if err != nil {
		return err
	}
	rawLength := alignUp(plan.lkSize, mtkSerialBlockSize)
	packetSize, err := mtkSerialPacketSize(cfg)
	if err != nil {
		return err
	}
	image, cleanup, err := prepareRawFlashImage(plan.lkPath, int64(plan.lkSize), rawLength)
	if err != nil {
		return err
	}
	defer cleanup()

	destination := fmt.Sprintf("phone %s BROM serial %s at eMMC offset 0x%x", phone.device, cfg.device, plan.offset)
	if err := confirmFlash(cfg, destination, image, plan.partition); err != nil {
		return err
	}

	fmt.Println("Phone BROM mode expects the phone powered off or in PreLoader/BROM VCOM mode.")
	fmt.Println("If the handshake waits, hold the boot/download key combo while reconnecting USB.")
	fmt.Printf("Using DA loader: %s (hw code 0x%04x)\n", loader.Path, loader.HWCode)
	fmt.Printf("Using device preloader EMI: %s (version 0x%x, length 0x%x)\n", emi.Path, emi.Version, len(emi.Data))
	fmt.Printf("Using MTK serial packet size: 0x%x\n", packetSize)

	client, err := connectMTKSerial(cfg.device)
	if err != nil {
		return err
	}
	client.preloaderEMI = emi
	defer func() {
		_ = client.port.Close()
	}()

	if err := probePhoneBROM(client, phone, plan.hwCode); err != nil {
		return err
	}
	if err := client.uploadLegacyDA(loader); err != nil {
		return err
	}
	if err := client.writeLegacyEMMCRaw(image, plan.offset, rawLength, packetSize); err != nil {
		return err
	}
	fmt.Println("Phone BROM DA flash complete.")
	return nil
}
