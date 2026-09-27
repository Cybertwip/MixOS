package main

import (
	"errors"
	"fmt"
	"os"
	"strings"
)

// uartMemOp is a raw read32/write32 over the tty VCOM UART: no DA upload,
// no auth, no eMMC commands -- just the command echo round-trip.
type uartMemOp struct {
	addr  uint32
	words uint32
	value uint32
}

// resolveUARTMemOp validates the -mtk-uart-read/-mtk-uart-write flags into
// an op. -address and -mtk-value accept 0x hex or decimal; -mtk-words
// defaults to 1 and caps at 256 so a typo cannot spray reads.
func resolveUARTMemOp(cfg config, write bool) (uartMemOp, error) {
	var op uartMemOp
	dev := strings.TrimSpace(cfg.device)
	if dev == "" || !isSerialDevicePath(dev) {
		return op, errors.New("-mtk-uart-read/-mtk-uart-write requires -device /dev/cu.usbmodem... (the phone's VCOM node)")
	}
	addrStr := strings.TrimSpace(cfg.mtkPayloadAddr)
	if addrStr == "" {
		return op, errors.New("-mtk-uart-read/-mtk-uart-write requires -address 0x...")
	}
	n, err := parseMTKNumber(addrStr, "-address")
	if err != nil {
		return op, err
	}
	op.addr = uint32(n)
	if write {
		vStr := strings.TrimSpace(cfg.mtkValue)
		if vStr == "" {
			return op, errors.New("-mtk-uart-write requires -mtk-value 0x...")
		}
		v, err := parseMTKNumber(vStr, "-mtk-value")
		if err != nil {
			return op, err
		}
		op.value = uint32(v)
		op.words = 1
		return op, nil
	}
	wStr := strings.TrimSpace(cfg.mtkWords)
	if wStr == "" {
		wStr = "1"
	}
	w, err := parseMTKNumber(wStr, "-mtk-words")
	if err != nil || w == 0 || w > 256 {
		return op, fmt.Errorf("bad -mtk-words %q: want 1..256", strings.TrimSpace(cfg.mtkWords))
	}
	op.words = uint32(w)
	return op, nil
}

// runMTKUARTMem runs a raw read32 (write=false) or write32 (write=true)
// over the tty VCOM UART. The tty transport is forced: the default libusb
// raw-bulk path speaks the same commands over USB, where this secured
// preloader refuses writes and faults reads. If the VCOM answers the MTK
// hello, the op runs; if it is log-only, the handshake times out cleanly.
func runMTKUARTMem(cfg config, write bool) error {
	op, err := resolveUARTMemOp(cfg, write)
	if err != nil {
		return err
	}
	_ = os.Setenv("MVII_MTK_TTY", "1")
	fmt.Println("Raw UART memory access over the tty VCOM (MVII_MTK_TTY=1 forced); no DA, auth, or eMMC commands.")
	client, err := connectMTKSerialWithOptions(cfg.device, mtkSerialConnectOptions{handshakeWake: true})
	if err != nil {
		return fmt.Errorf("UART command handshake on %s: %w (the VCOM may be log-only)", cfg.device, err)
	}
	defer func() { _ = client.port.Close() }()
	if !write {
		words, err := client.read32(op.addr, op.words)
		if err != nil {
			return fmt.Errorf("uart read32(0x%08x x%d): %w", op.addr, op.words, err)
		}
		for i, w := range words {
			fmt.Printf("  0x%08x: 0x%08x\n", op.addr+uint32(i)*4, w)
		}
		return nil
	}
	fmt.Printf("Writing 0x%08x to 0x%08x ...\n", op.value, op.addr)
	if err := client.write32(op.addr, op.value); err != nil {
		return fmt.Errorf("uart write32(0x%08x): %w", op.addr, err)
	}
	fmt.Println("  write accepted (status OK).")
	return nil
}
