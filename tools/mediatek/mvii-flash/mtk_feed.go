package main

import (
	"bufio"
	"bytes"
	"crypto/sha256"
	"debug/elf"
	"encoding/binary"
	"errors"
	"fmt"
	"hash/crc32"
	"io"
	"os"
	"os/signal"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"time"
)

const (
	mtkFeedDefaultPayloadAddr = 0x110000
	mtkFeedDefaultChunkSize   = 0x10000
	mtkFeedScatterChunkSize   = 0x10000
	mtkFeedScatterSessionSize = 0x200000
	mtkFeedMaxChunkSize       = 0x100000
	mtkFeedMaxPayloadBytes    = 16 * 1024 * 1024
	mtkFeedHeaderSize         = 20
	mtkFeedVersion            = 1

	mtkFeedFrameHello        = 0x0001
	mtkFeedFrameStart        = 0x0002
	mtkFeedFrameData         = 0x0003
	mtkFeedFrameFinish       = 0x0004
	mtkFeedFrameRead         = 0x0005
	mtkFeedFrameReboot       = 0x0006
	mtkFeedFrameRunStage1    = 0x0007
	mtkFeedFrameCommand      = 0x0008
	mtkFeedFrameAck          = 0x8001
	mtkFeedFrameDone         = 0x8002
	mtkFeedFrameLog          = 0x8003
	mtkFeedFrameProgress     = 0x8004
	mtkFeedFrameReadData     = 0x8005
	mtkFeedFrameStage1Status = 0x8006
	mtkFeedFrameError        = 0x80FF

	mtkFeedFlagBootAfterFlash   = 1 << 0 // Deprecated protocol bit; rejected because flashing must not copy to DRAM.
	mtkFeedFlagRebootAfterFlash = 1 << 1
	mtkFeedFlagEnableBoot1      = 1 << 2
	mtkFeedFlagSparseStream     = 1 << 3
	mtkFeedFlagBootStatus       = 1 << 4

	mviiBootStatusMagic   = 0x5342374d
	mviiBootStatusVersion = 3
	mviiBootStatusOffset  = 0x01f3fe00

	// Runtime console ring (mt6592_bootstatus.h MT6592_CONSOLE_RING_*): a 64 KiB
	// region at the LK-slot tail holding one header sector plus the raw serial
	// console stream from the ARM runtime.
	mviiConsoleRingMagic          = 0x4c43374d // M7CL
	mviiConsoleRingOffset         = 0x01f30000
	mviiConsoleRingDataOffset     = mviiConsoleRingOffset + 512
	mviiConsoleRingDataSize       = 0x10000 - 512 - mviiBootStatusSize
	mviiBootStatusBootImageOffset = 0x0283fe00
	mviiBootStatusSize            = 0x200
	mviiBootStatusMessageOffset   = 96
	mviiBootStatusEntryOffset     = 44
	mviiBootStatusLKExtraOffset   = 60
	mviiBootStatusMMSYSOffset     = 76
	mviiBootStatusV1MessageSize   = 384
	mviiBootStatusV1BootargOffset = 480
	mviiBootStatusV2MessageSize   = 352
	mviiBootStatusV2BootargOffset = 448
	mviiBootStatusV2VisibleOffset = 480
	mviiBootStatusV3MessageSize   = 320
	mviiBootStatusV3BootargOffset = 416
	mviiBootStatusV3VisibleOffset = 448
	mviiBootStatusV3PowerOffset   = 480
	mviiBootStatusFlagFramebuffer = 1 << 0
	mviiBootStatusFlagStorage     = 1 << 1
	mviiBootStatusFlagBootVolume  = 1 << 2
	mviiBootStatusFlagFlash       = 1 << 3
	mviiBootStatusFlagLive        = 1 << 4
	mviiBootStatusFlagError       = 1 << 5
	mviiBootStatusFlagComplete    = 1 << 6
	mviiBootStatusEndMagic        = 0x444e4521 // "!END" little-endian
	mviiBootStatusEndOffset       = mviiBootStatusSize - 8
	mviiJ36FallbackFBAddr         = 0x82700000
	mviiJ36FallbackFBWidth        = 640
	mviiJ36FallbackFBHeight       = 480
	mviiJ36FallbackFBPitch        = 2560
	mviiJ36FallbackFBBPP          = 32
	mtkImageMagic                 = 0x58881688
	mtkImageHeaderSize            = 0x200
)

var mtkFeedMagic = []byte{'M', '7', 'F', '1'}

// MVII feed protocol, version 1.
//
// Every frame is:
//
//	magic[4] = "M7F1"
//	version  uint16 little-endian
//	type     uint16 little-endian
//	seq      uint32 little-endian
//	len      uint32 little-endian
//	crc32    uint32 little-endian, IEEE CRC of payload bytes
//	payload  len bytes
//
// Host flow:
//
//	HELLO    payload may be empty; host sends it repeatedly until the payload responds.
//	START    [target_offset:u64][image_size:u64][transfer_len:u64][emmc_part:u32]
//	         [block_size:u32][chunk_size:u32][flags:u32][sha256(transfer stream):32]
//	         flags bit 0 is deprecated and rejected; flashing never mirrors
//	         streamed images into DRAM.
//	         flags bit 1 asks the payload to watchdog-reset after FINISH.
//	         flags bit 2 asks the payload to enable eMMC BOOT1 as the hardware
//	         boot partition after FINISH.
//	         flags bit 3 allows DATA stream offsets to skip unwritten sparse
//	         holes; skipped ranges are left untouched.
//	DATA     [target_offset:u64][stream_offset:u64][bytes...]
//	FINISH   [transfer_len:u64][sha256(transfer stream):32]
//	READ     [target_offset:u64][length:u32][emmc_part:u32][block_size:u32]
//	REBOOT   optional reason bytes; asks a still-running payload to watchdog-reset
//	         back into BROM USB-download mode so the host can reconnect without
//	         a battery pull after an interrupted run.
//	COMMAND  ASCII command for live hardware probes (help, probe, keys, display,
//	         sd, usb, wifi, audio, speaker-noise). The payload remains resident.
//
// Payload responses:
//
//	HELLO    optional [max_chunk:u32][block_size:u32][flags:u32][name...]
//	ACK      [status:u32][message...], status 0 means accepted
//	DONE     [status:u32][message...], status 0 means flushed and complete
//	LOG      UTF-8/ASCII diagnostic text
//	READDATA [target_offset:u64][length:u32][bytes...]
//	ERROR    [status:u32][message...]
type mtkFeedFrame struct {
	Type    uint16
	Seq     uint32
	Payload []byte
}

type mtkFeedPayloadImage struct {
	Path      string
	Kind      string
	LoadAddr  uint32
	EntryAddr uint32
	Data      []byte
}

type mtkFeedHello struct {
	MaxChunk  uint32
	BlockSize uint32
	Flags     uint32
	Name      string
}

type mtkFeedWritePlan struct {
	Label          string
	PartitionName  string
	Path           string
	PartitionDelta uint64
	SourceSize     uint64
	FileSize       uint64
	ImageSize      uint64
	TransferLength uint64
	TargetOffset   uint64
	Part           byte
	Flags          uint32
	Hash           [sha256.Size]byte
	Sparse         androidSparseImageInfo
	IsSparse       bool
	AutoLength     bool
	LengthNote     string
}

func shouldUseDefaultMTKFeedBundle(cfg config) bool {
	return cfg.mtkFeedBundle && shouldUseDefaultMTKFeedBootChain(cfg)
}

func shouldUseDefaultMTKFeedBootChain(cfg config) bool {
	if cfg.image != "" || cfg.rawOffset != "" {
		return false
	}
	if cfg.rawLength != "" && !isMTKFeedAutoRawLength(cfg.rawLength) {
		return false
	}
	if !strings.EqualFold(strings.TrimSpace(cfg.mtkFeedPart), "user") && strings.TrimSpace(cfg.mtkFeedPart) != "" {
		return false
	}
	// Only the files the SELECTED target needs. `-upload assets' deliberately
	// does not require lk.bin/MVIIS1.bin/boot.img to be present, because the
	// entire reason it exists is to change the boot pictures without touching the
	// boot chain -- and the same now goes for every other slice.
	for _, image := range uploadRequiredImages(cfg, effectiveUploadTarget(cfg)) {
		if !fileExists(image) {
			return false
		}
	}
	return true
}

func mtkFeedHandoffDisabledError() error {
	return errors.New("-mtk-feed-handoff is disabled; flashing never mirrors streamed images into DRAM")
}

func defaultMVIIArmImagePath(cfg config) string {
	return filepath.Join(cfg.root, "boot.img")
}

func defaultMVIILKImagePath(cfg config) string {
	return filepath.Join(cfg.root, "lk.bin")
}

func defaultMVIIS1ImagePath(cfg config) string {
	return filepath.Join(cfg.root, "MVIIS1.bin")
}

// The asset slot: every picture the device draws before the OS. Not part of any
// image -- LK reads it off the LOGO partition at boot -- which is the whole
// point of `-upload assets': new art is one partition and a few seconds, instead of a
// bootloader rebuild and a three-image flash.
func defaultMVIIAssetImagePath(cfg config) string {
	return filepath.Join(cfg.root, "assets.bin")
}

func isMTKFeedAutoRawLength(rawLength string) bool {
	return rawLength == "" || strings.EqualFold(rawLength, "auto") || strings.EqualFold(rawLength, "minimal")
}

func writePreloaderMTKFeed(cfg config, plPath string) error {
	info, err := os.Stat(plPath)
	if err != nil {
		return err
	}
	if info.IsDir() {
		return fmt.Errorf("%s is a directory, not a preloader image", plPath)
	}
	if info.Size() <= 0 {
		return fmt.Errorf("preloader image %s is empty", plPath)
	}
	if cfg.rawOffset != "" {
		offset, err := parseMTKNumber(cfg.rawOffset, "-raw-offset")
		if err != nil {
			return err
		}
		if offset != 0 {
			return fmt.Errorf("-mtk-write-preloader writes eMMC BOOT1 at offset 0; got -raw-offset 0x%x", offset)
		}
	}
	if cfg.rawLength != "" && !strings.EqualFold(cfg.rawLength, "auto") && !strings.EqualFold(cfg.rawLength, "minimal") {
		return fmt.Errorf("-mtk-write-preloader uses the preloader image length padded to 512 bytes; remove -raw-length %s", cfg.rawLength)
	}

	feedCfg := cfg
	if strings.TrimSpace(feedCfg.mtkFeedPayload) == "" {
		feedCfg.mtkFeedPayload = "auto"
	}
	feedCfg.mtkFeedPart = "boot1"
	feedCfg.rawOffset = "0x0"
	feedCfg.rawLength = "auto"
	feedCfg.mtkFeedHandoff = false
	feedCfg.mtkFeedExtraFlags |= mtkFeedFlagEnableBoot1

	fmt.Println("Writing preloader through native MTK feed payload (MSDC-owned path), not legacy DA stage2.")
	return feedPayloadMTKSerial(feedCfg, plPath, info)
}

type mtkScatterFlashItem struct {
	Entry          mtkScatterEntry
	Path           string
	Size           uint64
	TransferLength uint64
	Part           byte
	Sparse         androidSparseImageInfo
	IsSparse       bool
}

func flashScatterMTKFeed(cfg config, scatterPath string) error {
	entries, err := parseMTKScatterFile(scatterPath)
	if err != nil {
		return err
	}
	items, skipped, err := planMTKScatterFlash(scatterPath, entries)
	if err != nil {
		return err
	}
	if len(items) == 0 {
		return fmt.Errorf("%s has no downloadable EMMC_USER images to flash", scatterPath)
	}

	feedCfg := cfg
	if strings.TrimSpace(feedCfg.mtkFeedPayload) == "" {
		feedCfg.mtkFeedPayload = "auto"
	}
	payload, err := loadMTKFeedPayload(feedCfg)
	if err != nil {
		return err
	}
	chunkSize, err := mtkFeedChunkSize(feedCfg)
	if err != nil {
		return err
	}
	if !mtkFeedChunkSizeExplicitlyConfigured(feedCfg) {
		chunkSize = mtkFeedScatterChunkSize
	}
	if err := confirmMTKScatterFlash(feedCfg, scatterPath, items, skipped); err != nil {
		return err
	}
	if feedCfg.yes {
		for _, text := range skipped {
			fmt.Printf("Skipping scatter entry: %s\n", text)
		}
	}

	sessionLimit, err := mtkFeedScatterSessionLimit()
	if err != nil {
		return err
	}
	fmt.Printf("Booting MTK feed payload: %s (%s, 0x%x bytes)\n", payload.Path, payload.Kind, len(payload.Data))
	fmt.Printf("Loading payload to 0x%x, entry 0x%x\n", payload.LoadAddr, payload.EntryAddr)
	fmt.Println("MTK feed scatter mode expects the J36 Ultra powered off or in BROM/preloader VCOM mode.")
	fmt.Printf("Scatter feed chunk size: 0x%x; payload session cap: 0x%x\n", chunkSize, sessionLimit)

	for i, item := range items {
		fmt.Printf("Scatter %d/%d: %s %s -> eMMC USER offset 0x%x, length 0x%x",
			i+1, len(items), item.Entry.PartitionName, filepath.Base(item.Path), item.Entry.LinearStart, item.TransferLength)
		if item.IsSparse {
			fmt.Print(" (Android sparse)")
		}
		fmt.Println()

		for segmentStart := uint64(0); segmentStart < item.TransferLength; {
			segmentLen := sessionLimit
			if remaining := item.TransferLength - segmentStart; remaining < segmentLen {
				segmentLen = remaining
			}
			finalSegment := i == len(items)-1 && segmentStart+segmentLen == item.TransferLength
			if err := flashMTKScatterSegment(feedCfg, payload, item, i+1, len(items), segmentStart, segmentLen,
				chunkSize, !finalSegment, finalSegment && feedCfg.mtkFeedReboot); err != nil {
				return err
			}
			segmentStart += segmentLen
		}
	}
	if feedCfg.mtkFeedReboot {
		fmt.Println("Watchdog reset was requested after the final scatter image.")
	}
	followDevice, err := bootPreloaderAfterMTKFeed(feedCfg)
	if err != nil {
		return err
	}
	if followDevice != "" {
		feedCfg.device = followDevice
	}
	if err := followMTKFeedBoot(feedCfg); err != nil {
		return err
	}
	fmt.Println("MTK native scatter feed complete.")
	return nil
}

func mtkFeedScatterSessionLimit() (uint64, error) {
	value := strings.TrimSpace(os.Getenv("MVII_MTK_SCATTER_SESSION_BYTES"))
	if value == "" {
		return mtkFeedScatterSessionSize, nil
	}
	parsed, err := parseMTKNumber(value, "MVII_MTK_SCATTER_SESSION_BYTES")
	if err != nil {
		return 0, err
	}
	parsed -= parsed % mtkSerialBlockSize
	if parsed < mtkSerialBlockSize {
		return 0, fmt.Errorf("MVII_MTK_SCATTER_SESSION_BYTES must be at least 0x%x after 512-byte alignment", mtkSerialBlockSize)
	}
	return parsed, nil
}

// mtkFeedFeedSessionSize is the default per-session byte budget for the
// single-image and default-bundle feed paths. 1 MiB is the value proven
// reliable end-to-end on the J36 Ultra BROM VCOM. A nominal 1 MiB
// transaction can still drop around 448-704 KiB after several resident-payload
// transactions, so leave headroom and finish each START/DATA/FINISH at 512 KiB.
const mtkFeedFeedSessionSize = 0x80000

// mtkFeedSegmentAttempts is how many times a single bounded segment write is
// retried on a transport drop before the flash is failed. Each segment is an
// idempotent raw eMMC range write, so retrying after a reconnect is safe and
// recovers from the fragile BROM VCOM dropping mid-session (LIBUSB_ERROR_IO)
// or the board re-enumerating in preloader mode between sessions.
const mtkFeedSegmentAttempts = 6

// mtkFeedSegmentAttempts bounds how many times one

// mtkFeedSingleSessionLimit bounds how many bytes one MVIIFlash feed session
// streams in one START/DATA/FINISH transaction. The payload remains resident
// and USB remains connected between successful transactions; reconnect is only
// recovery for a real transport failure.
// Override with MVII_MTK_FEED_SESSION_BYTES.
func mtkFeedSingleSessionLimit() (uint64, error) {
	if value := strings.TrimSpace(os.Getenv("MVII_MTK_FEED_SESSION_BYTES")); value != "" {
		parsed, err := parseMTKNumber(value, "MVII_MTK_FEED_SESSION_BYTES")
		if err != nil {
			return 0, err
		}
		parsed -= parsed % mtkSerialBlockSize
		if parsed < mtkSerialBlockSize {
			return 0, fmt.Errorf("MVII_MTK_FEED_SESSION_BYTES must be at least 0x%x after 512-byte alignment", mtkSerialBlockSize)
		}
		return parsed, nil
	}
	return mtkFeedFeedSessionSize, nil
}

type feedSession struct {
	Start  uint64
	Length uint64
}

// planFeedSessions splits a transfer into contiguous [Start, Start+Length)
// sessions no larger than sessionLimit. Every session is a multiple of the eMMC
// block size as long as transferLength and sessionLimit are, so each session's
// START/DATA/FINISH stays 512-byte aligned.
func planFeedSessions(transferLength, sessionLimit uint64) []feedSession {
	if sessionLimit == 0 || sessionLimit >= transferLength {
		return []feedSession{{Start: 0, Length: transferLength}}
	}
	var sessions []feedSession
	for start := uint64(0); start < transferLength; {
		segLen := sessionLimit
		if remaining := transferLength - start; remaining < segLen {
			segLen = remaining
		}
		sessions = append(sessions, feedSession{Start: start, Length: segLen})
		start += segLen
	}
	return sessions
}

func flashMTKScatterSegment(cfg config, payload mtkFeedPayloadImage, item mtkScatterFlashItem, itemIndex, itemCount int,
	segmentStart, segmentLen uint64, chunkSize int, resetAfter, rebootAfter bool) error {
	client, hello, stopInterruptReset, err := startMTKFeedPayloadSession(cfg, payload)
	if err != nil {
		return err
	}
	defer stopInterruptReset()
	defer func() {
		_ = client.port.Close()
	}()

	if hello.BlockSize != 0 && hello.BlockSize != mtkSerialBlockSize {
		return fmt.Errorf("feed payload reports block size 0x%x, expected 0x%x", hello.BlockSize, mtkSerialBlockSize)
	}
	if adjusted, err := negotiateFeedChunkSizeForScatter(hello, chunkSize); err != nil {
		return err
	} else {
		chunkSize = adjusted
	}

	seq := uint32(1)
	flags := uint32(0)
	if item.IsSparse {
		flags |= mtkFeedFlagSparseStream
	}
	if rebootAfter {
		flags |= mtkFeedFlagRebootAfterFlash
	}
	var streamHash [sha256.Size]byte
	targetOffset := item.Entry.LinearStart + segmentStart

	if segmentStart == 0 && segmentLen == item.TransferLength {
		fmt.Printf("  segment: whole image length 0x%x\n", segmentLen)
	} else {
		fmt.Printf("  segment: scatter %d/%d %s expanded 0x%x..0x%x -> eMMC USER 0x%x\n",
			itemIndex, itemCount, item.Entry.PartitionName, segmentStart, segmentStart+segmentLen, targetOffset)
	}

	start := buildMTKFeedStartPayload(targetOffset, segmentLen, segmentLen,
		uint32(item.Part), mtkSerialBlockSize, uint32(chunkSize), flags, streamHash)
	if err := client.writeMTKFeedFrame(mtkFeedFrameStart, seq, start); err != nil {
		return fmt.Errorf("send feed START for %s segment 0x%x: %w", item.Entry.PartitionName, segmentStart, err)
	}
	if err := client.waitMTKFeedAck(seq, fmt.Sprintf("START %s segment 0x%x", item.Entry.PartitionName, segmentStart), false); err != nil {
		return err
	}
	seq++
	if item.IsSparse {
		seq, err = client.streamMTKFeedSparseImageRange(item.Path, item.Sparse, targetOffset, segmentStart, segmentLen, chunkSize, seq)
	} else {
		seq, err = client.streamMTKFeedImageRange(item.Path, item.Size, targetOffset, segmentStart, segmentLen, chunkSize, seq)
	}
	if err != nil {
		return err
	}
	finish := buildMTKFeedFinishPayload(segmentLen, streamHash)
	if err := client.writeMTKFeedFrame(mtkFeedFrameFinish, seq, finish); err != nil {
		return fmt.Errorf("send feed FINISH for %s segment 0x%x: %w", item.Entry.PartitionName, segmentStart, err)
	}
	if err := client.waitMTKFeedAck(seq, fmt.Sprintf("FINISH %s segment 0x%x", item.Entry.PartitionName, segmentStart), true); err != nil {
		return err
	}
	seq++

	if resetAfter {
		if err := client.requestMTKFeedPayloadReboot(seq); err != nil {
			fmt.Printf("Warning: could not reset feed payload after segment 0x%x: %v\n", segmentStart, err)
		}
	}
	return nil
}

func startMTKFeedPayloadSession(cfg config, payload mtkFeedPayloadImage) (*mtkSerialClient, mtkFeedHello, func(), error) {
	client, err := connectMTKSerialForFeed(cfg.device)
	if err != nil {
		return nil, mtkFeedHello{}, func() {}, err
	}
	closeOnError := true
	defer func() {
		if closeOnError {
			_ = client.port.Close()
		}
	}()

	hello := client.feedPayloadHello
	if !client.feedPayloadReady {
		if err := client.disableMT6592Watchdog(); err != nil {
			fmt.Printf("Warning: could not disable MT6592 watchdog before feed payload boot: %v\n", err)
		}
		if err := client.sendDA(payload.LoadAddr, 0, payload.Data); err != nil {
			return nil, mtkFeedHello{}, func() {}, fmt.Errorf("send feed payload to 0x%x: %w", payload.LoadAddr, err)
		}
		if err := client.jumpDA(payload.EntryAddr); err != nil {
			fmt.Printf("JUMP_DA reported %v - waiting for the feed payload protocol anyway.\n", err)
		}

		hello, err = client.waitMTKFeedHello()
		if err != nil {
			return nil, mtkFeedHello{}, func() {}, err
		}
	}
	if hello.Name != "" {
		fmt.Printf("Feed payload ready: %s\n", hello.Name)
	} else {
		fmt.Println("Feed payload ready.")
	}
	stopInterruptReset := installMTKFeedInterruptReset(client)
	closeOnError = false
	return client, hello, stopInterruptReset, nil
}

func readBootStatusMTKFeed(cfg config) error {
	feedCfg := cfg
	if strings.TrimSpace(feedCfg.mtkFeedPayload) == "" {
		feedCfg.mtkFeedPayload = "auto"
	}
	payload, err := loadMTKFeedPayload(feedCfg)
	if err != nil {
		return err
	}

	fmt.Printf("MVII device status: reading boot-status @0x%x via native payload\n", mviiBootStatusOffset)
	fmt.Printf("Feed payload image: %s (%s, 0x%x bytes), load 0x%x\n",
		filepath.Base(payload.Path), payload.Kind, len(payload.Data), payload.LoadAddr)

	client, err := connectMTKSerialForFeedReadOnly(feedCfg.device)
	if err != nil {
		return err
	}
	defer func() {
		_ = client.port.Close()
	}()
	hello := client.feedPayloadHello
	if client.feedPayloadReady {
		fmt.Println("Reusing existing MVIIFlash payload; no reset or payload upload needed.")
	} else {
		if err := client.disableMT6592Watchdog(); err != nil {
			fmt.Printf("Warning: could not disable MT6592 watchdog before feed payload boot: %v\n", err)
		}
		if err := client.sendDA(payload.LoadAddr, 0, payload.Data); err != nil {
			return fmt.Errorf("send feed payload to 0x%x: %w", payload.LoadAddr, err)
		}
		if err := client.jumpDA(payload.EntryAddr); err != nil {
			fmt.Printf("JUMP_DA reported %v - waiting for the feed payload protocol anyway.\n", err)
		}

		hello, err = client.waitMTKFeedHello()
		if err != nil {
			return err
		}
	}
	if hello.Name != "" {
		fmt.Printf("Feed payload ready: %s\n", hello.Name)
	} else {
		fmt.Println("Feed payload ready.")
	}
	stopInterruptReset := installMTKFeedInterruptReset(client)
	defer stopInterruptReset()
	if hello.BlockSize != 0 && hello.BlockSize != mtkSerialBlockSize {
		return fmt.Errorf("feed payload reports block size 0x%x, expected 0x%x", hello.BlockSize, mtkSerialBlockSize)
	}

	seq := uint32(1)
	client.feedQuiet = true
	data, seq, err := client.readMTKFeedBytes(seq, mviiBootStatusOffset, mviiBootStatusSize, mtkLegacyEMMCPartUser, "MVII boot status")
	if err != nil {
		return err
	}
	printMVIIBootStatusAt(mviiBootStatusOffset, data)
	seq = client.dumpMVIIConsoleRing(seq)

	// The running firmware publishes its live status only at the LK-slot tail
	// (mviiBootStatusOffset). The BOOTIMG-tail sector is a legacy location that a
	// current flash never overwrites, so it typically holds a stale record from
	// an earlier experiment (e.g. the old "HELLO VIRTUA" standalone stage1).
	// Only fall back to it when the authoritative LK-slot read has no record at
	// all, and label it plainly so a stale hit is never mistaken for live state.
	primaryHasRecord := len(data) >= 4 && binary.LittleEndian.Uint32(data[0:4]) == mviiBootStatusMagic
	if !primaryHasRecord {
		bootData, _, bootErr := client.readMTKFeedBytes(seq, mviiBootStatusBootImageOffset, mviiBootStatusSize, mtkLegacyEMMCPartUser, "MVII BOOTIMG boot status")
		if bootErr == nil {
			magic := uint32(0)
			if len(bootData) >= 4 {
				magic = binary.LittleEndian.Uint32(bootData[0:4])
			}
			if magic == mviiBootStatusMagic {
				fmt.Println("Legacy BOOTIMG-tail status (no live LK-slot record; this sector is not written by the current firmware and may be stale):")
				printMVIIBootStatusAt(mviiBootStatusBootImageOffset, bootData)
			} else {
				fmt.Printf("Legacy BOOTIMG-tail status @0x%x: no MVII status record (magic 0x%08x)\n",
					mviiBootStatusBootImageOffset, magic)
			}
		} else {
			fmt.Printf("Legacy BOOTIMG-tail status read failed: %v\n", bootErr)
		}
	}

	_ = seq
	fmt.Println("Read-only check complete; MVIIFlash is still running for another status read.")
	return nil
}

