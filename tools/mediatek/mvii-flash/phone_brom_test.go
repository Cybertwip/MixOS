package main

import (
	"encoding/binary"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// Fake DA loader with one entry for hwCode carrying payload.
func writeFakeDALoader(t *testing.T, hwCode uint16, payload []byte) string {
	t.Helper()
	const recordSize = 0xDC
	off := uint32(0x6C + recordSize)
	data := make([]byte, int(off)+len(payload))
	binary.LittleEndian.PutUint32(data[0x68:0x6C], 1)
	entry := data[0x6C : 0x6C+recordSize]
	binary.LittleEndian.PutUint16(entry[0:2], 0xDADA)
	binary.LittleEndian.PutUint16(entry[2:4], hwCode)
	binary.LittleEndian.PutUint16(entry[12:14], 0x200)
	binary.LittleEndian.PutUint16(entry[16:18], 0)
	binary.LittleEndian.PutUint16(entry[18:20], 1)
	binary.LittleEndian.PutUint32(entry[20:24], off)
	binary.LittleEndian.PutUint32(entry[24:28], uint32(len(payload)))
	binary.LittleEndian.PutUint32(entry[28:32], 0x200000)
	copy(data[off:], payload)
	path := filepath.Join(t.TempDir(), "MTK_DA_test.bin")
	if err := os.WriteFile(path, data, 0o644); err != nil {
		t.Fatal(err)
	}
	return path
}

func writeScatterLK(t *testing.T, partition, size string) string {
	t.Helper()
	scatter := "- partition_index: SYS30\n" +
		"  partition_name: " + partition + "\n" +
		"  file_name: lk.img\n" +
		"  is_download: true\n" +
		"  type: NORMAL_ROM\n" +
		"  linear_start_addr: 0x2ef00000\n" +
		"  physical_start_addr: 0x2ef00000\n" +
		"  partition_size: " + size + "\n" +
		"  region: EMMC_USER\n"
	path := filepath.Join(t.TempDir(), "MT6765_scatter.txt")
	if err := os.WriteFile(path, []byte(scatter), 0o644); err != nil {
		t.Fatal(err)
	}
	return path
}

func writeStagedFile(t *testing.T, name string, payload []byte) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), name)
	if err := os.WriteFile(path, payload, 0o644); err != nil {
		t.Fatal(err)
	}
	return path
}

// stagedConfig builds the full staged phone-BROM shape for a cph2385 root.
func stagedConfig(t *testing.T, root string) config {
	t.Helper()
	return config{
		root:              root,
		device:            "/dev/cu.usbmodemXXXX",
		upload:            "lk",
		partition:         "lk_a",
		partitionExplicit: true,
		mtkScatter:        writeScatterLK(t, "lk_a", "0x500000"),
		daLoader:          writeFakeDALoader(t, 0x6765, []byte("DA6765!")),
		preloader:         writeStagedFile(t, "preloader.img", []byte("pre")),
		authFile:          writeStagedFile(t, "auth_sv5.auth", []byte("auth")),
	}
}

func TestPhoneHWCode(t *testing.T) {
	for soc, want := range map[string]uint16{"mt6765": 0x6765, "mt6739": 0x6739, "mt6833": 0x6833} {
		got, err := phoneHWCode(soc)
		if err != nil || got != want {
			t.Errorf("phoneHWCode(%q) = 0x%x, %v; want 0x%x", soc, got, err, want)
		}
	}
	if _, err := phoneHWCode("exynos"); err == nil {
		t.Error("phoneHWCode(exynos) = nil, want error")
	}
}

func TestPlanPhoneBROMValid(t *testing.T) {
	phone := writePhoneRoot(t, "device=cph2385-4gb\nsoc=mt6765\n")
	info, ok := detectPhoneRoot(phone)
	if !ok {
		t.Fatal("fixture not detected")
	}
	cfg := stagedConfig(t, phone)
	plan, err := planPhoneBROM(cfg, info)
	if err != nil {
		t.Fatalf("planPhoneBROM = %v, want a valid plan", err)
	}
	if plan.hwCode != 0x6765 || plan.offset != 0x2ef00000 || plan.slotSize != 0x500000 || plan.partition != "lk_a" {
		t.Fatalf("plan = %+v, want hw 0x6765 lk_a at 0x2ef00000 size 0x500000", plan)
	}
	// A valid plan still ends the run: the wire write is step 6, so the
	// return must stay non-nil and state that nothing was written.
	err = runPhoneBROMPlan(cfg, info)
	if err == nil || !strings.Contains(err.Error(), "nothing was written") {
		t.Fatalf("runPhoneBROMPlan = %v, want the not-implemented refusal", err)
	}
}

