package main

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
)

const (
	mtkHWCodeMT6592 = 0x6592

	mtkCmdSendDA          = 0xD7
	mtkCmdJumpDA          = 0xD5
	mtkCmdRead32          = 0xD1
	mtkCmdWrite32         = 0xD4
	mtkCmdGetTargetConfig = 0xD8
	mtkCmdGetHWSWVersion  = 0xFC
	mtkCmdGetHWCode       = 0xFD
	mtkCmdGetBLVersion    = 0xFE
	mtkCmdGetBROMVersion  = 0xFF

	mtkLegacyACK              = 0x5A
	mtkLegacyNACK             = 0xA5
	mtkLegacyCONT             = 0x69
	mtkLegacySync             = 0xC0
	mtkLegacyEnableDRAM       = 0xE8
	mtkLegacySDMMCSwitchPart  = 0x60
	mtkLegacySDMMCWriteImage  = 0x61
	mtkLegacySDMMCWriteData   = 0x62
	mtkLegacyEMMCRead         = 0xD6
	mtkLegacyUSBSetupPort     = 0x70
	mtkLegacyUSBCheckStatus   = 0x72
	mtkLegacyHostLinux        = 0x0C
	mtkLegacyHWStorageEMMC    = 0x02
	mtkLegacyEMMCStorage      = 0x01
	mtkLegacyEMMCPartBoot1    = 0x01
	mtkLegacyEMMCPartUser     = 0x08
	mtkLegacyNeedDRAMConfig   = 0x0BC3
	mtkLegacyDRAMInfo         = 0x0BC4
	mtkDefaultSerialPacket    = 0x100000
	mtkMaximumSerialPacket    = 0x1000000
	mtkPreloaderDumpLength    = 0x100000
	mtkSerialBlockSize        = 0x200
	mtkSerialHandshakeTimeout = 45 * time.Second
	mtkSerialOpenHandshake    = 2500 * time.Millisecond
	mtkSerialReconnectTimeout = 120 * time.Second
	mtkSerialCommandTimeout   = 30 * time.Second
	mtkSerialWriteTimeout     = 120 * time.Second

	mtkMT6592WatchdogBase  = 0x10007000
	mtkMT6592MiscLock      = 0x10002050
	mtkMT6592USBDLFlag     = mtkMT6592MiscLock - 0x20
	mtkMT6592ResetControl  = mtkMT6592MiscLock + 0x08
	mtkUSBDLBitEnable      = 0x00000001
	mtkUSBDLByPreloader    = 0x00000002
	mtkUSBDLTimeoutMask    = 0x0000FFFC
	mtkUSBDLTimeoutMax     = mtkUSBDLTimeoutMask >> 2
	mtkUSBDLMagic          = 0x444C0000
	mtkMiscLockKeyMagic    = 0xAD98
	mtkWatchdogRestart     = 0x00001971
	mtkWatchdogRebootMode  = 0x22000014
	mtkWatchdogSoftwareRst = 0x00001209
)

// errMTKDRAMInit marks a failure inside the DA's DRAM/EMI calibration so callers
// can fall back to a different EMI profile instead of giving up.
var errMTKDRAMInit = errors.New("preloader DRAM init failed")

// mtkPort is the byte transport the MTK command layer talks over. It is
// satisfied by the macOS/Linux tty serial port (*mtkSerialPort) and, on macOS,
// by the libusb raw-bulk transport (*mtkUSBPort) which bypasses the kernel
// CDC-ACM driver that destabilises the MT6592 BROM.
type mtkPort interface {
	ReadExact(n int, timeout time.Duration) ([]byte, error)
	WriteAll(data []byte, timeout time.Duration) error
	DiscardInput(timeout time.Duration) error
	Close() error
}

type mtkSerialClient struct {
	port             mtkPort
	device           string
	commandTimeout   time.Duration
	writeTimeout     time.Duration
	writeMu          sync.Mutex
	blVersion        byte
	bromVersion      byte
	isBROM           bool
	feedPayloadReady bool
	feedPayloadHello mtkFeedHello
	feedQuiet        bool
	flashInfo        mtkLegacyFlashInfo
	preloaderEMI     *mtkPreloaderEMI
	emmcID           []byte
}

type mtkSerialConnectOptions struct {
	recoverFeedPayload bool
	reuseFeedPayload   bool
}

type mtkTargetConfig struct {
	Raw      uint32
	SBC      bool
	SLA      bool
	DAA      bool
	EPP      bool
	Cert     bool
	MemRead  bool
	MemWrite bool
	CmdC8    bool
}

type mtkDALoader struct {
	Path             string
	V6               bool
	Old              bool
	HWCode           uint16
	HWSubCode        uint16
	HWVersion        uint16
	SWVersion        uint16
	PageSize         uint16
	EntryRegionIndex uint16
	Regions          []mtkDARegion
}

type mtkDARegion struct {
	BufferOffset uint32
	Length       uint32
	StartAddr    uint32
	StartOffset  uint32
	SignatureLen uint32
}

type mtkLegacyFlashInfo struct {
	Storage      string
	FlashSize    uint64
	EMMCUserSize uint64
}

type mtkPreloaderEMI struct {
	Path       string
	Version    uint32
	Data       []byte
	UseDefault bool
	BuiltIn    bool
}

func flashWithMTKSerialDA(cfg config, image string) error {
	if cfg.rawOffset == "" {
		return errors.New("-backend=mtk-serial requires -raw-offset for the J36 Ultra raw eMMC write")
	}
	imageInfo, err := os.Stat(image)
	if err != nil {
		return err
	}
	if imageInfo.Size() < 0 {
		return fmt.Errorf("image size is invalid: %d", imageInfo.Size())
	}
	offset, err := parseMTKNumber(cfg.rawOffset, "-raw-offset")
	if err != nil {
		return err
	}
	rawLength := uint64(imageInfo.Size())
	if cfg.rawLength != "" {
		rawLength, err = parseMTKNumber(cfg.rawLength, "-raw-length")
		if err != nil {
			return err
		}
	}
	if rawLength < uint64(imageInfo.Size()) {
		return fmt.Errorf("-raw-length 0x%x is smaller than image size 0x%x", rawLength, uint64(imageInfo.Size()))
	}
	if rawLength%mtkSerialBlockSize != 0 {
		aligned := alignUp(rawLength, mtkSerialBlockSize)
		fmt.Printf("Padded MTK serial write length from 0x%x to 0x%x for 512-byte eMMC alignment.\n", rawLength, aligned)
		rawLength = aligned
	}
	packetSize, err := mtkSerialPacketSize(cfg)
	if err != nil {
		return err
	}
	loaderPath, err := resolveMTKDALoader(cfg)
	if err != nil {
		return err
	}
	loader, err := parseMTKDALoader(loaderPath, mtkHWCodeMT6592, 0, 0)
	if err != nil {
		return err
	}
	preloaderEMI, err := resolveMTKPreloaderEMI(cfg)
	if err != nil {
		return err
	}
	flashImage, cleanup, err := prepareRawFlashImage(image, imageInfo.Size(), rawLength)
	if err != nil {
		return err
	}
	defer cleanup()

	lengthText := fmt.Sprintf("0x%x", rawLength)
	if err := confirmRawFlash(cfg, fmt.Sprintf("MediaTek BROM/preloader serial %s", cfg.device), image, cfg.rawOffset, lengthText); err != nil {
		return err
	}

	fmt.Println("MTK serial mode expects the J36 Ultra powered off or in PreLoader/BROM VCOM mode.")
	fmt.Println("If the handshake waits, hold the board's boot/download key combo while reconnecting USB.")
	fmt.Printf("Using DA loader: %s\n", loader.Path)
	if preloaderEMI != nil {
		if preloaderEMI.UseDefault {
			fmt.Printf("Using MT6592 standard DA DRAM defaults: %s\n", preloaderEMI.Path)
		} else if preloaderEMI.BuiltIn {
			fmt.Printf("Using MT6592 standard EMI profile: %s (version 0x%x, length 0x%x)\n", preloaderEMI.Path, preloaderEMI.Version, len(preloaderEMI.Data))
		} else {
			fmt.Printf("Using device preloader EMI: %s (version 0x%x, length 0x%x)\n", preloaderEMI.Path, preloaderEMI.Version, len(preloaderEMI.Data))
		}
	}
	fmt.Printf("Using MTK serial packet size: 0x%x\n", packetSize)

	client, err := connectMTKSerial(cfg.device)
	if err != nil {
		return err
	}
	client.preloaderEMI = preloaderEMI
	defer func() {
		_ = client.port.Close()
	}()

	if err := client.probeMT6592(); err != nil {
		return err
	}
	if err := client.uploadLegacyDA(loader); err != nil {
		return err
	}
	if err := client.writeLegacyEMMCRaw(flashImage, offset, rawLength, packetSize); err != nil {
		return err
	}
	fmt.Println("MTK serial DA flash complete.")
	return nil
}

// mtkDumpEMICandidates returns the EMI profiles to try, in order, when bringing
// up DRAM for a preloader dump: the user's chosen profile first, then the generic
// MT6592 standard LPDDR2/LPDDR3 records as fallbacks. The fallbacks matter because
// the DA's built-in defaults (-mtk-dram da-default) have no EMI for this board and
// fail calibration, while the standard profile has been observed to bring DRAM up.
func mtkDumpEMICandidates(cfg config) ([]*mtkPreloaderEMI, error) {
	primary, err := resolveMTKPreloaderEMI(cfg)
	if err != nil {
		return nil, err
	}
	var out []*mtkPreloaderEMI
	seen := map[string]bool{}
	add := func(e *mtkPreloaderEMI) {
		if e == nil || seen[e.Path] {
			return
		}
		seen[e.Path] = true
		out = append(out, e)
	}
	add(primary)
	if std, err := mt6592StandardEMI("lpddr2"); err == nil {
		add(std)
	}
	if std, err := mt6592StandardEMI("lpddr3"); err == nil {
		add(std)
	}
	if len(out) == 0 {
		return nil, errors.New("no EMI profile available to bring DRAM up for the dump")
	}
	return out, nil
}

// bringUpMTKSerialDA connects, handshakes, probes and uploads the DA, trying each
// EMI candidate in turn. A DRAM-init failure (errMTKDRAMInit) triggers a fresh
// reconnect with the next profile; any other error aborts immediately. The first
// candidate uses the initial connection; later candidates need the board to still
// answer the BROM handshake, which usually means leaving it on the download key.
func bringUpMTKSerialDA(cfg config, loader mtkDALoader, candidates []*mtkPreloaderEMI) (*mtkSerialClient, error) {
	var lastErr error
	for i, emi := range candidates {
		label := "device EMI"
		switch {
		case emi.UseDefault:
			label = "DA built-in DRAM defaults"
		case emi.BuiltIn:
			label = "generic EMI profile"
		}
		fmt.Printf("DRAM bring-up attempt %d/%d using %s: %s\n", i+1, len(candidates), label, emi.Path)

		client, err := connectMTKSerial(cfg.device)
		if err != nil {
			return nil, err
		}
		client.preloaderEMI = emi
		if err := client.probeMT6592(); err != nil {
			_ = client.port.Close()
			return nil, err
		}
		if err := client.uploadLegacyDA(loader); err != nil {
			_ = client.port.Close()
			if errors.Is(err, errMTKDRAMInit) && i+1 < len(candidates) {
				fmt.Printf("DRAM bring-up with %s failed: %v\n", emi.Path, err)
				fmt.Println("Retrying with the next EMI profile; keep the board in BROM/download mode (power-cycle or re-hold the boot key if the next handshake stalls).")
				lastErr = err
				continue
			}
			return nil, err
		}
		return client, nil
	}
	if lastErr == nil {
		lastErr = errors.New("no EMI candidates were tried")
	}
	return nil, fmt.Errorf("all EMI profiles failed DRAM init: %w", lastErr)
}

// dumpPreloaderMTKSerial brings up the DA exactly like a flash run, then reads
// the device's own preloader out of the eMMC boot1 partition and saves it. The
// saved binary carries the board's real EMI record (MTK_BLOADER_INFO/MTK_BIN),
// so a later flash run can pass it via -preloader instead of the generic EMI
// guess that crashes the DA's eMMC write engine.
func dumpPreloaderMTKSerial(cfg config) error {
	length := uint64(mtkPreloaderDumpLength)
	if v := strings.TrimSpace(os.Getenv("MVII_MTK_PRELOADER_DUMP_LENGTH")); v != "" {
		parsed, err := parseMTKNumber(v, "MVII_MTK_PRELOADER_DUMP_LENGTH")
		if err != nil {
			return err
		}
		length = parsed
	}
	if length == 0 || length%mtkSerialBlockSize != 0 {
		return fmt.Errorf("preloader dump length 0x%x must be a non-zero 512-byte multiple", length)
	}
	packetSize, err := mtkSerialPacketSize(cfg)
	if err != nil {
		return err
	}
	loaderPath, err := resolveMTKDALoader(cfg)
	if err != nil {
		return err
	}
	loader, err := parseMTKDALoader(loaderPath, mtkHWCodeMT6592, 0, 0)
	if err != nil {
		return err
	}
	outPath, err := resolveDumpPreloaderPath(cfg, loader.Path)
	if err != nil {
		return err
	}
	candidates, err := mtkDumpEMICandidates(cfg)
	if err != nil {
		return err
	}

	fmt.Printf("Dumping J36 Ultra preloader from eMMC boot1 (0x%x bytes) to %s\n", length, outPath)
	fmt.Println("MTK serial mode expects the J36 Ultra powered off or in PreLoader/BROM VCOM mode.")
	fmt.Println("If the handshake waits, hold the board's boot/download key combo while reconnecting USB.")
	fmt.Printf("Using DA loader: %s\n", loader.Path)
	fmt.Printf("Using MTK serial packet size: 0x%x\n", packetSize)

	client, err := bringUpMTKSerialDA(cfg, loader, candidates)
	if err != nil {
		return err
	}
	defer func() {
		_ = client.port.Close()
	}()

	data, err := client.readLegacyEMMC(mtkLegacyEMMCPartBoot1, 0, length, packetSize)
	if err != nil {
		return fmt.Errorf("read boot1 preloader: %w", err)
	}
	if err := os.MkdirAll(filepath.Dir(outPath), 0o755); err != nil {
		return err
	}
	if err := os.WriteFile(outPath, data, 0o644); err != nil {
		return err
	}
	fmt.Printf("Saved %d bytes of eMMC boot1 to %s\n", len(data), outPath)

	emi, err := extractMTKPreloaderEMI(outPath, data)
	if err != nil {
		fmt.Printf("Warning: saved boot1 dump but could not extract EMI metadata: %v\n", err)
		fmt.Println("Re-run with a larger MVII_MTK_PRELOADER_DUMP_LENGTH if the preloader spills past the dump window.")
		return nil
	}
	fmt.Printf("Extracted device preloader EMI: version=0x%x length=0x%x\n", emi.Version, len(emi.Data))
	fmt.Printf("Now flash with the real board EMI, e.g.:\n  ./flash -backend=mtk-serial -device %s -preloader %s -raw-offset <offset> <image>\n", cfg.device, outPath)
	return nil
}