// dumpMVIIConsoleRing reads the 64 KiB runtime console ring that sits directly
// below the LK-slot status sector (mt6592_bootstatus.c console_ring_*) and
// prints the reconstructed serial log of the current boot.
func (c *mtkSerialClient) dumpMVIIConsoleRing(seq uint32) uint32 {
	header, next, err := c.readMTKFeedBytes(seq, mviiConsoleRingOffset, 512, mtkLegacyEMMCPartUser, "console ring header")
	seq = next
	if err != nil || len(header) < 16 {
		fmt.Printf("Console ring: header read failed: %v\n", err)
		return seq
	}
	magic := binary.LittleEndian.Uint32(header[0:4])
	if magic != mviiConsoleRingMagic {
		fmt.Println("Console ring: not present (runtime has not flushed console output on this boot)")
		return seq
	}
	dataSize := binary.LittleEndian.Uint32(header[8:12])
	writePos := binary.LittleEndian.Uint32(header[12:16])
	if dataSize == 0 || dataSize > mviiConsoleRingDataSize {
		dataSize = mviiConsoleRingDataSize
	}
	readLen := dataSize
	if writePos < dataSize {
		readLen = (writePos + 511) &^ 511 // payload requires 512-aligned lengths
	}
	if readLen == 0 {
		fmt.Println("Console ring: present but empty")
		return seq
	}
	var data []byte
	for off := uint32(0); off < readLen; {
		chunk := readLen - off
		if chunk > 0x8000 { // payload caps a single READ at 32 KiB
			chunk = 0x8000
		}
		part, next, err := c.readMTKFeedBytes(seq, uint64(mviiConsoleRingDataOffset)+uint64(off), chunk,
			mtkLegacyEMMCPartUser, "console ring data")
		seq = next
		if err != nil {
			fmt.Printf("Console ring: data read failed: %v\n", err)
			return seq
		}
		data = append(data, part...)
		off += chunk
	}
	var logBytes []byte
	if writePos <= dataSize {
		if int(writePos) < len(data) {
			data = data[:writePos]
		}
		logBytes = data
	} else {
		cut := writePos % dataSize
		logBytes = append(append([]byte{}, data[cut:]...), data[:cut]...)
	}
	clean := strings.Map(func(r rune) rune {
		if r == 0 {
			return -1
		}
		return r
	}, string(logBytes))
	lines := strings.Split(strings.TrimRight(clean, "\n"), "\n")
	wrapped := ""
	if writePos > dataSize {
		wrapped = ", ring wrapped: oldest lines overwritten"
	}
	fmt.Printf("Console ring (%d bytes captured%s):\n", writePos, wrapped)
	// The one-shot boot breadcrumbs live at the head; once the runtime settles
	// into a retry loop the tail is periodic spam. Print the full head and a
	// short tail, eliding the middle.
	const headLines = 250
	const tailLines = 30
	if len(lines) > headLines+tailLines+10 {
		for _, l := range lines[:headLines] {
			fmt.Printf("  | %s\n", l)
		}
		fmt.Printf("  ... (%d middle lines elided) ...\n", len(lines)-headLines-tailLines)
		for _, l := range lines[len(lines)-tailLines:] {
			fmt.Printf("  | %s\n", l)
		}
	} else {
		for _, l := range lines {
			fmt.Printf("  | %s\n", l)
		}
	}
	return seq
}

// writeMTKFeedBootStatusSector pushes an arbitrary 512-byte sector to the MVII
// boot-status offset (eMMC USER 0x1f3fe00) using the feed write protocol. This is
// the same on-device write path stage1 uses (mt6592_emmc_write_part), so it can
// double as a probe of whether runtime eMMC writes land on this board at all.
func (c *mtkSerialClient) writeMTKFeedBootStatusSector(seq uint32, sector []byte, chunkSize int) (uint32, error) {
	if len(sector) != mviiBootStatusSize {
		return seq, fmt.Errorf("boot-status sector must be 0x%x bytes, got 0x%x", mviiBootStatusSize, len(sector))
	}
	hash := sha256.Sum256(sector)
	start := buildMTKFeedStartPayload(uint64(mviiBootStatusOffset), uint64(mviiBootStatusSize),
		uint64(mviiBootStatusSize), uint32(mtkLegacyEMMCPartUser), mtkSerialBlockSize, uint32(chunkSize), 0, hash)
	if err := c.writeMTKFeedFrame(mtkFeedFrameStart, seq, start); err != nil {
		return seq, fmt.Errorf("send feed START for boot-status write: %w", err)
	}
	if err := c.waitMTKFeedAck(seq, "START boot-status write", false); err != nil {
		return seq, err
	}
	seq++
	data := buildMTKFeedDataPayload(uint64(mviiBootStatusOffset), 0, sector)
	if err := c.writeMTKFeedFrame(mtkFeedFrameData, seq, data); err != nil {
		return seq, fmt.Errorf("send feed DATA for boot-status write: %w", err)
	}
	if err := c.waitMTKFeedAck(seq, "DATA boot-status write", false); err != nil {
		return seq, err
	}
	seq++
	finish := buildMTKFeedFinishPayload(uint64(mviiBootStatusSize), hash)
	if err := c.writeMTKFeedFrame(mtkFeedFrameFinish, seq, finish); err != nil {
		return seq, fmt.Errorf("send feed FINISH for boot-status write: %w", err)
	}
	if err := c.waitMTKFeedAck(seq, "FINISH boot-status write", true); err != nil {
		return seq, err
	}
	seq++
	return seq, nil
}

func (c *mtkSerialClient) writeMTKFeedLKBootStatusPending(seq uint32, lkImage string, chunkSize int) (uint32, error) {
	sector, source, err := mviiBootStatusFlashPendingSector(lkImage)
	if err != nil {
		return seq, err
	}
	fmt.Printf("Writing MVII boot-status flash marker at eMMC USER 0x%x (%s)\n", uint64(mviiBootStatusOffset), source)
	return c.writeMTKFeedBootStatusSector(seq, sector, chunkSize)
}

func mviiBootStatusFlashPendingSector(lkImage string) ([]byte, string, error) {
	if sector, ok, err := readEmbeddedLKBootStatusSector(lkImage); err != nil {
		return nil, "", err
	} else if ok {
		return sector, "embedded LK marker", nil
	}
	return buildMVIIBootStatusFlashPendingSector("LK image flashed; waiting for MVII LK"), "host marker", nil
}

func readEmbeddedLKBootStatusSector(lkImage string) ([]byte, bool, error) {
	info, err := os.Stat(lkImage)
	if err != nil {
		return nil, false, err
	}
	sectorOffset := int64(j36UltraScatterLKSize - mviiBootStatusSize)
	if info.Size() < sectorOffset+int64(mviiBootStatusSize) {
		return nil, false, nil
	}
	f, err := os.Open(lkImage)
	if err != nil {
		return nil, false, err
	}
	defer f.Close()
	if _, err := f.Seek(sectorOffset, io.SeekStart); err != nil {
		return nil, false, fmt.Errorf("seek embedded boot-status marker in %s: %w", lkImage, err)
	}
	sector := make([]byte, mviiBootStatusSize)
	if _, err := io.ReadFull(f, sector); err != nil {
		return nil, false, fmt.Errorf("read embedded boot-status marker in %s: %w", lkImage, err)
	}
	if binary.LittleEndian.Uint32(sector[0:4]) != uint32(mviiBootStatusMagic) {
		return nil, false, nil
	}
	return sector, true, nil
}

func buildMVIIBootStatusFlashPendingSector(message string) []byte {
	sector := make([]byte, mviiBootStatusSize)
	binary.LittleEndian.PutUint32(sector[0:4], uint32(mviiBootStatusMagic))
	binary.LittleEndian.PutUint32(sector[4:8], mviiBootStatusVersion)
	binary.LittleEndian.PutUint32(sector[8:12], mviiBootStatusSize)
	binary.LittleEndian.PutUint32(sector[12:16], 0x1001)
	binary.LittleEndian.PutUint32(sector[16:20], mviiBootStatusFlagFlash)
	copy(sector[mviiBootStatusMessageOffset:mviiBootStatusMessageOffset+mviiBootStatusV3MessageSize], message)
	binary.LittleEndian.PutUint32(sector[mviiBootStatusEndOffset:mviiBootStatusEndOffset+4], uint32(mviiBootStatusEndMagic))
	return sector
}

// selftestBootStatusWriteMTKFeed answers the "nothing written, or nothing read?"
// question directly. It writes a unique nonce sector to the boot-status offset
// through the running payload, reads it straight back, and restores the original
// sector. Because this is the exact write path stage1 uses on-device, the result
// tells us whether the on-eMMC breadcrumb can ever work on this board.
func selftestBootStatusWriteMTKFeed(cfg config) error {
	feedCfg := cfg
	if strings.TrimSpace(feedCfg.mtkFeedPayload) == "" {
		feedCfg.mtkFeedPayload = "auto"
	}
	payload, err := loadMTKFeedPayload(feedCfg)
	if err != nil {
		return err
	}

	fmt.Printf("MVII write self-test: probing eMMC USER writes at boot-status @0x%x via native payload\n", mviiBootStatusOffset)
	fmt.Printf("Feed payload image: %s (%s, 0x%x bytes), load 0x%x\n",
		filepath.Base(payload.Path), payload.Kind, len(payload.Data), payload.LoadAddr)

	client, err := connectMTKSerialForFeedReadOnly(feedCfg.device)
	if err != nil {
		return err
	}
	defer func() {
		_ = client.port.Close()
	}()

	hello := client.feedPayloadHello
	if client.feedPayloadReady {
		fmt.Println("Reusing existing MVIIFlash payload; no reset or payload upload needed.")
	} else {
		if err := client.disableMT6592Watchdog(); err != nil {
			fmt.Printf("Warning: could not disable MT6592 watchdog before feed payload boot: %v\n", err)
		}
		if err := client.sendDA(payload.LoadAddr, 0, payload.Data); err != nil {
			return fmt.Errorf("send feed payload to 0x%x: %w", payload.LoadAddr, err)
		}
		if err := client.jumpDA(payload.EntryAddr); err != nil {
			fmt.Printf("JUMP_DA reported %v - waiting for the feed payload protocol anyway.\n", err)
		}
		hello, err = client.waitMTKFeedHello()
		if err != nil {
			return err
		}
	}
	if hello.Name != "" {
		fmt.Printf("Feed payload ready: %s\n", hello.Name)
	} else {
		fmt.Println("Feed payload ready.")
	}
	stopInterruptReset := installMTKFeedInterruptReset(client)
	defer stopInterruptReset()
	if hello.BlockSize != 0 && hello.BlockSize != mtkSerialBlockSize {
		return fmt.Errorf("feed payload reports block size 0x%x, expected 0x%x", hello.BlockSize, mtkSerialBlockSize)
	}
	chunkSize, err := negotiateFeedChunkSize(hello, mtkFeedDefaultChunkSize)
	if err != nil {
		return err
	}

	client.feedQuiet = true
	seq := uint32(1)

	// 1) Save whatever is on the sector now so we can put it back afterwards.
	original, seq, err := client.readMTKFeedBytes(seq, mviiBootStatusOffset, mviiBootStatusSize, mtkLegacyEMMCPartUser, "boot-status (save)")
	if err != nil {
		return fmt.Errorf("read original boot-status sector: %w", err)
	}

	// 2) Build a probe sector carrying a unique nonce and write it.
	nonce := uint32(time.Now().UnixNano())
	if nonce == 0 {
		nonce = 0xA5A5A5A5
	}
	probe := make([]byte, mviiBootStatusSize)
	binary.LittleEndian.PutUint32(probe[0:4], uint32(mviiBootStatusMagic))
	binary.LittleEndian.PutUint32(probe[4:8], mviiBootStatusVersion)
	binary.LittleEndian.PutUint32(probe[8:12], mviiBootStatusSize)
	binary.LittleEndian.PutUint32(probe[20:24], nonce) // sequence field
	binary.LittleEndian.PutUint32(probe[mviiBootStatusEndOffset:mviiBootStatusEndOffset+4], uint32(mviiBootStatusEndMagic))
	binary.LittleEndian.PutUint32(probe[mviiBootStatusEndOffset+4:mviiBootStatusEndOffset+8], nonce)
	copy(probe[mviiBootStatusMessageOffset:mviiBootStatusMessageOffset+mviiBootStatusV3MessageSize],
		fmt.Sprintf("MVIIFlash WRITE SELFTEST nonce=0x%08x", nonce))

	fmt.Printf("Writing probe sector (nonce 0x%08x) to eMMC USER 0x%x ...\n", nonce, uint64(mviiBootStatusOffset))
	seq, err = client.writeMTKFeedBootStatusSector(seq, probe, chunkSize)
	if err != nil {
		fmt.Println()
		fmt.Println("RESULT: eMMC WRITE failed at the payload/protocol level (no ACK).")
		fmt.Printf("        %v\n", err)
		fmt.Println("        => runtime eMMC writes do not work here; the on-eMMC breadcrumb cannot work on this board.")
		return err
	}

	// 3) Read it back and see whether the nonce actually persisted.
	readback, seq, err := client.readMTKFeedBytes(seq, mviiBootStatusOffset, mviiBootStatusSize, mtkLegacyEMMCPartUser, "boot-status (verify)")
	if err != nil {
		return fmt.Errorf("read back probe sector: %w", err)
	}
	gotNonce := binary.LittleEndian.Uint32(readback[20:24])
	gotEnd := binary.LittleEndian.Uint32(readback[mviiBootStatusEndOffset : mviiBootStatusEndOffset+4])
	gotEndSeq := binary.LittleEndian.Uint32(readback[mviiBootStatusEndOffset+4 : mviiBootStatusEndOffset+8])
	writesWork := gotNonce == nonce && gotEnd == uint32(mviiBootStatusEndMagic) && gotEndSeq == nonce

	// 4) Restore the original sector regardless of the outcome.
	if _, rerr := client.writeMTKFeedBootStatusSector(seq, original, chunkSize); rerr != nil {
		fmt.Printf("Warning: could not restore original boot-status sector: %v\n", rerr)
	} else {
		fmt.Println("Restored original boot-status sector.")
	}

	fmt.Println()
	if writesWork {
		fmt.Println("RESULT: runtime eMMC WRITES WORK - probe nonce read back intact.")
		fmt.Println("        => The breadcrumb channel is viable. If a boot still shows only the baked")
		fmt.Println("           marker, the problem is that stage1 is not executing far enough to write")
		fmt.Println("           (boot/handoff), NOT the eMMC write path.")
	} else {
		fmt.Printf("RESULT: runtime eMMC WRITES DO NOT LAND - wrote nonce 0x%08x, read back 0x%08x.\n", nonce, gotNonce)
		fmt.Println("        => Reads work but writes are silently dropped on this board, so the on-eMMC")
		fmt.Println("           breadcrumb can never appear. stage1 needs a different status channel.")
	}
	return nil
}

// runStage1MTKFeed drives stage1's display bring-up in-process inside the resident
// MVIIFlash payload and prints each breadcrumb as it streams back over the same
// VCOM. The bridge is never dropped and the device is never rebooted: the payload
// runs the bring-up, streams progress, and returns to its command loop.
func runStage1MTKFeed(cfg config) error {
	feedCfg := cfg
	if strings.TrimSpace(feedCfg.mtkFeedPayload) == "" {
		feedCfg.mtkFeedPayload = "auto"
	}
	payload, err := loadMTKFeedPayload(feedCfg)
	if err != nil {
		return err
	}

	fmt.Println("MVII stage1 live run: driving display bring-up in-payload over the live VCOM (no reboot)")
	fmt.Printf("Feed payload image: %s (%s, 0x%x bytes), load 0x%x\n",
		filepath.Base(payload.Path), payload.Kind, len(payload.Data), payload.LoadAddr)

	// Always force a fresh upload for run-stage1 by default: this command embeds
	// the frequently-changing stage1 display code, so a resident (stale) payload
	// must be reset to BROM and replaced with the just-built MVIIFlash.bin.
	// When -mtk-feed-reuse is set, skip the reset and talk to the already-
	// resident payload directly (fast iteration when the payload hasn't changed).
	client, err := connectMTKSerialForFeedWithOptions(feedCfg.device, cfg.mtkFeedReuse)
	if err != nil {
		return err
	}
	defer func() {
		_ = client.port.Close()
	}()

	hello := client.feedPayloadHello
	if client.feedPayloadReady {
		fmt.Println("Reusing existing MVIIFlash payload already resident on the device (no upload).")
	} else {
		if err := client.disableMT6592Watchdog(); err != nil {
			fmt.Printf("Warning: could not disable MT6592 watchdog before feed payload boot: %v\n", err)
		}
		if err := client.sendDA(payload.LoadAddr, 0, payload.Data); err != nil {
			return fmt.Errorf("send feed payload to 0x%x: %w", payload.LoadAddr, err)
		}
		if err := client.jumpDA(payload.EntryAddr); err != nil {
			fmt.Printf("JUMP_DA reported %v - waiting for the feed payload protocol anyway.\n", err)
		}
		hello, err = client.waitMTKFeedHello()
		if err != nil {
			return err
		}
		fmt.Printf("Uploaded fresh payload: %s (0x%x bytes)\n", filepath.Base(payload.Path), len(payload.Data))
	}
	if hello.Name != "" {
		fmt.Printf("Feed payload ready: %s\n", hello.Name)
	} else {
		fmt.Println("Feed payload ready.")
	}
	stopInterruptReset := installMTKFeedInterruptReset(client)
	defer stopInterruptReset()

	client.feedQuiet = true
	const seq = uint32(1)
	if err := client.writeMTKFeedFrame(mtkFeedFrameRunStage1, seq, nil); err != nil {
		return fmt.Errorf("send RUN_STAGE1: %w", err)
	}

	fmt.Println("stage1 live breadcrumbs (streamed over USB, no eMMC, no reboot):")
	timeout := envDuration("MVII_MTK_STAGE1_FRAME_TIMEOUT", 30*time.Second)
	for {
		frame, err := client.readMTKFeedFrame(timeout)
		if err != nil {
			return fmt.Errorf("waiting for stage1 stream: %w", err)
		}
		switch frame.Type {
		case mtkFeedFrameStage1Status:
			printMVIIStage1StreamFrame(frame.Payload)
		case mtkFeedFrameLog:
			printMTKFeedLog(frame.Payload)
		case mtkFeedFrameProgress:
			// stage1 does not emit progress frames; ignore if present.
		case mtkFeedFrameDone:
			_, message := parseMTKFeedStatus(frame.Payload)
			if message == "" {
				message = "stage1 run complete"
			}
			fmt.Printf("Done: %s\n", message)
			fmt.Println("VCOM bridge still up; payload is back at its command loop for more reads/runs.")
			return nil
		case mtkFeedFrameError:
			status, message := parseMTKFeedStatus(frame.Payload)
			return fmt.Errorf("payload error 0x%x during stage1 run: %s", status, message)
		default:
			fmt.Printf("  (ignoring frame type 0x%04x seq %d)\n", frame.Type, frame.Seq)
		}
	}
}

// runMTKFeedCommand sends a small ASCII command to MVIIFlash and leaves the
// payload resident afterwards. By default we upload the just-built payload so
// new commands are not hidden behind a stale resident copy; pass
// -mtk-feed-reuse to keep talking to the current live payload.
func runMTKFeedCommand(cfg config, command string) error {
	client, stop, err := openMTKFeedCommandClient(cfg, command)
	if err != nil {
		return err
	}
	defer stop()

	if err := sendMTKFeedCommand(client, 1, command); err != nil {
		return err
	}
	fmt.Println("VCOM bridge still up; payload is back at its command loop.")
	return nil
}

// applyBootFlagOverMTKFeed arms -flag on a board that has no live console.
//
// A board in BROM has not run LK, so the USB device mvii_debug_console.c serves
// does not exist and setMVIIBootFlagOverConsole has nothing to open. The
// resident MVIIFlash payload is the only agent there -- and it can already do
// exactly the two things a boot flag needs, write an eMMC sector and reset,
// which is why the BROM menu was cut down to "debug mode" and "boot" in the
// first place. This just drives those two entries non-interactively:
//
//	debug -> command_debug_mode(): mt6592_dbgflag_arm(CONSOLE), clear the USBDL
//	         latch, watchdog reset
//	boot  -> command_boot_now():   normal boot, no flag armed
//	brom  -> the payload's own reboot request, which sets the USBDL latch so the
//	         board comes back in download mode instead of running LK
//
// Every mode ends in a reset performed by the device, so this is always the last
// thing an invocation does. It is called after the writes, never before: the
// flag is one-shot and a flash-then-reset would consume it.
func applyBootFlagOverMTKFeed(cfg config, mode string) error {
	feedCfg := cfg
	// Talk to whatever is already running. After a bundle flash the payload is
	// still resident -- feedDefaultMVIIBundleMTKSerial ends on closeSession(false),
	// which closes the port without resetting it -- and re-uploading over a VCOM
	// that was reopened a moment ago is the fragile path on this board.
	// openMTKFeedCommandClient falls back to a fresh upload on its own when
	// nothing answers, so this is a preference, not an assumption.
	feedCfg.mtkFeedReuse = true

	var command string
	switch mode {
	case bootFlagDebug:
		command = "debug mode"
	case bootFlagBoot:
		command = "boot"
	case bootFlagBROM:
		command = "" // no menu entry; the payload's reboot request does it
	default:
		return fmt.Errorf("-flag %q: expected %s, %s or %s", mode, bootFlagDebug, bootFlagBoot, bootFlagBROM)
	}

	label := command
	if label == "" {
		label = "reboot into BROM download mode"
	}
	fmt.Printf("\nNo live console on this board, so -flag %s goes through the BROM payload.\n", mode)

	client, stop, err := openMTKFeedCommandClient(feedCfg, label)
	if err != nil {
		return fmt.Errorf("arm -flag %s through the BROM payload: %w", mode, err)
	}
	defer stop()

	if command != "" {
		// The payload answers DONE and *then* resets, so a dropped link right after
		// the ack is the success case and not something to report as a failure.
		if err := sendMTKFeedCommand(client, 1, command); err != nil && !isDeviceGoneError(err) {
			return fmt.Errorf("arm -flag %s: %w", mode, err)
		}
	} else if err := client.requestMTKFeedPayloadRebootWithReason(1, "host requested BROM download mode"); err != nil {
		return fmt.Errorf("arm -flag %s: %w", mode, err)
	}
	return nil
}

func openMTKFeedCommandClient(cfg config, command string) (*mtkSerialClient, func(), error) {
	feedCfg := cfg
	command = strings.TrimSpace(command)
	if command == "" {
		command = "help"
	}
	if strings.TrimSpace(feedCfg.mtkFeedPayload) == "" {
		feedCfg.mtkFeedPayload = "auto"
	}
	payload, err := loadMTKFeedPayload(feedCfg)
	if err != nil {
		return nil, func() {}, err
	}

	fmt.Printf("MVII live payload command: %s\n", command)
	fmt.Printf("Feed payload image: %s (%s, 0x%x bytes), load 0x%x\n",
		filepath.Base(payload.Path), payload.Kind, len(payload.Data), payload.LoadAddr)

	client, err := connectMTKSerialForFeedWithOptions(feedCfg.device, cfg.mtkFeedReuse)
	if err != nil {
		return nil, func() {}, err
	}
	closeOnError := true
	defer func() {
		if closeOnError {
			_ = client.port.Close()
		}
	}()

	hello := client.feedPayloadHello
	if client.feedPayloadReady {
		fmt.Println("Reusing existing MVIIFlash payload; no reset or payload upload needed.")
	} else {
		if err := client.disableMT6592Watchdog(); err != nil {
			fmt.Printf("Warning: could not disable MT6592 watchdog before feed payload boot: %v\n", err)
		}
		if err := client.sendDA(payload.LoadAddr, 0, payload.Data); err != nil {
			return nil, func() {}, fmt.Errorf("send feed payload to 0x%x: %w", payload.LoadAddr, err)
		}
		if err := client.jumpDA(payload.EntryAddr); err != nil {
			fmt.Printf("JUMP_DA reported %v - waiting for the feed payload protocol anyway.\n", err)
		}
		hello, err = client.waitMTKFeedHello()
		if err != nil {
			return nil, func() {}, err
		}
		fmt.Printf("Uploaded fresh payload: %s (0x%x bytes)\n", filepath.Base(payload.Path), len(payload.Data))
	}
	if hello.Name != "" {
		fmt.Printf("Feed payload ready: %s\n", hello.Name)
	} else {
		fmt.Println("Feed payload ready.")
	}
	stopInterruptReset := installMTKFeedInterruptReset(client)
	if hello.BlockSize != 0 && hello.BlockSize != mtkSerialBlockSize {
		stopInterruptReset()
		return nil, func() {}, fmt.Errorf("feed payload reports block size 0x%x, expected 0x%x", hello.BlockSize, mtkSerialBlockSize)
	}

	client.feedQuiet = true
	closeOnError = false
	return client, func() {
		stopInterruptReset()
		_ = client.port.Close()
	}, nil
}

func sendMTKFeedCommand(client *mtkSerialClient, seq uint32, command string) error {
	command = strings.TrimSpace(command)
	if command == "" {
		command = "help"
	}
	if err := client.writeMTKFeedFrame(mtkFeedFrameCommand, seq, []byte(command)); err != nil {
		return fmt.Errorf("send COMMAND %q: %w", command, err)
	}

	fmt.Println("payload command output:")
	timeout := envDuration("MVII_MTK_COMMAND_FRAME_TIMEOUT", 30*time.Second)
	for {
		frame, err := client.readMTKFeedFrame(timeout)
		if err != nil {
			return fmt.Errorf("waiting for command output: %w", err)
		}
		switch frame.Type {
		case mtkFeedFrameLog:
			printMTKFeedLog(frame.Payload)
		case mtkFeedFrameStage1Status:
			printMVIIStage1StreamFrame(frame.Payload)
		case mtkFeedFrameProgress:
			printMTKFeedPayloadProgress(frame.Payload)
		case mtkFeedFrameDone:
			status, message := parseMTKFeedStatus(frame.Payload)
			if status != 0 {
				return fmt.Errorf("payload command %q failed: status=0x%x %s", command, status, message)
			}
			if message == "" {
				message = "command complete"
			}
			fmt.Printf("Done: %s\n", message)
			return nil
		case mtkFeedFrameError:
			status, message := parseMTKFeedStatus(frame.Payload)
			return fmt.Errorf("payload command %q error 0x%x: %s", command, status, message)
		case mtkFeedFrameHello:
			continue
		default:
			fmt.Printf("  (ignoring frame type 0x%04x seq %d)\n", frame.Type, frame.Seq)
		}
	}
}

func runMTKFeedInteractiveMenu(cfg config) error {
	client, stop, err := openMTKFeedCommandClient(cfg, "menu")
	if err != nil {
		return err
	}
	defer stop()

	reader := bufio.NewReader(os.Stdin)
	seq := uint32(1)
	for {
		printMTKFeedInteractiveMenu()
		line, err := reader.ReadString('\n')
		if err != nil && !errors.Is(err, io.EOF) {
			return err
		}
		choice := strings.ToLower(strings.TrimSpace(line))
		command, quit := mtkFeedInteractiveMenuChoice(choice)
		if quit {
			fmt.Println("Leaving payload resident; VCOM bridge is still at the command loop.")
			return nil
		}
		if command == "" {
			fmt.Println("Unknown choice; enter 1 (debug mode), 2 (boot), or q.")
			if errors.Is(err, io.EOF) {
				return nil
			}
			continue
		}
		if err := sendMTKFeedCommand(client, seq, command); err != nil {
			return err
		}
		seq++
		if errors.Is(err, io.EOF) {
			return nil
		}
	}
}