func TestPlanPhoneBROMFailures(t *testing.T) {
	phone := writePhoneRoot(t, "device=cph2385-4gb\nsoc=mt6765\n")
	info, ok := detectPhoneRoot(phone)
	if !ok {
		t.Fatal("fixture not detected")
	}
	valid := func() config { return stagedConfig(t, phone) }

	t.Run("missing auth", func(t *testing.T) {
		cfg := valid()
		cfg.authFile = filepath.Join(t.TempDir(), "nope.auth")
		if _, err := planPhoneBROM(cfg, info); err == nil || !strings.Contains(err.Error(), "auth") {
			t.Fatalf("err = %v, want the auth complaint", err)
		}
	})
	t.Run("missing scatter", func(t *testing.T) {
		cfg := valid()
		cfg.mtkScatter = ""
		if _, err := planPhoneBROM(cfg, info); err == nil {
			t.Fatal("err = nil, want the scatter complaint")
		}
	})
	t.Run("wrong partition", func(t *testing.T) {
		cfg := valid()
		cfg.partition = "lk_c"
		if _, err := planPhoneBROM(cfg, info); err == nil || !strings.Contains(err.Error(), "lk_c") {
			t.Fatalf("err = %v, want the partition complaint", err)
		}
	})
	t.Run("implicit partition", func(t *testing.T) {
		cfg := valid()
		cfg.partition = "boot"
		cfg.partitionExplicit = false
		if _, err := planPhoneBROM(cfg, info); err == nil || !strings.Contains(err.Error(), "-partition explicitly") {
			t.Fatalf("err = %v, want the explicit-partition refusal", err)
		}
	})
	t.Run("lk too big", func(t *testing.T) {
		bigRoot := t.TempDir()
		if err := os.WriteFile(filepath.Join(bigRoot, "build-info.txt"), []byte("device=cph2385-4gb\nsoc=mt6765\n"), 0o644); err != nil {
			t.Fatal(err)
		}
		big := make([]byte, 0x500001)
		if err := os.WriteFile(filepath.Join(bigRoot, "lk.bin"), big, 0o644); err != nil {
			t.Fatal(err)
		}
		bigInfo, ok := detectPhoneRoot(bigRoot)
		if !ok {
			t.Fatal("big fixture not detected")
		}
		cfg := valid()
		cfg.root = bigRoot
		if _, err := planPhoneBROM(cfg, bigInfo); err == nil || !strings.Contains(err.Error(), "0x500000") {
			t.Fatalf("err = %v, want the slot-size complaint", err)
		}
	})
	t.Run("DA wrong hw", func(t *testing.T) {
		cfg := valid()
		cfg.daLoader = writeFakeDALoader(t, 0x6592, []byte("DA6592"))
		if _, err := planPhoneBROM(cfg, info); err == nil || !strings.Contains(err.Error(), "DA") {
			t.Fatalf("err = %v, want the DA hw complaint", err)
		}
	})
	t.Run("missing preloader", func(t *testing.T) {
		cfg := valid()
		cfg.preloader = filepath.Join(t.TempDir(), "nope.img")
		if _, err := planPhoneBROM(cfg, info); err == nil || !strings.Contains(err.Error(), "preloader") {
			t.Fatalf("err = %v, want the preloader complaint", err)
		}
	})
}

// The guard carve-out: the fully staged shape passes refusePhoneWrite (run
// routes it to the host-side plan), while the same serial+upload shape with
// staging files missing stays refused against the j36 feed.
func TestRefusePhoneWriteAllowsStagedBROM(t *testing.T) {
	phone := writePhoneRoot(t, "device=cph2385-4gb\nsoc=mt6765\n")
	info, ok := detectPhoneRoot(phone)
	if !ok {
		t.Fatal("fixture not detected")
	}
	if err := refusePhoneWrite(stagedConfig(t, phone), info); err != nil {
		t.Fatalf("staged BROM refused: %v", err)
	}
	bare := config{root: phone, upload: "lk", device: "/dev/cu.usbmodemXXXX"}
	if err := refusePhoneWrite(bare, info); err == nil {
		t.Fatal("bare serial+upload allowed, want the j36-feed refusal")
	}
	if !isPhoneBROMShape(stagedConfig(t, phone)) {
		t.Fatal("isPhoneBROMShape = false for the staged shape")
	}
	if isPhoneBROMShape(bare) {
		t.Fatal("isPhoneBROMShape = true for bare serial+upload")
	}
}
