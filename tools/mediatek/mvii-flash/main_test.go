package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

func TestParseDumpedPartitionSpansAndHiddenGaps(t *testing.T) {
	text := `DaHandler - Dumped sector 18432 with sector count 20480 as /tmp/protect_f.bin.
DaHandler - Dumped sector 38912 with sector count 20480 as /tmp/protect_s.bin.
DaHandler - Dumped sector 102912 with sector count 12288 as /tmp/2.bin.
DaHandler - Dumped sector 133632 with sector count 2048 as /tmp/3.bin.
DaHandler - Dumped sector 163840 with sector count 2457600 as /tmp/4.bin.
DaHandler - Dumped sector 2621440 with sector count 868352 as /tmp/5.bin.
DaHandler - Dumped sector 3489792 with sector count 4291478527 as /tmp/data.bin.`
	candidates := []string{"protect_f", "protect_s", "2", "3", "4", "5", "data"}
	files := []string{"/tmp/protect_f.bin", "/tmp/protect_s.bin", "/tmp/2.bin", "/tmp/3.bin", "/tmp/4.bin", "/tmp/5.bin", "/tmp/data.bin"}

	spans := parseDumpedPartitionSpans(text, candidates, files)
	if len(spans) != len(candidates) {
		t.Fatalf("spans = %d, want %d", len(spans), len(candidates))
	}
	if spans[2].Name != "2" || spans[2].Sector != 102912 || spans[2].Sectors != 12288 {
		t.Fatalf("span[2] = %+v", spans[2])
	}

	gaps := hiddenGapsFromSpans(spans)
	if len(gaps) != 4 {
		t.Fatalf("gaps = %d, want 4: %+v", len(gaps), gaps)
	}
	want := []struct {
		start  uint64
		length uint64
		before string
		after  string
	}{
		{0x0, 0x900000, "start of user area", "protect_f"},
		{0x1d00000, 0x1540000, "protect_s", "2"},
		{0x3840000, 0x900000, "2", "3"},
		{0x4240000, 0xdc0000, "3", "4"},
	}
	for i := range want {
		if gaps[i].Start != want[i].start || gaps[i].Length != want[i].length || gaps[i].Before != want[i].before || gaps[i].After != want[i].after {
			t.Fatalf("gap[%d] = %+v, want %+v", i, gaps[i], want[i])
		}
	}
}

func TestAndroidBootHeaderOffsets(t *testing.T) {
	data := []byte("prefixANDROID!middleANDROID!suffix")
	offsets := androidBootHeaderOffsets(data)
	if len(offsets) != 2 || offsets[0] != 6 || offsets[1] != 20 {
		t.Fatalf("offsets = %+v", offsets)
	}
}

func TestInferredRawBootLength(t *testing.T) {
	gap := rawGap{Start: 0x1d00000, Length: 0x1540000, Before: "protect_s", After: "2"}
	candidates := []rawBootCandidate{
		{Offset: 0x1f40000, Gap: gap},
		{Offset: 0x2840000, Gap: gap},
	}
	if got := inferredRawBootLength(candidates); got != 0x900000 {
		t.Fatalf("inferredRawBootLength = 0x%x, want 0x900000", got)
	}
}

func TestPrepareRawFlashImagePadsToRawLength(t *testing.T) {
	image := filepath.Join(t.TempDir(), "boot.img")
	if err := os.WriteFile(image, []byte("boot"), 0o644); err != nil {
		t.Fatal(err)
	}
	padded, cleanup, err := prepareRawFlashImage(image, 4, 16)
	if err != nil {
		t.Fatal(err)
	}
	defer cleanup()
	if padded == image {
		t.Fatalf("padded image reused source path")
	}
	data, err := os.ReadFile(padded)
	if err != nil {
		t.Fatal(err)
	}
	if len(data) != 16 || string(data[:4]) != "boot" {
		t.Fatalf("padded data = %q len=%d", data, len(data))
	}
	for i, b := range data[4:] {
		if b != 0 {
			t.Fatalf("padding byte %d = %x", i+4, b)
		}
	}
}

func TestRawBlockPlanUsesRawOffsetAndLength(t *testing.T) {
	cfg := config{device: "/dev/disk4", rawOffset: "0x1f40000", rawLength: "0x900000"}
	plan, err := makeRawBlockFlashPlan(cfg, 6144)
	if err != nil {
		t.Fatal(err)
	}
	if plan.offset != 0x1f40000 || plan.length != 0x900000 {
		t.Fatalf("plan offset/length = 0x%x/0x%x", plan.offset, plan.length)
	}
	if runtime.GOOS == "darwin" {
		if plan.diskDevice != "/dev/disk4" || plan.writeDevice != "/dev/rdisk4" {
			t.Fatalf("darwin plan devices = disk %q write %q", plan.diskDevice, plan.writeDevice)
		}
	} else if plan.diskDevice != "/dev/disk4" || plan.writeDevice != "/dev/disk4" {
		t.Fatalf("plan devices = disk %q write %q", plan.diskDevice, plan.writeDevice)
	}
}

func TestSerialDevicePathIsNotRawBlock(t *testing.T) {
	if !isSerialDevicePath("/dev/cu.usbmodem142301") {
		t.Fatalf("expected /dev/cu.usbmodem142301 to be classified as serial")
	}
	if !isSerialDevicePath("/dev/tty.usbmodem142301") {
		t.Fatalf("expected /dev/tty.usbmodem142301 to be classified as serial")
	}
	if isSerialDevicePath("/dev/disk4") {
		t.Fatalf("expected /dev/disk4 to be classified as block-like")
	}
	err := validateRawBlockDevice(rawBlockFlashPlan{
		inputDevice: "/dev/cu.usbmodem142301",
		diskDevice:  "/dev/cu.usbmodem142301",
		writeDevice: "/dev/cu.usbmodem142301",
		length:      1,
	})
	if err == nil || !strings.Contains(err.Error(), "serial/CDC") {
		t.Fatalf("validateRawBlockDevice err = %v", err)
	}
}

func TestWriteRawBlockImageWritesOffsetAndPadding(t *testing.T) {
	dir := t.TempDir()
	image := filepath.Join(dir, "boot.img")
	if err := os.WriteFile(image, []byte("boot"), 0o644); err != nil {
		t.Fatal(err)
	}
	device := filepath.Join(dir, "device.bin")
	original := bytes.Repeat([]byte{0xcc}, 64)
	if err := os.WriteFile(device, original, 0o644); err != nil {
		t.Fatal(err)
	}

	plan := rawBlockFlashPlan{
		inputDevice: device,
		diskDevice:  device,
		writeDevice: device,
		offset:      8,
		length:      16,
		imageSize:   4,
	}
	expectedHash, err := writeRawBlockImage(image, plan)
	if err != nil {
		t.Fatal(err)
	}
	if err := verifyRawBlockImage(plan, expectedHash); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(device)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(data[:8], original[:8]) || !bytes.Equal(data[24:], original[24:]) {
		t.Fatalf("raw write changed bytes outside requested range: %x", data)
	}
	if got := string(data[8:12]); got != "boot" {
		t.Fatalf("image bytes = %q, want boot", got)
	}
	if !bytes.Equal(data[12:24], make([]byte, 12)) {
		t.Fatalf("padding bytes = %x, want zeroes", data[12:24])
	}
}