// The BROM menu is deliberately two entries long.
//
// Everything else that used to be here -- probe, panel, peek/poke, pmicr/pmicw,
// led, wifi -- ran on hardware the BROM had not finished setting up, and on this
// board that showed: the live `panel` command reprogrammed the display clocks
// and took the BROM's own USB transport with it (LIBUSB_ERROR_NO_DEVICE mid
// command). Rather than keep a menu of commands that work until they matter, the
// BROM's job is now the one thing it is reliable at -- write a flag to eMMC and
// reset -- and all the diagnostics moved to `flash -dbg`, which talks to the
// same code running on a fully booted board over a USB device we own.
func printMTKFeedInteractiveMenu() {
	fmt.Println()
	fmt.Println("MVII J36 Ultra BROM menu")
	fmt.Println("  1. debug mode   arm the one-shot debug flag, then reboot into the live console")
	fmt.Println("  2. boot         reboot straight into a normal boot")
	fmt.Println("  q. quit         leave the payload resident")
	fmt.Println()
	fmt.Println("  After 1, wait for the board to come back up and run:  flash -dbg")
	fmt.Print("> ")
}

func mtkFeedInteractiveMenuChoice(choice string) (string, bool) {
	switch choice {
	case "1", "debug", "debug mode", "dbg":
		return "debug mode", false
	case "2", "boot":
		return "boot", false
	case "q", "quit", "exit":
		return "", true
	case "", "h", "help", "?":
		return "help", false
	default:
		return "", false
	}
}

// printMVIIStage1StreamFrame renders one streamed stage1 status record as a single
// live-trace line, so a full bring-up reads as a tidy step-by-step log.
func printMVIIStage1StreamFrame(payload []byte) {
	if len(payload) < mviiBootStatusSize {
		fmt.Printf("  stage1: short status frame (0x%x bytes)\n", len(payload))
		return
	}
	stage := binary.LittleEndian.Uint32(payload[12:16])
	flags := binary.LittleEndian.Uint32(payload[16:20])
	sequence := binary.LittleEndian.Uint32(payload[20:24])
	lastError := binary.LittleEndian.Uint32(payload[92:96])
	message := trimMTKFeedString(payload[mviiBootStatusMessageOffset : mviiBootStatusMessageOffset+mviiBootStatusV3MessageSize])

	line := fmt.Sprintf("  [seq %3d] %s (0x%04x)", sequence, mviiBootStatusStageName(stage), stage)
	if lastError != 0 {
		line += " | " + mviiBootStatusMarker(lastError)
	}
	if flags&mviiBootStatusFlagError != 0 {
		line += " [ERROR]"
	}
	if flags&mviiBootStatusFlagComplete != 0 {
		line += " [COMPLETE]"
	}
	if message != "" {
		line += " -- " + message
	}
	fmt.Println(line)
}

func (c *mtkSerialClient) probeMTKFeedImageHeaders(seq uint32, parts []mtkLivePartition) uint32 {
	type imageProbe struct {
		Label  string
		Offset uint64
	}
	var probes []imageProbe
	seen := map[uint64]bool{}
	add := func(label string, offset uint64) {
		if offset >= j36UltraEMMCUserBytes || seen[offset] {
			return
		}
		seen[offset] = true
		probes = append(probes, imageProbe{Label: label, Offset: offset})
	}
	add("scatter fallback LK", j36UltraScatterLKOffset)
	add("scatter fallback BOOTIMG", j36UltraScatterBootImageOffset)
	for _, part := range parts {
		if part.Size < 0x10000 {
			continue
		}
		add(part.Name+" start", part.Offset)
	}

	fmt.Println("Image header probes:")
	for _, probe := range probes {
		data, next, err := c.readMTKFeedBytes(seq, probe.Offset, mtkSerialBlockSize, mtkLegacyEMMCPartUser,
			probe.Label+" header")
		seq = next
		if err != nil {
			fmt.Printf("  %-28s @0x%08x read-error: %v\n", probe.Label, probe.Offset, err)
			continue
		}
		printMTKFeedImageHeaderProbe(probe.Label, probe.Offset, data)
	}
	return seq
}

func printMTKFeedImageHeaderProbe(label string, offset uint64, data []byte) {
	if len(data) < mtkSerialBlockSize {
		fmt.Printf("  %-28s @0x%08x short=0x%x\n", label, offset, len(data))
		return
	}
	word0 := binary.LittleEndian.Uint32(data[0:4])
	word1 := binary.LittleEndian.Uint32(data[4:8])
	if word0 == mtkImageMagic {
		name := trimMTKFeedString(data[8:40])
		if name == "" {
			name = "?"
		}
		fmt.Printf("  %-28s @0x%08x MTK name=%q payload=0x%x\n", label, offset, name, word1)
		return
	}
	if bytes.Equal(data[0:8], []byte("ANDROID!")) {
		kernelSize := binary.LittleEndian.Uint32(data[8:12])
		ramdiskSize := binary.LittleEndian.Uint32(data[16:20])
		secondSize := binary.LittleEndian.Uint32(data[24:28])
		fmt.Printf("  %-28s @0x%08x ANDROID kernel=0x%x ramdisk=0x%x second=0x%x\n",
			label, offset, kernelSize, ramdiskSize, secondSize)
		return
	}
	ascii := printableASCII(data[:32])
	if ascii != "" {
		fmt.Printf("  %-28s @0x%08x w0=0x%08x w1=0x%08x ascii=%q\n", label, offset, word0, word1, ascii)
		return
	}
	fmt.Printf("  %-28s @0x%08x w0=0x%08x w1=0x%08x\n", label, offset, word0, word1)
}

func printableASCII(data []byte) string {
	end := 0
	for end < len(data) {
		b := data[end]
		if b == 0 {
			break
		}
		if b < 0x20 || b > 0x7e {
			return ""
		}
		end++
	}
	if end < 4 {
		return ""
	}
	return string(data[:end])
}

type bootStatusProbeOffset struct {
	Label  string
	Offset uint64
}

func bootStatusProbeOffsets(parts []mtkLivePartition) []bootStatusProbeOffset {
	var out []bootStatusProbeOffset
	seen := map[uint64]bool{}
	add := func(label string, offset uint64) {
		if offset >= j36UltraEMMCUserBytes || seen[offset] {
			return
		}
		seen[offset] = true
		out = append(out, bootStatusProbeOffset{Label: label, Offset: offset})
	}
	for _, part := range parts {
		if !looksLikeLKStatusCandidate(part) {
			continue
		}
		if part.Size >= j36UltraScatterLKSize {
			add(fmt.Sprintf("%s MVII UBOOT-slot boot status", part.Name),
				part.Offset+j36UltraScatterLKSize-mviiBootStatusSize)
		}
		if part.Size >= mviiBootStatusSize {
			add(fmt.Sprintf("%s partition-tail boot status", part.Name),
				part.Offset+part.Size-mviiBootStatusSize)
		}
	}
	return out
}

func looksLikeLKStatusCandidate(part mtkLivePartition) bool {
	name := strings.ToUpper(strings.TrimSpace(part.Name))
	if strings.Contains(name, "UBOOT") || name == "LK" || strings.Contains(name, "LK_") ||
		strings.HasPrefix(name, "LK") {
		return true
	}
	if part.Size == j36UltraScatterLKSize {
		return true
	}
	return strings.HasPrefix(name, "EBR") && part.Size <= 0x100000
}

func planMTKScatterFlash(scatterPath string, entries []mtkScatterEntry) ([]mtkScatterFlashItem, []string, error) {
	dir := filepath.Dir(scatterPath)
	var items []mtkScatterFlashItem
	var skipped []string
	for _, entry := range entries {
		if !entry.IsDownload || entry.FileName == "" || strings.EqualFold(entry.FileName, "NONE") {
			continue
		}
		region := strings.ToUpper(strings.TrimSpace(entry.Region))
		if region != "EMMC_USER" {
			skipped = append(skipped, fmt.Sprintf("%s (%s): use -mtk-write-preloader for BOOT1/preloader regions", entry.PartitionName, region))
			continue
		}
		imagePath := filepath.Join(dir, entry.FileName)
		info, err := os.Stat(imagePath)
		if err != nil {
			if os.IsNotExist(err) {
				skipped = append(skipped, fmt.Sprintf("%s (%s): image %s is missing", entry.PartitionName, region, imagePath))
				continue
			}
			return nil, skipped, fmt.Errorf("scatter entry %s image %s: %w", entry.PartitionName, imagePath, err)
		}
		if info.IsDir() {
			return nil, skipped, fmt.Errorf("scatter entry %s image %s is a directory", entry.PartitionName, imagePath)
		}
		if info.Size() <= 0 {
			return nil, skipped, fmt.Errorf("scatter entry %s image %s is empty", entry.PartitionName, imagePath)
		}
		if entry.LinearStart%mtkSerialBlockSize != 0 {
			return nil, skipped, fmt.Errorf("scatter entry %s offset 0x%x is not 512-byte aligned", entry.PartitionName, entry.LinearStart)
		}
		sparseInfo, isSparse, err := readAndroidSparseImageInfo(imagePath)
		if err != nil {
			return nil, skipped, err
		}
		size := uint64(info.Size())
		transferLength := alignUp(size, mtkSerialBlockSize)
		if isSparse {
			transferLength = sparseInfo.ExpandedSize
		}
		if entry.PartitionSize != 0 && transferLength > entry.PartitionSize {
			return nil, skipped, fmt.Errorf("scatter entry %s transfer length 0x%x exceeds partition size 0x%x", entry.PartitionName, transferLength, entry.PartitionSize)
		}
		items = append(items, mtkScatterFlashItem{
			Entry:          entry,
			Path:           imagePath,
			Size:           size,
			TransferLength: transferLength,
			Part:           mtkLegacyEMMCPartUser,
			Sparse:         sparseInfo,
			IsSparse:       isSparse,
		})
	}
	return items, skipped, nil
}

func confirmMTKScatterFlash(cfg config, scatterPath string, items []mtkScatterFlashItem, skipped []string) error {
	if cfg.yes {
		return nil
	}
	fmt.Printf("About to flash %d scatter image(s) from %s through the native MTK payload on %s.\n",
		len(items), scatterPath, cfg.device)
	for _, item := range items {
		kind := "raw"
		if item.IsSparse {
			kind = "Android sparse"
		}
		fmt.Printf("  %-10s %-16s -> eMMC USER offset 0x%x, length 0x%x (%s)\n",
			item.Entry.PartitionName, filepath.Base(item.Path), item.Entry.LinearStart, item.TransferLength, kind)
	}
	for _, text := range skipped {
		fmt.Printf("  skipping %s\n", text)
	}
	fmt.Println("This can make the target device unbootable if the scatter does not match this board.")
	fmt.Print("Type exactly 'FLASH J36 ULTRA' to continue: ")
	line, err := readLineFromStdin()
	if err != nil {
		return err
	}
	if strings.TrimSpace(line) != "FLASH J36 ULTRA" {
		return errors.New("confirmation did not match; leaving the device untouched")
	}
	return nil
}

func (c *mtkSerialClient) requireBROMForMTKFeed() error {
	if c.isBROM {
		return nil
	}
	return fmt.Errorf("native MVIIFlash feed payload requires BROM mode, but the target is already in preloader mode (BL=0x%02x); power the board fully off and force BROM VCOM, or use the preloader/fastboot path once LK exposes it", c.blVersion)
}

func connectMTKSerialForFeed(device string) (*mtkSerialClient, error) {
	return connectMTKSerialForFeedWithOptions(device, false)
}

func connectMTKSerialForFeedReadOnly(device string) (*mtkSerialClient, error) {
	return connectMTKSerialForFeedWithOptions(device, true)
}

func connectMTKSerialForFeedWithOptions(device string, reuseFeedPayload bool) (*mtkSerialClient, error) {
	client, err := connectMTKSerialWithOptions(device, mtkSerialConnectOptions{
		recoverFeedPayload: !reuseFeedPayload,
		reuseFeedPayload:   reuseFeedPayload,
	})
	if err != nil {
		return nil, err
	}
	if client.feedPayloadReady {
		return client, nil
	}
	if err := client.probeMT6592(); err != nil {
		_ = client.port.Close()
		return nil, err
	}
	if client.isBROM {
		return client, nil
	}
	if envFlag("MVII_MTK_FEED_NO_PRELOADER_BROM_RESET") {
		err := client.requireBROMForMTKFeed()
		_ = client.port.Close()
		return nil, err
	}

	oldDevice := client.device
	commandTimeout := client.commandTimeout
	writeTimeout := client.writeTimeout
	fmt.Printf("Target is in preloader mode (BL=0x%02x); requesting reset back into BROM.\n", client.blVersion)
	if err := client.resetPreloaderToBROM(); err != nil {
		_ = client.port.Close()
		return nil, err
	}
	_ = client.port.Close()
	time.Sleep(1200 * time.Millisecond)
	return reconnectMTKSerialBROMAfterPreloaderReset(oldDevice, commandTimeout, writeTimeout)
}

func reconnectMTKSerialBROMAfterPreloaderReset(device string, commandTimeout, writeTimeout time.Duration) (*mtkSerialClient, error) {
	// On macOS the libusb transport owns the device, so the kernel tty node is
	// either gone or "resource busy"; reconnect over libusb instead.
	if client, attempted, err := tryReconnectMTKUSBBROM(device, commandTimeout, writeTimeout); attempted {
		return client, err
	}
	timeout := envDuration("MVII_MTK_BROM_RECONNECT_TIMEOUT", envDuration("MVII_MTK_SERIAL_RECONNECT_TIMEOUT", mtkSerialReconnectTimeout))
	deadline := time.Now().Add(timeout)
	fmt.Printf("Reopening MTK BROM serial device after preloader reset (timeout %s)\n", timeout)
	var lastErr error
	lastReport := time.Time{}
	for time.Now().Before(deadline) {
		for _, candidate := range mtkSerialReconnectCandidates(device) {
			port, err := openMTKSerialPort(candidate, 115200)
			if err != nil {
				lastErr = fmt.Errorf("%s: %w", candidate, err)
				continue
			}
			_ = port.DiscardInput(10 * time.Millisecond)
			client := &mtkSerialClient{
				port:           port,
				device:         candidate,
				commandTimeout: commandTimeout,
				writeTimeout:   writeTimeout,
			}
			handshakeDeadline := time.Now().Add(3 * time.Second)
			if handshakeDeadline.After(deadline) {
				handshakeDeadline = deadline
			}
			if err := client.handshake(handshakeDeadline); err != nil {
				_ = port.Close()
				lastErr = fmt.Errorf("%s: %w", candidate, err)
				continue
			}
			fmt.Println("MTK serial handshake successful.")
			_ = client.port.DiscardInput(0)
			if err := client.probeMT6592(); err != nil {
				_ = port.Close()
				lastErr = fmt.Errorf("%s: %w", candidate, err)
				continue
			}
			if !client.isBROM {
				_ = port.Close()
				lastErr = fmt.Errorf("%s: target returned to preloader instead of BROM", candidate)
				time.Sleep(500 * time.Millisecond)
				continue
			}
			fmt.Printf("Reconnected to MTK BROM on %s.\n", candidate)
			return client, nil
		}
		if now := time.Now(); now.Sub(lastReport) >= 2*time.Second {
			remaining := time.Until(deadline).Round(time.Second)
			if lastErr != nil {
				errs := lastErr.Error()
				if strings.Contains(errs, "no such file or directory") || strings.Contains(errs, "device not configured") {
					fmt.Printf("Waiting for MTK BROM after preloader reset (%s left)\n", remaining)
				} else {
					fmt.Printf("Waiting for MTK BROM after preloader reset (%s left): %v\n", remaining, lastErr)
				}
			} else {
				fmt.Printf("Waiting for MTK BROM after preloader reset (%s left)\n", remaining)
			}
			lastReport = now
		}
		time.Sleep(200 * time.Millisecond)
	}
	if lastErr == nil {
		lastErr = errors.New("no BROM handshake response")
	}
	return nil, fmt.Errorf("MTK BROM reconnect timed out after preloader reset: %w", lastErr)
}

func planDefaultMTKFeedBootChain(cfg config) ([]mtkFeedWritePlan, error) {
	if cfg.mtkFeedHandoff {
		return nil, mtkFeedHandoffDisabledError()
	}
	if cfg.rawOffset != "" {
		return nil, errors.New("default MVII feed boot chain plans its own offsets; remove -raw-offset or pass an explicit image")
	}
	if cfg.rawLength != "" && !isMTKFeedAutoRawLength(cfg.rawLength) {
		return nil, errors.New("default MVII feed boot chain uses minimal per-image lengths; remove explicit -raw-length or pass an explicit image")
	}

	target := effectiveUploadTarget(cfg)
	if !validUploadTarget(target) {
		return nil, invalidUploadTargetError(cfg.upload)
	}

	components, err := uploadFeedComponents(cfg, target)
	if err != nil {
		return nil, err
	}
	if len(components) == 0 {
		return nil, fmt.Errorf("-upload %s selects nothing to write", target)
	}

	plans := make([]mtkFeedWritePlan, 0, len(components))
	for i, c := range components {
		// The reboot request rides on the LAST write of whatever this run
		// selected, exactly as it used to ride on boot.img in the full bundle --
		// so the board comes back up on the new image without a separate power
		// cycle. That is the difference between "small iterations" and "small
		// iterations plus a trip to the power button".
		last := i == len(components)-1

		planCfg := cfg
		if c.RawOffset != "" {
			planCfg.rawOffset = c.RawOffset
		}
		plan, err := planMTKFeedWrite(planCfg, c.Path, c.Info, last && cfg.mtkFeedReboot)
		if err != nil {
			return nil, err
		}
		if c.PartitionName != "" {
			plan.PartitionName = c.PartitionName
		}
		if c.PartitionDelta != 0 {
			plan.PartitionDelta = c.PartitionDelta
		}
		if c.LabelSuffix != "" {
			plan.Label += " " + c.LabelSuffix
		}
		plans = append(plans, plan)
	}
	return plans, nil
}

// One image the selected -upload target writes, resolved and checked, in write
// order. Split out from the planner so "which files" and "where they go" are one
// list rather than a chain of conditionals.
type uploadFeedComponent struct {
	Path           string
	Info           os.FileInfo
	Kind           string
	PartitionName  string
	PartitionDelta uint64
	RawOffset      string
	LabelSuffix    string
}

func uploadFeedComponents(cfg config, target string) ([]uploadFeedComponent, error) {
	var out []uploadFeedComponent

	add := func(c uploadFeedComponent) error {
		info, err := os.Stat(c.Path)
		if err != nil {
			return fmt.Errorf("%s %s: %w", c.Kind, c.Path, err)
		}
		if info.IsDir() {
			return fmt.Errorf("%s is a directory, not %s", c.Path, c.Kind)
		}
		c.Info = info
		out = append(out, c)
		return nil
	}

	if uploadWritesReleaseLK(target) {
		image := defaultMVIIReleaseLKImagePath(cfg)
		if !fileExists(image) {
			return nil, fmt.Errorf("-upload release needs %s, the SD-handoff bootloader, and it is not in this package. "+
				"It is built alongside lk.bin by the mvii-armv7-pc target, so a package without it predates the "+
				"SD hand-off; rebuild and re-run. It goes to UBOOT in place of lk.bin", image)
		}
		if err := add(uploadFeedComponent{
			Path:        image,
			Kind:        "the MVII release (SD-handoff) LK image",
			LabelSuffix: "(UBOOT/SD handoff)",
		}); err != nil {
			return nil, err
		}
	}
	if uploadWritesStockSlotLK(target) {
		if err := add(uploadFeedComponent{
			Path: defaultMVIILKImagePath(cfg),
			Kind: "the MVII LK image",
		}); err != nil {
			return nil, err
		}
	}
	if uploadWritesSystem(target) {
		if err := add(uploadFeedComponent{
			Path:           defaultMVIIS1ImagePath(cfg),
			Kind:           "the MVII stage1 image",
			PartitionName:  "BOOTIMG",
			PartitionDelta: j36UltraBootImageStage1Offset,
			RawOffset:      fmt.Sprintf("0x%x", j36UltraScatterBootImageOffset+j36UltraBootImageStage1Offset),
			LabelSuffix:    "(BOOTIMG/kernel payload)",
		}); err != nil {
			return nil, err
		}
		if err := add(uploadFeedComponent{
			Path: defaultMVIIArmImagePath(cfg),
			Kind: "the MVII boot image",
		}); err != nil {
			return nil, err
		}
	}
	if uploadWritesAssets(target) {
		image := defaultMVIIAssetImagePath(cfg)
		if err := add(uploadFeedComponent{
			Path:          image,
			Kind:          "the MVII asset slot",
			PartitionName: "LOGO",
		}); err != nil {
			return nil, err
		}
		if info := out[len(out)-1].Info; info.Size() < 32 {
			return nil, fmt.Errorf("%s is %d bytes: the generator wrote a stub, which means it had no Pillow or no art. "+
				"Rebuild and check the asset-slot line in the build log", image, info.Size())
		}
	}
	return out, nil
}

func feedDefaultMVIIBundleMTKSerial(cfg config) error {
	plans, err := planDefaultMTKFeedBootChain(cfg)
	if err != nil {
		return err
	}
	profile, err := configuredMTKPreloaderProfile(cfg)
	if err != nil {
		return err
	}
	if profile != nil {
		fmt.Println("Applying explicit MTK preloader profile over scatter defaults.")
		applyMTKPreloaderProfileOffsets(plans, profile)
	}

	payload, err := loadMTKFeedPayload(cfg)
	if err != nil {
		return err
	}
	chunkSize, err := mtkFeedChunkSize(cfg)
	if err != nil {
		return err
	}
	if err := confirmMTKFeedBundleFlash(cfg, plans); err != nil {
		return err
	}
	fmt.Println("Default MVII feed uses concrete scatter offsets; it does not probe live PMT before writing.")
	for i, plan := range plans {
		fmt.Printf("  planned write %d/%d: %s -> eMMC part 0x%x offset 0x%x length 0x%x\n",
			i+1, len(plans), plan.Label, plan.Part, plan.TargetOffset, plan.TransferLength)
	}

	fmt.Printf("Booting MTK feed payload: %s (%s, 0x%x bytes)\n", payload.Path, payload.Kind, len(payload.Data))
	fmt.Printf("Loading payload to 0x%x, entry 0x%x\n", payload.LoadAddr, payload.EntryAddr)
	written := describeFeedPlans(plans)
	if cfg.mtkFeedReboot {
		fmt.Printf("Default MVII feed writes %s, then sends the explicit reboot request.\n", written)
	} else {
		fmt.Printf("Default MVII feed writes %s, then stops at flash complete.\n", written)
	}
	fmt.Println("MTK feed mode expects the J36 Ultra powered off or in BROM/preloader VCOM mode.")

	// The resident MVIIFlash payload can complete repeated START/DATA/FINISH
	// transactions without disconnecting. Keep each transaction at or below the
	// proven 1 MiB transport limit, but do not reboot/re-upload the payload between
	// segments. Resetting between segments is what leaves this MT6592 board in the
	// fragile preloader/BROM reconnect loop.
	sessionLimit, err := mtkFeedSingleSessionLimit()
	if err != nil {
		return err
	}

	var client *mtkSerialClient
	var stopInterruptReset func()
	seq := uint32(1)

	closeSession := func(resetPayload bool) {
		if client == nil {
			return
		}
		if resetPayload {
			if err := client.requestMTKFeedPayloadReboot(seq); err != nil {
				fmt.Printf("Warning: could not reset feed payload between bundle sessions: %v\n", err)
			}
		}
		if stopInterruptReset != nil {
			stopInterruptReset()
			stopInterruptReset = nil
		}
		_ = client.port.Close()
		client = nil
	}
	defer closeSession(false)

	ensureSession := func() error {
		if client != nil {
			return nil
		}
		c, hello, stop, err := startMTKFeedPayloadSession(cfg, payload)
		if err != nil {
			return err
		}
		if hello.BlockSize != 0 && hello.BlockSize != mtkSerialBlockSize {
			stop()
			_ = c.port.Close()
			return fmt.Errorf("feed payload reports block size 0x%x, expected 0x%x", hello.BlockSize, mtkSerialBlockSize)
		}
		if cs, err := negotiateFeedChunkSize(hello, chunkSize); err != nil {
			stop()
			_ = c.port.Close()
			return err
		} else {
			chunkSize = cs
		}
		client = c
		stopInterruptReset = stop
		seq = 1
		return nil
	}

	writeRawSegment := func(plan mtkFeedWritePlan, segment feedSession, finalSegment bool, segmentCount int) error {
		segTarget := plan.TargetOffset + segment.Start
		// Per-flash completion flags belong on the segment that completes the
		// image; earlier fallback segments must not reboot or mark boot status.
		flags := plan.Flags &^ (mtkFeedFlagBootAfterFlash | mtkFeedFlagRebootAfterFlash |
			mtkFeedFlagBootStatus | mtkFeedFlagEnableBoot1)
		var segHash [sha256.Size]byte
		if finalSegment {
			flags = plan.Flags
			if segmentCount == 1 {
				segHash = plan.Hash
			}
		}

		if err := ensureSession(); err != nil {
			return err
		}
		start := buildMTKFeedStartPayload(segTarget, segment.Length, segment.Length,
			uint32(plan.Part), mtkSerialBlockSize, uint32(chunkSize), flags, segHash)
		if err := client.writeMTKFeedFrame(mtkFeedFrameStart, seq, start); err != nil {
			return fmt.Errorf("send feed START for %s: %w", plan.Label, err)
		}
		if err := client.waitMTKFeedAck(seq, "START "+plan.Label, false); err != nil {
			return err
		}
		seq++
		var err error
		seq, err = client.streamMTKFeedImageRange(plan.Path, plan.SourceSize, segTarget,
			segment.Start, segment.Length, chunkSize, seq)
		if err != nil {
			return err
		}
		finish := buildMTKFeedFinishPayload(segment.Length, segHash)
		if err := client.writeMTKFeedFrame(mtkFeedFrameFinish, seq, finish); err != nil {
			return fmt.Errorf("send feed FINISH for %s: %w", plan.Label, err)
		}
		if err := client.waitMTKFeedAck(seq, "FINISH "+plan.Label, true); err != nil {
			return err
		}
		seq++
		return nil
	}

	writeBoundedSegmentWithRetry := func(plan mtkFeedWritePlan, segment feedSession, segmentIndex, segmentCount int) error {
		finalSegment := segmentIndex == segmentCount-1
		var segErr error
		for attempt := 1; attempt <= mtkFeedSegmentAttempts; attempt++ {
			segErr = writeRawSegment(plan, segment, finalSegment, segmentCount)
			if segErr == nil {
				break
			}
			closeSession(false) // transport state is unknown; reconnect fresh
			if attempt < mtkFeedSegmentAttempts {
				fmt.Printf("Segment write failed (%v); retrying %s segment %d/%d (attempt %d/%d)\n",
					segErr, plan.Label, segmentIndex+1, segmentCount, attempt+1, mtkFeedSegmentAttempts)
				time.Sleep(2 * time.Second)
			}
		}
		return segErr
	}

	for i, plan := range plans {
		fmt.Printf("Default write %d/%d: %s -> eMMC part 0x%x offset 0x%x, length 0x%x, chunk 0x%x, flags 0x%x, SHA256 %x\n",
			i+1, len(plans), plan.Label, plan.Part, plan.TargetOffset, plan.TransferLength, chunkSize, plan.Flags, plan.Hash[:])
		if plan.LengthNote != "" {
			fmt.Println(plan.LengthNote)
		}

		if plan.IsSparse {
			// Sparse images keep the single-session streamer (not used by the
			// default 3-image bundle).
			if err := ensureSession(); err != nil {
				return err
			}
			start := buildMTKFeedStartPayload(plan.TargetOffset, plan.ImageSize, plan.TransferLength,
				uint32(plan.Part), mtkSerialBlockSize, uint32(chunkSize), plan.Flags, plan.Hash)
			if err := client.writeMTKFeedFrame(mtkFeedFrameStart, seq, start); err != nil {
				return fmt.Errorf("send feed START for %s: %w", plan.Label, err)
			}
			if err := client.waitMTKFeedAck(seq, "START "+plan.Label, false); err != nil {
				return err
			}
			seq++
			seq, err = client.streamMTKFeedSparseImage(plan.Path, plan.Sparse, plan.TargetOffset, plan.TransferLength, chunkSize, seq)
			if err != nil {
				return err
			}
			finish := buildMTKFeedFinishPayload(plan.TransferLength, plan.Hash)
			if err := client.writeMTKFeedFrame(mtkFeedFrameFinish, seq, finish); err != nil {
				return fmt.Errorf("send feed FINISH for %s: %w", plan.Label, err)
			}
			if err := client.waitMTKFeedAck(seq, "FINISH "+plan.Label, true); err != nil {
				return err
			}
			seq++
			continue
		}

		segments := planFeedSessions(plan.TransferLength, sessionLimit)
		if len(segments) > 1 {
			fmt.Printf("  streaming %s as %d resident-payload transaction(s) of up to 0x%x; USB stays connected\n",
				plan.Label, len(segments), sessionLimit)
		}
		for si, segment := range segments {
			if err := writeBoundedSegmentWithRetry(plan, segment, si, len(segments)); err != nil {
				return err
			}
		}
	}
	closeSession(false)
	if cfg.mtkFeedReboot {
		fmt.Println("Explicit watchdog reset was requested after the bundled LK write.")
	}
	followDevice, err := bootPreloaderAfterMTKFeed(cfg)
	if err != nil {
		return err
	}
	if followDevice != "" {
		cfg.device = followDevice
	}
	if err := followMTKFeedBoot(cfg); err != nil {
		return err
	}
	fmt.Println("flash complete")
	return nil
}

