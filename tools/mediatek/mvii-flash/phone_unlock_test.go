package main

import (
	"bytes"
	"strings"
	"testing"
)

func makeUnlockTestImage(t *testing.T, lock byte, withFlag bool) []byte {
	t.Helper()
	img := make([]byte, 0x5000)
	copy(img[0:], "EMMC_BOOT\x00\x00\x00\x01\x00\x00\x00\x00\x02\x00\x00")
	// BRLYT table at 0x200 with unpatched 0x08 pre-values.
	copy(img[0x200:], "BRLYT\x00\x00\x00")
	for _, off := range []int{0x20D, 0x21D, 0x211, 0x212, 0x221, 0x222} {
		img[off] = 0x08
	}
	// Code blob at 0x800 with a recognizable marker.
	copy(img[0x800:], "MMM\x01CODE-MARKER-AT-0x800")
	for i := 0x820; i < 0x900; i++ {
		img[i] = byte(i & 0xFF)
	}
	if withFlag {
		flag := bytes.Repeat([]byte{0xAA}, 0x78)
		copy(flag[0:], "AND_ROMINFO_v")
		flag[0x4C] = lock
		copy(img[0xAEC:], flag)
	}
	return img
}

func TestPatchUnlockPreloader(t *testing.T) {
	in := makeUnlockTestImage(t, 0x22, true)
	out, report, err := patchUnlockPreloader(in)
	if err != nil {
		t.Fatalf("patchUnlockPreloader: %v", err)
	}
	if len(out) != len(in) {
		t.Fatalf("len(out) = %d, want %d", len(out), len(in))
	}
	if report.CodeOffset != 0x800 || report.FlagOffset != 0xAEC || report.LockBefore != 0x22 {
		t.Fatalf("report = %+v, want code 0x800 flag 0xaec lock 0x22", report)
	}
	moved := len(in) - 0x3000 - 0x800
	if !bytes.Equal(out[0x2000:0x2000+moved], in[0x800:0x800+moved]) {
		t.Fatal("code blob was not relocated intact to 0x2000")
	}
	for off, want := range map[int]byte{0x20D: 0x20, 0x21D: 0x20, 0x211: 0x10, 0x212: 0x10, 0x221: 0x10, 0x222: 0x10} {
		if out[off] != want {
			t.Fatalf("BRLYT %x = %#02x, want %#02x", off, out[off], want)
		}
	}
	if !bytes.HasPrefix(out[0x1000:], []byte("AND_ROMINFO_v")) {
		t.Fatal("flag block missing at 0x1000")
	}
	if out[0x1000+0x4C] != 0x00 {
		t.Fatalf("lock byte = %#02x, want 0x00", out[0x1000+0x4C])
	}
	for i := 0; i < 0x78; i++ {
		if i == 0x4C {
			continue
		}
		if out[0x1000+i] != in[0xAEC+i] {
			t.Fatalf("flag byte %x differs from source", i)
		}
	}
	for _, b := range out[0x800:0x1000] {
		if b != 0 {
			t.Fatal("0x800..0x1000 gap not zeroed")
		}
	}
}

func TestPatchUnlockPreloaderRefuses(t *testing.T) {
	good := makeUnlockTestImage(t, 0x22, true)
	once, _, err := patchUnlockPreloader(good)
	if err != nil {
		t.Fatalf("first patch: %v", err)
	}
	if _, _, err := patchUnlockPreloader(once); err == nil || !strings.Contains(err.Error(), "already patched") {
		t.Fatalf("re-patch = %v, want already-patched refusal", err)
	}

	raw := make([]byte, 0x5000)
	copy(raw[0:], "MMM\x018\x00\x00\x00FILE_INF")
	if _, _, err := patchUnlockPreloader(raw); err == nil || !strings.Contains(err.Error(), "RAW") {
		t.Fatalf("RAW image = %v, want RAW refusal", err)
	}

	noFlag := makeUnlockTestImage(t, 0x22, false)
	if _, _, err := patchUnlockPreloader(noFlag); err == nil || !strings.Contains(err.Error(), "ROMINFO") {
		t.Fatalf("missing flag = %v, want ROMINFO error", err)
	}

	if _, _, err := patchUnlockPreloader(make([]byte, 0x1000)); err == nil || !strings.Contains(err.Error(), "too small") {
		t.Fatalf("short image = %v, want too-small error", err)
	}
}