func TestLegacyMTKWriteShimCreatesLauncher(t *testing.T) {
	mtkScript := filepath.Join(t.TempDir(), "mtk.py")
	if err := os.WriteFile(mtkScript, []byte("print('mtk')\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	tool, cleanup, err := legacyMTKWriteShim(commandSpec{name: "python3", args: []string{mtkScript}})
	if err != nil {
		t.Fatal(err)
	}
	defer cleanup()
	if tool.name != "python3" || len(tool.args) != 2 || tool.args[1] != mtkScript {
		t.Fatalf("shim tool = %+v", tool)
	}
	data, err := os.ReadFile(tool.args[0])
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(data), "MVII legacy write preflight") {
		t.Fatalf("shim source does not include legacy write preflight")
	}
}

func TestParseMTKDALoaderFindsMT6592Stages(t *testing.T) {
	dir := t.TempDir()
	loaderPath := filepath.Join(dir, "MTK_AllInOne_DA_mt6590.bin")
	data := make([]byte, 0x500)
	binary.LittleEndian.PutUint32(data[0x68:0x6c], 1)
	entry := data[0x6c : 0x6c+0xdc]
	binary.LittleEndian.PutUint16(entry[0:2], 0xdada)
	binary.LittleEndian.PutUint16(entry[2:4], 0x6592)
	binary.LittleEndian.PutUint16(entry[12:14], 0x200)
	binary.LittleEndian.PutUint16(entry[16:18], 1)
	binary.LittleEndian.PutUint16(entry[18:20], 3)
	writeRegion := func(index int, offset, length, addr uint32) {
		pos := 20 + index*20
		binary.LittleEndian.PutUint32(entry[pos:pos+4], offset)
		binary.LittleEndian.PutUint32(entry[pos+4:pos+8], length)
		binary.LittleEndian.PutUint32(entry[pos+8:pos+12], addr)
	}
	writeRegion(1, 0x300, 4, 0x111000)
	writeRegion(2, 0x304, 4, 0x112000)
	copy(data[0x300:0x304], []byte("DA1!"))
	copy(data[0x304:0x308], []byte("DA2!"))
	if err := os.WriteFile(loaderPath, data, 0o644); err != nil {
		t.Fatal(err)
	}
	loader, err := parseMTKDALoader(loaderPath, 0x6592, 0, 0)
	if err != nil {
		t.Fatal(err)
	}
	if loader.HWCode != 0x6592 || len(loader.Regions) != 3 {
		t.Fatalf("loader = %+v", loader)
	}
	da1, err := readDARegion(loader, 1)
	if err != nil {
		t.Fatal(err)
	}
	if string(da1) != "DA1!" {
		t.Fatalf("stage1 = %q", da1)
	}
}

func TestPrepareDADataChecksumPadsOddLength(t *testing.T) {
	checksum, payload := prepareDAData([]byte{0x34, 0x12, 0x78})
	if !bytes.Equal(payload, []byte{0x34, 0x12, 0x78, 0x00}) {
		t.Fatalf("payload = %x", payload)
	}
	if checksum != 0x1234^0x0078 {
		t.Fatalf("checksum = 0x%x", checksum)
	}
}

func TestExtractMTKPreloaderEMIFromMTKBin(t *testing.T) {
	emi := []byte{0x10, 0x20, 0x30, 0x40}
	data := append([]byte("prefix"), []byte("MTK_BLOADER_INFO_v15\x00")...)
	data = append(data, []byte("metadata")...)
	data = append(data, []byte("MTK_BIN")...)
	data = append(data, bytes.Repeat([]byte{0xaa}, 5)...)
	data = append(data, emi...)

	got, err := extractMTKPreloaderEMI("preloader.bin", data)
	if err != nil {
		t.Fatal(err)
	}
	if got.Version != 0x0F {
		t.Fatalf("EMI version = 0x%x, want 0x0f", got.Version)
	}
	if !bytes.Equal(got.Data, emi) {
		t.Fatalf("EMI data = %x, want %x", got.Data, emi)
	}
}

func TestExtractMTKPreloaderEMIFromWrappedPreloader(t *testing.T) {
	emi := []byte{0xde, 0xad, 0xbe, 0xef, 0x01, 0x02}
	inner := append([]byte("MTK_BLOADER_INFO_v10\x00"), []byte("MTK_BIN")...)
	inner = append(inner, bytes.Repeat([]byte{0xbb}, 5)...)
	inner = append(inner, emi...)

	wrapped := make([]byte, 0x30)
	copy(wrapped, []byte{0x4D, 0x4D, 0x4D, 0x01, 0x38, 0x00, 0x00, 0x00})
	wrapped = append(wrapped, inner...)
	wrapped = binary.LittleEndian.AppendUint32(wrapped, uint32(len(inner)))
	binary.LittleEndian.PutUint32(wrapped[0x20:0x24], uint32(len(wrapped)))
	binary.LittleEndian.PutUint32(wrapped[0x2C:0x30], 0)

	got, err := extractMTKPreloaderEMI("preloader.bin", wrapped)
	if err != nil {
		t.Fatal(err)
	}
	if got.Version != 0x0A {
		t.Fatalf("EMI version = 0x%x, want 0x0a", got.Version)
	}
	if !bytes.Equal(got.Data, emi) {
		t.Fatalf("EMI data = %x, want %x", got.Data, emi)
	}
}

func TestResolveMTKPreloaderEMIStandardDRAM(t *testing.T) {
	got, err := resolveMTKPreloaderEMI(config{mtkDRAM: "mt6592-standard"})
	if err != nil {
		t.Fatal(err)
	}
	if got == nil || got.UseDefault || !got.BuiltIn || got.Version != 0x0D || got.Path != "mt6592-standard-lpddr2-v13-default" {
		t.Fatalf("standard EMI config = %+v", got)
	}
	if len(got.Data) != 0xBC {
		t.Fatalf("standard EMI length = 0x%x, want 0xbc", len(got.Data))
	}
	if typ := binary.LittleEndian.Uint32(got.Data[4:8]); typ != 0x0002 {
		t.Fatalf("standard EMI type = 0x%x, want LPDDR2 0x0002", typ)
	}
}

func TestResolveMTKPreloaderEMILPDDR3DRAM(t *testing.T) {
	got, err := resolveMTKPreloaderEMI(config{mtkDRAM: "mt6592-lpddr3"})
	if err != nil {
		t.Fatal(err)
	}
	if got == nil || got.UseDefault || !got.BuiltIn || got.Version != 0x0D || got.Path != "mt6592-standard-lpddr3-v13-default" {
		t.Fatalf("standard LPDDR3 EMI config = %+v", got)
	}
	if len(got.Data) != 0xBC {
		t.Fatalf("standard LPDDR3 EMI length = 0x%x, want 0xbc", len(got.Data))
	}
	if typ := binary.LittleEndian.Uint32(got.Data[4:8]); typ != 0x0003 {
		t.Fatalf("standard LPDDR3 EMI type = 0x%x, want LPDDR3 0x0003", typ)
	}
}

func TestResolveMTKPreloaderEMIDefaultDRAMRequest(t *testing.T) {
	got, err := resolveMTKPreloaderEMI(config{mtkDRAM: "mt6592-da-default"})
	if err != nil {
		t.Fatal(err)
	}
	if got == nil || !got.UseDefault || got.Version != 0 || got.Path != "mt6592-standard-da-default" {
		t.Fatalf("DA default EMI config = %+v", got)
	}
}

func TestResolveDumpPreloaderPath(t *testing.T) {
	loader := filepath.Join("pkg", "tools", "mtk-da", "MTK_AllInOne_DA_mt6590.bin")
	wantNextToLoader := filepath.Join("pkg", "tools", "mtk-da", "preloader_j36ultra.bin")

	got, err := resolveDumpPreloaderPath(config{mtkDumpPreloader: "auto"}, loader)
	if err != nil {
		t.Fatal(err)
	}
	if got != wantNextToLoader {
		t.Fatalf("auto dump path = %q, want %q", got, wantNextToLoader)
	}

	got, err = resolveDumpPreloaderPath(config{mtkDumpPreloader: ""}, loader)
	if err != nil {
		t.Fatal(err)
	}
	if got != wantNextToLoader {
		t.Fatalf("empty dump path = %q, want %q", got, wantNextToLoader)
	}

	dir := t.TempDir()
	got, err = resolveDumpPreloaderPath(config{mtkDumpPreloader: dir}, loader)
	if err != nil {
		t.Fatal(err)
	}
	if want := filepath.Join(dir, "preloader_j36ultra.bin"); got != want {
		t.Fatalf("directory dump path = %q, want %q", got, want)
	}

	explicit := filepath.Join(dir, "custom.bin")
	got, err = resolveDumpPreloaderPath(config{mtkDumpPreloader: explicit}, loader)
	if err != nil {
		t.Fatal(err)
	}
	if got != explicit {
		t.Fatalf("explicit dump path = %q, want %q", got, explicit)
	}
}

func TestIsAllZero(t *testing.T) {
	if !isAllZero([]byte{0, 0, 0, 0}) {
		t.Fatal("all-zero slice should report true")
	}
	if isAllZero([]byte{0, 0, 1, 0}) {
		t.Fatal("slice with a set byte should report false")
	}
}

func TestMTKFeedFrameCodec(t *testing.T) {
	payload := []byte("hello feed")
	encoded := encodeMTKFeedFrame(mtkFeedFrameData, 42, payload)
	frame, err := decodeMTKFeedFrame(encoded)
	if err != nil {
		t.Fatal(err)
	}
	if frame.Type != mtkFeedFrameData || frame.Seq != 42 || !bytes.Equal(frame.Payload, payload) {
		t.Fatalf("decoded frame = %+v", frame)
	}

	encoded[len(encoded)-1] ^= 0xff
	if _, err := decodeMTKFeedFrame(encoded); err == nil || !strings.Contains(err.Error(), "CRC mismatch") {
		t.Fatalf("corrupt frame err = %v, want CRC mismatch", err)
	}
}

func TestMTKFeedStartPayloadLayout(t *testing.T) {
	sum := sha256.Sum256([]byte("image"))
	payload := buildMTKFeedStartPayload(0x1f40000, 0x1234, 0x2000, uint32(mtkLegacyEMMCPartUser), 0x200, 0x10000, mtkFeedFlagRebootAfterFlash, sum)
	if len(payload) != 72 {
		t.Fatalf("START payload length = %d, want 72", len(payload))
	}
	if got := binary.LittleEndian.Uint64(payload[0:8]); got != 0x1f40000 {
		t.Fatalf("target offset = 0x%x", got)
	}
	if got := binary.LittleEndian.Uint64(payload[8:16]); got != 0x1234 {
		t.Fatalf("image size = 0x%x", got)
	}
	if got := binary.LittleEndian.Uint64(payload[16:24]); got != 0x2000 {
		t.Fatalf("transfer length = 0x%x", got)
	}
	if got := binary.LittleEndian.Uint32(payload[24:28]); got != uint32(mtkLegacyEMMCPartUser) {
		t.Fatalf("eMMC part = 0x%x", got)
	}
	if got := binary.LittleEndian.Uint32(payload[36:40]); got != mtkFeedFlagRebootAfterFlash {
		t.Fatalf("flags = 0x%x", got)
	}
	if !bytes.Equal(payload[40:72], sum[:]) {
		t.Fatalf("hash bytes = %x, want %x", payload[40:72], sum)
	}
}

func TestMTKFeedTransferLength(t *testing.T) {
	tests := []struct {
		name      string
		imageSize uint64
		rawLength string
		want      uint64
		wantAuto  bool
		wantErr   string
	}{
		{name: "empty uses minimal aligned image", imageSize: 0x1801, want: 0x1a00, wantAuto: true},
		{name: "auto uses minimal aligned image", imageSize: 0x1801, rawLength: "auto", want: 0x1a00, wantAuto: true},
		{name: "minimal uses minimal aligned image", imageSize: 0x1801, rawLength: "minimal", want: 0x1a00, wantAuto: true},
		{name: "explicit is preserved when aligned", imageSize: 0x1801, rawLength: "0x900000", want: 0x900000},
		{name: "explicit is aligned up", imageSize: 0x1801, rawLength: "0x1801", want: 0x1a00},
		{name: "explicit cannot truncate image", imageSize: 0x1801, rawLength: "0x1000", wantErr: "smaller than image size"},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got, gotAuto, err := mtkFeedTransferLength(tt.imageSize, tt.rawLength)
			if tt.wantErr != "" {
				if err == nil || !strings.Contains(err.Error(), tt.wantErr) {
					t.Fatalf("err = %v, want containing %q", err, tt.wantErr)
				}
				return
			}
			if err != nil {
				t.Fatal(err)
			}
			if got != tt.want || gotAuto != tt.wantAuto {
				t.Fatalf("mtkFeedTransferLength() = 0x%x, %v; want 0x%x, %v", got, gotAuto, tt.want, tt.wantAuto)
			}
		})
	}
}