func applyDefaultMTKFeedReboot(cfg *config, image string) string {
	if cfg == nil || cfg.mtkFeedRebootSet || cfg.mtkFeedReboot {
		return ""
	}
	if isJ36BootSlotImagePath(*cfg, image) {
		cfg.mtkFeedReboot = true
		return "BOOTIMG image: rebooting into the flashed stock-LK boot image after write (pass -mtk-feed-reboot=false to stage without booting)."
	}
	if isJ36LKSlotImagePath(*cfg, image) {
		cfg.mtkFeedReboot = true
		return "UBOOT-slot image: rebooting into the flashed MVII slot image after write (pass -mtk-feed-reboot=false to stage without booting)."
	}
	return ""
}

func planMTKFeedWrite(cfg config, image string, imageInfo os.FileInfo, rebootAfterWrite bool) (mtkFeedWritePlan, error) {
	if imageInfo.Size() < 0 {
		return mtkFeedWritePlan{}, fmt.Errorf("image size is invalid: %d", imageInfo.Size())
	}
	sourceSize := uint64(imageInfo.Size())
	emmcPart, err := parseMTKFeedPart(cfg.mtkFeedPart)
	if err != nil {
		return mtkFeedWritePlan{}, err
	}
	targetOffset, _, err := mtkFeedTargetOffset(cfg, image, emmcPart)
	if err != nil {
		return mtkFeedWritePlan{}, err
	}
	if targetOffset%mtkSerialBlockSize != 0 {
		return mtkFeedWritePlan{}, fmt.Errorf("-raw-offset 0x%x must be 512-byte aligned for eMMC writes", targetOffset)
	}
	if cfg.mtkFeedHandoff {
		return mtkFeedWritePlan{}, mtkFeedHandoffDisabledError()
	}
	if err := validateMTKFeedImageForTarget(image); err != nil {
		return mtkFeedWritePlan{}, err
	}
	sparseInfo, sparse, err := readAndroidSparseImageInfo(image)
	if err != nil {
		return mtkFeedWritePlan{}, err
	}
	readSize := sourceSize
	imageSize := sourceSize
	transferLength := uint64(0)
	autoLength := false
	lengthNote := ""
	if sparse {
		imageSize = sparseInfo.ExpandedSize
		transferLength, autoLength, err = mtkFeedSparseTransferLength(sparseInfo.ExpandedSize, cfg.rawLength)
	} else {
		readSize, imageSize, transferLength, autoLength, lengthNote, err =
			mtkFeedRawTransferPlan(image, sourceSize, cfg.rawLength)
	}
	if err != nil {
		return mtkFeedWritePlan{}, err
	}

	feedFlags := uint32(0)
	if rebootAfterWrite {
		feedFlags |= mtkFeedFlagRebootAfterFlash
		if isJ36LKSlotImagePath(cfg, image) {
			feedFlags |= mtkFeedFlagBootStatus
		}
	}
	feedFlags |= cfg.mtkFeedExtraFlags
	if sparse {
		feedFlags |= mtkFeedFlagSparseStream
	}

	if rebootAfterWrite && isJ36LKSlotImagePath(cfg, image) && transferLength > j36UltraScatterLKSize {
		return mtkFeedWritePlan{}, fmt.Errorf("%s transfer length 0x%x exceeds the J36 Ultra UBOOT slot size 0x%x",
			image, transferLength, j36UltraScatterLKSize)
	}
	if emmcPart == mtkLegacyEMMCPartUser {
		if err := validateMTKFeedOffsetAgainstScatter(image, targetOffset, transferLength); err != nil {
			return mtkFeedWritePlan{}, err
		}
		if err := validateJ36StockScatterFeedTarget(cfg, image, targetOffset, transferLength); err != nil {
			return mtkFeedWritePlan{}, err
		}
	}

	hashLength := transferLength
	if sparse {
		hashLength = alignUp(sourceSize, mtkSerialBlockSize)
	}
	streamHash, err := hashPaddedFile(image, readSize, hashLength)
	if err != nil {
		return mtkFeedWritePlan{}, err
	}

	label := filepath.Base(image)
	partitionName := ""
	if isJ36BootSlotImagePath(cfg, image) {
		partitionName = "BOOTIMG"
		label += " (BOOTIMG/stage2)"
	} else if isJ36LKSlotImagePath(cfg, image) {
		partitionName = "UBOOT"
		label += " (postponed UBOOT-slot image)"
	}
	return mtkFeedWritePlan{
		Label:          label,
		PartitionName:  partitionName,
		Path:           image,
		SourceSize:     readSize,
		FileSize:       sourceSize,
		ImageSize:      imageSize,
		TransferLength: transferLength,
		TargetOffset:   targetOffset,
		Part:           emmcPart,
		Flags:          feedFlags,
		Hash:           streamHash,
		Sparse:         sparseInfo,
		IsSparse:       sparse,
		AutoLength:     autoLength,
		LengthNote:     lengthNote,
	}, nil
}

// "lk.bin, MVIIS1.bin and boot.img" -- built from the plans rather than from the
// selected target, so the sentence and the writes cannot disagree.
func describeFeedPlans(plans []mtkFeedWritePlan) string {
	names := make([]string, 0, len(plans))
	for _, plan := range plans {
		names = append(names, filepath.Base(plan.Path))
	}
	switch len(names) {
	case 0:
		return "nothing"
	case 1:
		return names[0]
	default:
		return strings.Join(names[:len(names)-1], ", ") + " and " + names[len(names)-1]
	}
}

func confirmMTKFeedBundleFlash(cfg config, plans []mtkFeedWritePlan) error {
	if cfg.yes {
		return nil
	}
	fmt.Printf("About to flash -upload %s through the native MTK payload on %s.\n",
		effectiveUploadTarget(cfg), cfg.device)
	for _, plan := range plans {
		fmt.Printf("  %-28s -> eMMC part 0x%x offset 0x%x, length 0x%x (%s)\n",
			plan.Label, plan.Part, plan.TargetOffset, plan.TransferLength, formatBytes(plan.TransferLength))
	}
	fmt.Printf("This writes %s, in that order, and nothing else.\n", describeFeedPlans(plans))
	fmt.Println("This can make the target device unbootable if the stock scatter offsets are wrong.")
	fmt.Print("Type exactly 'FLASH J36 ULTRA' to continue: ")
	line, err := readLineFromStdin()
	if err != nil {
		return err
	}
	if strings.TrimSpace(line) != "FLASH J36 ULTRA" {
		return errors.New("confirmation did not match; leaving the device untouched")
	}
	return nil
}

func feedPayloadMTKSerial(cfg config, image string, imageInfo os.FileInfo) error {
	if imageInfo.Size() < 0 {
		return fmt.Errorf("image size is invalid: %d", imageInfo.Size())
	}
	sourceSize := uint64(imageInfo.Size())
	emmcPart, err := parseMTKFeedPart(cfg.mtkFeedPart)
	if err != nil {
		return err
	}
	targetOffset, defaultedOffset, err := mtkFeedTargetOffset(cfg, image, emmcPart)
	if err != nil {
		return err
	}
	if defaultedOffset {
		cfg.rawOffset = fmt.Sprintf("0x%x", targetOffset)
		if strings.TrimSpace(cfg.mtkPreloadProfile) != "" {
			fmt.Printf("Using MTK preloader profile raw offset 0x%x.\n", targetOffset)
		} else if isJ36LKSlotImagePath(cfg, image) {
			fmt.Printf("Using J36 Ultra stock scatter UBOOT/LK raw offset 0x%x.\n", targetOffset)
		} else {
			fmt.Printf("Using J36 Ultra stock scatter BOOTIMG raw offset 0x%x.\n", targetOffset)
		}
	}
	if targetOffset%mtkSerialBlockSize != 0 {
		return fmt.Errorf("-raw-offset 0x%x must be 512-byte aligned for eMMC writes", targetOffset)
	}
	if cfg.mtkFeedHandoff {
		return mtkFeedHandoffDisabledError()
	}
	if err := validateMTKFeedImageForTarget(image); err != nil {
		return err
	}

	if msg := applyDefaultMTKFeedReboot(&cfg, image); msg != "" {
		fmt.Println(msg)
	}

	sparseInfo, sparse, err := readAndroidSparseImageInfo(image)
	if err != nil {
		return err
	}
	readSize := sourceSize
	imageSize := sourceSize
	transferLength := uint64(0)
	autoLength := false
	lengthNote := ""
	if sparse {
		imageSize = sparseInfo.ExpandedSize
		transferLength, autoLength, err = mtkFeedSparseTransferLength(sparseInfo.ExpandedSize, cfg.rawLength)
	} else {
		readSize, imageSize, transferLength, autoLength, lengthNote, err =
			mtkFeedRawTransferPlan(image, sourceSize, cfg.rawLength)
	}
	if err != nil {
		return err
	}
	if cfg.mtkFeedReboot && isJ36LKSlotImagePath(cfg, image) && transferLength > j36UltraScatterLKSize {
		return fmt.Errorf("%s transfer length 0x%x exceeds the J36 Ultra UBOOT slot size 0x%x",
			image, transferLength, j36UltraScatterLKSize)
	}
	if autoLength {
		if sparse {
			fmt.Printf("Using Android sparse expanded transfer length 0x%x from source size 0x%x.\n",
				transferLength, sourceSize)
		} else if transferLength != sourceSize {
			fmt.Printf("Using minimal feed transfer length 0x%x: image size 0x%x padded to 512-byte eMMC alignment.\n",
				transferLength, sourceSize)
		} else {
			fmt.Printf("Using minimal feed transfer length 0x%x.\n", transferLength)
		}
	}
	chunkSize, err := mtkFeedChunkSize(cfg)
	if err != nil {
		return err
	}
	feedFlags := uint32(0)
	if cfg.mtkFeedReboot {
		feedFlags |= mtkFeedFlagRebootAfterFlash
		if isJ36LKSlotImagePath(cfg, image) {
			feedFlags |= mtkFeedFlagBootStatus
		}
	}
	feedFlags |= cfg.mtkFeedExtraFlags
	if sparse {
		feedFlags |= mtkFeedFlagSparseStream
	}
	if emmcPart == mtkLegacyEMMCPartUser {
		if err := validateMTKFeedOffsetAgainstScatter(image, targetOffset, transferLength); err != nil {
			return err
		}
		if err := validateJ36StockScatterFeedTarget(cfg, image, targetOffset, transferLength); err != nil {
			return err
		}
	}
	payload, err := loadMTKFeedPayload(cfg)
	if err != nil {
		return err
	}
	hashLength := transferLength
	if sparse {
		hashLength = alignUp(sourceSize, mtkSerialBlockSize)
	}
	streamHash, err := hashPaddedFile(image, readSize, hashLength)
	if err != nil {
		return err
	}

	if err := confirmRawFlash(cfg, fmt.Sprintf("MediaTek native payload on %s eMMC part 0x%x", cfg.device, emmcPart), image, cfg.rawOffset, fmt.Sprintf("0x%x", transferLength)); err != nil {
		return err
	}

	fmt.Printf("Booting MTK feed payload: %s (%s, 0x%x bytes)\n", payload.Path, payload.Kind, len(payload.Data))
	fmt.Printf("Loading payload to 0x%x, entry 0x%x\n", payload.LoadAddr, payload.EntryAddr)
	fmt.Printf("Streaming %s to eMMC part 0x%x offset 0x%x, chunk 0x%x, flags 0x%x, SHA256 %x\n",
		formatBytes(transferLength), emmcPart, targetOffset, chunkSize, feedFlags, streamHash[:])
	if lengthNote != "" {
		fmt.Println(lengthNote)
	}
	if feedFlags&mtkFeedFlagEnableBoot1 != 0 {
		fmt.Println("After the write, the payload will set eMMC BOOT1 as the hardware boot partition.")
	}
	if cfg.mtkFeedReboot {
		fmt.Println("After the write, the payload will watchdog-reset so the flashed image boots.")
		if isJ36LKSlotImagePath(cfg, image) {
			fmt.Println("Note: the postponed MVII UBOOT-slot image runs before Android boot.img is parsed.")
		}
		if isDefaultMVIIArmImage(cfg, image) {
			fmt.Println("Note: the default boot.img is an MVII bring-up image, not stock Android.")
		}
	}
	fmt.Println("MTK feed mode expects the J36 Ultra powered off or in BROM/preloader VCOM mode.")

	// The MT6592 BROM VCOM cannot sustain a single multi-megabyte feed session
	// (the bulk pipe drops with LIBUSB_ERROR_IO part-way, e.g. a 5 MiB boot.img
	// dies near 2.3 MiB). Large raw images are streamed in bounded sessions with a
	// payload reset between them, the same way the scatter path already survives
	// big writes. Sparse images keep the single-session streamer.
	if !sparse {
		sessionLimit, err := mtkFeedSingleSessionLimit()
		if err != nil {
			return err
		}
		if transferLength > sessionLimit {
			return feedPayloadMTKSerialSegmented(cfg, payload, image, readSize, targetOffset,
				transferLength, emmcPart, chunkSize, cfg.mtkFeedExtraFlags, sessionLimit)
		}
	}

	client, err := connectMTKSerialForFeed(cfg.device)
	if err != nil {
		return err
	}
	defer func() {
		_ = client.port.Close()
	}()
	if err := client.disableMT6592Watchdog(); err != nil {
		fmt.Printf("Warning: could not disable MT6592 watchdog before feed payload boot: %v\n", err)
	}
	if err := client.sendDA(payload.LoadAddr, 0, payload.Data); err != nil {
		if payload.Kind == "elf32-arm" {
			return fmt.Errorf("send feed payload to 0x%x: %w (BROM rejected the ELF load span; use -mtk-feed-payload auto or boot/MVIIFlash.bin)", payload.LoadAddr, err)
		}
		return fmt.Errorf("send feed payload to 0x%x: %w", payload.LoadAddr, err)
	}
	if err := client.jumpDA(payload.EntryAddr); err != nil {
		fmt.Printf("JUMP_DA reported %v - waiting for the feed payload protocol anyway.\n", err)
	}

	hello, err := client.waitMTKFeedHello()
	if err != nil {
		return err
	}
	if hello.Name != "" {
		fmt.Printf("Feed payload ready: %s\n", hello.Name)
	} else {
		fmt.Println("Feed payload ready.")
	}
	stopInterruptReset := installMTKFeedInterruptReset(client)
	defer stopInterruptReset()
	if hello.BlockSize != 0 && hello.BlockSize != mtkSerialBlockSize {
		return fmt.Errorf("feed payload reports block size 0x%x, expected 0x%x", hello.BlockSize, mtkSerialBlockSize)
	}
	if cs, err := negotiateFeedChunkSize(hello, chunkSize); err != nil {
		return err
	} else {
		chunkSize = cs
	}

	seq := uint32(1)
	if isJ36LKSlotImagePath(cfg, image) {
		if seq, err = client.zeroMTKFeedBootStatus(seq, chunkSize); err != nil {
			return fmt.Errorf("clear MVII boot-status sector: %w", err)
		}
	}
	start := buildMTKFeedStartPayload(targetOffset, imageSize, transferLength, uint32(emmcPart), mtkSerialBlockSize, uint32(chunkSize), feedFlags, streamHash)
	if err := client.writeMTKFeedFrame(mtkFeedFrameStart, seq, start); err != nil {
		return fmt.Errorf("send feed START: %w", err)
	}
	if err := client.waitMTKFeedAck(seq, "START", false); err != nil {
		return err
	}
	seq++
	if sparse {
		seq, err = client.streamMTKFeedSparseImage(image, sparseInfo, targetOffset, transferLength, chunkSize, seq)
	} else {
		seq, err = client.streamMTKFeedImage(image, readSize, targetOffset, transferLength, chunkSize, seq)
	}
	if err != nil {
		return err
	}
	finish := buildMTKFeedFinishPayload(transferLength, streamHash)
	if err := client.writeMTKFeedFrame(mtkFeedFrameFinish, seq, finish); err != nil {
		return fmt.Errorf("send feed FINISH: %w", err)
	}
	if err := client.waitMTKFeedAck(seq, "FINISH", true); err != nil {
		return err
	}
	seq++
	// When rebooting, the payload's FINISH handler writes the pending boot-status
	// marker itself (FEED_FLAG_BOOT_STATUS) and immediately watchdog-resets, so a
	// host-side pending write here would race the reboot and fail. Only write it
	// from the host on the staged (no-reboot) path.
	if isJ36LKSlotImagePath(cfg, image) && !cfg.mtkFeedReboot {
		seq, err = client.writeMTKFeedLKBootStatusPending(seq, image, chunkSize)
		if err != nil {
			return fmt.Errorf("write MVII boot-status flash marker: %w", err)
		}
	}
	if cfg.mtkFeedReboot {
		fmt.Println("Watchdog reset armed; the board reboots into the flashed image now.")
		if isJ36LKSlotImagePath(cfg, image) {
			fmt.Println("Watch the panel: the MVII LK drives the display, then hands back to BROM on its own.")
			fmt.Println("Next: run  ./flash -device " + cfg.device + " -mtk-read-boot-status  to read this run's fresh MVII telemetry.")
		}
	}
	if err := client.port.Close(); err != nil {
		fmt.Printf("Warning: close feed serial port before post-feed boot: %v\n", err)
	}
	followDevice, err := bootPreloaderAfterMTKFeed(cfg)
	if err != nil {
		return err
	}
	if followDevice != "" {
		cfg.device = followDevice
	}
	if err := followMTKFeedBoot(cfg); err != nil {
		return err
	}
	fmt.Println("flash complete")
	return nil
}

// feedPayloadMTKSerialSegmented streams a large raw image to eMMC as a series of
// bounded feed sessions, resetting and reconnecting the MVIIFlash payload between
// them. This mirrors the scatter path (flashMTKScatterSegment): the MT6592 BROM
// VCOM cannot hold a single multi-megabyte session open, so each session writes
// its own sub-range with a fresh START/FINISH and the payload is watchdog-reset to
// BROM in between. Only the final session carries the reboot/boot-status flags.
func feedPayloadMTKSerialSegmented(cfg config, payload mtkFeedPayloadImage, image string,
	readSize, targetOffset, transferLength uint64, emmcPart byte, chunkSize int,
	extraFlags uint32, sessionLimit uint64) error {

	sessions := planFeedSessions(transferLength, sessionLimit)
	fmt.Printf("Large image: streaming 0x%x bytes as %d MVIIFlash session(s) of up to 0x%x to keep the MT6592 BROM USB stable.\n",
		transferLength, len(sessions), sessionLimit)

	for i, session := range sessions {
		final := i == len(sessions)-1
		// Each session is an idempotent raw eMMC range write; retry on a
		// transport drop with a fresh reconnect rather than failing the flash.
		var sessErr error
		for attempt := 1; attempt <= mtkFeedSegmentAttempts; attempt++ {
			sessErr = feedPayloadMTKSerialSession(cfg, payload, image, readSize, targetOffset,
				session, emmcPart, chunkSize, extraFlags, i+1, len(sessions), final)
			if sessErr == nil {
				break
			}
			if attempt < mtkFeedSegmentAttempts {
				fmt.Printf("Session write failed (%v); retrying session %d/%d (attempt %d/%d)\n",
					sessErr, i+1, len(sessions), attempt+1, mtkFeedSegmentAttempts)
				time.Sleep(2 * time.Second)
			}
		}
		if sessErr != nil {
			return sessErr
		}
	}

	followDevice, err := bootPreloaderAfterMTKFeed(cfg)
	if err != nil {
		return err
	}
	if followDevice != "" {
		cfg.device = followDevice
	}
	if err := followMTKFeedBoot(cfg); err != nil {
		return err
	}
	fmt.Println("flash complete")
	return nil
}

// feedPayloadMTKSerialSession runs one bounded feed session: it (re)connects to the
// payload, writes [session.Start, session.Start+session.Length) of the image, and
// resets the payload afterwards unless this is the final session. On the final
// session the reboot/boot-status flags (when requested) are attached so the payload
// watchdog-resets into the flashed image itself.
func feedPayloadMTKSerialSession(cfg config, payload mtkFeedPayloadImage, image string,
	readSize, targetOffset uint64, session feedSession, emmcPart byte, chunkSize int,
	extraFlags uint32, index, count int, final bool) error {

	client, hello, stopInterruptReset, err := startMTKFeedPayloadSession(cfg, payload)
	if err != nil {
		return err
	}
	defer stopInterruptReset()
	defer func() {
		_ = client.port.Close()
	}()

	if hello.BlockSize != 0 && hello.BlockSize != mtkSerialBlockSize {
		return fmt.Errorf("feed payload reports block size 0x%x, expected 0x%x", hello.BlockSize, mtkSerialBlockSize)
	}
	if cs, err := negotiateFeedChunkSize(hello, chunkSize); err != nil {
		return err
	} else {
		chunkSize = cs
	}

	segTarget := targetOffset + session.Start
	segLen := session.Length

	// Carry over only offset-independent extra flags on every session; per-flash
	// completion flags (reboot, boot-status marker, BOOT1 enable) belong on the
	// final session's FINISH, which is the one that actually completes the image.
	flags := extraFlags &^ (mtkFeedFlagBootAfterFlash | mtkFeedFlagRebootAfterFlash |
		mtkFeedFlagBootStatus | mtkFeedFlagEnableBoot1)
	if final {
		flags |= extraFlags & mtkFeedFlagEnableBoot1
		if cfg.mtkFeedReboot {
			flags |= mtkFeedFlagRebootAfterFlash
			if isJ36LKSlotImagePath(cfg, image) {
				flags |= mtkFeedFlagBootStatus
			}
		}
	}

	var streamHash [sha256.Size]byte // the payload validates length, not hash, per session
	fmt.Printf("Session %d/%d: eMMC part 0x%x offset 0x%x, length 0x%x, chunk 0x%x, flags 0x%x\n",
		index, count, emmcPart, segTarget, segLen, chunkSize, flags)

	seq := uint32(1)
	start := buildMTKFeedStartPayload(segTarget, segLen, segLen, uint32(emmcPart),
		mtkSerialBlockSize, uint32(chunkSize), flags, streamHash)
	if err := client.writeMTKFeedFrame(mtkFeedFrameStart, seq, start); err != nil {
		return fmt.Errorf("send feed START for session %d: %w", index, err)
	}
	if err := client.waitMTKFeedAck(seq, fmt.Sprintf("START session %d", index), false); err != nil {
		return err
	}
	seq++

	seq, err = client.streamMTKFeedImageRange(image, readSize, segTarget, session.Start, segLen, chunkSize, seq)
	if err != nil {
		return err
	}

	finish := buildMTKFeedFinishPayload(segLen, streamHash)
	if err := client.writeMTKFeedFrame(mtkFeedFrameFinish, seq, finish); err != nil {
		return fmt.Errorf("send feed FINISH for session %d: %w", index, err)
	}
	if err := client.waitMTKFeedAck(seq, fmt.Sprintf("FINISH session %d", index), true); err != nil {
		return err
	}
	seq++

	if !final {
		// Reset the payload to BROM so the next session reconnects to a fresh USB
		// session; this is what keeps the fragile MT6592 VCOM alive across the whole
		// image.
		if err := client.requestMTKFeedPayloadReboot(seq); err != nil {
			fmt.Printf("Warning: could not reset feed payload after session %d: %v\n", index, err)
		}
	} else if cfg.mtkFeedReboot {
		fmt.Println("Watchdog reset armed; the board reboots into the flashed image now.")
		if isJ36LKSlotImagePath(cfg, image) {
			fmt.Println("Watch the panel: the MVII LK drives the display, then hands back to BROM on its own.")
			fmt.Println("Next: run  ./flash -device " + cfg.device + " -mtk-read-boot-status  to read this run's fresh MVII telemetry.")
		}
	}
	return nil
}

func bootPreloaderAfterMTKFeed(cfg config) (string, error) {
	if !cfg.mtkFeedReboot || !cfg.mtkFeedBootPreloader {
		return "", nil
	}
	if strings.TrimSpace(cfg.preloader) == "" {
		for _, candidate := range defaultMTKPreloaderCandidates(cfg.root) {
			if fileExists(candidate) {
				cfg.preloader = candidate
				break
			}
		}
	}
	if strings.TrimSpace(cfg.preloader) == "" {
		fmt.Println("Warning: -mtk-feed-boot-preloader=true but no stock preloader image was found; leaving the board at the watchdog-reset boot path.")
		return "", nil
	}
	fmt.Println("Post-feed boot: reconnecting and booting the stock preloader from RAM to force the UBOOT/LK load path.")
	time.Sleep(1200 * time.Millisecond)
	return bootPreloaderMTKSerialWithDevice(cfg)
}