// resolveDumpPreloaderPath turns the -mtk-dump-preloader value into a concrete
// output file. "auto" (or a directory) writes preloader_j36ultra.bin next to the
// DA loader so defaultMTKPreloaderCandidates() auto-discovers it on later runs.
func resolveDumpPreloaderPath(cfg config, loaderPath string) (string, error) {
	value := strings.TrimSpace(cfg.mtkDumpPreloader)
	if value == "" || strings.EqualFold(value, "auto") {
		return filepath.Join(filepath.Dir(loaderPath), "preloader_j36ultra.bin"), nil
	}
	abs, err := filepath.Abs(value)
	if err != nil {
		return "", err
	}
	if info, err := os.Stat(abs); err == nil && info.IsDir() {
		return filepath.Join(abs, "preloader_j36ultra.bin"), nil
	}
	return abs, nil
}

// validatePreloaderEMISignature warns when a device preloader was supplied whose
// embedded eMMC ID does not match the ID the DA just reported, since that
// mismatch is a common cause of the DA crashing mid-write.
func (c *mtkSerialClient) validatePreloaderEMISignature(dramInfo, reversed []byte) {
	emi := c.preloaderEMI
	if emi == nil || emi.UseDefault || emi.BuiltIn || len(emi.Data) < 0x20 || len(dramInfo) < 9 || len(reversed) < 9 {
		return
	}
	emiID := emi.Data[0x10:0x20] // v13-style EMI records carry the NAND/eMMC ID here.
	if isAllZero(emiID) {
		return
	}
	if bytes.Contains(emiID, dramInfo[:9]) || bytes.Contains(emiID, reversed[:9]) {
		fmt.Printf("Device eMMC ID matches preloader EMI record %s.\n", emi.Path)
		return
	}
	fmt.Printf("Warning: device eMMC ID %x is not present in preloader EMI %s (embedded ID %x); this preloader may be for a different board and can crash the DA on write.\n",
		dramInfo[:9], emi.Path, emiID)
}

func isAllZero(data []byte) bool {
	for _, b := range data {
		if b != 0 {
			return false
		}
	}
	return true
}

// writePreloaderMTKSerial brings up the DA and writes a preloader image to the
// eMMC BOOT1 hardware partition (offset 0). The eMMC boot partitions are a
// physically separate region accessed through a PARTITION_ACCESS switch, so this
// is a different write path than the user-area sdmmc_write_data that crashes on
// this board — it may succeed where that one dies.
func writePreloaderMTKSerial(cfg config, plPath string) error {
	data, err := os.ReadFile(plPath)
	if err != nil {
		return err
	}
	if len(data) == 0 {
		return fmt.Errorf("preloader image %s is empty", plPath)
	}
	if rem := len(data) % mtkSerialBlockSize; rem != 0 {
		data = append(data, make([]byte, mtkSerialBlockSize-rem)...)
	}
	length := uint64(len(data))
	packetSize, err := mtkSerialPacketSize(cfg)
	if err != nil {
		return err
	}
	loaderPath, err := resolveMTKDALoader(cfg)
	if err != nil {
		return err
	}
	loader, err := parseMTKDALoader(loaderPath, mtkHWCodeMT6592, 0, 0)
	if err != nil {
		return err
	}
	preloaderEMI, err := resolveMTKPreloaderEMI(cfg)
	if err != nil {
		return err
	}

	if err := confirmRawFlash(cfg, fmt.Sprintf("MediaTek BROM eMMC BOOT1 partition on %s", cfg.device), plPath, "0x0", fmt.Sprintf("0x%x", length)); err != nil {
		return err
	}

	fmt.Printf("Writing preloader %s (0x%x bytes) to eMMC BOOT1 partition offset 0\n", plPath, length)
	fmt.Printf("Using DA loader: %s\n", loader.Path)
	fmt.Printf("Using MTK serial packet size: 0x%x\n", packetSize)

	client, err := connectMTKSerial(cfg.device)
	if err != nil {
		return err
	}
	client.preloaderEMI = preloaderEMI
	defer func() {
		_ = client.port.Close()
	}()
	if err := client.probeMT6592(); err != nil {
		return err
	}
	if err := client.uploadLegacyDA(loader); err != nil {
		return err
	}
	if err := client.writeLegacyEMMCPartition(mtkLegacyEMMCPartBoot1, 0, data, packetSize); err != nil {
		return fmt.Errorf("write eMMC BOOT1 partition: %w", err)
	}
	fmt.Println("Preloader written to eMMC BOOT1. Power-cycle and check whether the board boots its own preloader (preloader VCOM / display) instead of BROM.")
	return nil
}

// writeLegacyEMMCPartition writes data to an arbitrary eMMC hardware partition
// (e.g. BOOT1) at a byte offset, mirroring writeLegacyEMMCRawData but with the
// partition selected explicitly and the payload supplied in memory.
func (c *mtkSerialClient) writeLegacyEMMCPartition(part byte, offset uint64, data []byte, packetSize int) error {
	length := uint64(len(data))
	if length == 0 || length%mtkSerialBlockSize != 0 {
		return fmt.Errorf("eMMC partition write length 0x%x must be a non-zero 512-byte multiple", length)
	}
	if err := c.switchLegacyEMMCPart(part); err != nil {
		return err
	}
	fmt.Printf("DA eMMC write header: storage=0x%x part=0x%x addr=0x%x length=0x%x packet=0x%x\n",
		mtkLegacyEMMCStorage, part, offset, length, packetSize)
	if err := c.writeLegacyFields(
		[]byte{mtkLegacySDMMCWriteData},
		[]byte{mtkLegacyEMMCStorage},
		[]byte{part},
		uint64Bytes(offset),
		uint64Bytes(length),
		uint32Bytes(uint32(packetSize)),
	); err != nil {
		return fmt.Errorf("send sdmmc_write_data header: %w", err)
	}
	ack, err := c.readByte(c.commandTimeout)
	if err != nil {
		return fmt.Errorf("read sdmmc_write_data header ACK: %w", err)
	}
	if ack != mtkLegacyACK {
		return fmt.Errorf("legacy DA rejected the eMMC write header: ACK=0x%02x", ack)
	}

	written := uint64(0)
	lastProgress := time.Time{}
	for written < length {
		count := uint64(packetSize)
		if remaining := length - written; remaining < count {
			count = remaining
		}
		chunk := data[written : written+count]
		if err := c.writeByte(mtkLegacyACK); err != nil {
			return err
		}
		if err := c.port.WriteAll(chunk, c.writeTimeout); err != nil {
			return fmt.Errorf("write eMMC chunk at 0x%x: %w", offset+written, err)
		}
		var checksumBytes [2]byte
		binary.BigEndian.PutUint16(checksumBytes[:], sum16(chunk))
		if err := c.writeRaw(checksumBytes[:]); err != nil {
			return fmt.Errorf("write eMMC chunk checksum at 0x%x: %w", offset+written, err)
		}
		resp, err := c.readByte(c.commandTimeout)
		if err != nil {
			return fmt.Errorf("read eMMC chunk ACK at 0x%x: %w", offset+written, err)
		}
		if resp != mtkLegacyCONT {
			return fmt.Errorf("eMMC chunk ACK at 0x%x = 0x%02x, want 0x%02x", offset+written, resp, mtkLegacyCONT)
		}
		written += count
		printRawBlockProgress(written, length, &lastProgress)
	}
	printRawBlockProgress(length, length, &lastProgress)
	return nil
}

// bootPreloaderMTKSerial loads the device's own preloader into SRAM via the BROM
// and jumps to it, so the board runs its normal boot chain (preloader -> DRAM ->
// LK -> fastboot) instead of dropping to BROM download mode. It writes nothing to
// eMMC, so the worst case is the board simply doesn't boot and you power-cycle
// back into BROM. Used to recover a board whose eMMC boot config is broken (or
// unwritable) but whose firmware is otherwise intact.
func bootPreloaderMTKSerial(cfg config) error {
	_, err := bootPreloaderMTKSerialWithDevice(cfg)
	return err
}

func bootPreloaderMTKSerialWithDevice(cfg config) (string, error) {
	plPath := strings.TrimSpace(cfg.preloader)
	if plPath == "" {
		for _, candidate := range defaultMTKPreloaderCandidates(cfg.root) {
			if fileExists(candidate) {
				plPath = candidate
				break
			}
		}
	}
	if plPath == "" {
		return "", errors.New("-mtk-boot-preloader needs -preloader /path/to/preloader_*.bin (or a preloader*.bin next to the DA loader)")
	}
	data, err := os.ReadFile(plPath)
	if err != nil {
		return "", err
	}
	loadAddr, code, err := parseMTKPreloaderImage(data)
	if err != nil {
		return "", fmt.Errorf("parse preloader %s: %w", plPath, err)
	}

	fmt.Printf("Booting device preloader from RAM: %s\n", plPath)
	fmt.Printf("Loading 0x%x bytes of preloader code to SRAM 0x%x, then jumping (no eMMC write)\n", len(code), loadAddr)
	fmt.Println("MTK serial mode expects the board powered off or in BROM/preloader VCOM mode.")

	client, err := connectMTKSerialForFeed(cfg.device)
	if err != nil {
		return "", err
	}
	defer func() {
		_ = client.port.Close()
	}()
	if err := client.disableMT6592Watchdog(); err != nil {
		fmt.Printf("Warning: could not disable MT6592 watchdog before preloader boot: %v\n", err)
	}
	if err := client.clearPreloaderBROMDownloadFlag(); err != nil {
		fmt.Printf("Warning: could not clear MT6592 USB-download flag before preloader boot: %v\n", err)
	}
	if err := client.sendDA(loadAddr, 0, code); err != nil {
		return "", fmt.Errorf("send preloader to SRAM 0x%x: %w", loadAddr, err)
	}
	if err := client.jumpDA(loadAddr); err != nil {
		// The preloader can take over USB the instant it runs, so the BROM's
		// post-jump status may not come back. Treat that as "jumped".
		fmt.Printf("JUMP_DA reported %v — the preloader likely took over already.\n", err)
	}
	fmt.Println("Jumped into the preloader. The board should now init DRAM, load LK, and continue booting.")
	fmt.Println("Watch the display, and check for a fastboot/adb device (and the BROM VCOM disappearing).")
	fmt.Println("If nothing happens after ~15s, the LK/boot image on eMMC is likely also unreadable — that points to an eMMC hardware fault (ISP/replacement), not software.")
	return client.device, nil
}

// parseMTKPreloaderImage locates the GFH_FILE_INFO header in a preloader image
// (raw boot1 dumps carry an EMMC_BOOT/BRLYT wrapper before it) and returns the
// SRAM jump address and the code to upload, mirroring mtkclient's parse_preloader:
// daaddr = load_addr + jump_offset, dadata = image[jump_offset:].
func parseMTKPreloaderImage(data []byte) (uint32, []byte, error) {
	gfh := bytes.Index(data, []byte{0x4D, 0x4D, 0x4D, 0x01}) // "MMM\x01" GFH magic
	if gfh < 0 {
		return 0, nil, errors.New("no GFH_FILE_INFO (MMM\\x01) magic found; not a recognizable MTK preloader image")
	}
	if gfh+0x34 > len(data) {
		return 0, nil, errors.New("preloader GFH header is truncated")
	}
	loadAddr := binary.LittleEndian.Uint32(data[gfh+0x1C : gfh+0x20])
	fileLen := binary.LittleEndian.Uint32(data[gfh+0x20 : gfh+0x24])
	jumpOffset := binary.LittleEndian.Uint32(data[gfh+0x30 : gfh+0x34])

	end := len(data)
	if fileLen != 0 && gfh+int(fileLen) <= len(data) {
		end = gfh + int(fileLen)
	}
	blob := data[gfh:end]
	if int(jumpOffset) >= len(blob) {
		return 0, nil, fmt.Errorf("preloader jump offset 0x%x exceeds image length 0x%x", jumpOffset, len(blob))
	}
	if loadAddr == 0 {
		return 0, nil, errors.New("preloader GFH load address is zero")
	}
	code := append([]byte(nil), blob[jumpOffset:]...)
	return loadAddr + jumpOffset, code, nil
}

func connectMTKSerial(device string) (*mtkSerialClient, error) {
	return connectMTKSerialWithOptions(device, mtkSerialConnectOptions{})
}

