package main

import (
	"encoding/binary"
	"errors"
	"fmt"
	"os"
	"strings"
)

// probePhoneJump is deliberately independent of the DA upload and flash flows.
// The only mutation requested from the target is one JUMP_DA. Code reached by
// an accepted jump may itself perform normal boot operations.
func probePhoneJump(cfg config) error {
	if cfg.preloader == "" || cfg.device == "" {
		return errors.New("-mtk-probe-jump requires -preloader and -device")
	}
	if cfg.upload != "" || cfg.unlock || cfg.image != "" || cfg.probeDA || cfg.mtkPhoneWriteBoot1 != "" {
		return errors.New("-mtk-probe-jump must be used without upload, unlock, image, or DA-probe options")
	}
	data, err := os.ReadFile(cfg.preloader)
	if err != nil {
		return err
	}
	entry, code, err := parseMTKPreloaderImage(data)
	if err != nil {
		return err
	}
	if entry != 0x201000 || len(code) < 16 {
		return fmt.Errorf("this MT6765 probe expects preloader entry 0x201000 and at least 16 code bytes; got 0x%x", entry)
	}
	fmt.Printf("Bare JUMP_DA probe: candidate stock preloader entry 0x%x from %s.\n", entry, cfg.preloader)
	fmt.Println("This can reset or hang the phone. No SEND_DA, auth, watchdog writes, or storage commands are sent.")
	c, err := connectMTKSerialWithOptions(cfg.device, mtkSerialConnectOptions{handshakeWake: true, waitForUSB: true})
	if err != nil {
		return err
	}
	defer c.port.Close()
	hw, _, err := c.getHWCode()
	if err != nil {
		return err
	}
	if hw != 0x0766 {
		return fmt.Errorf("jump probe requires MT6765 hw code 0x0766; got 0x%04x", hw)
	}
	ver, isBROM, err := c.getBLVersion()
	if err != nil {
		return err
	}
	if isBROM {
		return errors.New("target is in BROM; no resident preloader entry established, so no jump sent")
	}
	target, err := c.getTargetConfig()
	if err != nil {
		return err
	}
	fmt.Printf("Preloader BL=0x%02x HW=0x%04x target-config=0x%08x\n", ver, hw, target.Raw)
	return probeResidentPreloaderJump(c, entry, code[:16])
}

func probeResidentPreloaderJump(c *mtkSerialClient, entry uint32, expected []byte) error {
	if len(expected) != 16 {
		return errors.New("jump probe requires 16 reference entry bytes")
	}
	words, err := c.read32(entry, 4)
	if err != nil {
		// Only a complete, explicit initial refusal leaves the command stream
		// in a known state. Never jump after a timeout or partial read.
		if !strings.Contains(err.Error(), "initial status 0x1001") {
			return fmt.Errorf("entry read failed; no jump sent: %w", err)
		}
		fmt.Printf("Entry read refused: %v. The address comes from the stock file; current RAM contents are unverified.\n", err)
	} else {
		for i, word := range words {
			if word != binary.LittleEndian.Uint32(expected[i*4:]) {
				return fmt.Errorf("entry word %d differs from stock file (RAM 0x%08x); no jump sent", i, word)
			}
		}
		fmt.Println("First 16 resident entry bytes match the stock preloader file.")
	}
	fmt.Printf("Sending one bare JUMP_DA to 0x%x.\n", entry)
	if err := c.jumpDA(entry); err != nil {
		return fmt.Errorf("bare jump probe (no success inferred from USB loss): %w", err)
	}
	fmt.Println("JUMP_DA returned status 0. Execution and fastboot availability still require independent observation.")
	return nil
}