func followMTKFeedBoot(cfg config) error {
	if !cfg.mtkFeedFollowBoot {
		return nil
	}
	timeout, err := parseMTKFeedFollowTimeout(cfg.mtkFeedFollowTimeout)
	if err != nil {
		return err
	}
	deadline := time.Time{}
	timeoutText := "none"
	if timeout > 0 {
		deadline = time.Now().Add(timeout)
		timeoutText = timeout.String()
	}

	fmt.Printf("m7f follow dev=%s timeout=%s\n", cfg.device, timeoutText)
	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, os.Interrupt)
	defer signal.Stop(sigCh)

	var port *mtkSerialPort
	var opened string
	var received uint64
	var lastErr error
	nextOpen := time.Time{}
	reportedOpen := map[string]bool{}
	buf := make([]byte, 1024)
	for deadline.IsZero() || time.Now().Before(deadline) {
		select {
		case <-sigCh:
			if port != nil {
				_ = port.Close()
			}
			fmt.Printf("\nm7f stop bytes=0x%x\n", received)
			return nil
		default:
		}

		if port == nil {
			if !nextOpen.IsZero() && time.Now().Before(nextOpen) {
				time.Sleep(20 * time.Millisecond)
				continue
			}
			port, opened, err = openMTKFollowSerial(cfg.device)
			if err != nil {
				lastErr = err
				nextOpen = time.Now().Add(200 * time.Millisecond)
				continue
			}
			if !reportedOpen[opened] {
				fmt.Printf("m7f open %s\n", opened)
				reportedOpen[opened] = true
			}
		}

		n, readErr := port.file.Read(buf)
		if n > 0 {
			received += uint64(n)
			if _, err := os.Stdout.Write(buf[:n]); err != nil {
				_ = port.Close()
				return err
			}
		}
		if readErr == nil {
			if n == 0 {
				time.Sleep(2 * time.Millisecond)
			}
			continue
		}
		if isWouldBlock(readErr) || errors.Is(readErr, io.EOF) {
			time.Sleep(2 * time.Millisecond)
			continue
		}
		lastErr = fmt.Errorf("%s: %w", opened, readErr)
		_ = port.Close()
		port = nil
		opened = ""
		nextOpen = time.Now().Add(200 * time.Millisecond)
	}
	if port != nil {
		_ = port.Close()
	}
	if received == 0 && lastErr != nil {
		fmt.Println("m7f done bytes=0x0 err=0x1")
		return nil
	}
	fmt.Printf("\nm7f done bytes=0x%x\n", received)
	return nil
}

func parseMTKFeedFollowTimeout(value string) (time.Duration, error) {
	value = strings.TrimSpace(value)
	if value == "" {
		return 2 * time.Minute, nil
	}
	switch strings.ToLower(value) {
	case "0", "none", "forever":
		return 0, nil
	}
	timeout, err := time.ParseDuration(value)
	if err == nil {
		if timeout < 0 {
			return 0, fmt.Errorf("-mtk-feed-follow-timeout must not be negative: %s", value)
		}
		return timeout, nil
	}
	millis, parseErr := strconv.ParseInt(value, 10, 64)
	if parseErr == nil && millis >= 0 {
		return time.Duration(millis) * time.Millisecond, nil
	}
	return 0, fmt.Errorf("parse -mtk-feed-follow-timeout %q: %w", value, err)
}

func openMTKFollowSerial(device string) (*mtkSerialPort, string, error) {
	var lastErr error
	for _, candidate := range mtkSerialReconnectCandidates(device) {
		port, err := openMTKSerialPort(candidate, 115200)
		if err == nil {
			return port, candidate, nil
		}
		lastErr = fmt.Errorf("%s: %w", candidate, err)
	}
	if lastErr == nil {
		lastErr = errors.New("no serial device candidates")
	}
	return nil, "", lastErr
}

const (
	j36UltraScatterBootImageOffset = uint64(0x1f40000)
	j36UltraScatterBootImageSize   = uint64(0x0900000)
	j36UltraScatterLKOffset        = uint64(0x1d40000)
	j36UltraScatterLKSize          = uint64(0x0200000)
	// SYS13 LOGO in the stock MT6592 scatter: 8 MiB, declared downloadable by
	// the vendor's own table, and holding the Android boot logo -- something
	// MVII has never once read. That is why the asset slot lives there: nothing
	// is displaced, no offset shifts, and a board flashed with assets can still
	// be flashed back to stock without a layout change.
	j36UltraScatterLogoOffset      = uint64(0x38c0000)
	j36UltraScatterLogoSize        = uint64(0x0800000)
	j36UltraEMMCUserBytes          = uint64(0xe7000000)
	j36UltraBootImagePageSize      = uint64(0x800)
	j36UltraBootImageStage1Offset  = j36UltraBootImagePageSize + mtkImageHeaderSize
)

func isDefaultMVIIArmImage(cfg config, image string) bool {
	if cfg.image != "" {
		return false
	}
	cleanImage := filepath.Clean(image)
	return cleanImage == filepath.Clean(filepath.Join(cfg.root, "boot.img"))
}

func isDefaultMVIILKImage(cfg config, image string) bool {
	if cfg.image != "" {
		return false
	}
	cleanImage := filepath.Clean(image)
	return cleanImage == filepath.Clean(filepath.Join(cfg.root, "lk.bin"))
}

func mtkFeedTargetOffset(cfg config, image string, emmcPart byte) (uint64, bool, error) {
	if cfg.rawOffset != "" {
		offset, err := parseMTKNumber(cfg.rawOffset, "-raw-offset")
		return offset, false, err
	}
	if emmcPart == mtkLegacyEMMCPartUser {
		if offset, ok, err := preloaderProfileTargetOffset(cfg, image); err != nil || ok {
			return offset, true, err
		}
	}
	if emmcPart == mtkLegacyEMMCPartUser && isJ36LKSlotImagePath(cfg, image) {
		return j36UltraScatterLKOffset, true, nil
	}
	if emmcPart == mtkLegacyEMMCPartUser && isJ36BootSlotImagePath(cfg, image) {
		return j36UltraScatterBootImageOffset, true, nil
	}
	if emmcPart == mtkLegacyEMMCPartUser && isJ36AssetSlotImagePath(cfg, image) {
		return j36UltraScatterLogoOffset, true, nil
	}
	return 0, false, errors.New("-mtk-feed-payload requires an explicit -raw-offset unless flashing lk.bin to the J36 Ultra stock UBOOT slot or boot.img to the BOOTIMG slot")
}

// The release LK is an LK: same UBOOT slot, same offset defaulting, same
// oversize guard. All that differs is which build is resident, so anything that
// asks "is this the bootloader slot" has to say yes to both.
func isJ36LKSlotImagePath(cfg config, image string) bool {
	base := strings.ToLower(filepath.Base(image))
	if base == "lk.bin" || base == "lk-release.bin" {
		return true
	}
	return isDefaultMVIILKImage(cfg, image) ||
		filepath.Clean(image) == filepath.Clean(defaultMVIIReleaseLKImagePath(cfg))
}

func isJ36BootSlotImagePath(cfg config, image string) bool {
	base := strings.ToLower(filepath.Base(image))
	return base == "boot.img" || isDefaultMVIIArmImage(cfg, image)
}

func isJ36AssetSlotImagePath(cfg config, image string) bool {
	base := strings.ToLower(filepath.Base(image))
	if base == "assets.bin" {
		return true
	}
	if cfg.image != "" {
		return false
	}
	return filepath.Clean(image) == filepath.Clean(defaultMVIIAssetImagePath(cfg))
}

func validateJ36StockScatterFeedTarget(cfg config, image string, targetOffset, transferLength uint64) error {
	if envFlag("MVII_MTK_IGNORE_SCATTER_OFFSET") {
		return nil
	}
	if strings.TrimSpace(cfg.mtkPreloadProfile) != "" {
		return validateMTKPreloaderProfileFeedTarget(cfg, image, targetOffset, transferLength)
	}
	if isJ36LKSlotImagePath(cfg, image) {
		if targetOffset != j36UltraScatterLKOffset {
			return fmt.Errorf("%s is a J36 Ultra UBOOT-slot image, but the stock MT6592 scatter puts UBOOT at raw offset 0x%x; use -raw-offset 0x%x or set MVII_MTK_IGNORE_SCATTER_OFFSET=1 to override",
				image, j36UltraScatterLKOffset, j36UltraScatterLKOffset)
		}
		if transferLength > j36UltraScatterLKSize {
			return fmt.Errorf("%s transfer length 0x%x exceeds the J36 Ultra stock UBOOT/LK slot size 0x%x",
				image, transferLength, j36UltraScatterLKSize)
		}
	}
	if isJ36BootSlotImagePath(cfg, image) {
		if targetOffset != j36UltraScatterBootImageOffset {
			return fmt.Errorf("%s is a J36 Ultra boot image, but the stock MT6592 scatter puts BOOTIMG at raw offset 0x%x; use -raw-offset 0x%x or set MVII_MTK_IGNORE_SCATTER_OFFSET=1 to override",
				image, j36UltraScatterBootImageOffset, j36UltraScatterBootImageOffset)
		}
		if transferLength > j36UltraScatterBootImageSize {
			return fmt.Errorf("%s transfer length 0x%x exceeds the J36 Ultra stock BOOTIMG slot size 0x%x",
				image, transferLength, j36UltraScatterBootImageSize)
		}
	}
	if isJ36AssetSlotImagePath(cfg, image) {
		if targetOffset != j36UltraScatterLogoOffset {
			return fmt.Errorf("%s is a J36 Ultra asset slot, but the stock MT6592 scatter puts LOGO at raw offset 0x%x; use -raw-offset 0x%x or set MVII_MTK_IGNORE_SCATTER_OFFSET=1 to override",
				image, j36UltraScatterLogoOffset, j36UltraScatterLogoOffset)
		}
		if transferLength > j36UltraScatterLogoSize {
			return fmt.Errorf("%s transfer length 0x%x exceeds the J36 Ultra stock LOGO slot size 0x%x",
				image, transferLength, j36UltraScatterLogoSize)
		}
	}
	if targetOffset == j36UltraScatterLKOffset && transferLength > j36UltraScatterLKSize {
		return fmt.Errorf("%s transfer length 0x%x exceeds the J36 Ultra stock UBOOT/LK slot size 0x%x",
			image, transferLength, j36UltraScatterLKSize)
	}
	return nil
}

func mtkFeedTransferLength(imageSize uint64, rawLength string) (uint64, bool, error) {
	if rawLength == "" || strings.EqualFold(rawLength, "auto") || strings.EqualFold(rawLength, "minimal") {
		return alignUp(imageSize, mtkSerialBlockSize), true, nil
	}
	transferLength, err := parseMTKNumber(rawLength, "-raw-length")
	if err != nil {
		return 0, false, err
	}
	if transferLength < imageSize {
		return 0, false, fmt.Errorf("-raw-length 0x%x is smaller than image size 0x%x", transferLength, imageSize)
	}
	return alignUp(transferLength, mtkSerialBlockSize), false, nil
}

func mtkFeedRawTransferPlan(path string, fileSize uint64, rawLength string) (uint64, uint64, uint64, bool, string, error) {
	if rawLength != "" && !strings.EqualFold(rawLength, "auto") && !strings.EqualFold(rawLength, "minimal") {
		transferLength, autoLength, err := mtkFeedTransferLength(fileSize, rawLength)
		return fileSize, fileSize, transferLength, autoLength, "", err
	}

	readSize := fileSize
	imageSize := fileSize
	lengthNote := ""
	if info, ok, err := readMTKImageHeaderInfo(path, fileSize); err != nil {
		return 0, 0, 0, false, "", err
	} else if ok {
		aligned := alignUp(info.TotalSize, mtkSerialBlockSize)
		if aligned < fileSize {
			readSize = info.TotalSize
			imageSize = info.TotalSize
			lengthNote = fmt.Sprintf("Using MTK image header length 0x%x (payload 0x%x) instead of padded file length 0x%x.",
				aligned, info.PayloadSize, fileSize)
		}
	}

	transferLength, autoLength, err := mtkFeedTransferLength(imageSize, rawLength)
	return readSize, imageSize, transferLength, autoLength, lengthNote, err
}

type mtkImageHeaderInfo struct {
	PayloadSize uint64
	TotalSize   uint64
}

func readMTKImageHeaderInfo(path string, fileSize uint64) (mtkImageHeaderInfo, bool, error) {
	if fileSize < 8 {
		return mtkImageHeaderInfo{}, false, nil
	}
	f, err := os.Open(path)
	if err != nil {
		return mtkImageHeaderInfo{}, false, err
	}
	defer f.Close()

	header := make([]byte, 8)
	if _, err := io.ReadFull(f, header); err != nil {
		if errors.Is(err, io.EOF) || errors.Is(err, io.ErrUnexpectedEOF) {
			return mtkImageHeaderInfo{}, false, nil
		}
		return mtkImageHeaderInfo{}, false, fmt.Errorf("read MTK image header %s: %w", path, err)
	}
	if binary.LittleEndian.Uint32(header[0:4]) != mtkImageMagic {
		return mtkImageHeaderInfo{}, false, nil
	}
	payloadSize := uint64(binary.LittleEndian.Uint32(header[4:8]))
	totalSize := uint64(mtkImageHeaderSize) + payloadSize
	if payloadSize == 0 || totalSize < payloadSize || totalSize > fileSize {
		return mtkImageHeaderInfo{}, false, fmt.Errorf("%s has invalid MTK image payload length 0x%x for file size 0x%x",
			path, payloadSize, fileSize)
	}
	return mtkImageHeaderInfo{PayloadSize: payloadSize, TotalSize: totalSize}, true, nil
}

func mtkFeedSparseTransferLength(expandedSize uint64, rawLength string) (uint64, bool, error) {
	if rawLength == "" || strings.EqualFold(rawLength, "auto") || strings.EqualFold(rawLength, "minimal") {
		return expandedSize, true, nil
	}
	transferLength, err := parseMTKNumber(rawLength, "-raw-length")
	if err != nil {
		return 0, false, err
	}
	if transferLength < expandedSize {
		return 0, false, fmt.Errorf("-raw-length 0x%x is smaller than Android sparse expanded size 0x%x", transferLength, expandedSize)
	}
	return alignUp(transferLength, mtkSerialBlockSize), false, nil
}

type androidSparseImageInfo struct {
	HeaderSize   uint16
	ChunkHdrSize uint16
	BlockSize    uint32
	TotalBlocks  uint32
	TotalChunks  uint32
	ExpandedSize uint64
}

const (
	androidSparseMagic      = 0xed26ff3a
	androidSparseHeaderSize = 28
	androidSparseChunkSize  = 12
	androidSparseChunkRaw   = 0xcac1
	androidSparseChunkFill  = 0xcac2
	androidSparseChunkSkip  = 0xcac3
	androidSparseChunkCRC   = 0xcac4
)

func readAndroidSparseImageInfo(path string) (androidSparseImageInfo, bool, error) {
	f, err := os.Open(path)
	if err != nil {
		return androidSparseImageInfo{}, false, err
	}
	defer f.Close()

	header := make([]byte, androidSparseHeaderSize)
	n, err := io.ReadFull(f, header)
	if err != nil {
		if errors.Is(err, io.ErrUnexpectedEOF) || errors.Is(err, io.EOF) {
			return androidSparseImageInfo{}, false, nil
		}
		return androidSparseImageInfo{}, false, fmt.Errorf("read Android sparse header %s: %w", path, err)
	}
	if n != androidSparseHeaderSize || binary.LittleEndian.Uint32(header[0:4]) != androidSparseMagic {
		return androidSparseImageInfo{}, false, nil
	}

	major := binary.LittleEndian.Uint16(header[4:6])
	headerSize := binary.LittleEndian.Uint16(header[8:10])
	chunkHdrSize := binary.LittleEndian.Uint16(header[10:12])
	blockSize := binary.LittleEndian.Uint32(header[12:16])
	totalBlocks := binary.LittleEndian.Uint32(header[16:20])
	totalChunks := binary.LittleEndian.Uint32(header[20:24])
	if major != 1 {
		return androidSparseImageInfo{}, true, fmt.Errorf("%s uses unsupported Android sparse major version %d", path, major)
	}
	if headerSize < androidSparseHeaderSize || chunkHdrSize < androidSparseChunkSize {
		return androidSparseImageInfo{}, true, fmt.Errorf("%s has invalid Android sparse header sizes", path)
	}
	if blockSize == 0 || blockSize%mtkSerialBlockSize != 0 {
		return androidSparseImageInfo{}, true, fmt.Errorf("%s has invalid Android sparse block size 0x%x", path, blockSize)
	}
	expanded := uint64(blockSize) * uint64(totalBlocks)
	if expanded == 0 || expanded/uint64(blockSize) != uint64(totalBlocks) {
		return androidSparseImageInfo{}, true, fmt.Errorf("%s has invalid Android sparse expanded size", path)
	}
	return androidSparseImageInfo{
		HeaderSize:   headerSize,
		ChunkHdrSize: chunkHdrSize,
		BlockSize:    blockSize,
		TotalBlocks:  totalBlocks,
		TotalChunks:  totalChunks,
		ExpandedSize: expanded,
	}, true, nil
}

type mtkFeedFlashImageKind string

const (
	mtkFeedImageUnknown         mtkFeedFlashImageKind = "unknown raw image"
	mtkFeedImageAndroidBoot     mtkFeedFlashImageKind = "Android boot image"
	mtkFeedImageRockchipLoader  mtkFeedFlashImageKind = "Rockchip loader image"
	mtkFeedImageRockchipTrust   mtkFeedFlashImageKind = "Rockchip trust image"
	mtkFeedImageRockchipConfig  mtkFeedFlashImageKind = "Rockchip AndroidTool config"
	mtkFeedImageRockchipPackage mtkFeedFlashImageKind = "Rockchip firmware package"
)

func validateMTKFeedImageForTarget(path string) error {
	kind, err := detectMTKFeedFlashImageKind(path)
	if err != nil {
		return err
	}
	switch kind {
	case mtkFeedImageRockchipLoader, mtkFeedImageRockchipTrust, mtkFeedImageRockchipConfig, mtkFeedImageRockchipPackage:
		return fmt.Errorf("%s is a %s, not an MT6592/J36 Ultra boot image; it cannot boot or hand off to SD on this MediaTek target", path, kind)
	default:
		return nil
	}
}

func detectMTKFeedFlashImageKind(path string) (mtkFeedFlashImageKind, error) {
	f, err := os.Open(path)
	if err != nil {
		return mtkFeedImageUnknown, err
	}
	defer f.Close()

	head := make([]byte, 4096)
	n, err := f.Read(head)
	if err != nil && !errors.Is(err, io.EOF) {
		return mtkFeedImageUnknown, fmt.Errorf("read image header %s: %w", path, err)
	}
	head = head[:n]

	switch {
	case bytes.HasPrefix(head, []byte("ANDROID!")):
		return mtkFeedImageAndroidBoot, nil
	case bytes.HasPrefix(head, []byte("LOADER  ")):
		return mtkFeedImageRockchipLoader, nil
	case bytes.HasPrefix(head, []byte("BL3X")):
		return mtkFeedImageRockchipTrust, nil
	case bytes.HasPrefix(head, []byte{'C', 'F', 'G', 0}):
		return mtkFeedImageRockchipConfig, nil
	case bytes.HasPrefix(head, []byte("RKAF")) || bytes.HasPrefix(head, []byte("RKFW")):
		return mtkFeedImageRockchipPackage, nil
	default:
		return mtkFeedImageUnknown, nil
	}
}

type mtkScatterEntry struct {
	PartitionName string
	FileName      string
	IsDownload    bool
	Type          string
	LinearStart   uint64
	PartitionSize uint64
	Region        string
}

func validateMTKFeedOffsetAgainstScatter(image string, targetOffset, transferLength uint64) error {
	if envFlag("MVII_MTK_IGNORE_SCATTER_OFFSET") {
		return nil
	}
	entry, scatter, ok, err := findMTKScatterEntryForImage(image)
	if err != nil {
		return err
	}
	if !ok || !strings.EqualFold(entry.Region, "EMMC_USER") {
		return nil
	}
	if entry.LinearStart == targetOffset {
		if entry.PartitionSize != 0 && transferLength > entry.PartitionSize {
			return fmt.Errorf("%s is listed in %s as %s with partition size 0x%x, but transfer length is 0x%x",
				image, scatter, entry.PartitionName, entry.PartitionSize, transferLength)
		}
		return nil
	}

	return fmt.Errorf("%s is listed in %s as %s at raw offset 0x%x, but -raw-offset is 0x%x; use -raw-offset 0x%x or set MVII_MTK_IGNORE_SCATTER_OFFSET=1 to override",
		image, scatter, entry.PartitionName, entry.LinearStart, targetOffset, entry.LinearStart)
}

func findMTKScatterEntryForImage(image string) (mtkScatterEntry, string, bool, error) {
	dir := filepath.Dir(image)
	base := filepath.Base(image)
	matches, err := filepath.Glob(filepath.Join(dir, "*scatter*.txt"))
	if err != nil {
		return mtkScatterEntry{}, "", false, err
	}
	for _, scatter := range matches {
		entries, err := parseMTKScatterFile(scatter)
		if err != nil {
			return mtkScatterEntry{}, scatter, false, err
		}
		for _, entry := range entries {
			if strings.EqualFold(entry.FileName, base) {
				return entry, scatter, true, nil
			}
		}
	}
	return mtkScatterEntry{}, "", false, nil
}

func parseMTKScatterFile(path string) ([]mtkScatterEntry, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	lines := strings.Split(string(data), "\n")
	var entries []mtkScatterEntry
	current := map[string]string{}
	flush := func() error {
		if len(current) == 0 {
			return nil
		}
		fileName := strings.TrimSpace(current["file_name"])
		startText := strings.TrimSpace(current["linear_start_addr"])
		if startText == "" {
			// Header/pseudo rows without placement info; nothing to map.
			current = map[string]string{}
			return nil
		}
		// NOTE: entries with file_name NONE / is_download false (PROTECT_F,
		// PROTECT_S, NVRAM, ...) are intentionally kept so partition listings can
		// name every region. Flash callers re-filter on IsDownload/FileName.
		start, err := parseMTKNumber(startText, "linear_start_addr")
		if err != nil {
			return fmt.Errorf("parse %s partition %s linear_start_addr %q: %w", path, current["partition_name"], startText, err)
		}
		partitionSize := uint64(0)
		if sizeText := strings.TrimSpace(current["partition_size"]); sizeText != "" {
			partitionSize, err = parseMTKNumber(sizeText, "partition_size")
			if err != nil {
				return fmt.Errorf("parse %s partition %s partition_size %q: %w", path, current["partition_name"], sizeText, err)
			}
		}
		entries = append(entries, mtkScatterEntry{
			PartitionName: strings.TrimSpace(current["partition_name"]),
			FileName:      fileName,
			IsDownload:    strings.EqualFold(strings.TrimSpace(current["is_download"]), "true"),
			Type:          strings.TrimSpace(current["type"]),
			LinearStart:   start,
			PartitionSize: partitionSize,
			Region:        strings.TrimSpace(current["region"]),
		})
		current = map[string]string{}
		return nil
	}

	for _, line := range lines {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		if strings.HasPrefix(line, "- partition_index:") {
			if err := flush(); err != nil {
				return nil, err
			}
			current["partition_index"] = strings.TrimSpace(strings.TrimPrefix(line, "- partition_index:"))
			continue
		}
		if len(current) == 0 {
			continue
		}
		key, value, ok := strings.Cut(line, ":")
		if !ok {
			continue
		}
		current[strings.TrimSpace(key)] = strings.Trim(strings.TrimSpace(value), `"'`)
	}
	if err := flush(); err != nil {
		return nil, err
	}
	return entries, nil
}

func loadMTKFeedPayload(cfg config) (mtkFeedPayloadImage, error) {
	if cfg.mtkFeedPayload == "" {
		return mtkFeedPayloadImage{}, errors.New("-mtk-feed-payload must name a payload binary")
	}
	payloadPath, err := resolveMTKFeedPayloadPath(cfg)
	if err != nil {
		return mtkFeedPayloadImage{}, err
	}
	payloadCfg := cfg
	payloadCfg.mtkFeedPayload = payloadPath
	data, err := os.ReadFile(payloadPath)
	if err != nil {
		return mtkFeedPayloadImage{}, err
	}
	if len(data) == 0 {
		return mtkFeedPayloadImage{}, fmt.Errorf("feed payload %s is empty", payloadPath)
	}
	if bytes.HasPrefix(data, []byte{0x7F, 'E', 'L', 'F'}) {
		return loadMTKFeedELFPayload(payloadCfg, data)
	}
	if payloadCfg.mtkPayloadAddr == "" {
		if entry, code, err := parseMTKPreloaderImage(data); err == nil {
			entryAddr, err := parseMTKFeedAddress(payloadCfg.mtkPayloadEntry, "-mtk-payload-entry", entry)
			if err != nil {
				return mtkFeedPayloadImage{}, err
			}
			return mtkFeedPayloadImage{
				Path:      payloadPath,
				Kind:      "mtk-preloader",
				LoadAddr:  entry,
				EntryAddr: entryAddr,
				Data:      code,
			}, nil
		}
	}
	loadAddr, err := parseMTKFeedAddress(payloadCfg.mtkPayloadAddr, "-mtk-payload-addr", mtkFeedDefaultPayloadAddr)
	if err != nil {
		return mtkFeedPayloadImage{}, err
	}
	entryAddr, err := parseMTKFeedAddress(payloadCfg.mtkPayloadEntry, "-mtk-payload-entry", loadAddr)
	if err != nil {
		return mtkFeedPayloadImage{}, err
	}
	return mtkFeedPayloadImage{
		Path:      payloadPath,
		Kind:      "raw",
		LoadAddr:  loadAddr,
		EntryAddr: entryAddr,
		Data:      data,
	}, nil
}

func resolveMTKFeedPayloadPath(cfg config) (string, error) {
	value := strings.TrimSpace(cfg.mtkFeedPayload)
	if !strings.EqualFold(value, "auto") {
		return value, nil
	}
	candidates := []string{
		filepath.Join(cfg.root, "boot", "MVIIFlash.bin"),
		filepath.Join(cfg.root, "boot", "MVIIFlash.elf"),
		filepath.Join(cfg.root, "MVIIFlash.bin"),
		filepath.Join(cfg.root, "MVIIFlash.elf"),
	}
	for _, candidate := range candidates {
		if fileExists(candidate) {
			return candidate, nil
		}
	}
	return "", fmt.Errorf("-mtk-feed-payload auto could not find boot/MVIIFlash.bin or boot/MVIIFlash.elf under %s; rebuild the mvii-armv7-pc package", cfg.root)
}