func connectMTKSerialWithOptions(device string, options mtkSerialConnectOptions) (*mtkSerialClient, error) {
	// Prefer the libusb raw-bulk transport on macOS: the kernel CDC-ACM tty
	// driver destabilises the MT6592 BROM (re-enumeration / "device not
	// configured" right after the handshake). Set MVII_MTK_TTY=1 to force the
	// legacy tty path.
	if client, attempted, err := tryConnectMTKUSB(device, options); attempted {
		return client, err
	}
	timeout := envDuration("MVII_MTK_SERIAL_HANDSHAKE_TIMEOUT", mtkSerialHandshakeTimeout)
	openHandshake := envDuration("MVII_MTK_SERIAL_OPEN_HANDSHAKE_TIMEOUT", mtkSerialOpenHandshake)
	deadline := time.Now().Add(timeout)
	devices := mtkSerialDeviceCandidates(device)
	fmt.Printf("Opening MTK serial device(s): %s (timeout %s)\n", strings.Join(devices, ", "), timeout)
	var lastErr error
	lastReport := time.Time{}
	for time.Now().Before(deadline) {
		devices = mtkSerialDeviceCandidates(device)
		for _, candidate := range devices {
			port, err := openMTKSerialPort(candidate, 115200)
			if err != nil {
				lastErr = fmt.Errorf("%s: %w", candidate, err)
				continue
			}
			_ = port.DiscardInput(10 * time.Millisecond)
			client := &mtkSerialClient{
				port:           port,
				device:         candidate,
				commandTimeout: envDuration("MVII_MTK_SERIAL_TIMEOUT", mtkSerialCommandTimeout),
				writeTimeout:   envDuration("MVII_MTK_SERIAL_WRITE_TIMEOUT", envDuration("MVII_MTK_USB_WRITE_TIMEOUT", mtkSerialWriteTimeout)),
			}
			fmt.Printf("Opened %s. Waiting for MTK BROM/preloader handshake", candidate)
			handshakeDeadline := time.Now().Add(openHandshake)
			if handshakeDeadline.After(deadline) {
				handshakeDeadline = deadline
			}
			if err := client.handshake(handshakeDeadline); err != nil {
				fmt.Println()
				if options.reuseFeedPayload {
					hello, recovered, probeErr := client.probeMTKFeedPayloadQuick()
					if recovered {
						client.feedPayloadReady = true
						client.feedPayloadHello = hello
						if hello.Name != "" {
							fmt.Printf("Existing MVIIFlash payload detected on %s: %s; reusing it.\n", candidate, hello.Name)
						} else {
							fmt.Printf("Existing MVIIFlash payload detected on %s; reusing it.\n", candidate)
						}
						return client, nil
					}
					if probeErr != nil {
						lastErr = fmt.Errorf("%s: %w; MVIIFlash reuse probe: %v", candidate, err, probeErr)
					} else {
						lastErr = fmt.Errorf("%s: %w", candidate, err)
					}
					_ = port.Close()
					continue
				}
				if options.recoverFeedPayload {
					recovered, resetErr := client.requestMTKFeedPayloadResetQuick()
					if recovered {
						_ = port.Close()
						if resetErr != nil {
							lastErr = fmt.Errorf("%s: MVIIFlash payload reset request failed: %w", candidate, resetErr)
						} else {
							lastErr = fmt.Errorf("%s: existing MVIIFlash payload reset requested", candidate)
						}
						time.Sleep(2500 * time.Millisecond)
						continue
					}
					if resetErr != nil {
						lastErr = fmt.Errorf("%s: %w; MVIIFlash recovery probe: %v", candidate, err, resetErr)
					} else {
						lastErr = fmt.Errorf("%s: %w", candidate, err)
					}
				} else {
					lastErr = fmt.Errorf("%s: %w", candidate, err)
				}
				_ = port.Close()
				continue
			}
			fmt.Println()
			fmt.Println("MTK serial handshake successful.")
			_ = client.port.DiscardInput(0)
			return client, nil
		}
		if now := time.Now(); now.Sub(lastReport) >= 2*time.Second {
			remaining := time.Until(deadline).Round(time.Second)
			if lastErr != nil {
				errs := lastErr.Error()
				if strings.Contains(errs, "no such file or directory") || strings.Contains(errs, "device not configured") {
					fmt.Printf("Waiting for MTK serial device (%s left)\n", remaining)
				} else {
					fmt.Printf("Waiting for MTK serial device (%s left): %v\n", remaining, lastErr)
				}
			} else {
				fmt.Printf("Waiting for MTK serial device (%s left)\n", remaining)
			}
			lastReport = now
		}
		if len(devices) == 0 {
			lastErr = errors.New("no serial device path specified")
			time.Sleep(200 * time.Millisecond)
			continue
		}
		time.Sleep(200 * time.Millisecond)
	}
	if lastErr == nil {
		lastErr = errors.New("no handshake response")
	}
	return nil, fmt.Errorf("MTK serial handshake timed out on %s: %w", strings.Join(devices, ", "), lastErr)
}

func mtkSerialDeviceCandidates(device string) []string {
	device = strings.TrimSpace(device)
	if device == "" {
		return nil
	}
	added := map[string]bool{}
	var devices []string
	add := func(value string) {
		if value != "" && !added[value] {
			added[value] = true
			devices = append(devices, value)
		}
	}
	add(device)
	if strings.HasPrefix(device, "/dev/cu.") {
		ttyTwin := "/dev/tty." + strings.TrimPrefix(device, "/dev/cu.")
		if fileExists(ttyTwin) {
			add(ttyTwin)
		}
	}
	if strings.HasPrefix(device, "/dev/tty.") {
		cuTwin := "/dev/cu." + strings.TrimPrefix(device, "/dev/tty.")
		if fileExists(cuTwin) {
			add(cuTwin)
		}
	}
	addGlob := func(pattern string) {
		matches, _ := filepath.Glob(pattern)
		sort.Strings(matches)
		for _, match := range matches {
			add(match)
		}
	}
	if strings.Contains(filepath.Base(device), "usbmodem") {
		// Only glob every usbmodem node when the provided device string does not
		// name a specific instance (e.g. /dev/cu.usbmodem141300). Specific IDs
		// must be honored exactly (plus the cu<->tty twin) so the tool does not
		// silently select a different attached VCOM. Renumbering after DA stage 1
		// or high-speed re-enum is handled by the reconnect* helpers which force
		// globs regardless of how specific the original name was.
		base := filepath.Base(device)
		if !hasSpecificUsbModemDigits(base) {
			addGlob("/dev/cu.usbmodem*")
			addGlob("/dev/tty.usbmodem*")
		}
	}
	return devices
}

func hasSpecificUsbModemDigits(name string) bool {
	i := strings.Index(name, "usbmodem")
	if i < 0 {
		return false
	}
	for _, r := range name[i+len("usbmodem"):] {
		if r >= '0' && r <= '9' {
			return true
		}
	}
	return false
}

func (c *mtkSerialClient) handshake(deadline time.Time) error {
	_ = c.port.DiscardInput(20 * time.Millisecond)
	start := []byte{0xA0, 0x0A, 0x50, 0x05}
	expect := []byte{0x5F, 0xF5, 0xAF, 0xFA}
	index := 0
	lastDot := time.Time{}
	for time.Now().Before(deadline) {
		if index == 0 {
			_ = c.port.DiscardInput(5 * time.Millisecond)
		}
		if err := c.port.WriteAll(start[index:index+1], 100*time.Millisecond); err != nil {
			index = 0
			time.Sleep(20 * time.Millisecond)
			continue
		}
		got, err := c.port.ReadExact(1, 20*time.Millisecond)
		if err == nil && len(got) == 1 && got[0] == expect[index] {
			index++
			if index == len(start) {
				_ = c.port.DiscardInput(0)
				return nil
			}
			continue
		}
		index = 0
		if now := time.Now(); now.Sub(lastDot) > time.Second {
			fmt.Print(".")
			lastDot = now
		}
		time.Sleep(5 * time.Millisecond)
	}
	return errors.New("no MTK handshake echo")
}