func TestDetectMTKFeedFlashImageKind(t *testing.T) {
	tests := []struct {
		name string
		data []byte
		want mtkFeedFlashImageKind
	}{
		{name: "android boot", data: []byte("ANDROID!rest"), want: mtkFeedImageAndroidBoot},
		{name: "rockchip loader", data: []byte("LOADER  \x00\x00"), want: mtkFeedImageRockchipLoader},
		{name: "rockchip trust", data: []byte("BL3X\x00\x01"), want: mtkFeedImageRockchipTrust},
		{name: "rockchip config", data: []byte{'C', 'F', 'G', 0, 1, 2}, want: mtkFeedImageRockchipConfig},
		{name: "unknown", data: []byte("raw"), want: mtkFeedImageUnknown},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			path := filepath.Join(t.TempDir(), "image.bin")
			if err := os.WriteFile(path, tt.data, 0o644); err != nil {
				t.Fatal(err)
			}
			got, err := detectMTKFeedFlashImageKind(path)
			if err != nil {
				t.Fatal(err)
			}
			if got != tt.want {
				t.Fatalf("kind = %q, want %q", got, tt.want)
			}
		})
	}
}

func TestValidateMTKFeedImageForTargetRejectsRockchip(t *testing.T) {
	path := filepath.Join(t.TempDir(), "uboot.img")
	if err := os.WriteFile(path, []byte("LOADER  \x00\x00"), 0o644); err != nil {
		t.Fatal(err)
	}
	err := validateMTKFeedImageForTarget(path)
	if err == nil || !strings.Contains(err.Error(), "Rockchip loader image") {
		t.Fatalf("err = %v, want Rockchip loader rejection", err)
	}
}

func TestValidateMTKFeedOffsetAgainstScatterRejectsMismatch(t *testing.T) {
	dir := t.TempDir()
	image := filepath.Join(dir, "boot.img")
	if err := os.WriteFile(image, []byte("ANDROID!"), 0o644); err != nil {
		t.Fatal(err)
	}
	scatter := `- partition_index: SYS9
  partition_name: BOOTIMG
  file_name: boot.img
  linear_start_addr: 0x1da0000
  region: EMMC_USER
`
	if err := os.WriteFile(filepath.Join(dir, "MT6592_Android_scatter.txt"), []byte(scatter), 0o644); err != nil {
		t.Fatal(err)
	}
	err := validateMTKFeedOffsetAgainstScatter(image, 0x1f40000, 0x1000)
	if err == nil || !strings.Contains(err.Error(), "use -raw-offset 0x1da0000") {
		t.Fatalf("err = %v, want scatter offset guidance", err)
	}
}