func loadMTKFeedELFPayload(cfg config, data []byte) (mtkFeedPayloadImage, error) {
	if cfg.mtkPayloadAddr != "" {
		return mtkFeedPayloadImage{}, errors.New("-mtk-payload-addr is not supported for ELF payloads; link the ELF at its intended physical address or objcopy it to a raw binary")
	}
	file, err := elf.NewFile(bytes.NewReader(data))
	if err != nil {
		return mtkFeedPayloadImage{}, fmt.Errorf("parse ELF feed payload: %w", err)
	}
	defer file.Close()
	if file.Class != elf.ELFCLASS32 {
		return mtkFeedPayloadImage{}, fmt.Errorf("ELF feed payload must be 32-bit ARM, got %s", file.Class)
	}
	if file.Data != elf.ELFDATA2LSB {
		return mtkFeedPayloadImage{}, fmt.Errorf("ELF feed payload must be little-endian, got %s", file.Data)
	}
	if file.Machine != elf.EM_ARM {
		return mtkFeedPayloadImage{}, fmt.Errorf("ELF feed payload must target ARM, got %s", file.Machine)
	}

	type loadSegment struct {
		addr   uint64
		memsz  uint64
		filesz uint64
		prog   *elf.Prog
	}
	var segments []loadSegment
	for _, prog := range file.Progs {
		if prog.Type != elf.PT_LOAD || prog.Memsz == 0 {
			continue
		}
		addr := prog.Paddr
		if addr == 0 {
			addr = prog.Vaddr
		}
		if addr == 0 {
			return mtkFeedPayloadImage{}, errors.New("ELF PT_LOAD segment has no physical or virtual address")
		}
		if prog.Filesz > prog.Memsz {
			return mtkFeedPayloadImage{}, fmt.Errorf("ELF PT_LOAD filesz 0x%x exceeds memsz 0x%x", prog.Filesz, prog.Memsz)
		}
		segments = append(segments, loadSegment{addr: addr, memsz: prog.Memsz, filesz: prog.Filesz, prog: prog})
	}
	if len(segments) == 0 {
		return mtkFeedPayloadImage{}, errors.New("ELF feed payload has no PT_LOAD segments")
	}
	sort.Slice(segments, func(i, j int) bool {
		return segments[i].addr < segments[j].addr
	})
	base := segments[0].addr
	end := base
	for _, segment := range segments {
		segEnd := segment.addr + segment.memsz
		if segEnd < segment.addr {
			return mtkFeedPayloadImage{}, errors.New("ELF PT_LOAD address range overflows")
		}
		if segEnd > end {
			end = segEnd
		}
	}
	if base > uint64(^uint32(0)) || file.Entry > uint64(^uint32(0)) {
		return mtkFeedPayloadImage{}, errors.New("ELF feed payload uses addresses outside 32-bit MT6592 address space")
	}
	span := end - base
	if span == 0 || span > mtkFeedMaxPayloadBytes {
		return mtkFeedPayloadImage{}, fmt.Errorf("ELF load span 0x%x is invalid or too large for BROM upload", span)
	}
	out := make([]byte, span)
	for _, segment := range segments {
		if segment.filesz == 0 {
			continue
		}
		start := int(segment.addr - base)
		filesz := int(segment.filesz)
		reader := segment.prog.Open()
		if _, err := io.ReadFull(reader, out[start:start+filesz]); err != nil {
			return mtkFeedPayloadImage{}, fmt.Errorf("read ELF PT_LOAD at 0x%x: %w", segment.addr, err)
		}
	}
	entry, err := parseMTKFeedAddress(cfg.mtkPayloadEntry, "-mtk-payload-entry", uint32(file.Entry))
	if err != nil {
		return mtkFeedPayloadImage{}, err
	}
	if uint64(entry) < base || uint64(entry) >= end {
		return mtkFeedPayloadImage{}, fmt.Errorf("ELF entry 0x%x is outside loaded span 0x%x..0x%x", entry, base, end)
	}
	return mtkFeedPayloadImage{
		Path:      cfg.mtkFeedPayload,
		Kind:      "elf32-arm",
		LoadAddr:  uint32(base),
		EntryAddr: entry,
		Data:      out,
	}, nil
}

func parseMTKFeedAddress(value, name string, fallback uint32) (uint32, error) {
	value = strings.TrimSpace(value)
	if value == "" {
		envName := "MVII_MTK_PAYLOAD_ADDR"
		if name == "-mtk-payload-entry" {
			envName = "MVII_MTK_PAYLOAD_ENTRY"
		}
		value = strings.TrimSpace(os.Getenv(envName))
	}
	if value == "" {
		return fallback, nil
	}
	parsed, err := parseMTKNumber(value, name)
	if err != nil {
		return 0, err
	}
	if parsed > uint64(^uint32(0)) {
		return 0, fmt.Errorf("%s 0x%x is outside 32-bit MT6592 address space", name, parsed)
	}
	return uint32(parsed), nil
}

func mtkFeedChunkSize(cfg config) (int, error) {
	value := strings.TrimSpace(cfg.mtkFeedChunkSize)
	if value == "" {
		value = strings.TrimSpace(os.Getenv("MVII_MTK_FEED_CHUNK_SIZE"))
	}
	if value == "" {
		return mtkFeedDefaultChunkSize, nil
	}
	parsed, err := parseMTKNumber(value, "-mtk-feed-chunk-size")
	if err != nil {
		return 0, err
	}
	if parsed < mtkSerialBlockSize || parsed > mtkFeedMaxChunkSize || parsed%mtkSerialBlockSize != 0 {
		return 0, fmt.Errorf("-mtk-feed-chunk-size must be a 512-byte multiple between 0x%x and 0x%x", mtkSerialBlockSize, mtkFeedMaxChunkSize)
	}
	return int(parsed), nil
}

func mtkFeedChunkSizeExplicitlyConfigured(cfg config) bool {
	return strings.TrimSpace(cfg.mtkFeedChunkSize) != "" ||
		strings.TrimSpace(os.Getenv("MVII_MTK_FEED_CHUNK_SIZE")) != ""
}

// negotiateFeedChunkSize reconciles the host-requested chunk size with the
// MaxChunk the payload reported in its HELLO. We take the payload's value when
// it is larger (up to our safety cap) because bigger chunks mean fewer
// DATA/ACK turns. This helps with large images when the payload has per-chunk
// overhead or iteration limits. We still shrink if the payload requires it.
func negotiateFeedChunkSize(hello mtkFeedHello, start int) (int, error) {
	return negotiateFeedChunkSizeWithPolicy(hello, start, true)
}

func negotiateFeedChunkSizeForScatter(hello mtkFeedHello, start int) (int, error) {
	return negotiateFeedChunkSizeWithPolicy(hello, start, false)
}

func negotiateFeedChunkSizeWithPolicy(hello mtkFeedHello, start int, allowGrow bool) (int, error) {
	cs := start
	if cs == 0 {
		cs = mtkFeedDefaultChunkSize
	}
	if hello.MaxChunk != 0 {
		if uint32(cs) > hello.MaxChunk {
			cs = int(hello.MaxChunk)
			cs -= cs % mtkSerialBlockSize
			if cs < mtkSerialBlockSize {
				return 0, fmt.Errorf("feed payload max chunk 0x%x is smaller than one eMMC block", hello.MaxChunk)
			}
			fmt.Printf("Clamped feed chunk size to payload max: 0x%x\n", cs)
		} else if allowGrow && uint32(cs) < hello.MaxChunk {
			candidate := int(hello.MaxChunk)
			if candidate > mtkFeedMaxChunkSize {
				candidate = mtkFeedMaxChunkSize
			}
			candidate -= candidate % mtkSerialBlockSize
			if candidate >= mtkSerialBlockSize && candidate != cs {
				fmt.Printf("Using payload-supported chunk size 0x%x\n", candidate)
				cs = candidate
			}
		}
	}
	return cs, nil
}

func parseMTKFeedPart(value string) (byte, error) {
	switch strings.ToLower(strings.TrimSpace(value)) {
	case "", "user", "userdata", "emmc-user":
		return mtkLegacyEMMCPartUser, nil
	case "boot1", "boot-1", "emmc-boot1":
		return mtkLegacyEMMCPartBoot1, nil
	case "boot2", "boot-2", "emmc-boot2":
		return 0x02, nil
	default:
		parsed, err := parseMTKNumber(value, "-mtk-feed-part")
		if err != nil {
			return 0, err
		}
		if parsed > 0xFF {
			return 0, fmt.Errorf("-mtk-feed-part 0x%x does not fit in one byte", parsed)
		}
		return byte(parsed), nil
	}
}

func hashPaddedFile(path string, imageSize, transferLength uint64) ([sha256.Size]byte, error) {
	var out [sha256.Size]byte
	src, err := os.Open(path)
	if err != nil {
		return out, err
	}
	defer src.Close()
	h := sha256.New()
	n, err := io.Copy(h, io.LimitReader(src, int64(imageSize)))
	if err != nil {
		return out, fmt.Errorf("hash %s: %w", path, err)
	}
	if uint64(n) != imageSize {
		return out, fmt.Errorf("hash %s: read 0x%x bytes, want 0x%x", path, n, imageSize)
	}
	zeroes := make([]byte, mtkFeedDefaultChunkSize)
	for pad := transferLength - imageSize; pad > 0; {
		count := uint64(len(zeroes))
		if count > pad {
			count = pad
		}
		if _, err := h.Write(zeroes[:int(count)]); err != nil {
			return out, err
		}
		pad -= count
	}
	copy(out[:], h.Sum(nil))
	return out, nil
}

func buildMTKFeedStartPayload(targetOffset, imageSize, transferLength uint64, emmcPart, blockSize, chunkSize, flags uint32, streamHash [sha256.Size]byte) []byte {
	out := make([]byte, 0, 8+8+8+4+4+4+4+sha256.Size)
	out = binary.LittleEndian.AppendUint64(out, targetOffset)
	out = binary.LittleEndian.AppendUint64(out, imageSize)
	out = binary.LittleEndian.AppendUint64(out, transferLength)
	out = binary.LittleEndian.AppendUint32(out, emmcPart)
	out = binary.LittleEndian.AppendUint32(out, blockSize)
	out = binary.LittleEndian.AppendUint32(out, chunkSize)
	out = binary.LittleEndian.AppendUint32(out, flags)
	out = append(out, streamHash[:]...)
	return out
}

func buildMTKFeedDataPayload(targetOffset, streamOffset uint64, data []byte) []byte {
	out := make([]byte, 16+len(data))
	binary.LittleEndian.PutUint64(out[0:8], targetOffset)
	binary.LittleEndian.PutUint64(out[8:16], streamOffset)
	copy(out[16:], data)
	return out
}

func buildMTKFeedFinishPayload(transferLength uint64, streamHash [sha256.Size]byte) []byte {
	out := make([]byte, 0, 8+sha256.Size)
	out = binary.LittleEndian.AppendUint64(out, transferLength)
	out = append(out, streamHash[:]...)
	return out
}

func buildMTKFeedReadPayload(targetOffset uint64, length, emmcPart, blockSize uint32) []byte {
	out := make([]byte, 0, 8+4+4+4)
	out = binary.LittleEndian.AppendUint64(out, targetOffset)
	out = binary.LittleEndian.AppendUint32(out, length)
	out = binary.LittleEndian.AppendUint32(out, emmcPart)
	out = binary.LittleEndian.AppendUint32(out, blockSize)
	return out
}

func (c *mtkSerialClient) probeMTKFeedPayloadQuick() (mtkFeedHello, bool, error) {
	timeout := envDuration("MVII_MTK_FEED_RECOVERY_TIMEOUT", 1500*time.Millisecond)
	deadline := time.Now().Add(timeout)
	oldWriteTimeout := c.writeTimeout
	if c.writeTimeout <= 0 || c.writeTimeout > 2*time.Second {
		c.writeTimeout = 2 * time.Second
	}
	defer func() {
		c.writeTimeout = oldWriteTimeout
	}()

	_ = c.port.DiscardInput(20 * time.Millisecond)
	var lastErr error
	for seq := uint32(0); time.Now().Before(deadline); seq++ {
		if err := c.writeMTKFeedFrame(mtkFeedFrameHello, seq, []byte("mvii-flash-recover")); err != nil {
			return mtkFeedHello{}, false, err
		}
		frame, err := c.readMTKFeedFrame(timeoutUntil(deadline, 1*time.Second))
		if err != nil {
			lastErr = err
			if isDeviceGoneError(err) {
				break
			}
			continue
		}
		switch frame.Type {
		case mtkFeedFrameHello:
			return parseMTKFeedHello(frame.Payload), true, nil
		case mtkFeedFrameAck:
			status, message := parseMTKFeedStatus(frame.Payload)
			if status != 0 {
				lastErr = fmt.Errorf("feed payload rejected recovery HELLO: status=0x%x %s", status, message)
				continue
			}
			return mtkFeedHello{}, true, nil
		case mtkFeedFrameLog:
			printMTKFeedLog(frame.Payload)
		case mtkFeedFrameError:
			status, message := parseMTKFeedStatus(frame.Payload)
			lastErr = fmt.Errorf("feed payload recovery probe error: status=0x%x %s", status, message)
		default:
			lastErr = fmt.Errorf("unexpected feed frame 0x%04x during recovery probe", frame.Type)
		}
	}
	return mtkFeedHello{}, false, lastErr
}

func (c *mtkSerialClient) requestMTKFeedPayloadResetQuick() (bool, error) {
	hello, recovered, err := c.probeMTKFeedPayloadQuick()
	if !recovered {
		return false, err
	}
	if hello.Name != "" {
		fmt.Printf("Existing MVIIFlash payload detected on %s: %s; requesting clean preloader reset.\n", c.device, hello.Name)
	} else {
		fmt.Printf("Existing MVIIFlash payload detected on %s; requesting clean preloader reset.\n", c.device)
	}
	return true, c.requestMTKFeedPayloadRebootWithReason(1, "host reconnect via preloader")
}

func (c *mtkSerialClient) requestMTKFeedPayloadReboot(seq uint32) error {
	return c.requestMTKFeedPayloadRebootWithReason(seq, "host reconnect recovery")
}

func (c *mtkSerialClient) requestMTKFeedPayloadRebootWithReason(seq uint32, reason string) error {
	if err := c.writeMTKFeedFrame(mtkFeedFrameReboot, seq, []byte(reason)); err != nil {
		return err
	}
	deadline := time.Now().Add(envDuration("MVII_MTK_FEED_REBOOT_ACK_TIMEOUT", 1200*time.Millisecond))
	var lastErr error
	for time.Now().Before(deadline) {
		frame, err := c.readMTKFeedFrame(timeoutUntil(deadline, 500*time.Millisecond))
		if err != nil {
			lastErr = err
			if isDeviceGoneError(err) {
				break
			}
			continue
		}
		switch frame.Type {
		case mtkFeedFrameAck, mtkFeedFrameDone:
			status, message := parseMTKFeedStatus(frame.Payload)
			if status != 0 {
				return fmt.Errorf("feed payload rejected REBOOT: status=0x%x %s", status, message)
			}
			if message != "" {
				fmt.Printf("payload: %s\n", message)
			}
			return nil
		case mtkFeedFrameLog:
			printMTKFeedLog(frame.Payload)
		case mtkFeedFrameError:
			status, message := parseMTKFeedStatus(frame.Payload)
			return fmt.Errorf("feed payload rejected REBOOT: status=0x%x %s", status, message)
		}
	}
	if lastErr != nil {
		fmt.Printf("MVIIFlash reset frame sent; USB may drop before ACK (%v).\n", lastErr)
	}
	return nil
}

func (c *mtkSerialClient) writeMTKFeedFrameTimeout(frameType uint16, seq uint32, payload []byte, timeout time.Duration) error {
	if c.writeMu.TryLock() {
		defer c.writeMu.Unlock()
	}
	return c.port.WriteAll(encodeMTKFeedFrame(frameType, seq, payload), timeout)
}

func installMTKFeedInterruptReset(client *mtkSerialClient) func() {
	sigCh := make(chan os.Signal, 1)
	done := make(chan struct{})
	signal.Notify(sigCh, os.Interrupt)
	go func() {
		select {
		case <-sigCh:
			fmt.Println()
			fmt.Println("Interrupt received; requesting MVIIFlash payload reset to BROM before exit.")
			if client != nil && client.port != nil {
				if err := client.writeMTKFeedFrameTimeout(mtkFeedFrameReboot, 0x7ffffffe, []byte("host interrupted"), 750*time.Millisecond); err != nil {
					fmt.Printf("Warning: could not send MVIIFlash reset frame: %v\n", err)
				}
				time.Sleep(300 * time.Millisecond)
				_ = client.port.Close()
			}
			os.Exit(130)
		case <-done:
		}
	}()
	return func() {
		signal.Stop(sigCh)
		close(done)
	}
}

func (c *mtkSerialClient) waitMTKFeedHello() (mtkFeedHello, error) {
	timeout := envDuration("MVII_MTK_FEED_READY_TIMEOUT", 30*time.Second)
	deadline := time.Now().Add(timeout)
	fmt.Printf("Waiting for feed payload protocol (timeout %s)\n", timeout)
	nextHello := time.Now().Add(500 * time.Millisecond)
	var lastErr error
	for time.Now().Before(deadline) {
		if time.Now().After(nextHello) {
			if err := c.writeMTKFeedFrame(mtkFeedFrameHello, 0, []byte("mvii-flash")); err != nil {
				lastErr = err
			}
			nextHello = time.Now().Add(time.Second)
		}
		frame, err := c.readMTKFeedFrame(timeoutUntil(deadline, 5*time.Second))
		if err != nil {
			lastErr = err
			if isDeviceGoneError(err) {
				break
			}
			continue
		}
		switch frame.Type {
		case mtkFeedFrameHello:
			return parseMTKFeedHello(frame.Payload), nil
		case mtkFeedFrameAck:
			status, message := parseMTKFeedStatus(frame.Payload)
			if status != 0 {
				return mtkFeedHello{}, fmt.Errorf("feed payload rejected HELLO: status=0x%x %s", status, message)
			}
			return mtkFeedHello{Name: message}, nil
		case mtkFeedFrameLog:
			printMTKFeedLog(frame.Payload)
		case mtkFeedFrameError:
			status, message := parseMTKFeedStatus(frame.Payload)
			return mtkFeedHello{}, fmt.Errorf("feed payload error before HELLO: status=0x%x %s", status, message)
		}
	}
	if lastErr != nil {
		return mtkFeedHello{}, fmt.Errorf("timed out waiting for feed payload HELLO: %w", lastErr)
	}
	return mtkFeedHello{}, errors.New("timed out waiting for feed payload HELLO")
}

func (c *mtkSerialClient) streamMTKFeedImage(image string, imageSize, targetOffset, transferLength uint64, chunkSize int, seq uint32) (uint32, error) {
	return c.streamMTKFeedImageRange(image, imageSize, targetOffset, 0, transferLength, chunkSize, seq)
}

func (c *mtkSerialClient) streamMTKFeedImageRange(image string, imageSize, targetOffset, fileOffset, transferLength uint64, chunkSize int, seq uint32) (uint32, error) {
	src, err := os.Open(image)
	if err != nil {
		return seq, err
	}
	defer src.Close()
	if fileOffset < imageSize {
		if _, err := src.Seek(int64(fileOffset), io.SeekStart); err != nil {
			return seq, fmt.Errorf("seek image chunk at 0x%x: %w", fileOffset, err)
		}
	}

	buf := make([]byte, chunkSize)
	sent := uint64(0)
	lastProgress := time.Time{}
	for sent < transferLength {
		count := uint64(chunkSize)
		if remaining := transferLength - sent; remaining < count {
			count = remaining
		}
		chunk := buf[:int(count)]
		clear(chunk)
		absolute := fileOffset + sent
		if absolute < imageSize {
			toRead := count
			if remainingImage := imageSize - absolute; remainingImage < toRead {
				toRead = remainingImage
			}
			if _, err := io.ReadFull(src, chunk[:int(toRead)]); err != nil {
				return seq, fmt.Errorf("read image chunk at 0x%x: %w", absolute, err)
			}
		}
		payload := buildMTKFeedDataPayload(targetOffset+sent, sent, chunk)
		if err := c.writeMTKFeedFrame(mtkFeedFrameData, seq, payload); err != nil {
			return seq, fmt.Errorf("send feed DATA seq %d at 0x%x: %w", seq, sent, err)
		}
		if err := c.waitMTKFeedAck(seq, fmt.Sprintf("DATA seq %d at 0x%x", seq, sent), false); err != nil {
			return seq, err
		}
		sent += count
		printMTKFeedProgress(sent, transferLength, &lastProgress)
		seq++
	}
	printMTKFeedProgress(transferLength, transferLength, &lastProgress)
	return seq, nil
}

// zeroMTKFeedBootStatus clears the 512-byte MVII boot-status sector at the end of
// the J36 Ultra UBOOT slot (eMMC USER 0x1f3fe00). Current MVII slot images include a
// baked FLASH_PENDING sector there, but keeping the explicit clear before the
// image write prevents stale diagnostics if an older short LK image is used.
func (c *mtkSerialClient) zeroMTKFeedBootStatus(seq uint32, chunkSize int) (uint32, error) {
	zeros := make([]byte, mviiBootStatusSize)
	hash := sha256.Sum256(zeros)
	fmt.Printf("Clearing MVII boot-status sector at eMMC USER 0x%x (0x%x bytes)\n",
		uint64(mviiBootStatusOffset), uint64(mviiBootStatusSize))
	start := buildMTKFeedStartPayload(uint64(mviiBootStatusOffset), uint64(mviiBootStatusSize),
		uint64(mviiBootStatusSize), uint32(mtkLegacyEMMCPartUser), mtkSerialBlockSize, uint32(chunkSize), 0, hash)
	if err := c.writeMTKFeedFrame(mtkFeedFrameStart, seq, start); err != nil {
		return seq, fmt.Errorf("send feed START for boot-status clear: %w", err)
	}
	if err := c.waitMTKFeedAck(seq, "START boot-status clear", false); err != nil {
		return seq, err
	}
	seq++
	data := buildMTKFeedDataPayload(uint64(mviiBootStatusOffset), 0, zeros)
	if err := c.writeMTKFeedFrame(mtkFeedFrameData, seq, data); err != nil {
		return seq, fmt.Errorf("send feed DATA for boot-status clear: %w", err)
	}
	if err := c.waitMTKFeedAck(seq, "DATA boot-status clear", false); err != nil {
		return seq, err
	}
	seq++
	finish := buildMTKFeedFinishPayload(uint64(mviiBootStatusSize), hash)
	if err := c.writeMTKFeedFrame(mtkFeedFrameFinish, seq, finish); err != nil {
		return seq, fmt.Errorf("send feed FINISH for boot-status clear: %w", err)
	}
	if err := c.waitMTKFeedAck(seq, "FINISH boot-status clear", true); err != nil {
		return seq, err
	}
	seq++
	return seq, nil
}

func (c *mtkSerialClient) streamMTKFeedSparseImage(image string, info androidSparseImageInfo, targetOffset, transferLength uint64, chunkSize int, seq uint32) (uint32, error) {
	src, err := os.Open(image)
	if err != nil {
		return seq, err
	}
	defer src.Close()
	if _, err := src.Seek(int64(info.HeaderSize), io.SeekStart); err != nil {
		return seq, fmt.Errorf("seek Android sparse payload %s: %w", image, err)
	}

	buf := make([]byte, chunkSize)
	header := make([]byte, info.ChunkHdrSize)
	pattern := make([]byte, 4)
	expanded := uint64(0)
	lastProgress := time.Time{}
	for chunkIndex := uint32(0); chunkIndex < info.TotalChunks; chunkIndex++ {
		if _, err := io.ReadFull(src, header); err != nil {
			return seq, fmt.Errorf("read sparse chunk header %d: %w", chunkIndex, err)
		}
		chunkType := binary.LittleEndian.Uint16(header[0:2])
		chunkBlocks := binary.LittleEndian.Uint32(header[4:8])
		totalSize := binary.LittleEndian.Uint32(header[8:12])
		if totalSize < uint32(info.ChunkHdrSize) {
			return seq, fmt.Errorf("sparse chunk %d total size 0x%x is smaller than chunk header 0x%x", chunkIndex, totalSize, info.ChunkHdrSize)
		}
		dataBytes := uint64(chunkBlocks) * uint64(info.BlockSize)
		if dataBytes%mtkSerialBlockSize != 0 {
			return seq, fmt.Errorf("sparse chunk %d expands to unaligned size 0x%x", chunkIndex, dataBytes)
		}
		payloadBytes := uint64(totalSize - uint32(info.ChunkHdrSize))
		if expanded+dataBytes < expanded || expanded+dataBytes > transferLength {
			return seq, fmt.Errorf("sparse chunk %d expands past transfer length 0x%x", chunkIndex, transferLength)
		}

		switch chunkType {
		case androidSparseChunkRaw:
			if payloadBytes != dataBytes {
				return seq, fmt.Errorf("sparse raw chunk %d has payload 0x%x, expected 0x%x", chunkIndex, payloadBytes, dataBytes)
			}
			remaining := dataBytes
			for remaining > 0 {
				count := uint64(chunkSize)
				if remaining < count {
					count = remaining
				}
				chunk := buf[:int(count)]
				if _, err := io.ReadFull(src, chunk); err != nil {
					return seq, fmt.Errorf("read sparse raw chunk %d at expanded 0x%x: %w", chunkIndex, expanded, err)
				}
				if err := c.writeMTKFeedDataAndWait(seq, targetOffset+expanded, expanded, chunk); err != nil {
					return seq, err
				}
				expanded += count
				remaining -= count
				printMTKFeedProgress(expanded, transferLength, &lastProgress)
				seq++
			}
		case androidSparseChunkFill:
			if payloadBytes != 4 {
				return seq, fmt.Errorf("sparse fill chunk %d has payload 0x%x, expected 4", chunkIndex, payloadBytes)
			}
			if _, err := io.ReadFull(src, pattern); err != nil {
				return seq, fmt.Errorf("read sparse fill chunk %d pattern: %w", chunkIndex, err)
			}
			remaining := dataBytes
			for remaining > 0 {
				count := uint64(chunkSize)
				if remaining < count {
					count = remaining
				}
				chunk := buf[:int(count)]
				for pos := 0; pos < len(chunk); pos += len(pattern) {
					copy(chunk[pos:], pattern)
				}
				if err := c.writeMTKFeedDataAndWait(seq, targetOffset+expanded, expanded, chunk); err != nil {
					return seq, err
				}
				expanded += count
				remaining -= count
				printMTKFeedProgress(expanded, transferLength, &lastProgress)
				seq++
			}
		case androidSparseChunkSkip:
			if payloadBytes != 0 {
				return seq, fmt.Errorf("sparse skip chunk %d has payload 0x%x, expected 0", chunkIndex, payloadBytes)
			}
			expanded += dataBytes
			printMTKFeedProgress(expanded, transferLength, &lastProgress)
		case androidSparseChunkCRC:
			if payloadBytes != 4 {
				return seq, fmt.Errorf("sparse CRC chunk %d has payload 0x%x, expected 4", chunkIndex, payloadBytes)
			}
			if _, err := io.ReadFull(src, pattern); err != nil {
				return seq, fmt.Errorf("read sparse CRC chunk %d: %w", chunkIndex, err)
			}
		default:
			return seq, fmt.Errorf("unsupported Android sparse chunk type 0x%04x at chunk %d", chunkType, chunkIndex)
		}
	}
	if expanded != info.ExpandedSize {
		return seq, fmt.Errorf("Android sparse expanded length 0x%x, expected 0x%x", expanded, info.ExpandedSize)
	}
	if transferLength > expanded {
		expanded = transferLength
	}
	if err := c.writeMTKFeedDataAndWait(seq, targetOffset+expanded, expanded, nil); err != nil {
		return seq, err
	}
	printMTKFeedProgress(transferLength, transferLength, &lastProgress)
	return seq + 1, nil
}