// probeMT6592 reads the BROM identity/config tuple. Some boards (notably the
// J36 Ultra on macOS) drop the USB VCOM the instant the BROM answers the
// handshake: the first command write then fails with ENXIO ("device not
// configured"). That is a USB re-enumeration, not a transient error, so the
// stale descriptor can never recover — we must reopen the (possibly renumbered)
// node and replay the handshake before retrying the probe.
func (c *mtkSerialClient) probeMT6592() error {
	const maxReconnect = 8
	for attempt := 0; ; attempt++ {
		err := c.probeMT6592Once()
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

func (c *mtkSerialClient) probeMT6592Once() error {
	hwCode, hwVer, err := c.getHWCode()
	if err != nil {
		return err
	}
	fmt.Printf("MTK HW code: 0x%04x, HW version: 0x%04x\n", hwCode, hwVer)
	if hwCode != mtkHWCodeMT6592 {
		return fmt.Errorf("connected MediaTek target is 0x%04x, expected MT6592/J36 Ultra (0x%04x)", hwCode, mtkHWCodeMT6592)
	}
	// Disabling the watchdog must succeed before anything else: on the J36 Ultra
	// the BROM watchdog fires every few seconds and resets the SoC, which
	// re-enumerates USB mid-session (the "device not configured" drops and the
	// climbing usbmodem unit number). Treat a failure here as fatal so the probe
	// wrapper reconnects to the freshly reset BROM and retries the disable inside
	// the next watchdog window, instead of limping on with the watchdog armed.
	if err := c.disableMT6592Watchdog(); err != nil {
		return fmt.Errorf("disable MT6592 watchdog: %w", err)
	}
	target, err := c.getTargetConfig()
	if err != nil {
		return err
	}
	fmt.Printf("Target config: 0x%08x (SBC=%t SLA=%t DAA=%t)\n", target.Raw, target.SBC, target.SLA, target.DAA)
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
		// The regular MT6592 path can return zeros here; this call is best-effort metadata.
	} else {
		fmt.Printf("Warning: could not read HW/SW version tuple: %v\n", err)
	}
	return nil
}

func (c *mtkSerialClient) setPreloaderBROMDownloadFlag() error {
	if c.isBROM {
		return nil
	}
	timeout := uint32(mtkUSBDLTimeoutMax << 2)
	timeout &= mtkUSBDLTimeoutMask
	usbdlReg := (mtkUSBDLMagic | timeout | mtkUSBDLBitEnable) &^ uint32(mtkUSBDLByPreloader)

	fmt.Printf("Setting preloader reset-to-BROM flag: USBDL 0x%08x\n", usbdlReg)
	if err := c.write32(mtkMT6592MiscLock, mtkMiscLockKeyMagic); err != nil {
		return fmt.Errorf("unlock MT6592 BOOT_MISC: %w", err)
	}
	resetControl := uint32(1)
	if current, err := c.read32(mtkMT6592ResetControl, 1); err == nil && len(current) != 0 {
		resetControl = current[0] | 1
	}
	if err := c.write32(mtkMT6592ResetControl, resetControl); err != nil {
		return fmt.Errorf("mark USBDL flag watchdog-resettable: %w", err)
	}
	if err := c.write32(mtkMT6592MiscLock, 0); err != nil {
		return fmt.Errorf("lock MT6592 BOOT_MISC: %w", err)
	}
	if err := c.write32(mtkMT6592USBDLFlag, usbdlReg); err != nil {
		return fmt.Errorf("write MT6592 USBDL flag: %w", err)
	}
	return nil
}

func (c *mtkSerialClient) clearPreloaderBROMDownloadFlag() error {
	fmt.Println("Clearing MT6592 USB-download flag before preloader RAM boot.")
	if err := c.write32(mtkMT6592MiscLock, mtkMiscLockKeyMagic); err != nil {
		return fmt.Errorf("unlock MT6592 BOOT_MISC: %w", err)
	}
	if err := c.write32(mtkMT6592ResetControl, 0); err != nil {
		return fmt.Errorf("clear MT6592 reset-control USB-download latch: %w", err)
	}
	if err := c.write32(mtkMT6592MiscLock, 0); err != nil {
		return fmt.Errorf("lock MT6592 BOOT_MISC: %w", err)
	}
	if err := c.write32(mtkMT6592USBDLFlag, 0); err != nil {
		return fmt.Errorf("clear MT6592 USBDL flag: %w", err)
	}
	return nil
}

func (c *mtkSerialClient) resetPreloaderToBROM() error {
	if c.isBROM {
		return nil
	}
	if err := c.setPreloaderBROMDownloadFlag(); err != nil {
		return err
	}
	fmt.Println("Triggering MT6592 watchdog reset toward BROM.")
	if err := c.write32(mtkMT6592WatchdogBase+0x08, mtkWatchdogRestart); err != nil {
		return fmt.Errorf("restart MT6592 watchdog before BROM reset: %w", err)
	}
	if err := c.write32(mtkMT6592WatchdogBase, mtkWatchdogRebootMode); err != nil {
		return fmt.Errorf("arm MT6592 watchdog reset mode: %w", err)
	}
	if err := c.write32(mtkMT6592WatchdogBase+0x14, mtkWatchdogSoftwareRst); err != nil {
		fmt.Printf("Warning: watchdog reset command did not fully ACK before USB dropped: %v\n", err)
	}
	return nil
}

func (c *mtkSerialClient) uploadLegacyDA(loader mtkDALoader) error {
	if len(loader.Regions) <= 2 {
		return fmt.Errorf("DA loader %s has %d regions; legacy MT6592 needs stage 1 and stage 2", loader.Path, len(loader.Regions))
	}
	stage1, err := readDARegion(loader, 1)
	if err != nil {
		return err
	}
	stage2, err := readDARegion(loader, 2)
	if err != nil {
		return err
	}
	stage1Region := loader.Regions[1]
	stage2Region := loader.Regions[2]

	fmt.Printf("Uploading legacy DA stage 1: addr=0x%x length=0x%x\n", stage1Region.StartAddr, len(stage1))
	if err := c.sendDA(stage1Region.StartAddr, stage1Region.SignatureLen, stage1); err != nil {
		return fmt.Errorf("upload DA stage 1: %w", err)
	}
	if err := c.jumpDA(stage1Region.StartAddr); err != nil {
		return fmt.Errorf("jump DA stage 1: %w", err)
	}
	sync, err := c.readByte(c.commandTimeout)
	if err != nil {
		return fmt.Errorf("read DA sync: %w", err)
	}
	if sync != mtkLegacySync {
		return fmt.Errorf("DA sync byte = 0x%02x, want 0x%02x", sync, mtkLegacySync)
	}
	fmt.Println("DA stage 1 sync received.")

	storage, err := c.readStage1StorageType()
	if err != nil {
		return err
	}
	fmt.Printf("DA stage 1 reports storage: %s\n", storage)
	if err := c.writeByte(mtkLegacyACK); err != nil {
		return err
	}
	if ack, err := c.readN(3, c.commandTimeout); err == nil {
		fmt.Printf("DA stage 1 ACK trailer: % x\n", ack)
	} else {
		return fmt.Errorf("read DA stage 1 ACK trailer: %w", err)
	}
	if err := c.setStage2Config(storage); err != nil {
		return err
	}

	fmt.Printf("Uploading legacy DA stage 2: addr=0x%x length=0x%x\n", stage2Region.StartAddr, len(stage2))
	if err := c.bromSendStage(stage2Region.StartAddr, stage2, 0x1000); err != nil {
		return fmt.Errorf("upload DA stage 2: %w", err)
	}
	info, err := c.readLegacyFlashInfo()
	if err != nil {
		return err
	}
	if info.Storage != "emmc" {
		return fmt.Errorf("DA stage 2 reports %s storage; the J36 Ultra raw writer expects eMMC", info.Storage)
	}
	c.flashInfo = info
	fmt.Printf("DA stage 2 connected: eMMC user area 0x%x (%s)\n", info.EMMCUserSize, formatBytes(info.EMMCUserSize))
	if err := c.prepareStage2Transport(); err != nil {
		return err
	}
	return nil
}

func (c *mtkSerialClient) readStage1StorageType() (string, error) {
	nandInfo, err := c.readUint32(c.commandTimeout)
	if err != nil {
		return "", fmt.Errorf("read NAND info marker: %w", err)
	}
	nandIDCount, err := c.readUint16(c.commandTimeout)
	if err != nil {
		return "", fmt.Errorf("read NAND id count: %w", err)
	}
	nandIDs := make([]uint16, 0, nandIDCount)
	for i := 0; i < int(nandIDCount); i++ {
		id, err := c.readUint16(c.commandTimeout)
		if err != nil {
			return "", fmt.Errorf("read NAND id %d: %w", i, err)
		}
		nandIDs = append(nandIDs, id)
	}
	emmcInfo, err := c.readUint32(c.commandTimeout)
	if err != nil {
		return "", fmt.Errorf("read eMMC info marker: %w", err)
	}
	emmcIDs := make([]uint32, 0, 4)
	for i := 0; i < 4; i++ {
		id, err := c.readUint32(c.commandTimeout)
		if err != nil {
			return "", fmt.Errorf("read eMMC id %d: %w", i, err)
		}
		emmcIDs = append(emmcIDs, id)
	}
	fmt.Printf("DA storage probe: NAND_INFO=0x%08x NAND IDs=%v EMMC_INFO=0x%08x EMMC IDs=%v\n", nandInfo, nandIDs, emmcInfo, emmcIDs)
	if len(nandIDs) > 0 && nandIDs[0] != 0 {
		return "nand", nil
	}
	if len(emmcIDs) > 0 && emmcIDs[0] != 0 {
		return "emmc", nil
	}
	return "nor", nil
}

func (c *mtkSerialClient) setStage2Config(storage string) error {
	bmtFlag := byte(1)
	bmtPartSize := uint32(0)
	if storage == "emmc" {
		bmtFlag = 1
		bmtPartSize = 0x1500000
	}
	payload := []byte{
		c.bromVersion,
		c.blVersion,
		0x00, 0x08, // NOR chip type.
		0x00,                   // NOR chip select.
		0x70, 0x07, 0xFF, 0xFF, // NAND acccon.
		bmtFlag,
	}
	// force_charge: 0x01=On (legacy default), 0x02=Auto. On a battery-less board fed
	// from a DC supply, forcing charge on can make the PMIC fight the supply, so
	// allow overriding it with MVII_MTK_FORCE_CHARGE (e.g. 2 for Auto, 0 for Off).
	forceCharge := byte(0x01)
	if v := strings.TrimSpace(os.Getenv("MVII_MTK_FORCE_CHARGE")); v != "" {
		parsed, err := parseMTKNumber(v, "MVII_MTK_FORCE_CHARGE")
		if err != nil {
			return err
		}
		forceCharge = byte(parsed)
	}
	fmt.Printf("DA stage 2 config: force_charge=0x%02x bmtflag=0x%02x bmtpartsize=0x%x\n", forceCharge, bmtFlag, bmtPartSize)
	payload = binary.BigEndian.AppendUint32(payload, bmtPartSize)
	payload = append(payload,
		forceCharge,
		0x01, // reset keys.
		0x02, // EXT_26M.
		0x00, // MSDC boot channel.
	)
	payload = binary.BigEndian.AppendUint32(payload, 0) // MT6592 is_gpt_solution.
	if err := c.writeRaw(payload); err != nil {
		return fmt.Errorf("send DA stage 2 config: %w", err)
	}
	time.Sleep(350 * time.Millisecond)
	errorCode, err := c.readUint32(c.commandTimeout)
	if err != nil {
		if isDeviceGoneError(err) {
			if recErr := c.tryReopenForOngoingDA(); recErr == nil {
				// Re-try the response read on the (possibly renumbered) live node.
				errorCode, err = c.readUint32(c.commandTimeout)
			}
		}
		if err != nil {
			return fmt.Errorf("read DA stage 2 config response: %w", err)
		}
	}
	switch errorCode {
	case 0:
		extra, err := c.readN(20, c.commandTimeout)
		if err != nil {
			return fmt.Errorf("read MT6592 stage 2 DRAM trailer: %w", err)
		}
		fmt.Printf("DA stage 2 config accepted, DRAM trailer: % x\n", extra)
		return nil
	case mtkLegacyNeedDRAMConfig:
		return c.handleStage2DRAMConfig()
	default:
		return fmt.Errorf("DA stage 2 config rejected with status 0x%x", errorCode)
	}
}

func (c *mtkSerialClient) handleStage2DRAMConfig() error {
	afterStatus, err := c.readUint32(c.commandTimeout)
	if err != nil {
		return fmt.Errorf("read DA DRAM request marker: %w", err)
	}
	dramInfo, err := c.readN(16, c.commandTimeout)
	if err != nil {
		return fmt.Errorf("read DA DRAM signature: %w", err)
	}
	reversed := reverseMTKWords(dramInfo)
	pdram0 := append([]byte(nil), dramInfo[:9]...)
	pdram1 := append([]byte(nil), reversed[:9]...)
	c.emmcID = append([]byte(nil), dramInfo...)
	fmt.Printf("DA stage 2 requested external DRAM/EMI config: marker=0x%08x dram=%x reversed=%x\n", afterStatus, dramInfo, reversed)

	c.validatePreloaderEMISignature(dramInfo, reversed)

	if c.preloaderEMI == nil {
		return fmt.Errorf("DA stage 2 requested external DRAM/EMI config; pass -preloader /path/to/preloader_*.bin, or pass -mtk-dram mt6592-standard to send the J36 Ultra MT6592 LPDDR2 default EMI record (DRAM signatures %x or %x)", pdram0, pdram1)
	}

	status, err := c.readUint32(c.commandTimeout)
	if err != nil {
		return fmt.Errorf("read DA DRAM info response: %w", err)
	}
	if status != mtkLegacyDRAMInfo {
		return fmt.Errorf("DA DRAM info response 0x%x, want 0x%x", status, mtkLegacyDRAMInfo)
	}
	nandIDCount, err := c.readUint16(c.commandTimeout)
	if err != nil {
		return fmt.Errorf("read DA DRAM NAND id count: %w", err)
	}
	if nandIDCount > 0x100 {
		return fmt.Errorf("DA DRAM NAND id count %d is unexpectedly large", nandIDCount)
	}
	if nandIDCount != 0 {
		if _, err := c.readN(int(nandIDCount)*2, c.commandTimeout); err != nil {
			return fmt.Errorf("read DA DRAM NAND ids: %w", err)
		}
	}
	return c.sendStage2EMIConfig()
}

func (c *mtkSerialClient) sendStage2EMIConfig() error {
	emi := c.preloaderEMI
	if emi == nil || (!emi.UseDefault && len(emi.Data) == 0) {
		return errors.New("DA requested external DRAM/EMI config, but no usable preloader EMI data is loaded")
	}
	emiData := append([]byte(nil), emi.Data...)
	if emi.UseDefault {
		fmt.Printf("Requesting DA built-in MT6592 DRAM defaults: version=0xffffffff\n")
	} else {
		fmt.Printf("Sending DA DRAM EMI config: version=0x%x length=0x%x\n", emi.Version, len(emiData))
	}

	if err := c.writeByte(mtkLegacyEnableDRAM); err != nil {
		return fmt.Errorf("send ENABLE_DRAM command: %w", err)
	}
	version := emi.Version
	if version == 0 || emi.UseDefault {
		version = 0xFFFFFFFF
	}
	if err := c.writeRaw(uint32Bytes(version)); err != nil {
		return fmt.Errorf("send EMI version: %w", err)
	}
	ack, err := c.readByte(c.commandTimeout)
	if err != nil {
		return fmt.Errorf("read ENABLE_DRAM ACK: %w", err)
	}
	if ack == mtkLegacyNACK {
		return errors.New("DA rejected the EMI config; make sure -preloader is the original device preloader for this board")
	}
	if ack != mtkLegacyACK {
		return fmt.Errorf("ENABLE_DRAM ACK=0x%02x, want 0x%02x", ack, mtkLegacyACK)
	}
	if emi.UseDefault {
		return c.triggerStage2DRAMConfig(emi.Version)
	}

	switch emi.Version {
	case 0x0F, 0x10, 0x11, 0x14, 0x15:
		dramLength, err := c.readUint32(c.commandTimeout)
		if err != nil {
			return fmt.Errorf("read EMI DRAM length: %w", err)
		}
		fmt.Printf("DA EMI RAM length request: 0x%x\n", dramLength)
		if err := c.writeByte(mtkLegacyACK); err != nil {
			return fmt.Errorf("ACK EMI RAM length: %w", err)
		}
		fmt.Println("DA EMI RAM length ACK sent.")
		if err := c.writeRaw(uint32Bytes(uint32(len(emiData)))); err != nil {
			return fmt.Errorf("send EMI blob length: %w", err)
		}
		fmt.Printf("DA EMI blob length sent: 0x%x\n", len(emiData))
	case 0x0A, 0x0B:
		info, err := c.readN(0x10, c.commandTimeout)
		if err != nil {
			return fmt.Errorf("read EMI RAM info: %w", err)
		}
		dramLength, err := c.readUint32(c.commandTimeout)
		if err != nil {
			return fmt.Errorf("read EMI RAM length: %w", err)
		}
		fmt.Printf("DA EMI RAM info: %x length=0x%x\n", info, dramLength)
		if err := c.writeByte(mtkLegacyACK); err != nil {
			return fmt.Errorf("ACK EMI RAM info: %w", err)
		}
		fmt.Println("DA EMI RAM info ACK sent.")
	case 0x0C, 0x0D:
		dramLength, err := c.readUint32(c.commandTimeout)
		if err != nil {
			return fmt.Errorf("read EMI DRAM length: %w", err)
		}
		fmt.Printf("DA EMI RAM length request: 0x%x\n", dramLength)
		if err := c.writeByte(mtkLegacyACK); err != nil {
			return fmt.Errorf("ACK EMI RAM length: %w", err)
		}
		fmt.Println("DA EMI RAM length ACK sent.")
		if dramLength < uint32(len(emiData)) {
			emiData = emiData[:int(dramLength)]
		}
		if len(emiData) >= 4 {
			patched := make([]byte, len(emiData))
			binary.BigEndian.PutUint32(patched[:4], 0x100)
			copy(patched[4:], emiData[4:])
			emiData = patched
		}
	case 0x00:
		dramLength, err := c.readUint32(c.commandTimeout)
		if err != nil {
			return fmt.Errorf("read EMI DRAM length: %w", err)
		}
		fmt.Printf("DA EMI RAM length request: 0x%x\n", dramLength)
		if err := c.writeByte(mtkLegacyACK); err != nil {
			return fmt.Errorf("ACK EMI RAM length: %w", err)
		}
		fmt.Println("DA EMI RAM length ACK sent.")
		if dramLength < uint32(len(emiData)) {
			emiData = emiData[:int(dramLength)]
		}
		if err := c.writeRaw(uint32Bytes(dramLength)); err != nil {
			return fmt.Errorf("send EMI DRAM length: %w", err)
		}
	default:
		fmt.Printf("Warning: unknown EMI version 0x%x; sending preloader EMI blob as-is\n", emi.Version)
	}

	fmt.Printf("Sending DA EMI blob payload: 0x%x bytes\n", len(emiData))
	if err := c.writeRaw(emiData); err != nil {
		return fmt.Errorf("send EMI blob: %w", err)
	}
	fmt.Println("DA EMI blob payload sent; waiting for checksum.")
	checksum, err := c.readUint16(c.commandTimeout)
	if err != nil {
		return fmt.Errorf("read EMI checksum: %w", err)
	}
	fmt.Printf("DA EMI checksum: 0x%04x\n", checksum)
	if err := c.writeByte(mtkLegacyACK); err != nil {
		return fmt.Errorf("ACK EMI checksum: %w", err)
	}
	fmt.Println("DA EMI checksum ACK sent; triggering DRAM init.")
	return c.triggerStage2DRAMConfig(emi.Version)
}

func (c *mtkSerialClient) triggerStage2DRAMConfig(emiVersion uint32) error {
	if err := c.writeRaw(uint32Bytes(0x80000001)); err != nil {
		return fmt.Errorf("send DRAM config trigger: %w", err)
	}
	ret, err := c.readUint32(c.commandTimeout)
	if err != nil {
		return fmt.Errorf("read DRAM init result: %w", err)
	}
	if ret != 0 {
		return fmt.Errorf("%w with status 0x%x", errMTKDRAMInit, ret)
	}
	ramType, err := c.readByte(c.commandTimeout)
	if err != nil {
		return fmt.Errorf("read DRAM type: %w", err)
	}
	chipSelect, err := c.readByte(c.commandTimeout)
	if err != nil {
		return fmt.Errorf("read DRAM chip select: %w", err)
	}
	ramSizeData, err := c.readN(8, c.commandTimeout)
	if err != nil {
		return fmt.Errorf("read DRAM size: %w", err)
	}
	ramSize := binary.BigEndian.Uint64(ramSizeData)
	fmt.Printf("DA DRAM initialized: type=0x%02x chip-select=0x%02x size=0x%x (%s)\n", ramType, chipSelect, ramSize, formatBytes(ramSize))
	if emiVersion == 0x0D {
		if _, err := c.readN(20, c.commandTimeout); err != nil {
			return fmt.Errorf("read EMI v0x0d DRAM trailer: %w", err)
		}
	}
	return nil
}

func (c *mtkSerialClient) bromSendStage(address uint32, data []byte, packetSize int) error {
	if packetSize <= 0 {
		return errors.New("invalid DA stage packet size")
	}
	header := make([]byte, 0, 12)
	header = binary.BigEndian.AppendUint32(header, address)
	header = binary.BigEndian.AppendUint32(header, uint32(len(data)))
	header = binary.BigEndian.AppendUint32(header, uint32(packetSize))
	if err := c.writeRaw(header); err != nil {
		return err
	}
	ack, err := c.readByte(c.commandTimeout)
	if err != nil {
		if isDeviceGoneError(err) {
			if recErr := c.tryReopenForOngoingDA(); recErr == nil {
				ack, err = c.readByte(c.commandTimeout)
			}
		}
		if err != nil {
			return fmt.Errorf("read stage header ACK: %w", err)
		}
	}
	if ack != mtkLegacyACK {
		return fmt.Errorf("stage header ACK = 0x%02x, want 0x%02x", ack, mtkLegacyACK)
	}
	for pos := 0; pos < len(data); pos += packetSize {
		end := pos + packetSize
		if end > len(data) {
			end = len(data)
		}
		if err := c.writeRaw(data[pos:end]); err != nil {
			return fmt.Errorf("write DA stage chunk at 0x%x: %w", address+uint32(pos), err)
		}
		ack, err := c.readByte(c.commandTimeout)
		if err != nil {
			if isDeviceGoneError(err) {
				if recErr := c.tryReopenForOngoingDA(); recErr == nil {
					ack, err = c.readByte(c.commandTimeout)
				}
			}
			if err != nil {
				return fmt.Errorf("read DA stage chunk ACK at 0x%x: %w", address+uint32(pos), err)
			}
		}
		if ack != mtkLegacyACK {
			return fmt.Errorf("DA stage chunk ACK at 0x%x = 0x%02x, want 0x%02x", address+uint32(pos), ack, mtkLegacyACK)
		}
	}
	time.Sleep(500 * time.Millisecond)
	if err := c.writeByte(mtkLegacyACK); err != nil {
		return err
	}
	ack, err = c.readByte(c.commandTimeout)
	if err != nil {
		if isDeviceGoneError(err) {
			if recErr := c.tryReopenForOngoingDA(); recErr == nil {
				ack, err = c.readByte(c.commandTimeout)
			}
		}
		if err != nil {
			return fmt.Errorf("read DA stage final ACK: %w", err)
		}
	}
	if ack != mtkLegacyACK {
		return fmt.Errorf("DA stage final ACK = 0x%02x, want 0x%02x", ack, mtkLegacyACK)
	}
	return nil
}

func (c *mtkSerialClient) readLegacyFlashInfo() (mtkLegacyFlashInfo, error) {
	if _, err := c.readN(0x1C, c.commandTimeout); err != nil {
		if isDeviceGoneError(err) {
			if recErr := c.tryReopenForOngoingDA(); recErr == nil {
				_, err = c.readN(0x1C, c.commandTimeout)
			}
		}
		if err != nil {
			return mtkLegacyFlashInfo{}, fmt.Errorf("read NOR flash info: %w", err)
		}
	}
	nandData, err := c.readN(0x11, c.commandTimeout)
	if err != nil {
		return mtkLegacyFlashInfo{}, fmt.Errorf("read NAND flash info: %w", err)
	}
	nandCount := uint16(0)
	if len(nandData) >= 17 {
		nandCount = binary.BigEndian.Uint16(nandData[15:17])
	}
	if nandCount == 0 && len(nandData) >= 13 {
		nandCount = binary.BigEndian.Uint16(nandData[11:13])
		if nandCount*2 > 4 {
			if _, err := c.readN(int(nandCount*2-4), c.commandTimeout); err != nil {
				return mtkLegacyFlashInfo{}, fmt.Errorf("read NAND32 device codes: %w", err)
			}
		}
	} else if nandCount > 0 {
		if _, err := c.readN(int(nandCount*2), c.commandTimeout); err != nil {
			return mtkLegacyFlashInfo{}, fmt.Errorf("read NAND64 device codes: %w", err)
		}
	}
	if _, err := c.readN(9, c.commandTimeout); err != nil {
		return mtkLegacyFlashInfo{}, fmt.Errorf("read NAND info2: %w", err)
	}
	emmcData, err := c.readN(0x5C, c.commandTimeout)
	if err != nil {
		return mtkLegacyFlashInfo{}, fmt.Errorf("read eMMC flash info: %w", err)
	}
	emmcUserSize := uint64(0)
	if len(emmcData) >= 68 {
		emmcUserSize = binary.BigEndian.Uint64(emmcData[60:68])
	}
	if _, err := c.readN(0x1C, c.commandTimeout); err != nil {
		return mtkLegacyFlashInfo{}, fmt.Errorf("read SD/MMC flash info: %w", err)
	}
	if _, err := c.readN(0x26, c.commandTimeout); err != nil {
		return mtkLegacyFlashInfo{}, fmt.Errorf("read DA config info: %w", err)
	}
	pass, err := c.readN(0x0A, c.commandTimeout)
	if err != nil {
		return mtkLegacyFlashInfo{}, fmt.Errorf("read DA pass info: %w", err)
	}
	if len(pass) < 10 {
		return mtkLegacyFlashInfo{}, io.ErrUnexpectedEOF
	}
	if pass[0] != mtkLegacyACK && binary.BigEndian.Uint32(pass[1:5])&0xFF != mtkLegacyACK {
		return mtkLegacyFlashInfo{}, fmt.Errorf("DA pass info did not ACK: % x", pass)
	}
	if pass[0] != mtkLegacyACK {
		_, _ = c.readByte(100 * time.Millisecond)
	}
	storage := "nor"
	if emmcUserSize != 0 {
		storage = "emmc"
	}
	return mtkLegacyFlashInfo{Storage: storage, FlashSize: emmcUserSize, EMMCUserSize: emmcUserSize}, nil
}

func (c *mtkSerialClient) writeLegacyEMMCRaw(image string, offset, length uint64, packetSize int) error {
	mode := strings.ToLower(strings.TrimSpace(os.Getenv("MVII_MTK_WRITE_MODE")))
	if mode == "" {
		mode = "data"
	}
	// Switch to the eMMC user partition before writing, by default. Our successful
	// boot1 read does sdmmc_switch_part first; the write embeds the partition byte
	// in its header but the MT6592 DA's write engine still faults unless a
	// partition is actively selected. Opt out with MVII_MTK_NO_SWITCH_PART.
	switchPart := !envFlag("MVII_MTK_NO_SWITCH_PART")
	switch strings.ReplaceAll(mode, "_", "-") {
	case "image", "write-image", "sdmmc-image", "sdmmc-write-image":
		return c.writeLegacyEMMCRawImage(image, offset, length, switchPart)
	case "data", "write-data", "sdmmc-data", "sdmmc-write-data":
		return c.writeLegacyEMMCRawData(image, offset, length, packetSize, switchPart)
	default:
		return fmt.Errorf("MVII_MTK_WRITE_MODE must be image or data, got %q", mode)
	}
}

func (c *mtkSerialClient) writeLegacyEMMCRawData(image string, offset, length uint64, packetSize int, switchPart bool) error {
	src, err := os.Open(image)
	if err != nil {
		return err
	}
	defer src.Close()
	if err := c.checkLegacyWriteBounds(offset, length); err != nil {
		return err
	}
	if envFlag("MVII_MTK_PROBE_READ") {
		if err := c.probeLegacyEMMCRead(offset, packetSize); err != nil {
			return fmt.Errorf("eMMC read probe: %w", err)
		}
	}
	if switchPart {
		if err := c.switchLegacyEMMCPart(mtkLegacyEMMCPartUser); err != nil {
			return err
		}
	}
	// The storage byte normally follows mtkclient (DaStorage EMMC = 0x01). Our
	// working read uses the hardware-storage code (0x02); MVII_MTK_WRITE_STORAGE=2
	// lets us test whether this DA build expects that for write-data too.
	storageByte := byte(mtkLegacyEMMCStorage)
	if v := strings.TrimSpace(os.Getenv("MVII_MTK_WRITE_STORAGE")); v != "" {
		parsed, err := parseMTKNumber(v, "MVII_MTK_WRITE_STORAGE")
		if err != nil {
			return err
		}
		storageByte = byte(parsed)
	}
	fmt.Printf("DA eMMC write header: storage=0x%x part=0x%x addr=0x%x length=0x%x packet=0x%x\n",
		storageByte, mtkLegacyEMMCPartUser, offset, length, packetSize)
	if err := c.writeLegacyFields(
		[]byte{mtkLegacySDMMCWriteData},
		[]byte{storageByte},
		[]byte{mtkLegacyEMMCPartUser},
		uint64Bytes(offset),
		uint64Bytes(length),
		uint32Bytes(uint32(packetSize)),
	); err != nil {
		return fmt.Errorf("send sdmmc_write_data header: %w", err)
	}
	fmt.Println("DA eMMC write-data header sent; waiting for ACK.")
	ack, err := c.readByte(c.commandTimeout)
	if err != nil {
		return fmt.Errorf("read sdmmc_write_data header ACK: %w", err)
	}
	if ack != mtkLegacyACK {
		return fmt.Errorf("legacy DA rejected the eMMC write header: ACK=0x%02x", ack)
	}

	buf := make([]byte, packetSize)
	written := uint64(0)
	lastProgress := time.Time{}
	for written < length {
		count := uint64(packetSize)
		if remaining := length - written; remaining < count {
			count = remaining
		}
		chunk := buf[:int(count)]
		if _, err := io.ReadFull(src, chunk); err != nil {
			return fmt.Errorf("read padded image chunk at 0x%x: %w", written, err)
		}
		if err := c.writeByte(mtkLegacyACK); err != nil {
			return err
		}
		if err := c.port.WriteAll(chunk, c.writeTimeout); err != nil {
			return fmt.Errorf("write eMMC chunk at target 0x%x: %w", offset+written, err)
		}
		checksum := sum16(chunk)
		var checksumBytes [2]byte
		binary.BigEndian.PutUint16(checksumBytes[:], checksum)
		if err := c.writeRaw(checksumBytes[:]); err != nil {
			return fmt.Errorf("write eMMC chunk checksum at target 0x%x: %w", offset+written, err)
		}
		resp, err := c.readByte(c.commandTimeout)
		if err != nil {
			return fmt.Errorf("read eMMC chunk ACK at target 0x%x: %w", offset+written, err)
		}
		if resp != mtkLegacyCONT {
			return fmt.Errorf("eMMC chunk ACK at target 0x%x = 0x%02x, want 0x%02x", offset+written, resp, mtkLegacyCONT)
		}
		written += count
		printRawBlockProgress(written, length, &lastProgress)
	}
	printRawBlockProgress(length, length, &lastProgress)
	return nil
}

func (c *mtkSerialClient) writeLegacyEMMCRawImage(image string, offset, length uint64, switchPart bool) error {
	src, err := os.Open(image)
	if err != nil {
		return err
	}
	defer src.Close()
	if err := c.checkLegacyWriteBounds(offset, length); err != nil {
		return err
	}
	if switchPart {
		if err := c.switchLegacyEMMCPart(mtkLegacyEMMCPartUser); err != nil {
			return err
		}
	}

	fmt.Printf("DA eMMC write-image header: part=0x%x addr=0x%x length=0x%x index=0x08/0x03\n",
		mtkLegacyEMMCPartUser, offset, length)
	if err := c.writeLegacyFields(
		[]byte{mtkLegacySDMMCWriteImage},
		[]byte{0x00}, // checksum level 0, matching legacy FlashTool/mtkclient behavior.
		[]byte{mtkLegacyEMMCPartUser},
		uint64Bytes(offset),
		uint64Bytes(length),
		[]byte{mtkLegacyEMMCPartUser}, // image index used by legacy EMMC user-area writes.
		[]byte{0x03},
	); err != nil {
		return fmt.Errorf("send sdmmc_write_image header: %w", err)
	}
	fmt.Println("DA eMMC write-image header sent; waiting for DA packet size.")
	daPacketSize, err := c.readUint32(c.commandTimeout)
	if err != nil {
		return fmt.Errorf("read sdmmc_write_image packet size: %w", err)
	}
	if daPacketSize < mtkSerialBlockSize || daPacketSize > mtkMaximumSerialPacket || daPacketSize%mtkSerialBlockSize != 0 {
		return fmt.Errorf("DA sdmmc_write_image packet size 0x%x is invalid", daPacketSize)
	}
	ack, err := c.readByte(c.commandTimeout)
	if err != nil {
		return fmt.Errorf("read sdmmc_write_image header ACK: %w", err)
	}
	if ack != mtkLegacyACK {
		return fmt.Errorf("legacy DA rejected the eMMC write-image header: ACK=0x%02x", ack)
	}
	if err := c.writeByte(mtkLegacyACK); err != nil {
		return fmt.Errorf("ACK sdmmc_write_image header: %w", err)
	}
	fmt.Printf("DA eMMC write-image packet size: 0x%x\n", daPacketSize)

	buf := make([]byte, int(daPacketSize))
	written := uint64(0)
	lastProgress := time.Time{}
	for written < length {
		count := uint64(daPacketSize)
		if remaining := length - written; remaining < count {
			count = remaining
		}
		chunk := buf[:int(count)]
		if _, err := io.ReadFull(src, chunk); err != nil {
			return fmt.Errorf("read padded image chunk at 0x%x: %w", written, err)
		}
		if err := c.port.WriteAll(chunk, c.writeTimeout); err != nil {
			return fmt.Errorf("write eMMC image chunk at target 0x%x: %w", offset+written, err)
		}
		written += count
		finalChunk := written == length
		checksum := sum16(chunk)
		var checksumBytes [2]byte
		binary.BigEndian.PutUint16(checksumBytes[:], checksum)
		if finalChunk {
			if err := c.writeRaw(checksumBytes[:]); err != nil {
				return fmt.Errorf("write final eMMC image checksum at target 0x%x: %w", offset+written-count, err)
			}
		}
		resp, err := c.readByte(c.commandTimeout)
		if err != nil {
			return fmt.Errorf("read eMMC image chunk response at target 0x%x: %w", offset+written-count, err)
		}
		if resp != mtkLegacyCONT {
			return fmt.Errorf("eMMC image chunk response at target 0x%x = 0x%02x, want 0x%02x", offset+written-count, resp, mtkLegacyCONT)
		}
		if finalChunk {
			if err := c.writeRaw(checksumBytes[:]); err != nil {
				return fmt.Errorf("write final eMMC image checksum confirm at target 0x%x: %w", offset+written-count, err)
			}
			ack, err := c.readByte(c.commandTimeout)
			if err != nil {
				return fmt.Errorf("read final eMMC image ACK: %w", err)
			}
			if ack != mtkLegacyACK {
				return fmt.Errorf("final eMMC image ACK = 0x%02x, want 0x%02x", ack, mtkLegacyACK)
			}
		} else if err := c.writeByte(mtkLegacyACK); err != nil {
			return fmt.Errorf("ACK eMMC image chunk at target 0x%x: %w", offset+written-count, err)
		}
		printRawBlockProgress(written, length, &lastProgress)
	}
	printRawBlockProgress(length, length, &lastProgress)
	return nil
}

func (c *mtkSerialClient) switchLegacyEMMCPart(part byte) error {
	fmt.Printf("DA eMMC switch part: part=0x%x\n", part)
	if err := c.writeByte(mtkLegacySDMMCSwitchPart); err != nil {
		return fmt.Errorf("send sdmmc_switch_part command: %w", err)
	}
	ack, err := c.readByte(c.commandTimeout)
	if err != nil {
		return fmt.Errorf("read sdmmc_switch_part command ACK: %w", err)
	}
	if ack != mtkLegacyACK {
		return fmt.Errorf("legacy DA rejected sdmmc_switch_part command: ACK=0x%02x", ack)
	}
	if err := c.writeByte(part); err != nil {
		return fmt.Errorf("send sdmmc_switch_part part: %w", err)
	}
	ack, err = c.readByte(c.commandTimeout)
	if err != nil {
		return fmt.Errorf("read sdmmc_switch_part part ACK: %w", err)
	}
	if ack != mtkLegacyACK {
		return fmt.Errorf("legacy DA rejected eMMC part 0x%x: ACK=0x%02x", part, ack)
	}
	fmt.Printf("DA eMMC part selected: 0x%x\n", part)
	return nil
}

func (c *mtkSerialClient) checkLegacyWriteBounds(offset, length uint64) error {
	if length == 0 {
		return errors.New("refusing to write zero bytes")
	}
	if offset%mtkSerialBlockSize != 0 || length%mtkSerialBlockSize != 0 {
		return fmt.Errorf("MTK legacy eMMC writes must be 512-byte aligned; offset=0x%x length=0x%x", offset, length)
	}
	if c.flashInfo.EMMCUserSize != 0 {
		if offset > c.flashInfo.EMMCUserSize || length > c.flashInfo.EMMCUserSize-offset {
			return fmt.Errorf("raw write range 0x%x..0x%x exceeds eMMC user area 0x%x", offset, offset+length, c.flashInfo.EMMCUserSize)
		}
	}
	return nil
}

// probeLegacyEMMCRead reads a single sector from the eMMC user area at the write
// target. It isolates whether the DA's storage engine works at all on this board:
// if the read succeeds the failure is write-specific, if the read EOFs too the
// whole storage engine (not just SDMMC_WRITE_DATA) is faulting.
func (c *mtkSerialClient) probeLegacyEMMCRead(offset uint64, packetSize int) error {
	fmt.Printf("Probing eMMC read of 0x%x bytes at 0x%x before writing...\n", mtkSerialBlockSize, offset)
	data, err := c.readLegacyEMMC(mtkLegacyEMMCPartUser, offset, mtkSerialBlockSize, packetSize)
	if err != nil {
		return err
	}
	fmt.Printf("eMMC read probe OK: first 16 bytes = % x\n", data[:min(16, len(data))])
	return nil
}

// readLegacyEMMC reads from an eMMC hardware partition using the legacy DA
// READ_CMD (0xD6) protocol: switch partition, send the read header, then pull
// each packet followed by its big-endian checksum and ACK it. It mirrors
// mtkclient's legacy readflash for eMMC.
func (c *mtkSerialClient) readLegacyEMMC(part byte, offset, length uint64, packetSize int) ([]byte, error) {
	if length == 0 {
		return nil, errors.New("refusing to read zero bytes")
	}
	if offset%mtkSerialBlockSize != 0 || length%mtkSerialBlockSize != 0 {
		return nil, fmt.Errorf("MTK legacy eMMC reads must be 512-byte aligned; offset=0x%x length=0x%x", offset, length)
	}
	if err := c.switchLegacyEMMCPart(part); err != nil {
		return nil, err
	}
	fmt.Printf("DA eMMC read header: part=0x%x addr=0x%x length=0x%x packet=0x%x\n", part, offset, length, packetSize)
	if err := c.writeLegacyFields(
		[]byte{mtkLegacyEMMCRead},
		[]byte{mtkLegacyHostLinux},
		[]byte{mtkLegacyHWStorageEMMC},
		uint64Bytes(offset),
		uint64Bytes(length),
		uint32Bytes(uint32(packetSize)),
	); err != nil {
		return nil, fmt.Errorf("send emmc read header: %w", err)
	}
	ack, err := c.readByte(c.commandTimeout)
	if err != nil {
		return nil, fmt.Errorf("read emmc read header ACK: %w", err)
	}
	if ack != mtkLegacyACK {
		_ = c.writeByte(mtkLegacyNACK)
		status, _ := c.readUint32(c.commandTimeout)
		return nil, fmt.Errorf("legacy DA rejected the eMMC read header: ACK=0x%02x status=0x%x", ack, status)
	}

	out := make([]byte, 0, length)
	lastProgress := time.Time{}
	for uint64(len(out)) < length {
		count := uint64(packetSize)
		if remaining := length - uint64(len(out)); remaining < count {
			count = remaining
		}
		chunk, err := c.readN(int(count), c.writeTimeout)
		if err != nil {
			return nil, fmt.Errorf("read emmc data at 0x%x: %w", offset+uint64(len(out)), err)
		}
		checksum, err := c.readUint16(c.commandTimeout)
		if err != nil {
			return nil, fmt.Errorf("read emmc checksum at 0x%x: %w", offset+uint64(len(out)), err)
		}
		if want := sum16(chunk); checksum != want {
			return nil, fmt.Errorf("emmc read checksum mismatch at 0x%x: got 0x%04x want 0x%04x (DRAM/EMI likely unstable; try -mtk-dram da-default)", offset+uint64(len(out)), checksum, want)
		}
		if err := c.writeByte(mtkLegacyACK); err != nil {
			return nil, fmt.Errorf("ACK emmc read packet at 0x%x: %w", offset+uint64(len(out)), err)
		}
		out = append(out, chunk...)
		printRawBlockProgress(uint64(len(out)), length, &lastProgress)
	}
	return out, nil
}

func (c *mtkSerialClient) getHWCode() (uint16, uint16, error) {
	for attempt := 0; attempt < 3; attempt++ {
		_ = c.port.DiscardInput(0)
		if err := c.echo([]byte{mtkCmdGetHWCode}); err != nil {
			if attempt == 2 {
				return 0, 0, fmt.Errorf("GET_HW_CODE: %w", err)
			}
			time.Sleep(20 * time.Millisecond)
			continue
		}
		value, err := c.readUint32(c.commandTimeout)
		if err != nil {
			if attempt == 2 {
				return 0, 0, fmt.Errorf("read HW code: %w", err)
			}
			time.Sleep(20 * time.Millisecond)
			continue
		}
		return uint16(value >> 16), uint16(value), nil
	}
	return 0, 0, errors.New("GET_HW_CODE failed after retries")
}

func (c *mtkSerialClient) getTargetConfig() (mtkTargetConfig, error) {
	if err := c.echo([]byte{mtkCmdGetTargetConfig}); err != nil {
		return mtkTargetConfig{}, fmt.Errorf("GET_TARGET_CONFIG: %w", err)
	}
	data, err := c.readN(6, c.commandTimeout)
	if err != nil {
		return mtkTargetConfig{}, fmt.Errorf("read target config: %w", err)
	}
	raw := binary.BigEndian.Uint32(data[:4])
	status := binary.BigEndian.Uint16(data[4:6])
	if status > 0xFF {
		return mtkTargetConfig{}, fmt.Errorf("GET_TARGET_CONFIG status 0x%x", status)
	}
	return mtkTargetConfig{
		Raw:      raw,
		SBC:      raw&0x1 != 0,
		SLA:      raw&0x2 != 0,
		DAA:      raw&0x4 != 0,
		EPP:      raw&0x8 != 0,
		Cert:     raw&0x10 != 0,
		MemRead:  raw&0x20 != 0,
		MemWrite: raw&0x40 != 0,
		CmdC8:    raw&0x80 != 0,
	}, nil
}

func (c *mtkSerialClient) getBLVersion() (byte, bool, error) {
	_ = c.port.DiscardInput(5 * time.Millisecond)
	if err := c.writeByte(mtkCmdGetBLVersion); err != nil {
		return 0, false, err
	}
	value, err := c.readByte(c.commandTimeout)
	if err != nil {
		return 0, false, err
	}
	return value, value == mtkCmdGetBLVersion, nil
}

func (c *mtkSerialClient) getBROMVersion() (byte, error) {
	_ = c.port.DiscardInput(5 * time.Millisecond)
	if err := c.writeByte(mtkCmdGetBROMVersion); err != nil {
		return 0, err
	}
	return c.readByte(c.commandTimeout)
}

func (c *mtkSerialClient) getHWSWVersion() (uint16, uint16, uint16, error) {
	// Best-effort metadata only, and the MT6592 BROM frequently never answers
	// this command. Bound it tightly so a non-response can't stall the probe for
	// the full command timeout.
	data, err := c.mtkCmdTimeout([]byte{mtkCmdGetHWSWVersion}, 8, 2*time.Second)
	if err != nil {
		return 0, 0, 0, err
	}
	return binary.BigEndian.Uint16(data[0:2]), binary.BigEndian.Uint16(data[2:4]), binary.BigEndian.Uint16(data[4:6]), nil
}

func (c *mtkSerialClient) disableMT6592Watchdog() error {
	if err := c.write32(0x10007000, 0x22000000); err != nil {
		return err
	}
	return c.write32(0x10000500, 0x22000000)
}

func (c *mtkSerialClient) read32(addr uint32, count uint32) ([]uint32, error) {
	if count == 0 {
		return nil, nil
	}
	if err := c.echo([]byte{mtkCmdRead32}); err != nil {
		return nil, err
	}
	if err := c.echo(uint32Bytes(addr)); err != nil {
		return nil, err
	}
	if err := c.echo(uint32Bytes(count)); err != nil {
		return nil, err
	}
	status, err := c.readUint16(c.commandTimeout)
	if err != nil {
		return nil, err
	}
	if status > 0xFF {
		return nil, fmt.Errorf("read32(0x%x) initial status 0x%x", addr, status)
	}
	values := make([]uint32, count)
	for i := uint32(0); i < count; i++ {
		value, err := c.readUint32(c.commandTimeout)
		if err != nil {
			return nil, fmt.Errorf("read32(0x%x) word %d: %w", addr, i, err)
		}
		values[i] = value
	}
	status, err = c.readUint16(c.commandTimeout)
	if err != nil {
		return nil, err
	}
	if status > 0xFF {
		return nil, fmt.Errorf("read32(0x%x) final status 0x%x", addr, status)
	}
	return values, nil
}

func (c *mtkSerialClient) write32(addr uint32, value uint32) error {
	if err := c.echo([]byte{mtkCmdWrite32}); err != nil {
		return err
	}
	if err := c.echo(uint32Bytes(addr)); err != nil {
		return err
	}
	if err := c.echo(uint32Bytes(1)); err != nil {
		return err
	}
	status, err := c.readUint16(c.commandTimeout)
	if err != nil {
		return err
	}
	if status > 0xFF {
		return fmt.Errorf("write32(0x%x) initial status 0x%x", addr, status)
	}
	if err := c.echo(uint32Bytes(value)); err != nil {
		return err
	}
	status, err = c.readUint16(c.commandTimeout)
	if err != nil {
		return err
	}
	if status > 0xFF {
		return fmt.Errorf("write32(0x%x) final status 0x%x", addr, status)
	}
	return nil
}

func (c *mtkSerialClient) sendDA(address, sigLen uint32, data []byte) error {
	return c.sendDAWithSignature(address, sigLen, data)
}

func (c *mtkSerialClient) sendDAWithSignature(address, sigLen uint32, data []byte) error {
	checksum, payload := prepareDAData(data)
	if err := c.echo([]byte{mtkCmdSendDA}); err != nil {
		return err
	}
	if err := c.echo(uint32Bytes(address)); err != nil {
		return err
	}
	if err := c.echo(uint32Bytes(uint32(len(payload)))); err != nil {
		return err
	}
	if err := c.echo(uint32Bytes(sigLen)); err != nil {
		return err
	}
	status, err := c.readUint16(c.commandTimeout)
	if err != nil {
		return err
	}
	if status == 0x1D0D {
		return errors.New("target requires SLA authentication before accepting the DA")
	}
	if status == 0x1D12 {
		return errors.New("SEND_DA status 0x1d12 (BROM rejected the DA load parameters; check that the payload is linked and loaded into the MT6592 DA SRAM window)")
	}
	if status > 0xFF {
		return fmt.Errorf("SEND_DA status 0x%x", status)
	}
	if err := c.uploadData(payload, checksum); err != nil {
		return err
	}
	return nil
}

func (c *mtkSerialClient) uploadData(data []byte, expectedChecksum uint16) error {
	const chunkSize = 0x400
	for pos := 0; pos < len(data); pos += chunkSize {
		end := pos + chunkSize
		if end > len(data) {
			end = len(data)
		}
		if err := c.port.WriteAll(data[pos:end], c.writeTimeout); err != nil {
			return err
		}
	}
	time.Sleep(120 * time.Millisecond)
	checksum, err := c.readUint16(c.commandTimeout)
	if err != nil {
		return fmt.Errorf("read upload checksum: %w", err)
	}
	status, err := c.readUint16(c.commandTimeout)
	if err != nil {
		return fmt.Errorf("read upload status: %w", err)
	}
	if checksum != expectedChecksum && checksum != 0 {
		fmt.Printf("Warning: DA upload checksum 0x%04x did not match host checksum 0x%04x\n", checksum, expectedChecksum)
	}
	if status > 0xFF {
		return fmt.Errorf("DA upload status 0x%x", status)
	}
	return nil
}

func (c *mtkSerialClient) jumpDA(address uint32) error {
	if err := c.echo([]byte{mtkCmdJumpDA}); err != nil {
		return err
	}
	if err := c.writeRaw(uint32Bytes(address)); err != nil {
		return err
	}
	resp, err := c.readUint32(c.commandTimeout)
	if err != nil {
		return fmt.Errorf("read JUMP_DA address response: %w", err)
	}
	if resp != address {
		return fmt.Errorf("JUMP_DA address response 0x%x, want 0x%x", resp, address)
	}
	status, err := c.readUint16(c.commandTimeout)
	if err != nil {
		return fmt.Errorf("read JUMP_DA status: %w", err)
	}
	if status != 0 {
		return fmt.Errorf("JUMP_DA status 0x%x", status)
	}
	time.Sleep(100 * time.Millisecond)
	return nil
}

func (c *mtkSerialClient) prepareStage2Transport() error {
	speed, err := c.checkUSBStatus()
	if err != nil {
		return fmt.Errorf("DA USB status check failed: %w", err)
	}
	fmt.Printf("DA USB speed status: 0x%02x\n", speed)
	if speed != 0 {
		return nil
	}
	// Like mtkclient (reconnect defaults to True), move the DA onto its
	// high-speed bulk endpoint and reconnect. The legacy MT6592 DA re-enumerates
	// USB the moment a storage write begins; staying on the BROM VCOM makes that
	// first write tear down the stale node and the next read returns EOF.
	if envFlag("MVII_MTK_STAGE2_NO_RECONNECT") {
		fmt.Println("DA reports USB speed 0x00; MVII_MTK_STAGE2_NO_RECONNECT set, staying on the current serial VCOM transport (writes may EOF).")
		return nil
	}

	fmt.Println("DA reports USB speed 0x00; performing USB high-speed setup and reconnecting to stage 2")
	if err := c.setupUSBHighSpeed(); err != nil {
		return fmt.Errorf("request DA USB high-speed setup: %w", err)
	}
	oldPort := c.port
	if oldPort != nil {
		_ = oldPort.Close()
	}
	time.Sleep(time.Second)
	fmt.Println()
	fmt.Println("======================================================================")
	fmt.Println("The DA re-enumerated USB at high speed. macOS will not recreate the")
	fmt.Printf("serial node on the same port by itself, so reconnect the cable now:\n")
	fmt.Println("  1. Unplug the J36 Ultra USB cable.")
	fmt.Println("  2. Plug it into a DIFFERENT USB port.")
	fmt.Println("  3. Plug it back into the original port.")
	fmt.Println("Keep the board powered throughout (do not let it reset); the DA must")
	fmt.Println("stay resident. The flash continues automatically once the node returns.")
	fmt.Println("======================================================================")
	fmt.Println()
	reconnected, err := connectMTKSerialStage2(c.device, c.commandTimeout, c.writeTimeout)
	if err != nil {
		return err
	}
	c.port = reconnected.port
	c.device = reconnected.device
	speed, err = c.checkUSBStatus()
	if err != nil {
		return fmt.Errorf("DA USB status check after reconnect failed: %w", err)
	}
	fmt.Printf("DA stage 2 reconnected on %s, USB speed status: 0x%02x\n", c.device, speed)
	return nil
}

func (c *mtkSerialClient) checkUSBStatus() (byte, error) {
	return c.checkUSBStatusTimeout(c.commandTimeout)
}

func (c *mtkSerialClient) checkUSBStatusTimeout(timeout time.Duration) (byte, error) {
	if err := c.writeByte(mtkLegacyUSBCheckStatus); err != nil {
		if isDeviceGoneError(err) {
			if recErr := c.tryReopenForOngoingDA(); recErr == nil {
				err = c.writeByte(mtkLegacyUSBCheckStatus)
			}
		}
		if err != nil {
			return 0, err
		}
	}
	ack, err := c.readByte(timeout)
	if err != nil {
		if isDeviceGoneError(err) {
			if recErr := c.tryReopenForOngoingDA(); recErr == nil {
				ack, err = c.readByte(timeout)
			}
		}
		if err != nil {
			return 0, err
		}
	}
	if ack != mtkLegacyACK {
		return 0, fmt.Errorf("USB_CHECK_STATUS ACK=0x%02x", ack)
	}
	b, err := c.readByte(timeout)
	if err != nil {
		if isDeviceGoneError(err) {
			if recErr := c.tryReopenForOngoingDA(); recErr == nil {
				b, err = c.readByte(timeout)
			}
		}
		if err != nil {
			return 0, err
		}
	}
	return b, nil
}

func (c *mtkSerialClient) setupUSBHighSpeed() error {
	if err := c.writeByte(mtkLegacyUSBSetupPort); err != nil {
		return err
	}
	if err := c.writeByte(0x01); err != nil {
		return err
	}
	ack, err := c.readByte(5 * time.Second)
	if err != nil {
		fmt.Printf("Warning: DA did not ACK USB high-speed setup before reconnect: %v\n", err)
		return nil
	}
	if ack != mtkLegacyACK {
		return fmt.Errorf("USB high-speed setup ACK=0x%02x", ack)
	}
	return nil
}

func connectMTKSerialStage2(device string, commandTimeout, writeTimeout time.Duration) (*mtkSerialClient, error) {
	timeout := envDuration("MVII_MTK_SERIAL_RECONNECT_TIMEOUT", mtkSerialReconnectTimeout)
	deadline := time.Now().Add(timeout)
	fmt.Printf("Reopening MTK stage 2 serial device (timeout %s)\n", timeout)
	var lastErr error
	lastReport := time.Time{}
	for time.Now().Before(deadline) {
		// Re-scan every pass: after a physical replug the re-enumerated device may
		// appear under a different /dev/cu.usbmodem* name than it had before.
		for _, candidate := range mtkSerialReconnectCandidates(device) {
			port, err := openMTKSerialPort(candidate, 115200)
			if err != nil {
				lastErr = fmt.Errorf("%s: %w", candidate, err)
				continue
			}
			client := &mtkSerialClient{
				port:           port,
				device:         candidate,
				commandTimeout: commandTimeout,
				writeTimeout:   writeTimeout,
			}
			if _, err := client.checkUSBStatusTimeout(2 * time.Second); err != nil {
				_ = port.Close()
				lastErr = fmt.Errorf("%s: %w", candidate, err)
				continue
			}
			fmt.Printf("Reconnected to MTK stage 2 DA on %s.\n", candidate)
			return client, nil
		}
		if now := time.Now(); now.Sub(lastReport) >= 3*time.Second {
			remaining := time.Until(deadline).Round(time.Second)
			fmt.Printf("Waiting for the J36 Ultra serial node to return (%s left) — unplug, try another USB port, then plug back in.\n", remaining)
			lastReport = now
		}
		time.Sleep(200 * time.Millisecond)
	}
	if lastErr == nil {
		lastErr = errors.New("no stage 2 USB status response")
	}
	return nil, fmt.Errorf("MTK stage 2 serial reconnect timed out (last error: %w); keep the board powered and replug the USB cable", lastErr)
}

func mtkSerialReconnectCandidates(device string) []string {
	devices := mtkSerialDeviceCandidates(device)
	added := map[string]bool{}
	for _, candidate := range devices {
		added[candidate] = true
	}
	addGlob := func(pattern string) {
		matches, _ := filepath.Glob(pattern)
		sort.Strings(matches)
		for _, match := range matches {
			if !added[match] {
				added[match] = true
				devices = append(devices, match)
			}
		}
	}
	if strings.Contains(device, "usbmodem") {
		addGlob("/dev/cu.usbmodem*")
		addGlob("/dev/tty.usbmodem*")
	}
	return devices
}

// tryReopenForOngoingDA attempts to find a live serial node (using the
// renumber-aware glob) after the DA stage 1/2 has taken over and invalidated
// the previous open descriptor. It does not perform a BROM handshake; the
// caller resumes the DA protocol bytes on the new port.
func (c *mtkSerialClient) tryReopenForOngoingDA() error {
	orig := c.device
	if c.port != nil {
		_ = c.port.Close()
		c.port = nil
	}
	time.Sleep(100 * time.Millisecond)
	for _, cand := range mtkSerialReconnectCandidates(orig) {
		p, err := openMTKSerialPort(cand, 115200)
		if err != nil {
			continue
		}
		c.port = p
		c.device = cand
		_ = c.port.DiscardInput(5 * time.Millisecond)
		fmt.Printf("Reopened ongoing DA session on %s (recovered from device drop)\n", cand)
		return nil
	}
	return errors.New("no DA-stage serial candidate could be opened")
}

// reopenBROMHandshake recovers from a USB re-enumeration that drops the BROM
// VCOM right after the initial handshake (macOS surfaces this as ENXIO /
// "device not configured" on the first command write). It closes the stale
// descriptor, lets the device settle, reopens the (possibly renumbered) serial
// node and replays the BROM handshake. It deliberately does NOT probe, so it is
// safe to call from inside the probe sequence without recursing.
func (c *mtkSerialClient) reopenBROMHandshake() error {
	orig := c.device
	if c.port != nil {
		_ = c.port.Close()
		c.port = nil
	}
	// Give the host USB stack time to tear down and re-enumerate the device.
	time.Sleep(250 * time.Millisecond)
	timeout := envDuration("MVII_MTK_BROM_RECONNECT_TIMEOUT",
		envDuration("MVII_MTK_SERIAL_RECONNECT_TIMEOUT", mtkSerialReconnectTimeout))
	deadline := time.Now().Add(timeout)
	var lastErr error
	for time.Now().Before(deadline) {
		for _, cand := range mtkSerialReconnectCandidates(orig) {
			port, err := openMTKSerialPort(cand, 115200)
			if err != nil {
				lastErr = err
				continue
			}
			_ = port.DiscardInput(10 * time.Millisecond)
			c.port = port
			c.device = cand
			handshakeDeadline := time.Now().Add(3 * time.Second)
			if handshakeDeadline.After(deadline) {
				handshakeDeadline = deadline
			}
			if err := c.handshake(handshakeDeadline); err != nil {
				_ = port.Close()
				c.port = nil
				lastErr = err
				continue
			}
			_ = c.port.DiscardInput(0)
			fmt.Printf("Re-established MTK BROM handshake on %s after device drop.\n", cand)
			return nil
		}
		time.Sleep(200 * time.Millisecond)
	}
	if lastErr == nil {
		lastErr = errors.New("no BROM serial candidate could be reopened")
	}
	return fmt.Errorf("reopen BROM after device drop: %w", lastErr)
}

func isDeviceGoneError(err error) bool {
	if err == nil {
		return false
	}
	msg := strings.ToLower(err.Error())
	return strings.Contains(msg, "device not configured") ||
		strings.Contains(msg, "no such file or directory") ||
		strings.Contains(msg, "input/output error") ||
		strings.Contains(msg, "bad file descriptor") ||
		strings.Contains(msg, "closed network connection")
}

func (c *mtkSerialClient) mtkCmd(command []byte, bytesToRead int) ([]byte, error) {
	return c.mtkCmdTimeout(command, bytesToRead, c.commandTimeout)
}

func (c *mtkSerialClient) mtkCmdTimeout(command []byte, bytesToRead int, timeout time.Duration) ([]byte, error) {
	_ = c.port.DiscardInput(5 * time.Millisecond)
	if err := c.writeRaw(command); err != nil {
		return nil, err
	}
	echo, err := c.readN(len(command), timeout)
	if err != nil {
		return nil, err
	}
	if !bytes.Equal(echo, command) {
		return nil, fmt.Errorf("command echo % x, want % x", echo, command)
	}
	if bytesToRead == 0 {
		return nil, nil
	}
	return c.readN(bytesToRead, timeout)
}

func (c *mtkSerialClient) echo(data []byte) error {
	_ = c.port.DiscardInput(0)
	if err := c.writeRaw(data); err != nil {
		return err
	}
	echo, err := c.readN(len(data), c.commandTimeout)
	if err != nil {
		return err
	}
	if !bytes.Equal(echo, data) {
		return fmt.Errorf("echo mismatch: wrote % x, read % x", data, echo)
	}
	return nil
}

func (c *mtkSerialClient) writeRaw(data []byte) error {
	c.writeMu.Lock()
	defer c.writeMu.Unlock()
	return c.port.WriteAll(data, c.writeTimeout)
}

func (c *mtkSerialClient) writeByte(value byte) error {
	return c.writeRaw([]byte{value})
}

func (c *mtkSerialClient) readN(n int, timeout time.Duration) ([]byte, error) {
	return c.port.ReadExact(n, timeout)
}

func (c *mtkSerialClient) readByte(timeout time.Duration) (byte, error) {
	data, err := c.readN(1, timeout)
	if err != nil {
		return 0, err
	}
	return data[0], nil
}

func (c *mtkSerialClient) readUint16(timeout time.Duration) (uint16, error) {
	data, err := c.readN(2, timeout)
	if err != nil {
		return 0, err
	}
	return binary.BigEndian.Uint16(data), nil
}

func (c *mtkSerialClient) readUint32(timeout time.Duration) (uint32, error) {
	data, err := c.readN(4, timeout)
	if err != nil {
		return 0, err
	}
	return binary.BigEndian.Uint32(data), nil
}

func parseMTKDALoader(path string, hwCode uint16, hwVersion uint16, swVersion uint16) (mtkDALoader, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return mtkDALoader{}, err
	}
	if len(data) < 0x6C {
		return mtkDALoader{}, fmt.Errorf("%s is too small to be a MediaTek DA loader", path)
	}
	count := binary.LittleEndian.Uint32(data[0x68:0x6C])
	if count == 0 || count > 4096 {
		return mtkDALoader{}, fmt.Errorf("%s has invalid DA entry count %d", path, count)
	}
	v6 := bytes.Contains(data[:0x68], []byte("MTK_DA_v6"))
	recordSize := 0xDC
	oldLoader := false
	if len(data) >= 0x6C+0xD8+2 && bytes.Equal(data[0x6C+0xD8:0x6C+0xD8+2], []byte{0xDA, 0xDA}) {
		recordSize = 0xD8
		oldLoader = true
	}

	var matches []mtkDALoader
	for i := uint32(0); i < count; i++ {
		pos := 0x6C + int(i)*recordSize
		if pos+recordSize > len(data) {
			break
		}
		entry, ok := parseDAEntry(path, data[pos:pos+recordSize], oldLoader, v6)
		if !ok || entry.HWCode != hwCode {
			continue
		}
		if hwVersion != 0 && entry.HWVersion > hwVersion {
			continue
		}
		if swVersion != 0 && entry.SWVersion > swVersion {
			continue
		}
		matches = append(matches, entry)
	}
	if len(matches) == 0 {
		return mtkDALoader{}, fmt.Errorf("%s has no DA entry for hw code 0x%04x", path, hwCode)
	}
	sort.SliceStable(matches, func(i, j int) bool {
		if matches[i].HWVersion != matches[j].HWVersion {
			return matches[i].HWVersion > matches[j].HWVersion
		}
		return matches[i].SWVersion > matches[j].SWVersion
	})
	for _, match := range matches {
		if len(match.Regions) > 2 {
			if _, err := readDARegion(match, 1); err == nil {
				if _, err := readDARegion(match, 2); err == nil {
					return match, nil
				}
			}
		}
	}
	return matches[0], nil
}

