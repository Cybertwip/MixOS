package main

import (
	"bytes"
	"encoding/binary"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
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

func TestPhoneDACode(t *testing.T) {
	for soc, want := range map[string]uint16{"mt6765": 0x6765, "mt6739": 0x6739, "mt6833": 0x6833} {
		got, err := phoneDACode(soc)
		if err != nil || got != want {
			t.Errorf("phoneDACode(%q) = 0x%x, %v; want 0x%x", soc, got, err, want)
		}
	}
	if _, err := phoneDACode("exynos"); err == nil {
		t.Error("phoneDACode(exynos) = nil, want error")
	}
}

// BROM codes come from the facts table: mt6765 is board-observed
// (0x0766 on a retail CPH2385), mt6739/mt6833 from the reference flasher's
// chip table. A wrong one fails safe at probe time.
func TestPhoneBROMCode(t *testing.T) {
	for soc, want := range map[string]uint16{"mt6765": 0x0766, "mt6739": 0x0699, "mt6833": 0x0989} {
		got, err := phoneBROMCode(soc)
		if err != nil || got != want {
			t.Errorf("phoneBROMCode(%q) = 0x%x, %v; want 0x%x", soc, got, err, want)
		}
	}
	if _, err := phoneBROMCode("exynos"); err == nil {
		t.Error("phoneBROMCode(exynos) = nil, want error")
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
	if plan.hwCode != 0x0766 || plan.daCode != 0x6765 || plan.offset != 0x2ef00000 || plan.slotSize != 0x500000 || plan.partition != "lk_a" {
		t.Fatalf("plan = %+v, want BROM 0x0766 / DA 0x6765 lk_a at 0x2ef00000 size 0x500000", plan)
	}
}

// Auth is recorded, not required: the tool speaks no SLA exchange, so
// demanding the file would promise authentication that does not exist.
// Targets that enforce SLA/DAA are refused at probe time instead.
func TestPlanPhoneBROMAuthOptional(t *testing.T) {
	phone := writePhoneRoot(t, "device=cph2385-4gb\nsoc=mt6765\n")
	info, ok := detectPhoneRoot(phone)
	if !ok {
		t.Fatal("fixture not detected")
	}
	cfg := stagedConfig(t, phone)
	cfg.authFile = ""
	plan, err := planPhoneBROM(cfg, info)
	if err != nil {
		t.Fatalf("planPhoneBROM without auth = %v, want a valid plan", err)
	}
	if plan.auth != "" {
		t.Fatalf("plan.auth = %q, want empty", plan.auth)
	}
	if !isPhoneBROMShape(cfg) {
		t.Fatal("isPhoneBROMShape = false without auth, want the staged shape")
	}
}

func TestPlanPhoneBROMRawOffset(t *testing.T) {
	phone := writePhoneRoot(t, "device=cph2385-4gb\nsoc=mt6765\n")
	info, ok := detectPhoneRoot(phone)
	if !ok {
		t.Fatal("fixture not detected")
	}
	cfg := stagedConfig(t, phone)
	cfg.mtkScatter = ""
	cfg.rawOffset = "0x2ef00000"
	if !isPhoneBROMShape(cfg) {
		t.Fatal("isPhoneBROMShape = false for raw-offset form, want the staged shape")
	}
	plan, err := planPhoneBROM(cfg, info)
	if err != nil {
		t.Fatalf("planPhoneBROM = %v, want a valid plan", err)
	}
	if plan.offset != 0x2ef00000 || plan.slotSize != 0 || plan.partition != "lk_a" {
		t.Fatalf("plan = %+v, want offset 0x2ef00000 with unknown slot", plan)
	}
	cfg.rawOffset = "not-a-number"
	if _, err := planPhoneBROM(cfg, info); err == nil {
		t.Fatal("planPhoneBROM(bad offset) = nil, want the parse error")
	}
	cfg.rawOffset = ""
	if isPhoneBROMShape(cfg) {
		t.Fatal("isPhoneBROMShape = true with neither scatter nor raw-offset")
	}
	if _, err := planPhoneBROM(cfg, info); err == nil {
		t.Fatal("planPhoneBROM(no placement) = nil, want an error")
	}
}

// scriptPort is a canned mtkPort: reads drain the queued replies, writes are
// recorded for assertion. It pins our framing (echo, big-endian length
// round-trip, status words), not the peer's behavior.
type scriptPort struct {
	writes  [][]byte
	pending []byte
}

func (p *scriptPort) WriteAll(data []byte, _ time.Duration) error {
	p.writes = append(p.writes, append([]byte(nil), data...))
	return nil
}

func (p *scriptPort) DiscardInput(_ time.Duration) error { return nil }

func (p *scriptPort) Close() error { return nil }

func (p *scriptPort) ReadExact(n int, _ time.Duration) ([]byte, error) {
	if len(p.pending) < n {
		return nil, errors.New("script exhausted")
	}
	out := append([]byte(nil), p.pending[:n]...)
	p.pending = p.pending[n:]
	return out, nil
}

func be16(v uint16) []byte { return []byte{byte(v >> 8), byte(v)} }

func be32(v uint32) []byte {
	return []byte{byte(v >> 24), byte(v >> 16), byte(v >> 8), byte(v)}
}

func TestPrepareAuthData(t *testing.T) {
	even := []byte{0x01, 0x02}
	if got := prepareAuthData(even); !bytes.Equal(got, even) {
		t.Fatalf("prepareAuthData(even) = %x, want unchanged", got)
	}
	if got := prepareAuthData([]byte{0x01}); !bytes.Equal(got, []byte{0x01, 0x00}) {
		t.Fatalf("prepareAuthData(odd) = %x, want zero-padded", got)
	}
}

func TestSendAuth(t *testing.T) {
	newClient := func(replies []byte) (*mtkSerialClient, *scriptPort) {
		port := &scriptPort{pending: replies}
		return &mtkSerialClient{port: port, commandTimeout: time.Second, writeTimeout: time.Second}, port
	}
	blob := []byte{0xAA, 0xBB}

	t.Run("accepted", func(t *testing.T) {
		var replies []byte
		replies = append(replies, mtkCmdSendAuth)  // echo
		replies = append(replies, be32(2)...)      // length round-trip
		replies = append(replies, be16(0x0000)...) // status: proceed
		replies = append(replies, be16(0x1234)...) // crc (informational)
		replies = append(replies, be16(0x0000)...) // final status
		client, port := newClient(replies)
		if err := client.sendAuth(blob); err != nil {
			t.Fatalf("sendAuth = %v, want nil", err)
		}
		var joined []byte
		for _, w := range port.writes {
			joined = append(joined, w...)
		}
		if !bytes.Contains(joined, append([]byte{mtkCmdSendAuth}, be32(2)...)) {
			t.Fatalf("writes = %x, want echo + big-endian length", joined)
		}
		if !bytes.Contains(joined, blob) {
			t.Fatalf("writes = %x, want the blob", joined)
		}
	})
	t.Run("no auth needed", func(t *testing.T) {
		var replies []byte
		replies = append(replies, mtkCmdSendAuth)
		replies = append(replies, be32(2)...)
		replies = append(replies, be16(0x1D0C)...)
		client, _ := newClient(replies)
		if err := client.sendAuth(blob); err != nil {
			t.Fatalf("sendAuth(0x1D0C) = %v, want nil", err)
		}
	})
	t.Run("length mismatch", func(t *testing.T) {
		var replies []byte
		replies = append(replies, mtkCmdSendAuth)
		replies = append(replies, be32(99)...)
		client, _ := newClient(replies)
		if err := client.sendAuth(blob); err == nil || !strings.Contains(err.Error(), "length reply") {
			t.Fatalf("sendAuth = %v, want the length complaint", err)
		}
	})
	t.Run("status refusal", func(t *testing.T) {
		var replies []byte
		replies = append(replies, mtkCmdSendAuth)
		replies = append(replies, be32(2)...)
		replies = append(replies, be16(0x1D0D)...)
		client, _ := newClient(replies)
		if err := client.sendAuth(blob); err == nil || !strings.Contains(err.Error(), "SEND_AUTH status") {
			t.Fatalf("sendAuth = %v, want the status refusal", err)
		}
	})
}

func TestNeedsPhoneAuth(t *testing.T) {
	daa := mtkTargetConfig{DAA: true}
	if send, _ := needsPhoneAuth(daa, true); !send {
		t.Error("needsPhoneAuth(DAA, auth) = false, want the SEND_AUTH step")
	}
	if send, _ := needsPhoneAuth(daa, false); send {
		t.Error("needsPhoneAuth(DAA, no auth) = true, want skip")
	}
	if send, msg := needsPhoneAuth(mtkTargetConfig{}, true); send || msg != "" {
		t.Errorf("needsPhoneAuth(clear) = %v %q, want silent skip", send, msg)
	}
}

func TestPhoneFactsFor(t *testing.T) {
	want := map[string]phoneFacts{
		"mt6765": {bromCode: 0x0766, daCode: 0x6765, watchdog: 0x10007000, watchdogOff: 0x22000064, miscLock: 0x1001a100},
		"mt6739": {bromCode: 0x0699, daCode: 0x6739, watchdog: 0x10007000, watchdogOff: 0x22000064, miscLock: 0x1001a100},
		"mt6833": {bromCode: 0x0989, daCode: 0x6833, watchdog: 0x10007000, watchdogOff: 0x22000064, miscLock: 0},
	}
	for soc, w := range want {
		got, err := phoneFactsFor(soc)
		if err != nil || got != w {
			t.Errorf("phoneFactsFor(%q) = %+v, %v; want %+v", soc, got, err, w)
		}
	}
	if _, err := phoneFactsFor("mt9999"); err == nil {
		t.Error("phoneFactsFor(mt9999) = nil, want error")
	}
}

func TestPhoneUSBDLReg(t *testing.T) {
	reg := phoneUSBDLReg()
	if reg&mtkUSBDLMagic != mtkUSBDLMagic || reg&mtkUSBDLBitEnable == 0 || reg&mtkUSBDLByPreloader != 0 {
		t.Fatalf("phoneUSBDLReg() = 0x%08x, want magic+enabled addressed to BROM", reg)
	}
	if reg&^mtkUSBDLTimeoutMask&^(mtkUSBDLMagic|mtkUSBDLBitEnable) != 0 {
		t.Fatalf("phoneUSBDLReg() = 0x%08x, want no stray bits", reg)
	}
}

func TestParsePhoneWait(t *testing.T) {
	d, err := parsePhoneWait("")
	if err != nil || d != 0 {
		t.Fatalf("parsePhoneWait(\"\") = %v, %v; want 0, nil", d, err)
	}
	d, err = parsePhoneWait("90s")
	if err != nil || d != 90*time.Second {
		t.Fatalf("parsePhoneWait(90s) = %v, %v; want 90s", d, err)
	}
	if _, err := parsePhoneWait("junk"); err == nil {
		t.Fatal("parsePhoneWait(junk) = nil, want error")
	}
	if _, err := parsePhoneWait("-5s"); err == nil {
		t.Fatal("parsePhoneWait(-5s) = nil, want error")
	}
}

// failPort is a transport that is already gone: every read fails with a
// device-gone error, pinning the wait loop's reconnect path.
type failPort struct {
	scriptPort
	err error
}

func (p *failPort) ReadExact(_ int, _ time.Duration) ([]byte, error) {
	return nil, p.err
}

// A dead session plus a failing reconnect must expire cleanly, never probe
// a nil client (the SIGSEGV seen live when the 45s reopen found nothing).
func TestWaitPhoneBROMExpiredConnectFails(t *testing.T) {
	phone := writePhoneRoot(t, "device=cph2385-4gb\nsoc=mt6765\n")
	info, ok := detectPhoneRoot(phone)
	if !ok {
		t.Fatal("fixture not detected")
	}
	facts, err := phoneFactsFor("mt6765")
	if err != nil {
		t.Fatal(err)
	}
	port := &failPort{err: errors.New("read /dev/cu.usbmodem1: device not configured")}
	client := &mtkSerialClient{port: port, commandTimeout: time.Millisecond, writeTimeout: time.Millisecond}
	connect := func(string) (*mtkSerialClient, error) { return nil, errors.New("nope") }
	_, _, err = waitPhoneBROM(config{device: "/dev/cu.usbmodem1"}, info, facts, client, 50*time.Millisecond, connect)
	if err == nil || !strings.Contains(err.Error(), "BROM wait expired") {
		t.Fatalf("waitPhoneBROM = %v, want the clean expiry", err)
	}
}

// Against a dead peer every crash mode fails fast and ignored, the run
// still completes, and the three modes fire in order (the null jump itself
// fires only when its SEND_DA is accepted, so a dead peer never sees it --
// same as the reference, which skips the jump when the send raises).
func TestCrashPhonePreloader(t *testing.T) {
	phone := writePhoneRoot(t, "device=cph2385-4gb\nsoc=mt6765\n")
	info, ok := detectPhoneRoot(phone)
	if !ok {
		t.Fatal("fixture not detected")
	}
	facts, err := phoneFactsFor("mt6765")
	if err != nil {
		t.Fatal(err)
	}
	port := &failPort{err: errors.New("read /dev/cu.usbmodem1: device not configured")}
	client := &mtkSerialClient{port: port, commandTimeout: time.Millisecond, writeTimeout: time.Millisecond}
	crashPhonePreloader(client, info, facts)
	var singles []byte
	for _, w := range port.writes {
		if len(w) == 1 {
			singles = append(singles, w[0])
		}
	}
	first := func(b byte) int { return bytes.IndexByte(singles, b) }
	last := func(b byte) int { return bytes.LastIndexByte(singles, b) }
	if first(mtkCmdSendDA) < 0 || first(mtkCmdRead32) < 0 {
		t.Fatalf("command bytes = %x, want SEND_DA and READ32 present", singles)
	}
	if !(first(mtkCmdSendDA) < first(mtkCmdRead32) && first(mtkCmdRead32) < last(mtkCmdSendDA)) {
		t.Fatalf("command bytes = %x, want mode order SEND_DA, READ32, SEND_DA", singles)
	}
	if bytes.Contains(singles, []byte{mtkCmdJumpDA}) {
		t.Fatalf("command bytes = %x, want no jump on a dead peer", singles)
	}
}

func TestFlagFailureAdvice(t *testing.T) {
	err := flagFailureAdvice(errors.New("unlock BOOT_MISC: write32(0x1001a100) initial status 0x1001"))
	if err == nil || !strings.Contains(err.Error(), "0x1001") || !strings.Contains(err.Error(), "Vol-down") {
		t.Fatalf("flagFailureAdvice = %v, want cause plus key-combo guidance", err)
	}
}

func TestHandshakeWake(t *testing.T) {
	echoes := []byte{0x5F, 0xF5, 0xAF, 0xFA}
	port := &scriptPort{pending: append([]byte(nil), echoes...)}
	client := &mtkSerialClient{port: port, commandTimeout: time.Second, writeTimeout: time.Second, handshakeWake: true}
	if err := client.handshake(time.Now().Add(2 * time.Second)); err != nil {
		t.Fatalf("handshake(wake) = %v, want nil", err)
	}
	if len(port.writes) != 5 || !bytes.Equal(port.writes[0], []byte{0xA0}) {
		t.Fatalf("writes = %x, want lone wake 0xA0 then the 4-byte sequence", port.writes)
	}
	var seq []byte
	for _, w := range port.writes[1:] {
		seq = append(seq, w...)
	}
	if !bytes.Equal(seq, []byte{0xA0, 0x0A, 0x50, 0x05}) {
		t.Fatalf("sequence = %x, want the handshake bytes", seq)
	}

	plain := &scriptPort{pending: append([]byte(nil), echoes...)}
	plainClient := &mtkSerialClient{port: plain, commandTimeout: time.Second, writeTimeout: time.Second}
	if err := plainClient.handshake(time.Now().Add(2 * time.Second)); err != nil {
		t.Fatalf("handshake(plain) = %v, want nil", err)
	}
	if len(plain.writes) != 4 {
		t.Fatalf("plain writes = %d calls, want exactly 4 (J36 behavior unchanged)", len(plain.writes))
	}
}

func TestRefusePhoneSLA(t *testing.T) {
	phone := &phoneRoot{device: "cph2385-4gb", soc: "mt6765"}
	if err := refusePhoneSLA(phone, mtkTargetConfig{}); err != nil {
		t.Fatalf("refusePhoneSLA(clear) = %v, want nil", err)
	}
	if err := refusePhoneSLA(phone, mtkTargetConfig{SLA: true}); err == nil || !strings.Contains(err.Error(), "SLA") {
		t.Fatalf("refusePhoneSLA(SLA) = %v, want the SLA refusal", err)
	}
	// DAA alone proceeds: it is verified against the DA image, and the
	// vendor-signed DA may pass with no host exchange. Observed on a
	// retail CPH2385 (SBC+DAA, no SLA).
	if err := refusePhoneSLA(phone, mtkTargetConfig{DAA: true}); err != nil {
		t.Fatalf("refusePhoneSLA(DAA) = %v, want nil", err)
	}
}

func TestResolvePhoneEMI(t *testing.T) {
	if _, err := resolvePhoneEMI(config{}); err == nil || !strings.Contains(err.Error(), "-preloader") {
		t.Fatalf("resolvePhoneEMI(empty) = %v, want the -preloader demand", err)
	}
	pre := append([]byte("xxMTK_BLOADER_INFO_v17xxMTK_BIN\x00\x00\x00\x00\x00"), []byte("EMIPAYLOAD")...)
	path := writeStagedFile(t, "preloader.bin", pre)
	emi, err := resolvePhoneEMI(config{preloader: path})
	if err != nil {
		t.Fatalf("resolvePhoneEMI = %v, want parsed EMI", err)
	}
	if emi.Version != 0x11 || string(emi.Data) != "EMIPAYLOAD" {
		t.Fatalf("emi = version 0x%x data %q, want 0x11 EMIPAYLOAD", emi.Version, emi.Data)
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