func (c *mtkSerialClient) streamMTKFeedSparseImageRange(image string, info androidSparseImageInfo, targetOffset, rangeStart, rangeLength uint64, chunkSize int, seq uint32) (uint32, error) {
	rangeEnd := rangeStart + rangeLength
	if rangeLength == 0 || rangeEnd < rangeStart || rangeEnd > info.ExpandedSize {
		return seq, fmt.Errorf("sparse segment 0x%x..0x%x is outside expanded image size 0x%x", rangeStart, rangeEnd, info.ExpandedSize)
	}

	src, err := os.Open(image)
	if err != nil {
		return seq, err
	}
	defer src.Close()
	if _, err := src.Seek(int64(info.HeaderSize), io.SeekStart); err != nil {
		return seq, fmt.Errorf("seek Android sparse payload %s: %w", image, err)
	}

	buf := make([]byte, chunkSize)
	header := make([]byte, info.ChunkHdrSize)
	pattern := make([]byte, 4)
	expanded := uint64(0)
	writtenEnd := uint64(0)
	lastProgress := time.Time{}
	for chunkIndex := uint32(0); chunkIndex < info.TotalChunks; chunkIndex++ {
		if _, err := io.ReadFull(src, header); err != nil {
			return seq, fmt.Errorf("read sparse chunk header %d: %w", chunkIndex, err)
		}
		chunkType := binary.LittleEndian.Uint16(header[0:2])
		chunkBlocks := binary.LittleEndian.Uint32(header[4:8])
		totalSize := binary.LittleEndian.Uint32(header[8:12])
		if totalSize < uint32(info.ChunkHdrSize) {
			return seq, fmt.Errorf("sparse chunk %d total size 0x%x is smaller than chunk header 0x%x", chunkIndex, totalSize, info.ChunkHdrSize)
		}
		dataBytes := uint64(chunkBlocks) * uint64(info.BlockSize)
		payloadBytes := uint64(totalSize - uint32(info.ChunkHdrSize))
		chunkStart := expanded
		chunkEnd := expanded + dataBytes
		if chunkEnd < chunkStart || chunkEnd > info.ExpandedSize {
			return seq, fmt.Errorf("sparse chunk %d expands outside image size", chunkIndex)
		}
		overlapStart := chunkStart
		if overlapStart < rangeStart {
			overlapStart = rangeStart
		}
		overlapEnd := chunkEnd
		if overlapEnd > rangeEnd {
			overlapEnd = rangeEnd
		}

		switch chunkType {
		case androidSparseChunkRaw:
			if payloadBytes != dataBytes {
				return seq, fmt.Errorf("sparse raw chunk %d has payload 0x%x, expected 0x%x", chunkIndex, payloadBytes, dataBytes)
			}
			if overlapStart >= overlapEnd {
				if _, err := src.Seek(int64(payloadBytes), io.SeekCurrent); err != nil {
					return seq, fmt.Errorf("skip sparse raw chunk %d: %w", chunkIndex, err)
				}
				break
			}
			skipBefore := overlapStart - chunkStart
			if skipBefore != 0 {
				if _, err := src.Seek(int64(skipBefore), io.SeekCurrent); err != nil {
					return seq, fmt.Errorf("seek sparse raw chunk %d overlap: %w", chunkIndex, err)
				}
			}
			remaining := overlapEnd - overlapStart
			streamBase := overlapStart - rangeStart
			done := uint64(0)
			for remaining > 0 {
				count := uint64(chunkSize)
				if remaining < count {
					count = remaining
				}
				chunk := buf[:int(count)]
				if _, err := io.ReadFull(src, chunk); err != nil {
					return seq, fmt.Errorf("read sparse raw chunk %d at expanded 0x%x: %w", chunkIndex, overlapStart+done, err)
				}
				stream := streamBase + done
				if err := c.writeMTKFeedDataAndWait(seq, targetOffset+stream, stream, chunk); err != nil {
					return seq, err
				}
				done += count
				remaining -= count
				writtenEnd = stream + count
				printMTKFeedProgress(writtenEnd, rangeLength, &lastProgress)
				seq++
			}
			if skipAfter := chunkEnd - overlapEnd; skipAfter != 0 {
				if _, err := src.Seek(int64(skipAfter), io.SeekCurrent); err != nil {
					return seq, fmt.Errorf("seek sparse raw chunk %d tail: %w", chunkIndex, err)
				}
			}
		case androidSparseChunkFill:
			if payloadBytes != 4 {
				return seq, fmt.Errorf("sparse fill chunk %d has payload 0x%x, expected 4", chunkIndex, payloadBytes)
			}
			if _, err := io.ReadFull(src, pattern); err != nil {
				return seq, fmt.Errorf("read sparse fill chunk %d pattern: %w", chunkIndex, err)
			}
			if overlapStart >= overlapEnd {
				break
			}
			remaining := overlapEnd - overlapStart
			streamBase := overlapStart - rangeStart
			patternBase := overlapStart - chunkStart
			done := uint64(0)
			for remaining > 0 {
				count := uint64(chunkSize)
				if remaining < count {
					count = remaining
				}
				chunk := buf[:int(count)]
				for pos := range chunk {
					chunk[pos] = pattern[(patternBase+done+uint64(pos))&3]
				}
				stream := streamBase + done
				if err := c.writeMTKFeedDataAndWait(seq, targetOffset+stream, stream, chunk); err != nil {
					return seq, err
				}
				done += count
				remaining -= count
				writtenEnd = stream + count
				printMTKFeedProgress(writtenEnd, rangeLength, &lastProgress)
				seq++
			}
		case androidSparseChunkSkip:
			if payloadBytes != 0 {
				return seq, fmt.Errorf("sparse skip chunk %d has payload 0x%x, expected 0", chunkIndex, payloadBytes)
			}
		case androidSparseChunkCRC:
			if payloadBytes != 4 {
				return seq, fmt.Errorf("sparse CRC chunk %d has payload 0x%x, expected 4", chunkIndex, payloadBytes)
			}
			if _, err := io.ReadFull(src, pattern); err != nil {
				return seq, fmt.Errorf("read sparse CRC chunk %d: %w", chunkIndex, err)
			}
			dataBytes = 0
		default:
			return seq, fmt.Errorf("unsupported Android sparse chunk type 0x%04x at chunk %d", chunkType, chunkIndex)
		}
		expanded += dataBytes
		if expanded >= rangeEnd && chunkType != androidSparseChunkCRC {
			break
		}
	}
	if writtenEnd != rangeLength {
		if err := c.writeMTKFeedDataAndWait(seq, targetOffset+rangeLength, rangeLength, nil); err != nil {
			return seq, err
		}
		seq++
	}
	printMTKFeedProgress(rangeLength, rangeLength, &lastProgress)
	return seq, nil
}

func (c *mtkSerialClient) writeMTKFeedDataAndWait(seq uint32, target, stream uint64, data []byte) error {
	payload := buildMTKFeedDataPayload(target, stream, data)
	if err := c.writeMTKFeedFrame(mtkFeedFrameData, seq, payload); err != nil {
		return fmt.Errorf("send feed DATA seq %d at 0x%x: %w", seq, stream, err)
	}
	return c.waitMTKFeedAck(seq, fmt.Sprintf("DATA seq %d at 0x%x", seq, stream), false)
}

func (c *mtkSerialClient) waitMTKFeedAck(seq uint32, label string, final bool) error {
	timeout := envDuration("MVII_MTK_FEED_ACK_TIMEOUT", 30*time.Second)
	deadline := time.Now().Add(timeout)
	var lastErr error
	for time.Now().Before(deadline) {
		frame, err := c.readMTKFeedFrame(timeoutUntil(deadline, 5*time.Second))
		if err != nil {
			lastErr = err
			if isDeviceGoneError(err) {
				break
			}
			continue
		}
		switch frame.Type {
		case mtkFeedFrameAck, mtkFeedFrameDone:
			if frame.Seq != seq {
				fmt.Printf("Ignoring feed ACK for seq %d while waiting for seq %d.\n", frame.Seq, seq)
				continue
			}
			if final && frame.Type != mtkFeedFrameDone {
				fmt.Println("Payload ACKed FINISH; waiting for DONE.")
				continue
			}
			status, message := parseMTKFeedStatus(frame.Payload)
			if status != 0 {
				return fmt.Errorf("feed payload rejected %s: status=0x%x %s", label, status, message)
			}
			if message != "" {
				fmt.Printf("payload: %s\n", message)
			}
			return nil
		case mtkFeedFrameLog:
			if !c.feedQuiet {
				printMTKFeedLog(frame.Payload)
			}
		case mtkFeedFrameProgress:
			if !c.feedQuiet {
				printMTKFeedPayloadProgress(frame.Payload)
			}
		case mtkFeedFrameHello:
			// A queued HELLO response can arrive after START if the payload
			// answered a readiness probe just before START was sent.
			continue
		case mtkFeedFrameError:
			status, message := parseMTKFeedStatus(frame.Payload)
			return fmt.Errorf("feed payload error while waiting for %s: status=0x%x %s", label, status, message)
		default:
			fmt.Printf("Ignoring feed frame type 0x%04x seq %d while waiting for %s.\n", frame.Type, frame.Seq, label)
		}
	}
	if lastErr != nil {
		return fmt.Errorf("timed out waiting for feed ACK for %s: %w", label, lastErr)
	}
	return fmt.Errorf("timed out waiting for feed ACK for %s", label)
}

func (c *mtkSerialClient) waitMTKFeedReadDataAt(seq uint32, label string, wantOffset uint64, wantLength uint32) ([]byte, error) {
	timeout := envDuration("MVII_MTK_FEED_ACK_TIMEOUT", 30*time.Second)
	deadline := time.Now().Add(timeout)
	var lastErr error
	for time.Now().Before(deadline) {
		frame, err := c.readMTKFeedFrame(timeoutUntil(deadline, 5*time.Second))
		if err != nil {
			lastErr = err
			if isDeviceGoneError(err) {
				break
			}
			continue
		}
		switch frame.Type {
		case mtkFeedFrameReadData:
			if frame.Seq != seq {
				fmt.Printf("Ignoring feed READDATA for seq %d while waiting for seq %d.\n", frame.Seq, seq)
				continue
			}
			if len(frame.Payload) < 12 {
				return nil, fmt.Errorf("feed payload returned truncated READDATA for %s", label)
			}
			offset := binary.LittleEndian.Uint64(frame.Payload[0:8])
			length := binary.LittleEndian.Uint32(frame.Payload[8:12])
			data := frame.Payload[12:]
			if offset != wantOffset || length != wantLength || length != uint32(len(data)) {
				return nil, fmt.Errorf("feed payload returned READDATA offset/length 0x%x/0x%x, got %d bytes",
					offset, length, len(data))
			}
			return append([]byte(nil), data...), nil
		case mtkFeedFrameLog:
			printMTKFeedLog(frame.Payload)
		case mtkFeedFrameProgress:
			printMTKFeedPayloadProgress(frame.Payload)
		case mtkFeedFrameError:
			status, message := parseMTKFeedStatus(frame.Payload)
			return nil, fmt.Errorf("feed payload error while waiting for %s: status=0x%x %s", label, status, message)
		case mtkFeedFrameAck, mtkFeedFrameDone:
			status, message := parseMTKFeedStatus(frame.Payload)
			if status != 0 {
				return nil, fmt.Errorf("feed payload rejected %s: status=0x%x %s", label, status, message)
			}
			if message != "" {
				fmt.Printf("payload: %s\n", message)
			}
		default:
			fmt.Printf("Ignoring feed frame type 0x%04x seq %d while waiting for %s.\n", frame.Type, frame.Seq, label)
		}
	}
	if lastErr != nil {
		return nil, fmt.Errorf("timed out waiting for feed READDATA for %s: %w", label, lastErr)
	}
	return nil, fmt.Errorf("timed out waiting for feed READDATA for %s", label)
}

func (c *mtkSerialClient) writeMTKFeedFrame(frameType uint16, seq uint32, payload []byte) error {
	return c.writeRaw(encodeMTKFeedFrame(frameType, seq, payload))
}

func encodeMTKFeedFrame(frameType uint16, seq uint32, payload []byte) []byte {
	out := make([]byte, mtkFeedHeaderSize+len(payload))
	copy(out[0:4], mtkFeedMagic)
	binary.LittleEndian.PutUint16(out[4:6], mtkFeedVersion)
	binary.LittleEndian.PutUint16(out[6:8], frameType)
	binary.LittleEndian.PutUint32(out[8:12], seq)
	binary.LittleEndian.PutUint32(out[12:16], uint32(len(payload)))
	binary.LittleEndian.PutUint32(out[16:20], crc32.ChecksumIEEE(payload))
	copy(out[20:], payload)
	return out
}

func (c *mtkSerialClient) readMTKFeedFrame(timeout time.Duration) (mtkFeedFrame, error) {
	deadline := time.Now().Add(timeout)
	accum := make([]byte, 0, 256)
	slice := func() time.Duration { return timeoutUntil(deadline, 2*time.Second) }
	var lastErr error

	for time.Now().Before(deadline) {
		// Read 1 byte at a time during hunt so that a timeout never causes us to
		// drop a partial magic prefix; bytes stay in accum across iterations.
		b, err := c.readN(1, slice())
		if len(b) > 0 {
			accum = append(accum, b...)
			// Bound accum during long desyncs; keep tail that could start a magic.
			if len(accum) > 1024 {
				if i := bytes.Index(accum, mtkFeedMagic); i >= 0 {
					accum = accum[i:]
				} else {
					if k := len(accum) - 3; k > 0 {
						accum = accum[k:]
					}
				}
			}
		}
		if err != nil {
			if isDeviceGoneError(err) {
				return mtkFeedFrame{}, err
			}
			lastErr = err
			// timeout or transient: keep any partial magic in accum and retry
		}

		idx := bytes.Index(accum, mtkFeedMagic)
		if idx < 0 {
			continue
		}
		// Ensure we have a full header after the magic we found.
		if len(accum)-idx < mtkFeedHeaderSize {
			need := mtkFeedHeaderSize - (len(accum) - idx)
			more, rerr := c.readN(need, slice())
			if len(more) > 0 {
				accum = append(accum, more...)
			}
			if rerr != nil {
				if isDeviceGoneError(rerr) {
					return mtkFeedFrame{}, rerr
				}
				accum = accum[idx:]
				continue
			}
			idx = bytes.Index(accum, mtkFeedMagic)
			if idx < 0 || len(accum)-idx < mtkFeedHeaderSize {
				continue
			}
		}

		hdr := accum[idx : idx+mtkFeedHeaderSize]
		frame, length, checksum, perr := parseMTKFeedFrameHeader(hdr)
		if perr != nil {
			accum = accum[idx+1:]
			continue
		}
		total := mtkFeedHeaderSize + int(length)
		if len(accum)-idx < total {
			need := total - (len(accum) - idx)
			more, rerr := c.readN(need, slice())
			if len(more) > 0 {
				accum = append(accum, more...)
			}
			if rerr != nil {
				if isDeviceGoneError(rerr) {
					return mtkFeedFrame{}, rerr
				}
				accum = accum[idx:]
				continue
			}
		}
		if len(accum)-idx < total {
			accum = accum[idx:]
			continue
		}

		payload := accum[idx+mtkFeedHeaderSize : idx+total]
		if got := crc32.ChecksumIEEE(payload); got != checksum {
			accum = accum[idx+1:]
			continue
		}
		frame.Payload = append([]byte(nil), payload...)
		return frame, nil
	}
	if lastErr != nil {
		return mtkFeedFrame{}, fmt.Errorf("timed out waiting for feed frame: %w", lastErr)
	}
	return mtkFeedFrame{}, fmt.Errorf("timed out waiting for feed frame")
}

func decodeMTKFeedFrame(data []byte) (mtkFeedFrame, error) {
	frame, length, checksum, err := parseMTKFeedFrameHeader(data)
	if err != nil {
		return mtkFeedFrame{}, err
	}
	if uint64(len(data)-mtkFeedHeaderSize) != uint64(length) {
		return mtkFeedFrame{}, fmt.Errorf("feed frame length mismatch: header 0x%x actual 0x%x", length, len(data)-mtkFeedHeaderSize)
	}
	frame.Payload = append([]byte(nil), data[mtkFeedHeaderSize:]...)
	if got := crc32.ChecksumIEEE(frame.Payload); got != checksum {
		return mtkFeedFrame{}, fmt.Errorf("feed frame CRC mismatch: got 0x%08x want 0x%08x", got, checksum)
	}
	return frame, nil
}

func parseMTKFeedFrameHeader(data []byte) (mtkFeedFrame, uint32, uint32, error) {
	if len(data) < mtkFeedHeaderSize {
		return mtkFeedFrame{}, 0, 0, io.ErrUnexpectedEOF
	}
	if !bytes.Equal(data[0:4], mtkFeedMagic) {
		return mtkFeedFrame{}, 0, 0, fmt.Errorf("feed frame magic %q, want %q", string(data[0:4]), string(mtkFeedMagic))
	}
	version := binary.LittleEndian.Uint16(data[4:6])
	if version != mtkFeedVersion {
		return mtkFeedFrame{}, 0, 0, fmt.Errorf("feed frame version %d, want %d", version, mtkFeedVersion)
	}
	frame := mtkFeedFrame{
		Type: binary.LittleEndian.Uint16(data[6:8]),
		Seq:  binary.LittleEndian.Uint32(data[8:12]),
	}
	length := binary.LittleEndian.Uint32(data[12:16])
	checksum := binary.LittleEndian.Uint32(data[16:20])
	return frame, length, checksum, nil
}

func parseMTKFeedHello(payload []byte) mtkFeedHello {
	hello := mtkFeedHello{}
	if len(payload) >= 12 {
		hello.MaxChunk = binary.LittleEndian.Uint32(payload[0:4])
		hello.BlockSize = binary.LittleEndian.Uint32(payload[4:8])
		hello.Flags = binary.LittleEndian.Uint32(payload[8:12])
		hello.Name = trimMTKFeedString(payload[12:])
	} else {
		hello.Name = trimMTKFeedString(payload)
	}
	return hello
}

func parseMTKFeedStatus(payload []byte) (uint32, string) {
	if len(payload) < 4 {
		return 0, trimMTKFeedString(payload)
	}
	return binary.LittleEndian.Uint32(payload[0:4]), trimMTKFeedString(payload[4:])
}

func mviiBootStatusStageName(stage uint32) string {
	switch stage {
	case 0x1001:
		return "flash complete; waiting for MVII LK"
	case 0x1101:
		return "MVII LK stage1 entered"
	case 0x1102:
		return "MVII LK stage1 unpacking wrapped stage2"
	case 0x1103:
		return "MVII LK stage1 could not find stage2"
	case 0x1104:
		return "MVII LK stage1 handing off to stage2"
	case 0x1105:
		return "MVII LK stage1 USB charger armed (latent)"
	case 0x1106:
		return "MVII LK stage1 no VBUS; charger idle"
	case 0x1107:
		return "MVII LK stage1 charging empty battery at splash (survival hold)"
	case 0x1201:
		return "MVII minimal LK entered (UBOOT slot)"
	case 0x1202:
		return "MVII minimal LK armed the PMIC input path"
	case 0x1203:
		return "MVII minimal LK panel scanning out"
	case 0x1204:
		return "MVII minimal LK display bring-up failed; booting headless"
	case 0x1205:
		return "MVII minimal LK could not load boot.img"
	case 0x1206:
		return "MVII minimal LK jumping to the boot.img kernel"
	case 0x1207:
		return "MVII minimal LK eMMC ready (boot log now live on storage)"
	case 0x1208:
		return "MVII minimal LK accepted the boot.img header"
	case 0x1209:
		return "MVII minimal LK loaded the kernel into DRAM"
	case 0x120a:
		return "MVII minimal LK loaded the ramdisk into DRAM"
	case 0x120b:
		return "MVII minimal LK halted on a CPU exception (see the console ring for vector/PC/DFSR)"
	case 0x2001:
		return "MVII ARM stage2 entered"
	case 0x2002:
		return "display framebuffer bound"
	case 0x2003:
		return "display framebuffer not bound"
	case 0x2004:
		return "storage probed"
	case 0x2005:
		return "storage probe started"
	case 0x2006:
		return "storage layout completed"
	case 0x2007:
		return "eMMC block device ready"
	case 0x2008:
		return "MBR scan completed"
	case 0x2009:
		return "GPT scan completed"
	case 0x200a:
		return "stage2 handoff pending"
	case 0x200b:
		return "EFI-style Multiboot2 handoff buffer built"
	case 0x200c:
		return "stage2 handoff splash painted"
	case 0x200d:
		return "display scanout programming started"
	case 0x200e:
		return "display scanout programmed"
	case 0x200f:
		return "display scanout programming failed"
	case 0x2010:
		return "Multiboot2 handoff build begin"
	case 0x2011:
		return "Multiboot2 handoff buffer zeroed"
	case 0x2012:
		return "Multiboot2 framebuffer tag"
	case 0x2013:
		return "Multiboot2 boot-volume tag"
	case 0x2014:
		return "Multiboot2 end tag"
	case 0x2015:
		return "Multiboot2 total size written"
	case 0x2016:
		return "Multiboot2 framebuffer tag header"
	case 0x2017:
		return "Multiboot2 framebuffer address"
	case 0x2018:
		return "Multiboot2 framebuffer geometry"
	case 0x2019:
		return "Multiboot2 framebuffer format"
	case 0x201a:
		return "Multiboot2 framebuffer tag complete"
	case 0x201b:
		return "display safe profile completed"
	case 0x201c:
		return "DSI command-mode splash attempted"
	case 0x2101:
		return "EFI-style MVII runtime handoff"
	case 0x2102:
		return "MVII runtime entry not linked"
	case 0x2103:
		return "MVII runtime returned"
	case 0x2104:
		return "entering MVII boot_main"
	case 0x2201:
		return "MVII ARM runtime entered"
	case 0x2202:
		return "MVII runtime boot volume identified"
	case 0x2203:
		return "MVII runtime headless storage wait"
	case 0x2204:
		return "MVII ARM runtime heartbeat"
	case 0x2ffe:
		return "device key requested BROM reset"
	case 0x2fff:
		return "stage2 heartbeat loop"
	default:
		return "unknown"
	}
}

func mviiDisplayDiagPhaseName(phase uint32) string {
	switch phase & 0x7f {
	case 0x00:
		if phase&0x80 != 0 {
			return "DSI video transfer started"
		}
		return "unknown"
	case 0x01:
		return "validate framebuffer"
	case 0x02:
		return "MMSYS clocks"
	case 0x03:
		return "MIPI DSI PHY/PLL"
	case 0x04:
		return "DSI DCS wake/skip"
	case 0x05:
		return "RDMA/COLOR/BLS mutex route"
	case 0x06:
		return "RDMA memory source"
	case 0x07:
		return "DSI video timing"
	case 0x08:
		return "COLOR/BLS blocks"
	case 0x09:
		return "DSI video interrupts"
	case 0x0a:
		return "DSI video packet format"
	case 0x0b:
		return "DSI vertical timing"
	case 0x0c:
		return "DSI horizontal timing"
	case 0x0d:
		return "DSI host PHY timing"
	case 0x0e:
		return "DSI video mode switch"
	case 0x0f:
		return "DSI host access skipped"
	case 0x10:
		return "LCM power/reset sequence"
	case 0x11:
		return "DSI command mode setup"
	case 0x12:
		return "DCS soft reset"
	case 0x13:
		return "DCS RGB888 pixel format"
	case 0x14:
		return "DCS sleep out"
	case 0x15:
		return "DCS display on"
	case 0x16:
		return "DSI interrupt registers skipped"
	case 0x17:
		return "DSI video transfer start"
	case 0x18:
		return "DSI command mode write skipped"
	case 0x19:
		return "DSI command lane count"
	case 0x1a:
		return "DSI command packet format"
	case 0x1b:
		return "DSI read-ack path"
	case 0x1c:
		return "DSI memory-write continuation"
	case 0x1d:
		return "DSI command lane-count write skipped"
	case 0x1e:
		return "DSI video lane-count write skipped"
	case 0x1f:
		return "DSI command packet write skipped"
	case 0x20:
		return "DSI read-ack write skipped"
	case 0x21:
		return "DSI command memory-continuation write skipped"
	case 0x22:
		return "DSI video packet write skipped"
	case 0x23:
		return "DSI video memory-continuation write skipped"
	case 0x24:
		return "DSI DCS command queue header"
	case 0x25:
		return "DSI DCS command queue size"
	case 0x26:
		return "DSI DCS command queue start"
	case 0x27:
		return "DSI DCS command queue stop"
	case 0x28:
		return "DSI DCS command queue write skipped"
	case 0x29:
		return "DSI engine enable"
	case 0x2a:
		return "DSI engine enable skipped"
	case 0x2b:
		return "DSI engine disable"
	case 0x2c:
		return "DCS display off"
	case 0x2d:
		return "DCS memory address mode"
	case 0x2e:
		return "DCS tearing-effect line"
	case 0x2f:
		return "DCS display inversion"
	case 0x30:
		return "DSI vertical sync active"
	case 0x31:
		return "DSI vertical back porch"
	case 0x32:
		return "DSI vertical front porch"
	case 0x33:
		return "DSI vertical active"
	case 0x34:
		return "DSI horizontal sync active"
	case 0x35:
		return "DSI horizontal back porch"
	case 0x36:
		return "DSI horizontal front porch"
	case 0x37:
		return "DSI blanking low-power packet"
	case 0x38:
		return "DSI HS clock trail word count"
	case 0x39:
		return "DSI video mode register write"
	case 0x3a:
		return "DSI DCS engine enable"
	case 0x3b:
		return "DSI DCS engine enable skipped"
	case 0x3c:
		return "DSI video engine enable"
	case 0x3d:
		return "DSI video engine enable skipped"
	case 0x3e:
		return "DSI engine disable skipped"
	case 0x3f:
		return "DSI video lane-count register write"
	case 0x40:
		return "DSI video packet-size register write"
	case 0x41:
		return "DSI video timing registers skipped"
	case 0x42:
		return "DSI video mode register skipped"
	case 0x43:
		return "COLOR block skipped"
	case 0x44:
		return "COLOR width register"
	case 0x45:
		return "COLOR height register"
	case 0x46:
		return "COLOR main config register"
	case 0x47:
		return "COLOR start register"
	case 0x48:
		return "BLS reset register"
	case 0x49:
		return "BLS debug register"
	case 0x4a:
		return "BLS PWM duty register"
	case 0x4b:
		return "BLS setting register"
	case 0x4c:
		return "BLS FANA setting register"
	case 0x4d:
		return "BLS source size register"
	case 0x4e:
		return "BLS gain register"
	case 0x4f:
		return "BLS gamma registers"
	case 0x50:
		return "BLS PWM control registers"
	case 0x51:
		return "BLS interrupt enable register"
	case 0x52:
		return "BLS enable register"
	case 0x53:
		return "BLS block skipped"
	case 0x54:
		return "display mutex route skipped"
	case 0x55:
		return "MMSYS OVL output route"
	case 0x56:
		return "MMSYS display output select"
	case 0x57:
		return "display mutex disable"
	case 0x58:
		return "display mutex reset"
	case 0x59:
		return "display mutex module route"
	case 0x5a:
		return "display mutex DSI SOF"
	case 0x5b:
		return "display mutex interrupt status"
	case 0x5c:
		return "display mutex interrupt enable"
	case 0x5d:
		return "display mutex enable"
	case 0x60:
		return "RDMA block skipped"
	case 0x61:
		return "RDMA reset skipped"
	case 0x62:
		return "RDMA global reset"
	case 0x63:
		return "RDMA interrupt status clear"
	case 0x64:
		return "RDMA output size"
	case 0x65:
		return "RDMA memory format"
	case 0x66:
		return "RDMA framebuffer base"
	case 0x67:
		return "RDMA framebuffer pitch"
	case 0x68:
		return "RDMA memory GMC"
	case 0x69:
		return "RDMA FIFO"
	case 0x6a:
		return "RDMA interrupt enable"
	case 0x6b:
		return "RDMA engine enable"
	case 0x6c:
		return "RDMA interrupt status clear skipped"
	case 0x6d:
		return "RDMA interrupt enable skipped"
	case 0x6e:
		return "RDMA framebuffer register batch"
	case 0x6f:
		return "RDMA framebuffer register batch done"
	case 0x70:
		return "RDMA geometry register batch"
	case 0x71:
		return "RDMA framebuffer source register batch"
	case 0x72:
		return "RDMA arbitration/FIFO register batch"
	case 0x73:
		return "RDMA geometry skipped"
	case 0x74:
		return "RDMA framebuffer source skipped"
	case 0x75:
		return "RDMA arbitration/FIFO skipped"
	case 0x76:
		return "RDMA engine enable skipped"
	case 0x77:
		return "RDMA width register"
	case 0x78:
		return "RDMA height register"
	case 0x79:
		return "RDMA width skipped"
	case 0x7a:
		return "RDMA height skipped"
	case 0x7b:
		return "DSI_START skipped"
	case 0x7c:
		return "display safe profile completed, no scanout start"
	case 0x7d:
		return "DSI command-mode splash address window"
	case 0x7e:
		return "DSI command-mode splash pixels"
	case 0x7f:
		return "DSI command-mode splash complete"
	default:
		return "unknown"
	}
}