func parseDAEntry(path string, data []byte, oldLoader bool, v6 bool) (mtkDALoader, bool) {
	if len(data) < 20 {
		return mtkDALoader{}, false
	}
	magic := binary.LittleEndian.Uint16(data[0:2])
	if magic != 0xDADA {
		return mtkDALoader{}, false
	}
	entry := mtkDALoader{
		Path:      path,
		V6:        v6,
		Old:       oldLoader,
		HWCode:    binary.LittleEndian.Uint16(data[2:4]),
		HWSubCode: binary.LittleEndian.Uint16(data[4:6]),
		HWVersion: binary.LittleEndian.Uint16(data[6:8]),
	}
	cursor := 8
	if !oldLoader {
		entry.SWVersion = binary.LittleEndian.Uint16(data[cursor : cursor+2])
		cursor += 4 // sw_version + reserved1
	}
	if cursor+8 > len(data) {
		return mtkDALoader{}, false
	}
	entry.PageSize = binary.LittleEndian.Uint16(data[cursor : cursor+2])
	cursor += 4 // pagesize + reserved3
	entry.EntryRegionIndex = binary.LittleEndian.Uint16(data[cursor : cursor+2])
	cursor += 2
	regionCount := binary.LittleEndian.Uint16(data[cursor : cursor+2])
	cursor += 2
	for i := 0; i < int(regionCount); i++ {
		if cursor+20 > len(data) {
			return mtkDALoader{}, false
		}
		entry.Regions = append(entry.Regions, mtkDARegion{
			BufferOffset: binary.LittleEndian.Uint32(data[cursor : cursor+4]),
			Length:       binary.LittleEndian.Uint32(data[cursor+4 : cursor+8]),
			StartAddr:    binary.LittleEndian.Uint32(data[cursor+8 : cursor+12]),
			StartOffset:  binary.LittleEndian.Uint32(data[cursor+12 : cursor+16]),
			SignatureLen: binary.LittleEndian.Uint32(data[cursor+16 : cursor+20]),
		})
		cursor += 20
	}
	return entry, true
}

