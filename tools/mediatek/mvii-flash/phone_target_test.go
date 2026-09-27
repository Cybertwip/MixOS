package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// A phone boot dir carries build-info.txt (device= + soc=) next to lk.bin;
// a j36 dir carries neither. Detection is the hinge every guard below hangs
// off, so it gets its own cases first.
func writePhoneRoot(t *testing.T, info string) string {
	t.Helper()
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "build-info.txt"), []byte(info), 0644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "lk.bin"), []byte("fake-lk"), 0644); err != nil {
		t.Fatal(err)
	}
	return dir
}

func TestDetectPhoneRoot(t *testing.T) {
	phone := writePhoneRoot(t, "device=cph2381\nsoc=mt6833\ncommit=abc\n")
	got, ok := detectPhoneRoot(phone)
	if !ok {
		t.Fatal("phone boot dir not detected")
	}
	if got.device != "cph2381" || got.soc != "mt6833" || got.family != "mt68xx" {
		t.Fatalf("detected %+v, want cph2381/mt6833/mt68xx", got)
	}

	if _, ok := detectPhoneRoot(t.TempDir()); ok {
		t.Error("empty dir detected as phone root")
	}
	bad := writePhoneRoot(t, "device=\nsoc=mt6833\n")
	if _, ok := detectPhoneRoot(bad); ok {
		t.Error("empty device detected as phone root")
	}
	weird := writePhoneRoot(t, "device=cph2381\nsoc=exynos\n")
	if _, ok := detectPhoneRoot(weird); ok {
		t.Error("non-MTK soc detected as phone root")
	}
	if got := confirmWord(config{root: phone}); got != "FLASH CPH2381" {
		t.Errorf("phone confirm word = %q, want FLASH CPH2381", got)
	}
	if got := confirmWord(config{root: t.TempDir()}); got != "FLASH J36 ULTRA" {
		t.Errorf("j36 confirm word = %q, want FLASH J36 ULTRA", got)
	}
}

// The refusal matrix: with a phone -root, only explicit fastboot without
// -device and without j36-only verbs passes the guard. Every other
// device-touching shape must fail LOUDLY (a silent pass here is a phone
// flashed with j36 scatter offsets).
func TestRefusePhoneWrite(t *testing.T) {
	phone := writePhoneRoot(t, "device=lm-x120\nsoc=mt6739\n")
	info, ok := detectPhoneRoot(phone)
	if !ok {
		t.Fatal("fixture not detected")
	}

	allow := []config{
		{root: phone, backend: "fastboot", upload: "lk", partition: "lk", partitionExplicit: true},
		{root: phone, backend: "fastboot", partition: "lk_a", partitionExplicit: true}, // -upload unset: resolveImage finds lk.bin
		{root: phone, device: "/dev/cu.usbmodemXXXX", mtkPayloadAddr: "0x110000"},
		{root: phone, device: "/dev/cu.usbmodemXXXX", mtkPayloadAddr: "0x110000", image: "stub.bin"},
		{root: phone, device: "/dev/cu.usbmodemXXXX", mtkPayloadEntry: "0x112000"},
		{root: phone, unlock: true, device: "/dev/cu.usbmodemXXXX", preloader: "boot1.bin"},
		{root: phone, mtkPhoneWriteBoot1: "boot1.bin", device: "/dev/cu.usbmodemXXXX"},
		{root: phone, listOnly: true},
	}
	for i, cfg := range allow {
		if err := refusePhoneWrite(cfg, info); err != nil {
			t.Errorf("allow[%d] (%+v) refused: %v", i, cfg, err)
		}
	}

	refuse := []config{
		{root: phone, upload: "lk", device: "/dev/cu.usbmodemXXXX"},                               // serial VCOM: j36 BROM feed
		{root: phone, device: "/dev/cu.usbmodemXXXX"},                                             // serial alone: auto-feed, no address
		{root: phone, upload: "lk", device: "/dev/disk4"},                                         // block: j36 offsets
		{root: phone, upload: "lk", backend: "mtk-serial", device: "/dev/cu.usb1"},                // explicit j36 backend
		{root: phone, upload: "lk", backend: "raw-block", device: "/dev/disk4"},                   // explicit j36 backend
		{root: phone, upload: "lk", backend: "fastboot", device: "/dev/cu.usb1"},                  // serial hijacks fastboot into feed
		{root: phone, device: "/dev/cu.usb1", mtkPayloadAddr: "0x110000", upload: "lk"},           // -upload ignored by raw exec: refuse, don't silently drop
		{root: phone, device: "/dev/cu.usb1", mtkPayloadAddr: "0x110000", mtkFeedPayload: "auto"}, // feed flag poisons raw exec
		{root: phone, device: "/dev/cu.usb1", mtkPayloadAddr: "0x110000", dbgConsole: true},       // console has no phone peer
		{root: phone, upload: "release", backend: "fastboot"},                                     // lk-release.bin absent on phones
		{root: phone, upload: "full", backend: "fastboot"},                                        // MVIIS1.bin absent on phones
		{root: phone, upload: "bogus", backend: "fastboot"},                                       // unknown kind stays unknown
		{root: phone, backend: "fastboot", mtkReadBootStatus: true},                               // feed-based read verb
		{root: phone, backend: "fastboot", mtkDumpPreloader: "auto"},                              // feed-based read verb
		{root: phone, backend: "fastboot", bootFlag: "debug"},                                     // j36 flag mechanism
		{root: phone, backend: "fastboot", dbgConsole: true},                                      // j36 live console
	}
	for i, cfg := range refuse {
		if err := refusePhoneWrite(cfg, info); err == nil {
			t.Errorf("refuse[%d] (%+v) allowed a phone write path", i, cfg)
		}
	}
}

// The fastboot entry itself demands an explicit partition on phone roots:
// the "boot" default is the j36's, and lk.bin flashed at it misses the slot.
func TestPhoneFastbootNeedsExplicitPartition(t *testing.T) {
	phone := writePhoneRoot(t, "device=cph2381\nsoc=mt6833\n")
	cfg := config{root: phone, backend: "fastboot", partition: "boot", partitionExplicit: false}
	if err := flashWithFastboot(cfg, filepath.Join(phone, "lk.bin")); err == nil ||
		!strings.Contains(err.Error(), "-partition explicitly") {
		t.Errorf("implicit partition gave %v, want the explicit-partition refusal", err)
	}
}