func printMVIIBootStatus(data []byte) {
	printMVIIBootStatusAt(mviiBootStatusOffset, data)
}

func printMVIIBootStatusAt(offset uint64, data []byte) {
	fmt.Printf("Device status @0x%x:\n", offset)
	if len(data) < mviiBootStatusSize {
		fmt.Printf("  read: short sector, got 0x%x bytes and wanted 0x%x\n", len(data), mviiBootStatusSize)
		return
	}
	magic := binary.LittleEndian.Uint32(data[0:4])
	if magic != mviiBootStatusMagic {
		fmt.Printf("  boot: no MVII status record here (magic 0x%08x, want 0x%08x)\n",
			magic, uint32(mviiBootStatusMagic))
		return
	}
	version := binary.LittleEndian.Uint32(data[4:8])
	recordSize := binary.LittleEndian.Uint32(data[8:12])
	stage := binary.LittleEndian.Uint32(data[12:16])
	flags := binary.LittleEndian.Uint32(data[16:20])
	sequence := binary.LittleEndian.Uint32(data[20:24])
	fbAddr := binary.LittleEndian.Uint32(data[24:28])
	fbWidth := binary.LittleEndian.Uint32(data[28:32])
	fbHeight := binary.LittleEndian.Uint32(data[32:36])
	fbPitch := binary.LittleEndian.Uint32(data[36:40])
	fbBPP := binary.LittleEndian.Uint32(data[40:44])
	lastError := binary.LittleEndian.Uint32(data[92:96])
	messageSize := mviiBootStatusV1MessageSize
	if version >= 2 {
		messageSize = mviiBootStatusV2MessageSize
	}
	if version >= 3 {
		messageSize = mviiBootStatusV3MessageSize
	}
	message := trimMTKFeedString(data[mviiBootStatusMessageOffset : mviiBootStatusMessageOffset+messageSize])

	framebufferBound := flags&mviiBootStatusFlagFramebuffer != 0
	live := flags&mviiBootStatusFlagLive != 0
	fmt.Printf("  record: v%d size=0x%x seq=%d flags=0x%08x\n", version, recordSize, sequence, flags)
	fmt.Printf("  boot: %s\n", mviiBootStatusSummary(stage, flags, message))
	if framebufferBound && fbAddr != 0 {
		fmt.Printf("  screen: framebuffer 0x%08x %dx%d pitch=%d bpp=%d\n",
			fbAddr, fbWidth, fbHeight, fbPitch, fbBPP)
	} else {
		fmt.Println("  screen: no framebuffer report in this sector")
	}
	if lastError != 0 {
		fmt.Printf("  marker: %s\n", mviiBootStatusMarker(lastError))
	}
	if version >= 3 {
		printMVIIBootStatusV3Registers(data, lastError, message)
	}
	// A live stage1 record carries an end marker at the sector tail: its magic
	// plus a copy of the head sequence. Validating it here is what turns a single
	// read into an authoritative one instead of a possibly torn or outdated guess.
	if live {
		endMagic := binary.LittleEndian.Uint32(data[mviiBootStatusEndOffset : mviiBootStatusEndOffset+4])
		endSeq := binary.LittleEndian.Uint32(data[mviiBootStatusEndOffset+4 : mviiBootStatusEndOffset+8])
		switch {
		case endMagic != mviiBootStatusEndMagic:
			fmt.Printf("  commit: INCOMPLETE - end marker missing (tail 0x%08x, want 0x%08x); re-read recommended\n",
				endMagic, uint32(mviiBootStatusEndMagic))
		case endSeq != sequence:
			fmt.Printf("  commit: TORN/OUTDATED - head seq=%d but tail seq=%d; re-read recommended\n",
				sequence, endSeq)
		default:
			fmt.Printf("  commit: consistent (end marker seq=%d)\n", endSeq)
		}
		if flags&mviiBootStatusFlagError != 0 {
			fmt.Println("  state: ERROR flag set - a stage1 step reported failure (see marker)")
		}
		if flags&mviiBootStatusFlagComplete != 0 {
			fmt.Println("  state: COMPLETE - stage1 reached its idle end marker")
		}
	}
	if !live && flags&mviiBootStatusFlagFlash != 0 && stage == 0x1001 {
		fmt.Println("  note: baked marker only; stage1 did not run far enough to complete its first status-sector write.")
	} else if message != "" {
		// The runtime console mirror packs the newest serial lines into the
		// message field separated by " | "; render them as a per-line log.
		if lines := strings.Split(message, " | "); len(lines) > 1 {
			fmt.Println("  console (newest serial lines, oldest may be truncated):")
			for _, l := range lines {
				fmt.Printf("    | %s\n", l)
			}
		} else {
			fmt.Printf("  note: %s\n", message)
		}
	}
}

func mviiBootStatusU32(data []byte, offset int) uint32 {
	if offset < 0 || offset+4 > len(data) {
		return 0
	}
	return binary.LittleEndian.Uint32(data[offset : offset+4])
}

func printMVIIBootStatusV3Registers(data []byte, lastError uint32, message string) {
	if len(data) < mviiBootStatusSize {
		return
	}
	isLKScan := isMVIIBootStatusLKScanMarker(lastError)
	mmsys0 := mviiBootStatusU32(data, mviiBootStatusMMSYSOffset)
	mmsys1 := mviiBootStatusU32(data, mviiBootStatusMMSYSOffset+4)
	lcmRst := mviiBootStatusU32(data, mviiBootStatusMMSYSOffset+8)
	pwmBase := mviiBootStatusU32(data, mviiBootStatusMMSYSOffset+12)
	if !isLKScan && mmsys0 == 0 && mmsys1 == 0 && lcmRst == 0 && pwmBase == 0 {
		return
	}

	fmt.Printf("  entry: r0=0x%08x r1=0x%08x r2=0x%08x r3=0x%08x\n",
		mviiBootStatusU32(data, mviiBootStatusEntryOffset),
		mviiBootStatusU32(data, mviiBootStatusEntryOffset+4),
		mviiBootStatusU32(data, mviiBootStatusEntryOffset+8),
		mviiBootStatusU32(data, mviiBootStatusEntryOffset+12))
	fmt.Printf("  mmsys: cg0=0x%08x cg1=0x%08x lcm_rst=0x%08x pwm_base=0x%08x\n",
		mmsys0, mmsys1, lcmRst, pwmBase)

	if isLKScan {
		kpdSta := mviiBootStatusU32(data, mviiBootStatusLKExtraOffset)
		kpdMem1 := mviiBootStatusU32(data, mviiBootStatusLKExtraOffset+4)
		kpdMem2 := mviiBootStatusU32(data, mviiBootStatusLKExtraOffset+8)
		kpdMem3 := mviiBootStatusU32(data, mviiBootStatusLKExtraOffset+12)
		fmt.Printf("  lk-extra: kpd_sta=0x%08x kpd_mem1=0x%08x kpd_mem2=0x%08x kpd_mem3=0x%08x\n",
			kpdSta, kpdMem1, kpdMem2, kpdMem3)
		fmt.Printf("  kpd-bits: set=%s (raw matrix bits, compare held-button logs for polarity)\n",
			formatMTKKeypadBits(kpdMem1, kpdMem2, kpdMem3))
		if g0, ok0 := extractMTKHexField(message, "g0i"); ok0 {
			if g5, ok5 := extractMTKHexField(message, "g5i"); ok5 {
				if g7, ok7 := extractMTKHexField(message, "g7i"); ok7 {
					fmt.Printf("  gpio-in: g0=%s g5=%s g7=%s abs=%s\n",
						formatMTKBitList(g0, 0, 16),
						formatMTKBitList(g5, 80, 16),
						formatMTKBitList(g7, 112, 16),
						formatMTKGPIOAbsBits(g0, g5, g7))
				}
			}
		}
		fmt.Printf("  dsi: start=0x%08x mode=0x%08x int=0x%08x txrx=0x%08x ps=0x%08x vm=0x%08x lc=0x%08x ld0=0x%08x\n",
			mviiBootStatusU32(data, mviiBootStatusV3BootargOffset),
			mviiBootStatusU32(data, mviiBootStatusV3BootargOffset+4),
			mviiBootStatusU32(data, mviiBootStatusV3BootargOffset+8),
			mviiBootStatusU32(data, mviiBootStatusV3BootargOffset+12),
			mviiBootStatusU32(data, mviiBootStatusV3BootargOffset+16),
			mviiBootStatusU32(data, mviiBootStatusV3BootargOffset+20),
			mviiBootStatusU32(data, mviiBootStatusV3BootargOffset+24),
			mviiBootStatusU32(data, mviiBootStatusV3BootargOffset+28))
		fmt.Printf("  ddp: mout=0x%08x out=0x%08x ovl_en=0x%08x ovl_src=0x%08x ovl_bg=0x%08x rdma=0x%08x rdma_mem=0x%08x mutex_mod=0x%08x\n",
			mviiBootStatusU32(data, mviiBootStatusV3VisibleOffset),
			mviiBootStatusU32(data, mviiBootStatusV3VisibleOffset+4),
			mviiBootStatusU32(data, mviiBootStatusV3VisibleOffset+8),
			mviiBootStatusU32(data, mviiBootStatusV3VisibleOffset+12),
			mviiBootStatusU32(data, mviiBootStatusV3VisibleOffset+16),
			mviiBootStatusU32(data, mviiBootStatusV3VisibleOffset+20),
			mviiBootStatusU32(data, mviiBootStatusV3VisibleOffset+24),
			mviiBootStatusU32(data, mviiBootStatusV3VisibleOffset+28))
		fmt.Printf("  bls: en=0x%08x con0=0x%08x con1=0x%08x dbg=0x%08x pwm0=0x%08x pwm1=0x%08x\n",
			mviiBootStatusU32(data, mviiBootStatusV3PowerOffset),
			mviiBootStatusU32(data, mviiBootStatusV3PowerOffset+4),
			mviiBootStatusU32(data, mviiBootStatusV3PowerOffset+8),
			mviiBootStatusU32(data, mviiBootStatusV3PowerOffset+12),
			mviiBootStatusU32(data, mviiBootStatusV3PowerOffset+16),
			mviiBootStatusU32(data, mviiBootStatusV3PowerOffset+20))
		return
	}

	fmt.Printf("  visible: pwm_gpio=0x%08x bls_en=0x%08x bls_con0=0x%08x bls_con1=0x%08x bls_dbg=0x%08x disp_pwm0=0x%08x disp_pwm1=0x%08x gpio_data=0x%08x\n",
		mviiBootStatusU32(data, mviiBootStatusV3VisibleOffset),
		mviiBootStatusU32(data, mviiBootStatusV3VisibleOffset+4),
		mviiBootStatusU32(data, mviiBootStatusV3VisibleOffset+8),
		mviiBootStatusU32(data, mviiBootStatusV3VisibleOffset+12),
		mviiBootStatusU32(data, mviiBootStatusV3VisibleOffset+16),
		mviiBootStatusU32(data, mviiBootStatusV3VisibleOffset+20),
		mviiBootStatusU32(data, mviiBootStatusV3VisibleOffset+24),
		mviiBootStatusU32(data, mviiBootStatusV3VisibleOffset+28))
	fmt.Printf("  power: pwrap=0x%08x cid=0x%08x ldo0=0x%08x ldo1=0x%08x vio28=0x%08x vgp1=0x%08x\n",
		mviiBootStatusU32(data, mviiBootStatusV3PowerOffset),
		mviiBootStatusU32(data, mviiBootStatusV3PowerOffset+4),
		mviiBootStatusU32(data, mviiBootStatusV3PowerOffset+8),
		mviiBootStatusU32(data, mviiBootStatusV3PowerOffset+12),
		mviiBootStatusU32(data, mviiBootStatusV3PowerOffset+16),
		mviiBootStatusU32(data, mviiBootStatusV3PowerOffset+20))
}

func formatMTKKeypadBits(words ...uint32) string {
	var b strings.Builder
	first := true
	b.WriteByte('[')
	for wordIndex, word := range words {
		for bit := 0; bit < 16; bit++ {
			if word&(uint32(1)<<uint(bit)) == 0 {
				continue
			}
			if !first {
				b.WriteByte(',')
			}
			first = false
			b.WriteString(strconv.Itoa(wordIndex*16 + bit))
		}
	}
	b.WriteByte(']')
	return b.String()
}

func formatMTKGPIOAbsBits(g0, g5, g7 uint32) string {
	var b strings.Builder
	first := true
	b.WriteByte('[')
	first = appendMTKBitList(&b, first, g0, 0, 16)
	first = appendMTKBitList(&b, first, g5, 80, 16)
	first = appendMTKBitList(&b, first, g7, 112, 16)
	b.WriteByte(']')
	return b.String()
}

func formatMTKBitList(value uint32, base, width int) string {
	var b strings.Builder
	b.WriteByte('[')
	appendMTKBitList(&b, true, value, base, width)
	b.WriteByte(']')
	return b.String()
}

func appendMTKBitList(b *strings.Builder, first bool, value uint32, base, width int) bool {
	for bit := 0; bit < width; bit++ {
		if value&(uint32(1)<<uint(bit)) == 0 {
			continue
		}
		if !first {
			b.WriteByte(',')
		}
		first = false
		b.WriteString(strconv.Itoa(base + bit))
	}
	return first
}

func extractMTKHexField(message, key string) (uint32, bool) {
	prefix := key + "="
	for _, field := range strings.Fields(message) {
		if !strings.HasPrefix(field, prefix) {
			continue
		}
		raw := strings.TrimPrefix(field, prefix)
		raw = strings.TrimPrefix(raw, "0x")
		value, err := strconv.ParseUint(raw, 16, 32)
		if err != nil {
			return 0, false
		}
		return uint32(value), true
	}
	return 0, false
}

func isMVIIBootStatusLKScanMarker(lastError uint32) bool {
	if lastError&0xff000000 != 0xd1000000 {
		return false
	}
	step := (lastError >> 16) & 0xff
	phase := lastError & 0xff
	return step == 0x10 && phase >= 0x60 && phase <= 0x67
}

func mviiBootStatusSummary(stage, flags uint32, message string) string {
	if flags&mviiBootStatusFlagLive == 0 && flags&mviiBootStatusFlagFlash != 0 && stage == 0x1001 {
		return "LK image is present; no live stage1 status has replaced the baked marker"
	}
	name := mviiBootStatusStageName(stage)
	if name == "unknown" {
		return fmt.Sprintf("unknown stage 0x%04x", stage)
	}
	if message != "" && message != name {
		return fmt.Sprintf("%s (0x%04x)", name, stage)
	}
	return fmt.Sprintf("%s (0x%04x)", name, stage)
}

func mviiBootStatusMarker(value uint32) string {
	b := []byte{byte(value >> 24), byte(value >> 16), byte(value >> 8), byte(value)}
	printable := true
	for _, c := range b {
		if c < 0x20 || c > 0x7e {
			printable = false
			break
		}
	}
	if printable {
		return fmt.Sprintf("%q / 0x%08x", string(b), value)
	}
	// The stage2/runtime ARM trap handler (boot.c mvii_arm_trap) encodes faults
	// as 0xd1f0_0000 | (kind<<16) | ((pc>>2)&0xffff) — i.e. the [23:19] nibble is
	// fixed at 0b11110 and bits [18:16] carry the trap kind. Decode that before
	// the stage1/display-diag marker path, which would otherwise misread it as a
	// bogus "display diag" record (kind 0xf4 → step "unknown").
	if value&0xfff80000 == 0xd1f00000 {
		kind := (value >> 16) & 0x7
		pcLow := (value & 0xffff) << 2
		return fmt.Sprintf("stage2/runtime ARM trap: %s (kind=%d pc low bits=0x%05x raw=0x%08x)",
			mviiArmTrapKindName(kind), kind, pcLow, value)
	}
	if value&0xff000000 == 0xd1000000 {
		if text, ok := mviiDisplayDiagMarker(value); ok {
			return text
		}
		step := (value >> 16) & 0xff
		rc := (value >> 8) & 0xff
		phase := value & 0xff
		if name := mviiStage1StepName(step); name != "unknown" {
			return fmt.Sprintf("stage1 %s: %s (step=0x%02x rc=0x%02x phase=%s raw=0x%08x)",
				name, mviiStage1ResultName(rc), step, rc, mviiStage1PhaseName(step, phase), value)
		}
		return fmt.Sprintf("display diag phase=0x%02x context=0x%04x raw=0x%08x",
			value&0xff, (value>>8)&0xffff, value)
	}
	return fmt.Sprintf("0x%08x", value)
}

// mviiDisplayDiagMarker decodes mt6592_display.c's publish():
//
//	0xd1000000 | (phase & 0xff) | ((detail & 0xffff) << 8)
//
// which collides head-on with the stage1 step/rc/phase layout below — same 0xd1
// magic, different field boundaries. The collision is not academic: 0xd1078004
// is a successful LK-handoff hand-over (phase 0x04, detail 0x0780 = the DSI
// PSCTRL the runtime matched), and the stage1 reading turns it into "MMSYS
// route: rc=0x80 ... phase=DSI DCS wake/skip", which is a failure that never
// happened and reads like the reason the panel is dark.
//
// The two are told apart by asking whether the stage1 reading holds together at
// all: a genuine stage1 marker has both a known step byte and a known result
// byte. 0xd1078004 has step 0x07 ("MMSYS route") but rc 0x80, which is not a
// stage1 result code at all — the reading only ever looked valid because the
// decoder printed the unknown rc verbatim instead of rejecting it. So stage1
// keeps every marker where both fields check out, and everything else that
// carries one of publish()'s six phases is decoded here.
func mviiDisplayDiagMarker(value uint32) (string, bool) {
	phase := value & 0xff
	detail := (value >> 8) & 0xffff

	if mviiStage1StepKnown((value>>16)&0xff) && mviiStage1ResultKnown((value>>8)&0xff) {
		return "", false
	}

	var (
		name    string
		explain string
	)
	switch phase {
	case 0x01:
		name = "DTB framebuffer"
	case 0x02:
		name = "bound"
		explain = fmt.Sprintf("width=%d", detail)
	case 0x03:
		name = "presented"
		explain = fmt.Sprintf("fb=0x%05x000", detail)
	case 0x04:
		name = "LK handoff scanout preserved"
		explain = fmt.Sprintf("dsi psctrl=0x%04x", detail)
	case 0x80:
		name = "started"
	case 0xff:
		name = "failed"
	default:
		return "", false
	}
	if explain != "" {
		return fmt.Sprintf("display: %s (%s raw=0x%08x)", name, explain, value), true
	}
	return fmt.Sprintf("display: %s (raw=0x%08x)", name, value), true
}

// mviiArmTrapKindName maps the ARM exception-vector index the stage2 trap
// handler passes (start.S) to a human-readable name. Kind 4 (data abort) is the
// one an unaligned access on the MMU-off runtime produces.
func mviiArmTrapKindName(kind uint32) string {
	switch kind {
	case 0:
		return "reset"
	case 1:
		return "undefined instruction"
	case 2:
		return "supervisor call"
	case 3:
		return "prefetch abort"
	case 4:
		return "data abort"
	case 5:
		return "reserved"
	case 6:
		return "IRQ"
	case 7:
		return "FIQ"
	default:
		return "unknown"
	}
}

// mviiStage1StepKnown / mviiStage1ResultKnown exist so mviiDisplayDiagMarker can
// ask "is this really a stage1 record?" without matching on the formatted
// fallback strings the *Name functions return for values they do not recognise.
func mviiStage1StepKnown(step uint32) bool {
	return mviiStage1StepName(step) != "unknown"
}

func mviiStage1ResultKnown(rc uint32) bool {
	switch rc {
	case 0x00, 0x01, 0x02, 0x10, 0x11, 0x30, 0x7e:
		return true
	default:
		return false
	}
}

func mviiStage1StepName(step uint32) string {
	switch step {
	case 0x01:
		return "entry"
	case 0x02:
		return "fallback framebuffer"
	case 0x03:
		return "LCD power rails"
	case 0x04:
		return "GPIO backlight"
	case 0x05:
		return "display clocks"
	case 0x06:
		return "framebuffer paint"
	case 0x07:
		return "MMSYS route"
	case 0x08:
		return "DSI panel wake"
	case 0x09:
		return "DSI video mode"
	case 0x0a:
		return "RDMA scanout"
	case 0x0b:
		return "DSI video start"
	case 0x0c:
		return "idle"
	case 0x0d:
		return "DSI PLL/PHY"
	case 0x10:
		return "display init"
	case 0x11:
		return "first light"
	case 0x12:
		return "DRAM bring-up"
	default:
		return "unknown"
	}
}

func mviiStage1PhaseName(step, phase uint32) string {
	switch step {
	case 0x10:
		switch phase {
		case 0x20:
			return "PMIC/PWRAP"
		case 0x32:
			return "display power and clocks"
		case 0x34:
			return "display clocks ready"
		case 0x35:
			return "backlight"
		case 0x36:
			return "LK GPIO6 reset"
		case 0x37:
			return "LK LCD regulator cycle"
		case 0x38:
			return "LK GPIO112 reset"
		case 0x39:
			return "LK GPIO113 enable"
		case 0x3a:
			return "stock JD9365 init table"
		case 0x3b:
			return "LCD30 minimal DCS init"
		case 0x3c:
			return "DSI setup readback A"
		case 0x3d:
			return "DSI setup readback B"
		case 0x3e:
			return "DSI video readback A"
		case 0x3f:
			return "DSI video readback B"
		case 0x40:
			return "DSI setup readback C"
		case 0x41:
			return "DSI video readback C"
		case 0x42:
			return "JD9365 RGB888 COLMOD"
		case 0x60:
			return "LK handoff register scan entry"
		case 0x61:
			return "LK handoff MMSYS/DSI scan"
		case 0x62:
			return "LK handoff DDP scan"
		case 0x63:
			return "LK handoff BLS scan"
		case 0x64:
			return "LK handoff register scan complete"
		case 0x65:
			return "LK handoff status write failed"
		case 0x66:
			return "LK handoff OVL checker painted"
		case 0x67:
			return "LK handoff GPIO/MSDC/USB/AFE scan"
		case 0x03:
			return "panel init"
		}
	case 0x11:
		switch phase {
		case 0x18:
			return "DCS RGB888 green fill"
		case 0x19:
			return "DCS RGB888 green ready"
		case 0x1a:
			return "DCS green fill row"
		case 0x1b, 0x1c, 0x1d, 0x1e, 0x1f:
			return "DCS checker row"
		case 0x20:
			return "framebuffer write"
		case 0x21:
			return "framebuffer ready"
		case 0x30:
			return "OVL/RDMA scanout"
		case 0x31:
			return "solid green video ready"
		case 0x32:
			return "DDP route readback"
		case 0x33:
			return "DDP run readback"
		case 0x34:
			return "DDP sync readback"
		case 0x36:
			return "DDP layer readback"
		case 0x01:
			return "Hello World complete"
		case 0x02:
			return "Hello World failed"
		}
	case 0x12:
		switch phase {
		case 0x60:
			return "DRAM ready assumption"
		case 0x61:
			return "MEMPLL"
		case 0x62:
			return "REXTDN calibration"
		case 0x63:
			return "LPDDR2 init"
		case 0x64:
			return "DRAM post init"
		case 0x65:
			return "DQS gate training"
		case 0x66:
			return "framebuffer verify"
		}
	}
	return mviiDisplayDiagPhaseName(phase)
}

func mviiStage1ResultName(rc uint32) string {
	switch rc {
	case 0x00:
		return "OK"
	case 0x01:
		return "TIMEOUT"
	case 0x02:
		return "INVALID"
	case 0x10:
		return "PMIC_READ_FAILED"
	case 0x11:
		return "PMIC_WRITE_FAILED"
	case 0x30:
		return "BREADCRUMB_WRITE_FAILED"
	case 0x7e:
		return "ENTER"
	default:
		return fmt.Sprintf("rc=0x%02x", rc)
	}
}

func boolHex(value bool) uint32 {
	if value {
		return 1
	}
	return 0
}

func trimMTKFeedString(data []byte) string {
	if idx := bytes.IndexByte(data, 0); idx >= 0 {
		data = data[:idx]
	}
	return strings.TrimSpace(string(data))
}

func printMTKFeedLog(payload []byte) {
	text := trimMTKFeedString(payload)
	if text == "" {
		return
	}
	for _, line := range strings.Split(text, "\n") {
		line = strings.TrimSpace(line)
		if line != "" {
			fmt.Printf("payload: %s\n", line)
		}
	}
}

func printMTKFeedPayloadProgress(payload []byte) {
	if len(payload) < 16 {
		printMTKFeedLog(payload)
		return
	}
	done := binary.LittleEndian.Uint64(payload[0:8])
	total := binary.LittleEndian.Uint64(payload[8:16])
	if total == 0 {
		return
	}
	percent := float64(done) / float64(total) * 100
	if percent > 100 {
		percent = 100
	}
	fmt.Printf("payload progress: %s / %s (%.1f%%)\n", formatBytes(done), formatBytes(total), percent)
}

func printMTKFeedProgress(done, total uint64, last *time.Time) {
	if total == 0 {
		return
	}
	now := time.Now()
	if done < total && !last.IsZero() && now.Sub(*last) < 750*time.Millisecond {
		return
	}
	percent := float64(done) / float64(total) * 100
	if percent > 100 {
		percent = 100
	}
	fmt.Printf("  sent %s / %s (%.1f%%)\n", formatBytes(done), formatBytes(total), percent)
	*last = now
}

func timeoutUntil(deadline time.Time, max time.Duration) time.Duration {
	remaining := time.Until(deadline)
	if remaining <= 0 {
		return time.Millisecond
	}
	if remaining < max {
		return remaining
	}
	return max
}