func readDARegion(loader mtkDALoader, index int) ([]byte, error) {
	if index < 0 || index >= len(loader.Regions) {
		return nil, fmt.Errorf("DA region %d is missing", index)
	}
	region := loader.Regions[index]
	data, err := os.ReadFile(loader.Path)
	if err != nil {
		return nil, err
	}
	start := uint64(region.BufferOffset)
	end := start + uint64(region.Length)
	if end < start || end > uint64(len(data)) {
		return nil, fmt.Errorf("DA region %d points outside %s: offset=0x%x length=0x%x", index, loader.Path, region.BufferOffset, region.Length)
	}
	out := make([]byte, region.Length)
	copy(out, data[start:end])
	return out, nil
}

func resolveMTKDALoader(cfg config) (string, error) {
	if cfg.daLoader != "" {
		if !fileExists(cfg.daLoader) {
			return "", fmt.Errorf("-da-loader %s does not exist", cfg.daLoader)
		}
		return cfg.daLoader, nil
	}
	names := []string{"MTK_AllInOne_DA_mt6590.bin", "MTK_DA_V5.bin"}
	var candidates []string
	for _, name := range names {
		candidates = append(candidates,
			filepath.Join(cfg.root, name),
			filepath.Join(cfg.root, "tools", "mtk-da", name),
			filepath.Join(cfg.root, "tools", "mtkclient", "mtkclient", "Loader", name),
			filepath.Join(cfg.root, "mtkclient", "mtkclient", "Loader", name),
		)
		candidates = append(candidates, findUpwardCandidates(cfg.root, filepath.Join("Hardware", "Virtua", "mtkclient", "mtkclient", "Loader", name))...)
	}
	for _, candidate := range candidates {
		if fileExists(candidate) {
			return candidate, nil
		}
	}
	return "", errors.New("-backend=mtk-serial needs a MediaTek DA loader; pass -da-loader /path/to/MTK_AllInOne_DA_mt6590.bin")
}

