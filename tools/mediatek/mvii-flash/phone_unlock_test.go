package main

import (
	"bytes"
	"strings"
	"testing"
	"time"
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

func TestUnlockBoot1ReadFramesPart(t *testing.T) {
	data := make([]byte, 0x200)
	for i := range data {
		data[i] = byte(i)
	}
	pending := []byte{mtkLegacyACK, mtkLegacyACK, mtkLegacyACK}
	pending = append(pending, data...)
	pending = append(pending, be16(sum16(data))...)
	port := &scriptPort{pending: pending}
	client := &mtkSerialClient{port: port, commandTimeout: time.Second, writeTimeout: time.Second}
	got, err := client.readLegacyEMMC(mtkLegacyEMMCPartBoot1, 0, 0x200, 0x200)
	if err != nil {
		t.Fatalf("readLegacyEMMC: %v", err)
	}
	if !bytes.Equal(got, data) {
		t.Fatal("read data mismatch")
	}
	if len(port.pending) != 0 {
		t.Fatalf("%d scripted bytes unconsumed", len(port.pending))
	}
	// Switch cmd, part, six read-header fields, per-packet ACK.
	if len(port.writes) != 2+6+1 {
		t.Fatalf("writes = %d, want 9", len(port.writes))
	}
	if !bytes.Equal(port.writes[0], []byte{mtkLegacySDMMCSwitchPart}) || !bytes.Equal(port.writes[1], []byte{mtkLegacyEMMCPartBoot1}) {
		t.Fatalf("switch writes = %x %x, want 60 01", port.writes[0], port.writes[1])
	}
	if !bytes.Equal(port.writes[2], []byte{mtkLegacyEMMCRead}) {
		t.Fatalf("read cmd = %x, want d6", port.writes[2])
	}
	if _, err := client.readLegacyEMMC(mtkLegacyEMMCPartBoot1, 0, 0x100, 0x200); err == nil {
		t.Fatal("unaligned length accepted, want refusal")
	}
}

func TestUnlockBoot1WriteFramesPart(t *testing.T) {
	data := make([]byte, 0x200)
	for i := range data {
		data[i] = byte(0xFF - i)
	}
	port := &scriptPort{pending: []byte{mtkLegacyACK, mtkLegacyACK, mtkLegacyACK, mtkLegacyCONT}}
	client := &mtkSerialClient{port: port, commandTimeout: time.Second, writeTimeout: time.Second}
	if err := client.writeLegacyEMMCPartition(mtkLegacyEMMCPartBoot1, 0, data, 0x200); err != nil {
		t.Fatalf("writeLegacyEMMCPartition: %v", err)
	}
	// Switch cmd, part, six header fields, chunk ACK, chunk, checksum.
	if len(port.writes) != 2+6+3 {
		t.Fatalf("writes = %d, want 11", len(port.writes))
	}
	if !bytes.Equal(port.writes[4], []byte{mtkLegacyEMMCPartBoot1}) {
		t.Fatalf("header part = %x, want 01", port.writes[4])
	}
	if !bytes.Equal(port.writes[9], data) {
		t.Fatal("chunk bytes mismatch")
	}
	if !bytes.Equal(port.writes[10], be16(sum16(data))) {
		t.Fatal("chunk checksum mismatch")
	}
	if err := client.writeLegacyEMMCPartition(mtkLegacyEMMCPartBoot1, 0, data[:0x100], 0x200); err == nil {
		t.Fatal("unaligned length accepted, want refusal")
	}
}
