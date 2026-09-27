package main

import (
	"errors"
	"fmt"
	"os"
	"strings"
	"time"
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

// mtkCmdSendAuth is the BROM command that uploads the vendor auth blob
// (auth_sv5.auth) on targets enforcing DAA. The exchange -- echo, big-endian
// length round-trip, blob, crc, status -- follows the wire shape every
// MediaTek flasher speaks; the tool learns it needs this step from the
// target-config DAA bit at probe time.
const mtkCmdSendAuth = 0xE2

type phoneBROMPlan struct {
	device    string
	soc       string
	hwCode    uint16
	daCode    uint16
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

// phoneFacts are the per-soc BROM constants: the code BROM reports (a
// different namespace from the DA entry tags), the DA tag, and the watchdog
// + BOOT_MISC bases for session care. BROM codes are board-observed where
// noted, else taken from the reference flasher's chip table -- a wrong one
// fails safe at probe time, where the mismatch names both codes.
type phoneFacts struct {
	bromCode    uint16
	daCode      uint16
	watchdog    uint32
	watchdogOff uint32
	miscLock    uint32 // 0 when ungrounded: no auto preloader->BROM reset
}

func phoneFactsFor(soc string) (phoneFacts, error) {
	switch strings.ToLower(strings.TrimSpace(soc)) {
	case "mt6765":
		return phoneFacts{bromCode: 0x0766, daCode: 0x6765, // 0x0766 board-observed 2026-09-27 on a retail CPH2385
			watchdog: 0x10007000, watchdogOff: 0x22000064, miscLock: 0x1001a100}, nil
	case "mt6739":
		return phoneFacts{bromCode: 0x0699, daCode: 0x6739,
			watchdog: 0x10007000, watchdogOff: 0x22000064, miscLock: 0x1001a100}, nil
	case "mt6833":
		return phoneFacts{bromCode: 0x0989, daCode: 0x6833,
			watchdog: 0x10007000, watchdogOff: 0x22000064, miscLock: 0}, nil
	}
	return phoneFacts{}, fmt.Errorf("soc %q is not a known phone soc", soc)
}

// phoneDACode derives the DA loader entry tag from the soc in
// build-info.txt. DA bundles tag entries by model number (the vendored
// MT6765 entry carries hw 0x6765).
func phoneDACode(soc string) (uint16, error) {
	facts, err := phoneFactsFor(soc)
	if err != nil {
		return 0, err
	}
	return facts.daCode, nil
}

// phoneBROMCode is the code BROM itself reports via get_hw_code.
func phoneBROMCode(soc string) (uint16, error) {
	facts, err := phoneFactsFor(soc)
	if err != nil {
		return 0, err
	}
	return facts.bromCode, nil
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
	hwCode, err := phoneBROMCode(phone.soc)
	if err != nil {
		return nil, err
	}
	daCode, err := phoneDACode(phone.soc)
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
	loader, err := parseMTKDALoader(cfg.daLoader, daCode, 0, 0)
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
		daCode:    daCode,
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
	fmt.Printf("Phone BROM flash for %s (%s, BROM hw code 0x%04x, DA entry 0x%04x):\n", plan.device, plan.soc, plan.hwCode, plan.daCode)
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
		fmt.Printf("  Auth:      %s (sent via SEND_AUTH when BROM enforces DAA)\n", plan.auth)
	} else {
		fmt.Printf("  Auth:      none provided (DAA targets are attempted without it)\n")
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

// parsePhoneWait parses the -wait supplicant window. Empty means one
// acquisition attempt (the historical behavior); anything else must be a
// Go duration like 90s or 5m.
func parsePhoneWait(s string) (time.Duration, error) {
	s = strings.TrimSpace(s)
	if s == "" {
		return 0, nil
	}
	d, err := time.ParseDuration(s)
	if err != nil {
		return 0, fmt.Errorf("-wait %q: %w", s, err)
	}
	if d < 0 {
		return 0, fmt.Errorf("-wait %q: must not be negative", s)
	}
	return d, nil
}

// acquirePhoneBROM connects and probes until the phone answers or the -wait
// window expires: the supplicant loop, so one invocation survives replugs,
// port renumbering, and preloader boot timeouts instead of forcing re-runs.
// With no -wait it makes exactly one attempt. The caller owns the returned
// client (preloaderEMI already attached) and closes it.
func acquirePhoneBROM(cfg config, phone *phoneRoot, facts phoneFacts, emi *mtkPreloaderEMI) (*mtkSerialClient, mtkTargetConfig, error) {
	window, err := parsePhoneWait(cfg.waitFlag)
	if err != nil {
		return nil, mtkTargetConfig{}, err
	}
	deadline := time.Now().Add(window)
	var lastErr error
	for attempt := 1; ; attempt++ {
		client, err := connectMTKSerialWithOptions(cfg.device, mtkSerialConnectOptions{handshakeWake: true})
		if err == nil {
			client.preloaderEMI = emi
			target, err := probePhoneBROM(client, phone, facts)
			if err == nil {
				return client, target, nil
			}
			_ = client.port.Close()
			lastErr = err
		} else {
			lastErr = err
		}
		if window == 0 || !time.Now().Before(deadline) {
			return nil, mtkTargetConfig{}, lastErr
		}
		fmt.Printf("Phone not acquired (attempt %d): %v; waiting for the VCOM, replug with keys held.\n", attempt, lastErr)
		time.Sleep(2 * time.Second)
	}
}

// refusePhoneSLA fails fast when the target enforces SLA: the tool speaks
// no SLA exchange, so proceeding would only die at SEND_DA with 0x1D0D.
// Checked at probe time, before anything moves. DAA alone is not refused:
// it is verified against the DA image itself, and a vendor-signed DA may
// pass with no host exchange; if it does not, SEND_DA or the stage-1 sync
// fails cleanly with no eMMC touched.
func refusePhoneSLA(phone *phoneRoot, target mtkTargetConfig) error {
	if !target.SLA {
		return nil
	}
	return fmt.Errorf("phone target %s: BROM enforces SLA authentication and this tool speaks no SLA exchange; "+
		"flash this unit with SP Flash Tool (or mtkclient) and the stock auth_sv5.auth per LK-BRINGUP step 2", phone.device)
}

// probePhoneBROM is the phone sibling of probeMT6592: same reconnect loop,
// but the HW gate is the phone's own code and the watchdog poke is skipped --
// TOPRGU is ungrounded on phones, and a blind write32 to a guessed base
// risks worse than the watchdog itself. A mid-session BROM reset fails the
// run loudly; the LK slot stays reflashable because the preloader is never
// touched.
func probePhoneBROM(c *mtkSerialClient, phone *phoneRoot, facts phoneFacts) (mtkTargetConfig, error) {
	const maxReconnect = 8
	for attempt := 0; ; attempt++ {
		target, err := probePhoneBROMOnce(c, phone, facts, false)
		if err == nil {
			return target, nil
		}
		if !isDeviceGoneError(err) || attempt >= maxReconnect {
			return mtkTargetConfig{}, err
		}
		fmt.Printf("MTK device dropped during probe (%v); the BROM re-enumerated USB. Reconnecting...\n", err)
		if rerr := c.reopenBROMHandshake(); rerr != nil {
			return mtkTargetConfig{}, fmt.Errorf("%w; reconnect after device drop failed: %v", err, rerr)
		}
	}
}

func probePhoneBROMOnce(c *mtkSerialClient, phone *phoneRoot, facts phoneFacts, quiet bool) (mtkTargetConfig, error) {
	say := func(format string, args ...any) {
		if !quiet {
			fmt.Printf(format, args...)
		}
	}
	got, hwVer, err := c.getHWCode()
	if err != nil {
		return mtkTargetConfig{}, err
	}
	say("MTK HW code: 0x%04x, HW version: 0x%04x\n", got, hwVer)
	if got != facts.bromCode {
		return mtkTargetConfig{}, fmt.Errorf("connected MediaTek target is 0x%04x, expected %s (%s, hw code 0x%04x)",
			got, phone.device, phone.soc, facts.bromCode)
	}
	// Best-effort watchdog disable: on secured units BROM may refuse the
	// register write, in which case the session runs inside the watchdog
	// window and a drop fails the run loudly (the slot stays reflashable).
	if err := c.write32(facts.watchdog, facts.watchdogOff); err != nil {
		say("Warning: could not disable the phone watchdog (%v); mid-session resets will fail the run.\n", err)
	}
	target, err := c.getTargetConfig()
	if err != nil {
		return mtkTargetConfig{}, err
	}
	say("Target config: 0x%08x (SBC=%t SLA=%t DAA=%t MemRead=%t MemWrite=%t)\n",
		target.Raw, target.SBC, target.SLA, target.DAA, target.MemRead, target.MemWrite)
	if err := refusePhoneSLA(phone, target); err != nil {
		return mtkTargetConfig{}, err
	}
	blver, isBROM, err := c.getBLVersion()
	if err != nil {
		return mtkTargetConfig{}, err
	}
	mode := "preloader"
	if isBROM {
		mode = "BROM"
	}
	bromver, err := c.getBROMVersion()
	if err != nil {
		return mtkTargetConfig{}, err
	}
	c.blVersion = blver
	c.bromVersion = bromver
	c.isBROM = isBROM
	say("MTK mode: %s, BL version: 0x%02x, BROM version: 0x%02x\n", mode, blver, bromver)
	if _, _, _, err := c.getHWSWVersion(); err == nil {
		// Best-effort metadata, as on the J36 path.
	} else {
		say("Warning: could not read HW/SW version tuple: %v\n", err)
	}
	return target, nil
}

// phoneUSBDLReg is the BOOT_MISC download-mode word: magic + max timeout +
// enabled, addressed to BROM rather than the bootloader. Same word every
// MediaTek flasher writes; the consts are shared with the J36 path.
func phoneUSBDLReg() uint32 {
	timeout := uint32(mtkUSBDLTimeoutMax << 2)
	timeout &= mtkUSBDLTimeoutMask
	return (mtkUSBDLMagic | timeout | mtkUSBDLBitEnable) &^ uint32(mtkUSBDLByPreloader)
}

// setPhonePreloaderBROMFlag arms the preloader so its next reset lands in
// BROM download mode: unlock BOOT_MISC, mark it watchdog-resettable, relock,
// write the USBDL word. It needs a grounded misc_lock base, so socs without
// one (mt6833) cannot take this path and must enter BROM via the key combo.
func setPhonePreloaderBROMFlag(c *mtkSerialClient, facts phoneFacts) error {
	if facts.miscLock == 0 {
		return errors.New("no grounded BOOT_MISC base for this soc; power off, hold Vol-down, replug for BROM mode, and rerun")
	}
	usbdlReg := phoneUSBDLReg()
	fmt.Printf("Setting preloader reset-to-BROM flag: USBDL 0x%08x\n", usbdlReg)
	if err := c.write32(facts.miscLock, mtkMiscLockKeyMagic); err != nil {
		return fmt.Errorf("unlock BOOT_MISC: %w", err)
	}
	if err := c.write32(facts.miscLock+0x08, 1); err != nil {
		return fmt.Errorf("mark USBDL flag watchdog-resettable: %w", err)
	}
	if err := c.write32(facts.miscLock, 0); err != nil {
		return fmt.Errorf("lock BOOT_MISC: %w", err)
	}
	if err := c.write32(facts.miscLock-0x20, usbdlReg); err != nil {
		return fmt.Errorf("write USBDL flag: %w", err)
	}
	return nil
}

// resetPhonePreloaderToBROM uses the MT6765 BOOT_MISC and watchdog addresses
// from phoneFacts. It changes only SoC registers and leaves eMMC untouched.
// The last write may drop USB before the preloader can acknowledge it.
func resetPhonePreloaderToBROM(c *mtkSerialClient, facts phoneFacts) error {
	if c.isBROM {
		return nil
	}
	if err := setPhonePreloaderBROMFlag(c, facts); err != nil {
		return err
	}
	fmt.Println("Triggering phone watchdog reset toward BROM.")
	if err := c.write32(facts.watchdog+0x08, mtkWatchdogRestart); err != nil {
		return fmt.Errorf("restart phone watchdog before BROM reset: %w", err)
	}
	if err := c.write32(facts.watchdog, mtkWatchdogRebootMode); err != nil {
		return fmt.Errorf("arm phone watchdog reset mode: %w", err)
	}
	if err := c.write32(facts.watchdog+0x14, mtkWatchdogSoftwareRst); err != nil {
		fmt.Printf("Watchdog reset command lost its ACK as USB dropped: %v\n", err)
	}
	return nil
}

// waitPhoneBROM takes a live preloader-mode session and waits for BROM: it
// polls the live handle (closing and reopening mid-session wedges the
// preloader handshake), and only opens a fresh session after a genuine
// reset or replug drops the old one. The caller hands over its client;
// success returns the BROM session (caller-owned), failure closes
// everything and reports. Bounded by an overall deadline, not an attempt
// count, because a fresh connect already waits out its own window. The
// client is never nil at probe time: a failed reconnect loops back to the
// deadline check, never into the probe.
func waitPhoneBROM(cfg config, phone *phoneRoot, facts phoneFacts, client *mtkSerialClient, window time.Duration, connect func(string) (*mtkSerialClient, error)) (*mtkSerialClient, mtkTargetConfig, error) {
	deadline := time.Now().Add(window)
	for time.Now().Before(deadline) {
		if client == nil {
			var err error
			client, err = connect(cfg.device)
			if err != nil {
				continue
			}
		}
		target, err := probePhoneBROMOnce(client, phone, facts, true)
		if err == nil {
			if client.isBROM {
				fmt.Printf("BROM session acquired: target config 0x%08x (SBC=%t SLA=%t DAA=%t).\n",
					target.Raw, target.SBC, target.SLA, target.DAA)
				return client, target, nil
			}
			fmt.Printf("Waiting for BROM (%s left): preloader session alive; replug with Vol-down held to switch modes.\n",
				time.Until(deadline).Round(time.Second))
			time.Sleep(5 * time.Second)
			continue
		}
		if !isDeviceGoneError(err) {
			fmt.Printf("Waiting for BROM (%s left): preloader not answering (%v).\n",
				time.Until(deadline).Round(time.Second), err)
			time.Sleep(5 * time.Second)
			continue
		}
		// Reset or replug dropped the handle: fresh session next pass.
		// The connect waits out its own window, so a key-combo replug
		// lands here.
		_ = client.port.Close()
		client = nil
		fmt.Println("Device re-enumerated; reopening.")
	}
	if client != nil {
		_ = client.port.Close()
	}
	return nil, mtkTargetConfig{}, errors.New("BROM wait expired; power off, hold Vol-down (or Vol-up+Vol-down), replug for BROM mode, and rerun")
}

// flagFailureAdvice wraps a reset-to-BROM flag failure: on secured units
// the preloader refuses register writes (write32 status 0x1001), so the
// only way into BROM is the key combo at plug time.
func flagFailureAdvice(err error) error {
	return fmt.Errorf("%w; secured preloaders block register writes -- power off, hold Vol-down, replug for BROM mode, and rerun", err)
}

// crashPhonePreloader tries the generic preloader-to-BROM crash modes in
// order, checking for a BROM landing between modes so a success is never
// crashed again:
//
//	0: malformed DA download (null address, zero payload)
//	1: malformed register read (address 0, long count)
//	2: null jump (tiny ARM return stub at address 0, then jump there;
//	   on BootROM-mapped address 0 this reboots straight into BROM)
//
// All three are RAM/protocol operations only -- no DA runs, no eMMC
// command is ever issued, so the worst outcome is a reboot (normal boot
// lands invisible; the wait loop then expires cleanly). Secured
// preloaders may clean-refuse individual modes instead of crashing;
// every error here is expected and ignored. Stops at the first BROM.
func crashPhonePreloader(c *mtkSerialClient, phone *phoneRoot, facts phoneFacts) {
	armReturn := []byte{0x00, 0x01, 0x9F, 0xE5, 0x10, 0xFF, 0x2F, 0xE1}
	modes := []struct {
		name string
		fire func() error
	}{
		{"malformed DA download", func() error { return c.sendDA(0, 0x100, make([]byte, 0x100)) }},
		{"malformed register read", func() error { _, err := c.read32(0, 0x100); return err }},
		{"null jump", func() error {
			payload := append(append([]byte(nil), armReturn...), make([]byte, 0x110)...)
			if err := c.sendDA(0x0, 0x0, payload); err != nil {
				return err
			}
			return c.jumpDA(0x0)
		}},
	}
	for i, mode := range modes {
		fmt.Printf("Crash attempt %d/3 (%s)...\n", i+1, mode.name)
		// The result is the diagnosis: a status hex means the secured
		// preloader clean-refused the mode, a device-gone error means it
		// took the mode down with it, and nil means the mode was
		// accepted (watch the landing check below).
		if err := mode.fire(); err != nil {
			fmt.Printf("  result: %v\n", err)
		} else {
			fmt.Println("  result: accepted")
		}
		time.Sleep(time.Second)
		if _, err := probePhoneBROMOnce(c, phone, facts, true); err == nil && c.isBROM {
			fmt.Println("Crash landed BROM.")
			return
		}
	}
}

// needsPhoneAuth decides whether the SEND_AUTH step runs. In BROM it runs
// on DAA targets with a blob to send. In preloader mode it is skipped:
// the preloader does not speak 0xE2 and verifies the DA signature at
// SEND_DA time instead. A rejected signature returns 0x7024.
func needsPhoneAuth(target mtkTargetConfig, isBROM, hasAuth bool) (bool, string) {
	if !target.DAA {
		return false, ""
	}
	if !isBROM {
		return false, "Preloader mode: skipping SEND_AUTH (the preloader verifies the vendor DA signature at SEND_DA instead)."
	}
	if !hasAuth {
		return false, "Warning: BROM enforces DAA but no -auth file was given; attempting the DA upload without it."
	}
	return true, ""
}

// prepareAuthData pads the vendor auth blob to even length; BROM reads the
// transfer back as 16-bit words.
func prepareAuthData(auth []byte) []byte {
	if len(auth)%2 != 0 {
		out := make([]byte, len(auth)+1)
		copy(out, auth)
		return out
	}
	return auth
}

// sendAuth uploads the vendor auth blob (auth_sv5.auth) over SEND_AUTH. This
// is the DAA step: it runs after the probe, before the DA upload, and only
// when the target-config DAA bit says the target wants it. A 0x1D0C status
// means BROM declines the blob ("no auth needed") and is not an error.
func (c *mtkSerialClient) sendAuth(auth []byte) error {
	data := prepareAuthData(auth)
	if err := c.echo([]byte{mtkCmdSendAuth}); err != nil {
		return fmt.Errorf("SEND_AUTH echo: %w", err)
	}
	if err := c.writeRaw(uint32Bytes(uint32(len(data)))); err != nil {
		return fmt.Errorf("send auth length: %w", err)
	}
	rlen, err := c.readUint32(c.commandTimeout)
	if err != nil {
		return fmt.Errorf("read auth length reply: %w", err)
	}
	if rlen != uint32(len(data)) {
		return fmt.Errorf("auth length reply 0x%x, want 0x%x", rlen, len(data))
	}
	status, err := c.readUint16(c.commandTimeout)
	if err != nil {
		return fmt.Errorf("read auth status: %w", err)
	}
	if status == 0x1D0C {
		fmt.Println("BROM reports no auth needed.")
		return nil
	}
	if status > 0xFF {
		return fmt.Errorf("SEND_AUTH status 0x%x", status)
	}
	const chunkSize = 0x400
	for pos := 0; pos < len(data); pos += chunkSize {
		end := pos + chunkSize
		if end > len(data) {
			end = len(data)
		}
		if err := c.port.WriteAll(data[pos:end], c.writeTimeout); err != nil {
			return fmt.Errorf("write auth blob at 0x%x: %w", pos, err)
		}
	}
	time.Sleep(35 * time.Millisecond)
	if _, err := c.readUint16(c.commandTimeout); err != nil {
		return fmt.Errorf("read auth crc: %w", err)
	}
	status, err = c.readUint16(c.commandTimeout)
	if err != nil {
		return fmt.Errorf("read auth final status: %w", err)
	}
	if status > 0xFF {
		return fmt.Errorf("SEND_AUTH final status 0x%x", status)
	}
	fmt.Println("Auth blob accepted.")
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
	loader, err := parseMTKDALoader(plan.daPath, plan.daCode, 0, 0)
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

	facts, err := phoneFactsFor(phone.soc)
	if err != nil {
		return err
	}
	client, target, err := acquirePhoneBROM(cfg, phone, facts, emi)
	if err != nil {
		return err
	}
	defer func() {
		if client != nil && client.port != nil {
			_ = client.port.Close()
		}
	}()

	// An auth file is sent only by BROM. Acquiring the preloader must not
	// consume the operator's -wait window and silently skip that file before
	// trying a DA which this secured preloader may reject with 0x7024.
	if target.DAA && plan.auth != "" && !client.isBROM {
		window, err := parsePhoneWait(cfg.waitFlag)
		if err != nil {
			return err
		}
		if window == 0 {
			return errors.New("phone is in preloader mode with DAA enabled; -auth can only be sent in BROM mode. Power off, enter BROM with the download key combo, and retry (or pass -wait to allow a replug)")
		}
		fmt.Println("DAA-enabled preloader acquired; requesting reset to BROM so -auth can be sent.")
		if err := resetPhonePreloaderToBROM(client, facts); err != nil {
			if strings.Contains(err.Error(), "unlock BOOT_MISC:") && strings.Contains(err.Error(), "status 0x1001") {
				return fmt.Errorf("this preloader blocks reset to BROM (0x1001); -auth cannot be sent in preloader mode, so no DA or eMMC write was attempted: %w", err)
			}
			fmt.Printf("Automatic BROM reset was refused: %v\n", flagFailureAdvice(err))
			fmt.Printf("Waiting up to %s for a BROM replug with the download key combo held.\n", window)
		} else {
			_ = client.port.Close()
			client = nil
			fmt.Printf("Reset requested; waiting up to %s for BROM to enumerate.\n", window)
		}
		client, target, err = waitPhoneBROM(cfg, phone, facts, client, window, func(device string) (*mtkSerialClient, error) {
			return connectMTKSerialWithOptions(device, mtkSerialConnectOptions{handshakeWake: true})
		})
		if err != nil {
			return err
		}
		client.preloaderEMI = emi
	}
	if !client.isBROM {
		// Preloader mode is a first-class upload path, not a dead end:
		// the secured preloader verifies the vendor DA signature at
		// SEND_DA (unsigned payloads die with 0x7024) and runs the DA
		// itself. The one observed preloader-mode drop has the exact
		// signature of the macOS CDC-ACM flake (tty transport, deep in
		// bulk transfer); over libusb that vector is gone. Past the DA
		// jump the mode distinction evaporates -- the DA owns the CPU.
		fmt.Println("Phone is in preloader mode; uploading the vendor DA through it.")
	}
	if send, msg := needsPhoneAuth(target, client.isBROM, plan.auth != ""); send {
		authBlob, err := os.ReadFile(plan.auth)
		if err != nil {
			return fmt.Errorf("phone target %s: read auth file %s: %w", phone.device, plan.auth, err)
		}
		fmt.Printf("Uploading auth blob: %s (0x%x bytes)\n", plan.auth, len(authBlob))
		if err := client.sendAuth(authBlob); err != nil {
			return err
		}
	} else if msg != "" {
		fmt.Println(msg)
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