func resolveMTKPreloaderEMI(cfg config) (*mtkPreloaderEMI, error) {
	dramMode := strings.TrimSpace(cfg.mtkDRAM)
	if dramMode == "" {
		dramMode = strings.TrimSpace(os.Getenv("MVII_MTK_DRAM"))
	}
	if dramMode == "" {
		dramMode = "auto"
	}
	dramMode = strings.ToLower(strings.ReplaceAll(dramMode, "_", "-"))
	switch dramMode {
	case "auto", "preloader":
	case "standard", "mt6592-standard", "mt6592-default", "default":
		return mt6592StandardEMI("lpddr2")
	case "lpddr2", "mt6592-lpddr2":
		return mt6592StandardEMI("lpddr2")
	case "lpddr3", "mt6592-lpddr3":
		return mt6592StandardEMI("lpddr3")
	case "da-default", "mt6592-da-default", "empty-default", "0xffffffff":
		return &mtkPreloaderEMI{
			Path:       "mt6592-standard-da-default",
			Version:    0,
			UseDefault: true,
		}, nil
	default:
		return nil, fmt.Errorf("-mtk-dram must be auto, preloader, mt6592-standard, mt6592-lpddr3, or mt6592-lpddr2, got %q", cfg.mtkDRAM)
	}

	preloader := strings.TrimSpace(cfg.preloader)
	if preloader == "" {
		preloader = strings.TrimSpace(os.Getenv("MVII_MTK_PRELOADER"))
	}
	if preloader != "" {
		if !fileExists(preloader) {
			return nil, fmt.Errorf("-preloader %s does not exist", preloader)
		}
		return readMTKPreloaderEMI(preloader)
	}

	for _, candidate := range defaultMTKPreloaderCandidates(cfg.root) {
		emi, err := readMTKPreloaderEMI(candidate)
		if err == nil {
			return emi, nil
		}
		fmt.Printf("Warning: ignoring preloader candidate %s: %v\n", candidate, err)
	}
	if dramMode == "preloader" {
		return nil, errors.New("-mtk-dram preloader needs -preloader /path/to/preloader_*.bin or a preloader*.bin next to the package")
	}
	return nil, nil
}