func TestValidateMTKFeedOffsetAgainstScatterAcceptsMatch(t *testing.T) {
	dir := t.TempDir()
	image := filepath.Join(dir, "lk.bin")
	if err := os.WriteFile(image, []byte("lk"), 0o644); err != nil {
		t.Fatal(err)
	}
	scatter := `- partition_index: SYS8
  partition_name: UBOOT
  file_name: lk.bin
  linear_start_addr: 0x1d40000
  region: EMMC_USER
`
	if err := os.WriteFile(filepath.Join(dir, "scatter.txt"), []byte(scatter), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := validateMTKFeedOffsetAgainstScatter(image, 0x1d40000, 0x200); err != nil {
		t.Fatal(err)
	}
}

func TestValidateMTKFeedOffsetAgainstScatterRejectsOversize(t *testing.T) {
	dir := t.TempDir()
	image := filepath.Join(dir, "boot.img")
	if err := os.WriteFile(image, []byte("ANDROID!"), 0o644); err != nil {
		t.Fatal(err)
	}
	scatter := `- partition_index: SYS9
  partition_name: BOOTIMG
  file_name: boot.img
  linear_start_addr: 0x1da0000
  partition_size: 0xa00000
  region: EMMC_USER
`
	if err := os.WriteFile(filepath.Join(dir, "MT6592_Android_scatter.txt"), []byte(scatter), 0o644); err != nil {
		t.Fatal(err)
	}
	err := validateMTKFeedOffsetAgainstScatter(image, 0x1da0000, 0xa00200)
	if err == nil || !strings.Contains(err.Error(), "partition size 0xa00000") {
		t.Fatalf("err = %v, want scatter length guidance", err)
	}
}

func TestMTKFeedTargetOffsetDefaultsToJ36BootSlot(t *testing.T) {
	root := t.TempDir()
	image := filepath.Join(root, "boot.img")
	got, defaulted, err := mtkFeedTargetOffset(config{root: root}, image, mtkLegacyEMMCPartUser)
	if err != nil {
		t.Fatal(err)
	}
	if !defaulted || got != j36UltraScatterBootImageOffset {
		t.Fatalf("target offset = 0x%x defaulted=%v, want 0x%x true", got, defaulted, j36UltraScatterBootImageOffset)
	}
}

func TestMTKFeedTargetOffsetDefaultsToJ36LKSlot(t *testing.T) {
	root := t.TempDir()
	image := filepath.Join(root, "lk.bin")
	got, defaulted, err := mtkFeedTargetOffset(config{root: root}, image, mtkLegacyEMMCPartUser)
	if err != nil {
		t.Fatal(err)
	}
	if !defaulted || got != j36UltraScatterLKOffset {
		t.Fatalf("target offset = 0x%x defaulted=%v, want 0x%x true", got, defaulted, j36UltraScatterLKOffset)
	}
}

func TestDefaultMTKFeedBundleEnabledWhenImagesPresent(t *testing.T) {
	root := t.TempDir()
	if err := os.WriteFile(filepath.Join(root, "boot.img"), []byte("arm"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(root, "MVIIS1.bin"), []byte("s1"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(root, "lk.bin"), []byte("lk"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(root, "assets.bin"), bytes.Repeat([]byte("a"), 64), 0o644); err != nil {
		t.Fatal(err)
	}
	cfg := config{root: root, mtkFeedPayload: "auto", mtkFeedPart: "user", mtkFeedBundle: true}
	if !shouldUseDefaultMTKFeedBundle(cfg) {
		t.Fatal("expected default MTK feed bundle when every MVII image is present")
	}
	cfg.image = filepath.Join(root, "lk.bin")
	if shouldUseDefaultMTKFeedBundle(cfg) {
		t.Fatal("explicit -image should keep the feed path single-image")
	}
}

// The bare default is -upload full, so a package missing any one of the four
// images is not a bundle -- but the narrower targets that do not need the
// missing file still are.
func TestUploadTargetSelectsItsOwnRequiredImages(t *testing.T) {
	root := t.TempDir()
	if err := os.WriteFile(filepath.Join(root, "lk.bin"), []byte("lk"), 0o644); err != nil {
		t.Fatal(err)
	}
	base := config{root: root, mtkFeedPayload: "auto", mtkFeedPart: "user", mtkFeedBundle: true}

	if shouldUseDefaultMTKFeedBundle(base) {
		t.Fatal("-upload full must not engage with MVIIS1.bin, boot.img and assets.bin missing")
	}
	lkOnly := base
	lkOnly.upload = uploadLK
	if !shouldUseDefaultMTKFeedBundle(lkOnly) {
		t.Fatal("-upload lk needs only lk.bin, which is present")
	}
	assetsOnly := base
	assetsOnly.upload = uploadAssets
	if shouldUseDefaultMTKFeedBundle(assetsOnly) {
		t.Fatal("-upload assets must not engage without assets.bin")
	}
	if err := os.WriteFile(filepath.Join(root, "assets.bin"), bytes.Repeat([]byte("a"), 64), 0o644); err != nil {
		t.Fatal(err)
	}
	if !shouldUseDefaultMTKFeedBundle(assetsOnly) {
		t.Fatal("-upload assets should engage once assets.bin exists")
	}
	release := base
	release.upload = uploadRelease
	if shouldUseDefaultMTKFeedBundle(release) {
		t.Fatal("-upload release must not fall back to lk.bin when lk-release.bin is absent")
	}
	if err := os.WriteFile(filepath.Join(root, "lk-release.bin"), []byte("rel"), 0o644); err != nil {
		t.Fatal(err)
	}
	if !shouldUseDefaultMTKFeedBundle(release) {
		t.Fatal("-upload release should engage once lk-release.bin exists")
	}
}

func TestUploadTargetPlansOnlyItsOwnWrites(t *testing.T) {
	root := t.TempDir()
	for name, body := range map[string]string{
		"lk.bin":         "lk",
		"lk-release.bin": "release-lk",
		"MVIIS1.bin":     "stage1",
		"boot.img":       "ANDROID!boot",
	} {
		if err := os.WriteFile(filepath.Join(root, name), []byte(body), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	if err := os.WriteFile(filepath.Join(root, "assets.bin"), bytes.Repeat([]byte("a"), 64), 0o644); err != nil {
		t.Fatal(err)
	}

	for _, tc := range []struct {
		target string
		want   []string
	}{
		{uploadFull, []string{"lk.bin", "MVIIS1.bin", "boot.img", "assets.bin"}},
		{uploadLK, []string{"lk.bin"}},
		{uploadSystem, []string{"MVIIS1.bin", "boot.img"}},
		{uploadAssets, []string{"assets.bin"}},
		{uploadRelease, []string{"lk-release.bin"}},
	} {
		plans, err := planDefaultMTKFeedBootChain(config{root: root, mtkFeedPart: "user", upload: tc.target, mtkFeedReboot: true})
		if err != nil {
			t.Fatalf("-upload %s: %v", tc.target, err)
		}
		var got []string
		for _, plan := range plans {
			got = append(got, filepath.Base(plan.Path))
		}
		if strings.Join(got, ",") != strings.Join(tc.want, ",") {
			t.Fatalf("-upload %s wrote %v, want %v", tc.target, got, tc.want)
		}
		// The reboot rides on the last write of whatever was selected, and on
		// nothing before it.
		for i, plan := range plans {
			hasReboot := plan.Flags&mtkFeedFlagRebootAfterFlash != 0
			if want := i == len(plans)-1; hasReboot != want {
				t.Fatalf("-upload %s plan %d (%s) reboot = %v, want %v", tc.target, i, got[i], hasReboot, want)
			}
		}
	}
}

// The release LK is an LK: it belongs in UBOOT, not wherever an unrecognised
// image would land.
func TestUploadReleaseTargetsTheUBOOTSlot(t *testing.T) {
	root := t.TempDir()
	if err := os.WriteFile(filepath.Join(root, "lk-release.bin"), []byte("release-lk"), 0o644); err != nil {
		t.Fatal(err)
	}
	plans, err := planDefaultMTKFeedBootChain(config{root: root, mtkFeedPart: "user", upload: uploadRelease})
	if err != nil {
		t.Fatal(err)
	}
	if len(plans) != 1 || plans[0].TargetOffset != j36UltraScatterLKOffset {
		t.Fatalf("release plan = %d writes, offset 0x%x, want 1 write at 0x%x",
			len(plans), plans[0].TargetOffset, j36UltraScatterLKOffset)
	}
}

func TestUploadRejectsUnknownTarget(t *testing.T) {
	if validUploadTarget("kernel") {
		t.Fatal("kernel is not an -upload target")
	}
	err := invalidUploadTargetError("kernel")
	for _, name := range []string{uploadAssets, uploadFull, uploadLK, uploadRelease, uploadSystem} {
		if !strings.Contains(err.Error(), name) {
			t.Fatalf("error %q does not offer %s", err, name)
		}
	}
}

func TestPlanDefaultMTKFeedBootChainWritesLKStage1ThenBoot(t *testing.T) {
	root := t.TempDir()
	if err := os.WriteFile(filepath.Join(root, "lk.bin"), []byte("lk"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(root, "MVIIS1.bin"), []byte("stage1"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(root, "boot.img"), []byte("ANDROID!boot"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(root, "assets.bin"), bytes.Repeat([]byte("a"), 64), 0o644); err != nil {
		t.Fatal(err)
	}

	plans, err := planDefaultMTKFeedBootChain(config{root: root, mtkFeedPart: "user"})
	if err != nil {
		t.Fatal(err)
	}
	if len(plans) != 4 {
		t.Fatalf("plans = %d, want 4", len(plans))
	}
	want := []struct {
		base      string
		part      string
		offset    uint64
		delta     uint64
		hasReboot bool
	}{
		{"lk.bin", "UBOOT", j36UltraScatterLKOffset, 0, false},
		{"MVIIS1.bin", "BOOTIMG", j36UltraScatterBootImageOffset + j36UltraBootImageStage1Offset, j36UltraBootImageStage1Offset, false},
		{"boot.img", "BOOTIMG", j36UltraScatterBootImageOffset, 0, false},
		{"assets.bin", "LOGO", j36UltraScatterLogoOffset, 0, false},
	}
	for i := range want {
		if filepath.Base(plans[i].Path) != want[i].base ||
			plans[i].PartitionName != want[i].part ||
			plans[i].TargetOffset != want[i].offset ||
			plans[i].PartitionDelta != want[i].delta {
			t.Fatalf("plan %d = path %s part %s offset 0x%x delta 0x%x",
				i, plans[i].Path, plans[i].PartitionName, plans[i].TargetOffset, plans[i].PartitionDelta)
		}
		if got := plans[i].Flags&mtkFeedFlagRebootAfterFlash != 0; got != want[i].hasReboot {
			t.Fatalf("plan %d reboot flag = %v, want %v", i, got, want[i].hasReboot)
		}
	}
}

func TestPlanMTKFeedBundleWritesStage2ThenLK(t *testing.T) {
	root := t.TempDir()
	arm := filepath.Join(root, "boot.img")
	lk := filepath.Join(root, "lk.bin")
	if err := os.WriteFile(arm, []byte("arm"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(lk, []byte("lk"), 0o644); err != nil {
		t.Fatal(err)
	}
	armInfo, err := os.Stat(arm)
	if err != nil {
		t.Fatal(err)
	}
	lkInfo, err := os.Stat(lk)
	if err != nil {
		t.Fatal(err)
	}
	cfg := config{root: root, mtkFeedPart: "user"}
	armPlan, err := planMTKFeedWrite(cfg, arm, armInfo, false)
	if err != nil {
		t.Fatal(err)
	}
	lkPlan, err := planMTKFeedWrite(cfg, lk, lkInfo, true)
	if err != nil {
		t.Fatal(err)
	}
	if armPlan.TargetOffset != j36UltraScatterBootImageOffset || armPlan.Flags != 0 {
		t.Fatalf("arm plan = offset 0x%x flags 0x%x", armPlan.TargetOffset, armPlan.Flags)
	}
	// The LK plan clears bootstatus before the image write, then stamps a
	// flash-pending marker immediately before reboot. If stage1 runs and can
	// write eMMC, it overwrites this marker with the live stage diagnostics.
	if lkPlan.TargetOffset != j36UltraScatterLKOffset ||
		lkPlan.Flags&mtkFeedFlagRebootAfterFlash == 0 ||
		lkPlan.Flags&mtkFeedFlagBootStatus == 0 {
		t.Fatalf("lk plan = offset 0x%x flags 0x%x", lkPlan.TargetOffset, lkPlan.Flags)
	}
}

func TestDefaultMTKFeedRebootCoversBootAndLKImages(t *testing.T) {
	root := t.TempDir()

	bootCfg := config{root: root}
	bootMsg := applyDefaultMTKFeedReboot(&bootCfg, filepath.Join(root, "boot.img"))
	if !bootCfg.mtkFeedReboot || !strings.Contains(bootMsg, "BOOTIMG image") {
		t.Fatalf("boot.img default reboot = %v msg %q", bootCfg.mtkFeedReboot, bootMsg)
	}

	lkCfg := config{root: root}
	lkMsg := applyDefaultMTKFeedReboot(&lkCfg, filepath.Join(root, "lk.bin"))
	if !lkCfg.mtkFeedReboot || !strings.Contains(lkMsg, "UBOOT-slot image") {
		t.Fatalf("lk.bin default reboot = %v msg %q", lkCfg.mtkFeedReboot, lkMsg)
	}

	optOut := config{root: root, mtkFeedRebootSet: true}
	if msg := applyDefaultMTKFeedReboot(&optOut, filepath.Join(root, "boot.img")); optOut.mtkFeedReboot || msg != "" {
		t.Fatalf("explicit reboot=false must opt out, reboot=%v msg %q", optOut.mtkFeedReboot, msg)
	}

	other := config{root: root}
	if msg := applyDefaultMTKFeedReboot(&other, filepath.Join(root, "userdata.img")); other.mtkFeedReboot || msg != "" {
		t.Fatalf("non-boot image must not default reboot, reboot=%v msg %q", other.mtkFeedReboot, msg)
	}
}

func TestPlanMTKFeedWriteRejectsHandoff(t *testing.T) {
	root := t.TempDir()
	arm := filepath.Join(root, "boot.img")
	if err := os.WriteFile(arm, []byte("arm"), 0o644); err != nil {
		t.Fatal(err)
	}
	info, err := os.Stat(arm)
	if err != nil {
		t.Fatal(err)
	}

	_, err = planMTKFeedWrite(config{root: root, mtkFeedPart: "user", mtkFeedHandoff: true}, arm, info, false)
	if err == nil || !strings.Contains(err.Error(), "flashing never mirrors streamed images into DRAM") {
		t.Fatalf("planMTKFeedWrite error = %v, want DRAM handoff disabled", err)
	}
}

func TestMTKFeedPayloadNeverMirrorsFlashToDRAM(t *testing.T) {
	flashStagePath := filepath.Join("..", "..", "..", "..", "OS", "MVII", "Kernel", "ARM", "MediaTek", "J36Ultra", "Drivers", "flash_stage.c")
	flashStage, err := os.ReadFile(flashStagePath)
	if err != nil {
		if os.IsNotExist(err) {
			t.Skip("PowerEngine flash_stage.c is outside this standalone flashing-tool copy")
		}
		t.Fatal(err)
	}
	source := string(flashStage)
	for _, forbidden := range []string{
		"boot_mirror_stream_data",
		"boot_streamed_android_image",
		"boot_copy_component",
		"copy_bytes((uint8_t*)(uintptr_t)(component_addr",
	} {
		if strings.Contains(source, forbidden) {
			t.Fatalf("MVIIFlash must not mirror flashed images into DRAM; found %q", forbidden)
		}
	}
	for _, want := range []string{
		"MAX_FEED_CHUNK = 64u * 1024u",
		"DRAM handoff during flashing is disabled",
	} {
		if !strings.Contains(source, want) {
			t.Fatalf("MVIIFlash source missing %q", want)
		}
	}

	msdcPath := filepath.Join("..", "..", "..", "..", "OS", "MVII", "Kernel", "ARM", "MediaTek", "J36Ultra", "Drivers", "mt6592_msdc.c")
	msdc, err := os.ReadFile(msdcPath)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(msdc), "MSDC_MAX_BURST_BLOCKS = 128u") {
		t.Fatal("MSDC writer must keep 64 KiB write bursts, not 4 KiB bursts")
	}
}

func TestPlanMTKFeedWriteTrimsPaddedMTKLKImage(t *testing.T) {
	root := t.TempDir()
	lk := filepath.Join(root, "lk.bin")
	data := make([]byte, 0x8000)
	binary.LittleEndian.PutUint32(data[0:4], mtkImageMagic)
	binary.LittleEndian.PutUint32(data[4:8], 0x1234)
	if err := os.WriteFile(lk, data, 0o644); err != nil {
		t.Fatal(err)
	}
	info, err := os.Stat(lk)
	if err != nil {
		t.Fatal(err)
	}

	plan, err := planMTKFeedWrite(config{root: root, mtkFeedPart: "user"}, lk, info, false)
	if err != nil {
		t.Fatal(err)
	}
	if plan.FileSize != 0x8000 || plan.SourceSize != 0x1434 || plan.ImageSize != 0x1434 || plan.TransferLength != 0x1600 {
		t.Fatalf("plan sizes = file 0x%x source 0x%x image 0x%x transfer 0x%x",
			plan.FileSize, plan.SourceSize, plan.ImageSize, plan.TransferLength)
	}
	if plan.LengthNote == "" {
		t.Fatal("expected padded MTK image length note")
	}
}

func TestPlanMTKFeedWriteHonorsExplicitRawLengthForPaddedMTKImage(t *testing.T) {
	root := t.TempDir()
	lk := filepath.Join(root, "lk.bin")
	data := make([]byte, 0x8000)
	binary.LittleEndian.PutUint32(data[0:4], mtkImageMagic)
	binary.LittleEndian.PutUint32(data[4:8], 0x1234)
	if err := os.WriteFile(lk, data, 0o644); err != nil {
		t.Fatal(err)
	}
	info, err := os.Stat(lk)
	if err != nil {
		t.Fatal(err)
	}

	plan, err := planMTKFeedWrite(config{root: root, mtkFeedPart: "user", rawLength: "0x8000"}, lk, info, false)
	if err != nil {
		t.Fatal(err)
	}
	if plan.SourceSize != 0x8000 || plan.ImageSize != 0x8000 || plan.TransferLength != 0x8000 {
		t.Fatalf("explicit raw-length plan sizes = source 0x%x image 0x%x transfer 0x%x",
			plan.SourceSize, plan.ImageSize, plan.TransferLength)
	}
	if plan.LengthNote != "" {
		t.Fatalf("LengthNote = %q, want empty for explicit raw-length", plan.LengthNote)
	}
}

func TestParseMTKPMTPartitionsFindsBootSlots(t *testing.T) {
	buf := make([]byte, 0x10000)
	copy(buf[0x200:], []byte("3vTP"))
	writePMTTestEntry(buf[0x208:], "UBOOT", 0x60000, 0x2a00000)
	writePMTTestEntry(buf[0x208+96:], "BOOTIMG", 0xa00000, 0x3000000)
	parts := parseMTKPMTPartitions(buf, j36UltraEMMCUserBytes-uint64(len(buf)))
	if len(parts) != 2 {
		t.Fatalf("parts = %d, want 2", len(parts))
	}
	uboot, ok := findMTKLivePartition(parts, "UBOOT")
	if !ok {
		t.Fatal("UBOOT not found")
	}
	if uboot.Offset != 0x2a00000 || uboot.Size != 0x60000 {
		t.Fatalf("UBOOT = offset 0x%x size 0x%x", uboot.Offset, uboot.Size)
	}
}

func TestApplyMTKLivePartitionOffsets(t *testing.T) {
	plans := []mtkFeedWritePlan{
		{Label: "boot.img", PartitionName: "BOOTIMG", TargetOffset: j36UltraScatterBootImageOffset, TransferLength: 0x4000},
		{Label: "MVIIS1.bin", PartitionName: "BOOTIMG", PartitionDelta: j36UltraBootImageStage1Offset, TargetOffset: j36UltraScatterBootImageOffset + j36UltraBootImageStage1Offset, TransferLength: 0x400},
		{Label: "lk.bin", PartitionName: "UBOOT", TargetOffset: j36UltraScatterLKOffset, TransferLength: 0x4000},
	}
	parts := []mtkLivePartition{
		{Name: "BOOTIMG", Offset: 0x3000000, Size: 0xa00000, Source: "PMT@test"},
		{Name: "UBOOT", Offset: 0x2a00000, Size: 0x60000, Source: "PMT@test"},
	}
	applyMTKLivePartitionOffsets(plans, parts)
	if plans[0].TargetOffset != 0x3000000 ||
		plans[1].TargetOffset != 0x3000000+j36UltraBootImageStage1Offset ||
		plans[2].TargetOffset != 0x2a00000 {
		t.Fatalf("offsets = 0x%x/0x%x/0x%x", plans[0].TargetOffset, plans[1].TargetOffset, plans[2].TargetOffset)
	}
}

func TestPreloaderProfileOverridesFeedOffsets(t *testing.T) {
	profile := `{
  "target": "j36-ultra",
  "lk_partition": "LK_A",
  "bootimg_partition": "BOOT_A",
  "partitions": [
    {"name": "LK_A", "offset": "0x2a00000", "size": "0x60000"},
    {"name": "BOOT_A", "offset": "0x3000000", "size": "0xa00000"}
  ]
}`
	path := filepath.Join(t.TempDir(), "profile.json")
	if err := os.WriteFile(path, []byte(profile), 0o644); err != nil {
		t.Fatal(err)
	}
	cfg := config{mtkPreloadProfile: path}
	lkOffset, ok, err := preloaderProfileTargetOffset(cfg, filepath.Join(t.TempDir(), "lk.bin"))
	if err != nil {
		t.Fatal(err)
	}
	if !ok || lkOffset != 0x2a00000 {
		t.Fatalf("lk offset = 0x%x ok=%v", lkOffset, ok)
	}
	bootOffset, ok, err := preloaderProfileTargetOffset(cfg, filepath.Join(t.TempDir(), "boot.img"))
	if err != nil {
		t.Fatal(err)
	}
	if !ok || bootOffset != 0x3000000 {
		t.Fatalf("boot offset = 0x%x ok=%v", bootOffset, ok)
	}
}

func TestScanPreloaderHardcodedPartitions(t *testing.T) {
	data := make([]byte, 0x400)
	copy(data[0x40:], []byte("PRELOADER\x00"))
	copy(data[0x60:], []byte("UBOOT\x00"))
	copy(data[0x80:], []byte("BOOTIMG\x00"))
	base := uint32(0x200d00)
	putPreloaderPartitionTestEntry(data[0x120:], base+0x40, 0, 0x40000)
	putPreloaderPartitionTestEntry(data[0x120+24:], base+0x60, 0x1d40000, 0x200000)
	putPreloaderPartitionTestEntry(data[0x120+48:], base+0x80, 0x1f40000, 0x900000)
	table := scanPreloaderHardcodedPartitions(data)
	if len(table.Partitions) != 3 {
		t.Fatalf("partitions = %d, want 3", len(table.Partitions))
	}
	if table.Partitions[1].Name != "UBOOT" || table.Partitions[1].Offset != 0x1d40000 {
		t.Fatalf("UBOOT partition = %+v", table.Partitions[1])
	}
	if table.Partitions[2].Name != "BOOTIMG" || table.Partitions[2].Size != 0x900000 {
		t.Fatalf("BOOTIMG partition = %+v", table.Partitions[2])
	}
}

func putPreloaderPartitionTestEntry(dst []byte, namePtr uint32, start, length uint32) {
	binary.LittleEndian.PutUint32(dst[0:4], namePtr)
	binary.LittleEndian.PutUint32(dst[4:8], start)
	binary.LittleEndian.PutUint32(dst[8:12], length)
}

func writePMTTestEntry(dst []byte, name string, size, offset uint64) {
	copy(dst[:64], []byte(name))
	binary.LittleEndian.PutUint64(dst[64:72], size)
	binary.LittleEndian.PutUint64(dst[80:88], offset)
}

func TestValidateJ36StockScatterFeedTargetRejectsOldBootOffset(t *testing.T) {
	image := filepath.Join(t.TempDir(), "boot.img")
	err := validateJ36StockScatterFeedTarget(config{}, image, 0x1da0000, 0x200)
	if err == nil || !strings.Contains(err.Error(), fmt.Sprintf("BOOTIMG at raw offset 0x%x", j36UltraScatterBootImageOffset)) {
		t.Fatalf("err = %v, want J36 BOOTIMG offset guidance", err)
	}
}

func TestValidateJ36StockScatterFeedTargetRejectsLKToBootOffset(t *testing.T) {
	image := filepath.Join(t.TempDir(), "lk.bin")
	err := validateJ36StockScatterFeedTarget(config{}, image, 0x1f40000, 0x200)
	if err == nil || !strings.Contains(err.Error(), fmt.Sprintf("UBOOT at raw offset 0x%x", j36UltraScatterLKOffset)) {
		t.Fatalf("err = %v, want J36 UBOOT offset guidance", err)
	}
}

func TestHashPaddedFile(t *testing.T) {
	path := filepath.Join(t.TempDir(), "image.bin")
	if err := os.WriteFile(path, []byte("abc"), 0o644); err != nil {
		t.Fatal(err)
	}
	got, err := hashPaddedFile(path, 3, 8)
	if err != nil {
		t.Fatal(err)
	}
	wantData := append([]byte("abc"), make([]byte, 5)...)
	want := sha256.Sum256(wantData)
	if got != want {
		t.Fatalf("hash = %x, want %x", got, want)
	}
}

func TestLoadMTKFeedRawPayloadDefaults(t *testing.T) {
	t.Setenv("MVII_MTK_PAYLOAD_ADDR", "")
	t.Setenv("MVII_MTK_PAYLOAD_ENTRY", "")
	path := filepath.Join(t.TempDir(), "payload.bin")
	if err := os.WriteFile(path, []byte{0x01, 0x02, 0x03, 0x04}, 0o644); err != nil {
		t.Fatal(err)
	}
	got, err := loadMTKFeedPayload(config{mtkFeedPayload: path})
	if err != nil {
		t.Fatal(err)
	}
	if got.Kind != "raw" || got.LoadAddr != mtkFeedDefaultPayloadAddr || got.EntryAddr != mtkFeedDefaultPayloadAddr {
		t.Fatalf("payload metadata = %+v", got)
	}
}

func TestResolveMTKFeedPayloadAuto(t *testing.T) {
	root := t.TempDir()
	boot := filepath.Join(root, "boot")
	if err := os.MkdirAll(boot, 0o755); err != nil {
		t.Fatal(err)
	}
	elfPath := filepath.Join(boot, "MVIIFlash.elf")
	if err := os.WriteFile(elfPath, []byte{0x7f, 'E', 'L', 'F'}, 0o644); err != nil {
		t.Fatal(err)
	}
	binPath := filepath.Join(boot, "MVIIFlash.bin")
	if err := os.WriteFile(binPath, []byte{0x01, 0x02, 0x03, 0x04}, 0o644); err != nil {
		t.Fatal(err)
	}
	got, err := resolveMTKFeedPayloadPath(config{root: root, mtkFeedPayload: "auto"})
	if err != nil {
		t.Fatal(err)
	}
	if got != binPath {
		t.Fatalf("auto payload path = %q, want %q", got, binPath)
	}
}

func TestResolveMTKFeedPayloadAutoFallsBackToELF(t *testing.T) {
	root := t.TempDir()
	boot := filepath.Join(root, "boot")
	if err := os.MkdirAll(boot, 0o755); err != nil {
		t.Fatal(err)
	}
	want := filepath.Join(boot, "MVIIFlash.elf")
	if err := os.WriteFile(want, []byte{0x7f, 'E', 'L', 'F'}, 0o644); err != nil {
		t.Fatal(err)
	}
	got, err := resolveMTKFeedPayloadPath(config{root: root, mtkFeedPayload: "auto"})
	if err != nil {
		t.Fatal(err)
	}
	if got != want {
		t.Fatalf("auto payload path = %q, want %q", got, want)
	}
}

func TestLoadMTKFeedAutoPrefersRawFlashStageBin(t *testing.T) {
	t.Setenv("MVII_MTK_PAYLOAD_ADDR", "")
	t.Setenv("MVII_MTK_PAYLOAD_ENTRY", "")
	root := t.TempDir()
	boot := filepath.Join(root, "boot")
	if err := os.MkdirAll(boot, 0o755); err != nil {
		t.Fatal(err)
	}
	binData := []byte{0x10, 0x20, 0x30, 0x40}
	if err := os.WriteFile(filepath.Join(boot, "MVIIFlash.bin"), binData, 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(boot, "MVIIFlash.elf"), []byte{0x7f, 'E', 'L', 'F'}, 0o644); err != nil {
		t.Fatal(err)
	}
	got, err := loadMTKFeedPayload(config{root: root, mtkFeedPayload: "auto"})
	if err != nil {
		t.Fatal(err)
	}
	if got.Kind != "raw" || got.LoadAddr != mtkFeedDefaultPayloadAddr || got.EntryAddr != mtkFeedDefaultPayloadAddr {
		t.Fatalf("payload metadata = %+v", got)
	}
	if !bytes.Equal(got.Data, binData) {
		t.Fatalf("payload data = %x, want %x", got.Data, binData)
	}
}

func TestParseFlagsLeavesMTKFeedPayloadAutoSymbolic(t *testing.T) {
	t.Setenv("MVII_MTK_PAYLOAD_ADDR", "")
	t.Setenv("MVII_MTK_PAYLOAD_ENTRY", "")
	oldArgs := os.Args
	defer func() { os.Args = oldArgs }()
	os.Args = []string{"flash", "-mtk-feed-payload", "auto"}

	got, err := parseFlags()
	if err != nil {
		t.Fatal(err)
	}
	if got.mtkFeedPayload != "auto" {
		t.Fatalf("mtkFeedPayload = %q, want auto", got.mtkFeedPayload)
	}
}

func TestParseFlagsRejectsMTKFeedHandoff(t *testing.T) {
	t.Setenv("MVII_MTK_PAYLOAD_ADDR", "")
	t.Setenv("MVII_MTK_PAYLOAD_ENTRY", "")
	oldArgs := os.Args
	defer func() { os.Args = oldArgs }()
	os.Args = []string{"flash", "-mtk-feed-handoff"}

	_, err := parseFlags()
	if err == nil || !strings.Contains(err.Error(), "flashing never mirrors streamed images into DRAM") {
		t.Fatalf("parseFlags error = %v, want DRAM handoff disabled", err)
	}
}

func TestParseFlagsRegistersAdvancedMTKFeedFlags(t *testing.T) {
	t.Setenv("MVII_MTK_PAYLOAD_ADDR", "")
	t.Setenv("MVII_MTK_PAYLOAD_ENTRY", "")
	oldArgs := os.Args
	defer func() { os.Args = oldArgs }()
	os.Args = []string{
		"flash",
		"-mtk-write-preloader", "preloader.bin",
		"-mtk-feed-live-parts=false",
		"-mtk-feed-chunk-size", "0x4000",
		"-mtk-read-boot-status",
	}

	got, err := parseFlags()
	if err != nil {
		t.Fatal(err)
	}
	if got.mtkWritePreloader != "preloader.bin" {
		t.Fatalf("mtkWritePreloader = %q", got.mtkWritePreloader)
	}
	if got.mtkFeedLiveParts {
		t.Fatal("mtkFeedLiveParts = true, want false")
	}
	if got.mtkFeedChunkSize != "0x4000" {
		t.Fatalf("mtkFeedChunkSize = %q", got.mtkFeedChunkSize)
	}
	if !got.mtkReadBootStatus {
		t.Fatal("mtkReadBootStatus = false, want true")
	}
}

func TestParseMTKFeedPart(t *testing.T) {
	tests := map[string]byte{
		"user":  mtkLegacyEMMCPartUser,
		"boot1": mtkLegacyEMMCPartBoot1,
		"boot2": 0x02,
		"0x09":  0x09,
	}
	for text, want := range tests {
		got, err := parseMTKFeedPart(text)
		if err != nil {
			t.Fatalf("parseMTKFeedPart(%q): %v", text, err)
		}
		if got != want {
			t.Fatalf("parseMTKFeedPart(%q) = 0x%x, want 0x%x", text, got, want)
		}
	}
}

func TestPlanMTKScatterFlashSkipsMissingImages(t *testing.T) {
	dir := t.TempDir()
	scatter := filepath.Join(dir, "MT6592_Android_scatter.txt")
	boot := filepath.Join(dir, "boot.img")
	if err := os.WriteFile(scatter, []byte("scatter"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(boot, []byte("boot"), 0o644); err != nil {
		t.Fatal(err)
	}

	items, skipped, err := planMTKScatterFlash(scatter, []mtkScatterEntry{
		{
			PartitionName: "PRO_INFO",
			FileName:      "PRO_INFO.txt",
			IsDownload:    true,
			LinearStart:   0x80000,
			PartitionSize: 0x300000,
			Region:        "EMMC_USER",
		},
		{
			PartitionName: "BOOTIMG",
			FileName:      "boot.img",
			IsDownload:    true,
			LinearStart:   0x1f40000,
			PartitionSize: 0x900000,
			Region:        "EMMC_USER",
		},
	})
	if err != nil {
		t.Fatal(err)
	}
	if len(items) != 1 || items[0].Entry.PartitionName != "BOOTIMG" {
		t.Fatalf("items = %+v, want BOOTIMG only", items)
	}
	if len(skipped) != 1 || !strings.Contains(skipped[0], "PRO_INFO") || !strings.Contains(skipped[0], "missing") {
		t.Fatalf("skipped = %+v, want missing PRO_INFO", skipped)
	}
}

func TestNegotiateFeedChunkSizeForScatterDoesNotGrow(t *testing.T) {
	got, err := negotiateFeedChunkSizeForScatter(mtkFeedHello{MaxChunk: 0x20000}, 0x10000)
	if err != nil {
		t.Fatal(err)
	}
	if got != 0x10000 {
		t.Fatalf("scatter chunk = 0x%x, want 0x10000", got)
	}

	got, err = negotiateFeedChunkSizeForScatter(mtkFeedHello{MaxChunk: 0x8000}, 0x10000)
	if err != nil {
		t.Fatal(err)
	}
	if got != 0x8000 {
		t.Fatalf("clamped scatter chunk = 0x%x, want 0x8000", got)
	}
}

func TestMTKSerialDeviceCandidatesHonorsSpecificUsbModem(t *testing.T) {
	// When a specific usbmodem ID is given, candidates must be exactly that node
	// (and its cu<->tty twin) and must never include unrelated usbmodem* nodes.
	// This prevents accidentally talking to "the wrong usbmodem".
	cands := mtkSerialDeviceCandidates("/dev/cu.usbmodem141300")
	has141300 := false
	has141301 := false
	for _, c := range cands {
		if c == "/dev/cu.usbmodem141300" || c == "/dev/tty.usbmodem141300" {
			has141300 = true
		}
		if strings.Contains(c, "141301") {
			has141301 = true
		}
	}
	if !has141300 {
		t.Fatalf("candidates for specific 141300 missing the named device: %v", cands)
	}
	if has141301 {
		t.Fatalf("candidates for specific 141300 must not include other numbers like 141301: %v", cands)
	}

	// Non-specific (no trailing digits) still gets broad glob (for discovery cases).
	// We don't assert the glob results here (depends on /dev at test time), but at
	// least it should not have filtered a "bare" usbmodem request.
	bare := mtkSerialDeviceCandidates("/dev/cu.usbmodem")
	if len(bare) == 0 {
		t.Fatalf("bare usbmodem candidates was empty")
	}
}

func TestHasSpecificUsbModemDigits(t *testing.T) {
	if !hasSpecificUsbModemDigits("cu.usbmodem141300") {
		t.Fatal("expected 141300 to be specific")
	}
	if !hasSpecificUsbModemDigits("/dev/tty.usbmodem123") {
		t.Fatal("expected 123 to be specific")
	}
	if hasSpecificUsbModemDigits("cu.usbmodem") {
		t.Fatal("bare usbmodem should not be treated as specific ID")
	}
	if hasSpecificUsbModemDigits("cu.usbmodemFOO") {
		t.Fatal("usbmodemFOO (no digits) should not be specific")
	}
}

func TestIsMTKScatterFile(t *testing.T) {
	dir := t.TempDir()
	good := filepath.Join(dir, "MT6572_Android_scatter.txt")
	content := `- partition_index: SYS0
  partition_name: PRELOADER
  file_name: preloader.bin
  is_download: true
  type: SV5_BL_BIN
  linear_start_addr: 0x0
  partition_size: 0x40000
  region: EMMC_BOOT_1
- partition_index: SYS1
  partition_name: BOOTIMG
  file_name: boot.img
  is_download: true
  linear_start_addr: 0x1da0000
  partition_size: 0xa00000
  region: EMMC_USER
`
	if err := os.WriteFile(good, []byte(content), 0o644); err != nil {
		t.Fatal(err)
	}
	if !isMTKScatterFile(good) {
		t.Errorf("expected %s to be recognized as scatter", good)
	}

	// Also by name even if in subdir
	named := filepath.Join(dir, "some_scatter.txt")
	if err := os.WriteFile(named, []byte(content), 0o644); err != nil {
		t.Fatal(err)
	}
	if !isMTKScatterFile(named) {
		t.Errorf("scatter name match should accept even for non-absolute")
	}

	plain := filepath.Join(dir, "plain.txt")
	if err := os.WriteFile(plain, []byte("hello: world\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	if isMTKScatterFile(plain) {
		t.Errorf("plain text should not be treated as scatter")
	}

	nonexistent := filepath.Join(dir, "nope_scatter.txt")
	if isMTKScatterFile(nonexistent) {
		t.Errorf("nonexistent should not be scatter")
	}
}

func TestParseFlagsAcceptsScatterPositional(t *testing.T) {
	oldArgs := os.Args
	defer func() { os.Args = oldArgs }()

	dir := t.TempDir()
	sc := filepath.Join(dir, "MT6572_Android_scatter.txt")
	scContent := `- partition_index: SYS1
  partition_name: BOOTIMG
  file_name: boot.img
  is_download: true
  linear_start_addr: 0x1da0000
  region: EMMC_USER
`
	if err := os.WriteFile(sc, []byte(scContent), 0o644); err != nil {
		t.Fatal(err)
	}

	// Simulate: flash <scatter> -device /dev/cu.dummy -yes
	// We must present already-reordered args because parseFlags() relies on run() having done
	// reorderArgsWithFlagsFirst; flag.Parse stops at first non-flag.
	os.Args = reorderArgsWithFlagsFirst([]string{"flash", sc, "-device", "/dev/cu.usbmodemTEST", "-yes"})
	got, err := parseFlags()
	if err != nil {
		t.Fatalf("parseFlags: %v", err)
	}
	if got.mtkFlashScatter == "" || !strings.HasSuffix(got.mtkFlashScatter, "MT6572_Android_scatter.txt") {
		t.Fatalf("mtkFlashScatter = %q, want path to scatter", got.mtkFlashScatter)
	}
	if got.image != "" {
		t.Fatalf("image should be empty when scatter was provided, got %q", got.image)
	}
	if got.device != "/dev/cu.usbmodemTEST" {
		t.Fatalf("device = %q", got.device)
	}
}

// The 0xd1 marker space is shared by stage1's step/rc/phase records and
// mt6592_display.c's publish() phase/detail records, and the two disagree about
// where the field boundaries are. 0xd1078004 is the case that mattered on the
// J36: a successful LK-handoff hand-over that used to print as an "MMSYS route"
// failure with rc=0x80, sending the panel-is-dark investigation the wrong way.
func TestBootStatusMarkerSeparatesDisplayDiagFromStage1(t *testing.T) {
	cases := []struct {
		name  string
		value uint32
		want  string
	}{
		{"lk handoff preserved", 0xd1078004, "display: LK handoff scanout preserved (dsi psctrl=0x0780 raw=0xd1078004)"},
		{"bound", 0xd1028002, "display: bound (width=640 raw=0xd1028002)"},
		{"presented", 0xd1270003, "display: presented (fb=0x02700000 raw=0xd1270003)"},
		{"dtb", 0xd1000001, "display: DTB framebuffer (raw=0xd1000001)"},
		{"failed", 0xd10000ff, "display: failed (raw=0xd10000ff)"},
		// Both halves check out as stage1, so stage1 keeps it.
		{"real stage1 record", 0xd1070004, "stage1 MMSYS route: OK (step=0x07 rc=0x00 phase=DSI DCS wake/skip raw=0xd1070004)"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			if got := mviiBootStatusMarker(tc.value); got != tc.want {
				t.Fatalf("mviiBootStatusMarker(0x%08x) = %q, want %q", tc.value, got, tc.want)
			}
		})
	}
}