func mt6592StandardEMI(profile string) (*mtkPreloaderEMI, error) {
	data, err := mt6592StandardEMIData(profile)
	if err != nil {
		return nil, err
	}
	return &mtkPreloaderEMI{
		Path:    "mt6592-standard-" + profile + "-v13-default",
		Version: 0x0D,
		Data:    data,
		BuiltIn: true,
	}, nil
}

func mt6592StandardEMIData(profile string) ([]byte, error) {
	profile = strings.ToLower(strings.TrimSpace(profile))
	dramType := uint32(0)
	dramcDDR2CTL := uint32(0)
	dramcACTIM1 := uint32(0)
	switch profile {
	case "lpddr2":
		dramType = 0x0002
		dramcDDR2CTL = 0xA00632D1
		dramcACTIM1 = 0x01000510
	case "lpddr3":
		dramType = 0x0003
		dramcDDR2CTL = 0xA00632F1
		dramcACTIM1 = 0x11000510
	default:
		return nil, fmt.Errorf("unknown MT6592 standard EMI profile %q", profile)
	}

	out := make([]byte, 0, 0xBC)
	appendLE32 := func(values ...uint32) {
		for _, value := range values {
			out = binary.LittleEndian.AppendUint32(out, value)
		}
	}

	appendLE32(
		0x00000000, // sub_version; mtkclient/DA protocol patches this to BE 0x100 before upload.
		dramType,
		0x00000000, // eMMC ID checking length.
		0x00000000, // FW ID checking length.
	)
	out = append(out, make([]byte, 16)...) // NAND/eMMC ID.
	out = append(out, make([]byte, 8)...)  // FW ID.
	appendLE32(
		0x0000212E, // EMI_CONA_VAL.
		0xAA00AA00, // DRAMC_DRVCTL0_VAL.
		0xAA00AA00, // DRAMC_DRVCTL1_VAL.
		0x44584493, // DRAMC_ACTIM_VAL.
		0x01000000, // DRAMC_GDDR3CTL1_VAL.
		0xF0048683, // DRAMC_CONF1_VAL.
		dramcDDR2CTL,
		0xBF080401, // DRAMC_TEST2_3_VAL.
		0x0340633F, // DRAMC_CONF2_VAL.
		0x51642342, // DRAMC_PD_CTRL_VAL.
		0x00008888, // DRAMC_PADCTL3_VAL.
		0x88888888, // DRAMC_DQODLY_VAL.
		0x00000000, // DRAMC_ADDR_OUTPUT_DLY.
		0x00000000, // DRAMC_CLK_OUTPUT_DLY.
		dramcACTIM1,
		0x07800000, // DRAMC_MISCTL0_VAL.
		0x04002600, // DRAMC_ACTIM05T_VAL.
	)
	appendLE32(0, 0, 0, 0)                   // DRAM_RANK_SIZE.
	appendLE32(0, 0, 0, 0, 0, 0, 0, 0, 0, 0) // reserved.
	appendLE32(
		0x00C30001, // LPDDR mode register 1.
		0x00060002, // LPDDR mode register 2.
		0x00020003, // LPDDR mode register 3.
		0x00000006, // LPDDR mode register 5.
		0x00FF000A, // LPDDR mode register 10.
		0x0000003F, // LPDDR mode register 63.
	)
	if len(out) != 0xBC {
		return nil, fmt.Errorf("internal MT6592 standard EMI record is 0x%x bytes, want 0xbc", len(out))
	}
	return out, nil
}

func defaultMTKPreloaderCandidates(root string) []string {
	patterns := []string{
		filepath.Join(root, "preloader*.bin"),
		filepath.Join(root, "tools", "mtk-da", "preloader*.bin"),
		filepath.Join(root, "tools", "mtkclient", "Preloader", "preloader*.bin"),
		filepath.Join(root, "mtkclient", "Preloader", "preloader*.bin"),
	}
	var candidates []string
	added := map[string]bool{}
	for _, pattern := range patterns {
		matches, _ := filepath.Glob(pattern)
		sort.Strings(matches)
		for _, match := range matches {
			if !added[match] && fileExists(match) {
				added[match] = true
				candidates = append(candidates, match)
			}
		}
	}
	return candidates
}

func readMTKPreloaderEMI(path string) (*mtkPreloaderEMI, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	emi, err := extractMTKPreloaderEMI(path, data)
	if err != nil {
		return nil, err
	}
	return emi, nil
}

func extractMTKPreloaderEMI(path string, data []byte) (*mtkPreloaderEMI, error) {
	view := data
	if idx := bytes.Index(view, []byte{0x4D, 0x4D, 0x4D, 0x01, 0x38, 0x00, 0x00, 0x00}); idx >= 0 {
		wrapped := view[idx:]
		if len(wrapped) >= 0x30 {
			mlen := binary.LittleEndian.Uint32(wrapped[0x20:0x24])
			siglen := binary.LittleEndian.Uint32(wrapped[0x2C:0x30])
			if mlen >= siglen && mlen <= uint32(len(wrapped)) {
				unwrapped := wrapped[:int(mlen-siglen)]
				if len(unwrapped) >= 4 {
					dramSize := binary.LittleEndian.Uint32(unwrapped[len(unwrapped)-4:])
					if dramSize == 0 && len(unwrapped) > 0x804 {
						unwrapped = unwrapped[:len(unwrapped)-0x800]
						dramSize = binary.LittleEndian.Uint32(unwrapped[len(unwrapped)-4:])
					}
					if dramSize != 0 && int(dramSize)+4 <= len(unwrapped) {
						view = unwrapped[len(unwrapped)-int(dramSize)-4 : len(unwrapped)-4]
					} else {
						view = unwrapped
					}
				}
			}
		}
	}

	marker := []byte("MTK_BLOADER_INFO_v")
	idx := bytes.Index(view, marker)
	if idx < 0 {
		return nil, fmt.Errorf("%s does not contain MTK_BLOADER_INFO_v EMI metadata", path)
	}
	version, ok := parseMTKEMIVersion(view[idx+len(marker):])
	if !ok {
		return nil, fmt.Errorf("%s has unreadable MTK_BLOADER_INFO EMI version", path)
	}
	binIdx := bytes.Index(view, []byte("MTK_BIN"))
	if binIdx < 0 {
		return nil, fmt.Errorf("%s does not contain MTK_BIN EMI payload marker", path)
	}
	start := binIdx + 0x0C
	if start >= len(view) {
		return nil, fmt.Errorf("%s has empty MTK_BIN EMI payload", path)
	}
	emiData := append([]byte(nil), view[start:]...)
	return &mtkPreloaderEMI{Path: path, Version: version, Data: emiData}, nil
}

func parseMTKEMIVersion(data []byte) (uint32, bool) {
	value := uint32(0)
	digits := 0
	for _, b := range data {
		if b == 0 {
			break
		}
		if b < '0' || b > '9' {
			break
		}
		value = value*10 + uint32(b-'0')
		digits++
		if digits == 2 {
			break
		}
	}
	return value, digits > 0
}

func mtkSerialPacketSize(cfg config) (int, error) {
	value := cfg.mtkPacketSize
	if value == "" {
		value = strings.TrimSpace(os.Getenv("MVII_MTK_PACKET_SIZE"))
	}
	if value == "" {
		return mtkDefaultSerialPacket, nil
	}
	parsed, err := parseMTKNumber(value, "-mtk-packet-size")
	if err != nil {
		return 0, err
	}
	if parsed < mtkSerialBlockSize || parsed > mtkMaximumSerialPacket || parsed%mtkSerialBlockSize != 0 {
		return 0, fmt.Errorf("-mtk-packet-size must be a 512-byte multiple from 0x%x to 0x%x", mtkSerialBlockSize, mtkMaximumSerialPacket)
	}
	return int(parsed), nil
}

func prepareDAData(data []byte) (uint16, []byte) {
	payload := data
	if len(payload)%2 != 0 {
		payload = append(append([]byte{}, payload...), 0)
	}
	var checksum uint16
	for i := 0; i < len(payload); i += 2 {
		checksum ^= binary.LittleEndian.Uint16(payload[i : i+2])
	}
	return checksum, payload
}

func sum16(data []byte) uint16 {
	var sum uint32
	for _, b := range data {
		sum += uint32(b)
	}
	return uint16(sum & 0xFFFF)
}

func reverseMTKWords(data []byte) []byte {
	out := append([]byte(nil), data...)
	for pos := 0; pos+4 <= len(out); pos += 4 {
		out[pos], out[pos+3] = out[pos+3], out[pos]
		out[pos+1], out[pos+2] = out[pos+2], out[pos+1]
	}
	return out
}

func uint32Bytes(value uint32) []byte {
	var out [4]byte
	binary.BigEndian.PutUint32(out[:], value)
	return out[:]
}

func uint64Bytes(value uint64) []byte {
	var out [8]byte
	binary.BigEndian.PutUint64(out[:], value)
	return out[:]
}

// writeLegacyFields sends each field as its own drained serial transfer, matching
// the way mtkclient issues legacy DA command headers (one usbwrite per field).
// The MT6592 stage-2 command parser reads some headers field-by-field and faults
// if the whole header arrives coalesced into a single USB transfer.
func (c *mtkSerialClient) writeLegacyFields(fields ...[]byte) error {
	for _, field := range fields {
		if err := c.writeRaw(field); err != nil {
			return err
		}
	}
	return nil
}

func alignUp(value, align uint64) uint64 {
	if align == 0 || value%align == 0 {
		return value
	}
	return value + align - value%align
}

func envDuration(name string, fallback time.Duration) time.Duration {
	value := strings.TrimSpace(os.Getenv(name))
	if value == "" {
		return fallback
	}
	if duration, err := time.ParseDuration(value); err == nil {
		return duration
	}
	if millis, err := strconv.ParseInt(value, 10, 64); err == nil && millis > 0 {
		return time.Duration(millis) * time.Millisecond
	}
	return fallback
}

func envFlag(name string) bool {
	switch strings.ToLower(strings.TrimSpace(os.Getenv(name))) {
	case "1", "true", "yes", "on", "y":
		return true
	default:
		return false
	}
}
