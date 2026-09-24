package main

import (
	"bufio"
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"regexp"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"time"
)

const (
	mtkSectorSize      = uint64(0x200)
	maxRawGapScanBytes = uint64(64 * 1024 * 1024)
	rawBlockChunkSize  = 4 * 1024 * 1024
)

var dumpedSectorRE = regexp.MustCompile(`(?m)Dumped sector ([0-9]+) with sector count ([0-9]+) as (.+?)\.$`)

type config struct {
	root                 string
	target               string
	image                string
	backend              string
	tool                 string
	mtkclientRoot        string
	daLoader             string
	preloader            string
	mtkDRAM              string
	mtkPacketSize        string
	mtkDumpPreloader     string
	mtkBootPreloader     bool
	mtkWritePreloader    string
	mtkFlashScatter      string
	mtkScatter           string
	mtkFeedPayload       string
	mtkResetToBROM       bool
	mtkReadBootStatus    bool
	mtkSelftestWrite     bool
	mtkRunStage1         bool
	mtkFeedReuse         bool
	mtkReadPartitions    bool
	mtkAnalyzePreload    string
	mtkDecodeLK          string
	mtkPreloadProfile    string
	mtkPreloadProfileOut string
	mtkPayloadAddr       string
	mtkPayloadEntry      string
	mtkFeedChunkSize     string
	mtkFeedPart          string
	mtkFeedCommand       string
	mtkFeedBundle        bool
	upload               string
	mtkFeedLiveParts     bool
	mtkFeedHandoff       bool
	mtkFeedReboot        bool
	mtkFeedRebootSet     bool
	mtkFeedBootPreloader bool
	mtkFeedFollowBoot    bool
	mtkFeedFollowTimeout string
	mtkFeedExtraFlags    uint32
	mtkHWProbe           bool
	mtkSpeakerTest       bool
	mtkFeedMenu          bool
	dbgConsole           bool
	dbgRun               string
	bootFlag             string
	partition            string
	candidates           string
	rawOffset            string
	rawLength            string
	adbReboot            string
	serial               string
	device               string
	yes                  bool
	listOnly             bool
	printPartitions      bool
	detectBoot           bool
	installMTKDeps       bool
	partitionExplicit    bool
	prepare              bool
	reboot               bool
}

type commandSpec struct {
	name string
	args []string
}

func main() {
	// The flashing window is this same program built with `-tags gui`, and it
	// takes no arguments at all: it is handed to someone who has a board and a
	// package, not to a script. Short-circuiting before parseFlags is what makes
	// "ignores the command line" true rather than approximately true -- there is
	// no flag it could be talked into honouring. guiBuild is a constant, so the
	// default console build compiles this branch away entirely.
	if guiBuild {
		os.Exit(runFlashGUI())
	}
	if err := run(); err != nil {
		fmt.Fprintf(os.Stderr, "%s: %v\n", filepath.Base(os.Args[0]), err)
		os.Exit(1)
	}
}

func run() error {
	orig := os.Args

	// Handle `flash run ...` on the original args (supports file anywhere in the subcommand line).
	if len(orig) > 1 && orig[1] == "run" {
		sub := orig[2:]
		// reorder the subcommand args so its FlagSet sees the options
		sub = reorderArgsWithFlagsFirst(append([]string{"run"}, sub...))[1:]
		return runRawPayload(sub)
	}

	// Handle `flash read -partitions` / `flash read -address 0x... -output dump.bin` etc.
	if len(orig) > 1 && orig[1] == "read" {
		sub := orig[2:]
		sub = reorderArgsWithFlagsFirst(append([]string{"read"}, sub...))[1:]
		return runRead(sub)
	}

	// Go flag package stops flag parsing at the first non-flag arg.
	// Support the natural `./flash lk.bin -address 0x2920000 -device /dev/cu... -yes` form
	// by reordering so all -flags come before positionals for the main path.
	os.Args = reorderArgsWithFlagsFirst(orig)

	cfg, err := parseFlags()
	if errors.Is(err, flag.ErrHelp) {
		return nil
	}
	if err != nil {
		return err
	}
	if cfg.target != "arm" {
		return fmt.Errorf("./flash currently supports only -target=arm for the J36 Ultra / MT6592 port")
	}

	if cfg.installMTKDeps {
		return mtkclientRemovedError("-install-mtk-deps")
	}
	if cfg.listOnly {
		return listBackends(cfg)
	}
	// The live console is checked before every serial path below because it needs
	// no -device at all: it finds its own USB device by VID:PID, and the board it
	// talks to is booted rather than sitting in BROM.
	//
	// -flag goes first of the two: it is "put the board in mode X", and when X is
	// `debug` it can then hand straight over to -dbg / -dbg-run on the other side
	// of the reset, which is the whole reason it exists.
	//
	// A booted board answers on the console and there is nothing to flash from
	// there, so that is the whole invocation. A board in BROM has no console; the
	// mode is armed by the resident payload instead, *after* whatever writes this
	// run was going to do -- so we only note the intent here and fall through to
	// the normal flash dispatch.
	if cfg.bootFlag != "" {
		if !validBootFlag(cfg.bootFlag) {
			return fmt.Errorf("-flag %q: expected %s, %s or %s",
				cfg.bootFlag, bootFlagDebug, bootFlagBoot, bootFlagBROM)
		}
		handled, err := setMVIIBootFlagOverConsole(cfg.bootFlag, cfg.dbgConsole || cfg.dbgRun != "", cfg.dbgRun)
		if err != nil || handled {
			return err
		}
		// The flag is a one-shot eMMC sector, so the flash's own reset would consume
		// it before LK ever read it. Take the reset away from the flash; the flag
		// command performs the single reset at the end instead.
		cfg.mtkFeedReboot = false
		cfg.mtkFeedRebootSet = true
		fmt.Printf("-flag %s: no live console to ask, so this run flashes first and arms the flag "+
			"from BROM at the end, in place of the flash's own reset.\n", cfg.bootFlag)
	}
	if cfg.upload != "" && !validUploadTarget(cfg.upload) {
		return invalidUploadTargetError(cfg.upload)
	}
	// -upload writes eMMC, and only the BROM feed path can do that; the live
	// console runs on a booted board and has nothing to flash from. Taking
	// precedence over -dbg rather than erroring is deliberate: `-dbg -upload lk`
	// is the natural thing to type when -dbg is in every other command you run,
	// and the intent is unambiguous.
	if (cfg.dbgConsole || cfg.dbgRun != "") && cfg.upload != "" {
		fmt.Printf("-upload %s writes eMMC, which needs the board in BROM/preloader mode; ignoring -dbg for this run.\n",
			effectiveUploadTarget(cfg))
		cfg.dbgConsole = false
		cfg.dbgRun = ""
	}
	if cfg.dbgConsole || cfg.dbgRun != "" {
		return runMVIIDebugConsole(cfg.dbgRun)
	}
	if cfg.printPartitions {
		return mtkclientRemovedError("-print-partitions")
	}
	if cfg.detectBoot {
		return mtkclientRemovedError("-detect-boot-partition")
	}
	if cfg.mtkDumpPreloader != "" {
		if cfg.device == "" {
			return errors.New("-mtk-dump-preloader requires -device /dev/cu.usbmodem... or another MTK VCOM serial device")
		}
		return dumpPreloaderMTKSerial(cfg)
	}
	if cfg.mtkBootPreloader {
		if cfg.device == "" {
			return errors.New("-mtk-boot-preloader requires -device /dev/cu.usbmodem... or another MTK VCOM serial device")
		}
		return bootPreloaderMTKSerial(cfg)
	}
	if cfg.mtkWritePreloader != "" {
		if cfg.device == "" {
			return errors.New("-mtk-write-preloader requires -device /dev/cu.usbmodem... or another MTK VCOM serial device")
		}
		return writePreloaderMTKFeed(cfg, cfg.mtkWritePreloader)
	}
	if cfg.mtkFlashScatter != "" {
		if cfg.device == "" {
			return errors.New("scatter flash requires -device /dev/cu.usbmodem... or another MTK VCOM serial device")
		}
		return flashScatterMTKFeed(cfg, cfg.mtkFlashScatter)
	}
	if cfg.mtkResetToBROM {
		if cfg.device == "" {
			return errors.New("-mtk-reset-to-brom requires -device /dev/cu.usbmodem... or another MTK VCOM serial device")
		}
		return resetMTKTargetToBROM(cfg)
	}
	if cfg.mtkReadBootStatus {
		if cfg.device == "" {
			return errors.New("-mtk-read-boot-status requires -device /dev/cu.usbmodem... or another MTK VCOM serial device")
		}
		return readBootStatusMTKFeed(cfg)
	}
	if cfg.mtkSelftestWrite {
		if cfg.device == "" {
			return errors.New("-mtk-selftest-write requires -device /dev/cu.usbmodem... or another MTK VCOM serial device")
		}
		return selftestBootStatusWriteMTKFeed(cfg)
	}
	if cfg.mtkRunStage1 {
		if cfg.device == "" {
			return errors.New("-mtk-run-stage1 requires -device /dev/cu.usbmodem... or another MTK VCOM serial device")
		}
		return runStage1MTKFeed(cfg)
	}
	if cfg.mtkHWProbe {
		if cfg.mtkFeedCommand != "" {
			return errors.New("use either -mtk-hw-probe or -mtk-feed-command, not both")
		}
		cfg.mtkFeedCommand = "probe"
	}
	if cfg.mtkSpeakerTest {
		if cfg.mtkFeedCommand != "" {
			return errors.New("use either -mtk-speaker-test or -mtk-feed-command, not both")
		}
		cfg.mtkFeedCommand = "speaker-noise"
	}
	if strings.EqualFold(cfg.mtkFeedCommand, "menu") {
		cfg.mtkFeedMenu = true
		cfg.mtkFeedCommand = ""
	}
	if cfg.mtkFeedMenu {
		if cfg.device == "" {
			return errors.New("-mtk-feed-menu requires -device /dev/cu.usbmodem... or another MTK VCOM serial device")
		}
		return runMTKFeedInteractiveMenu(cfg)
	}
	if cfg.mtkFeedCommand != "" {
		if cfg.device == "" {
			return errors.New("-mtk-feed-command requires -device /dev/cu.usbmodem... or another MTK VCOM serial device")
		}
		return runMTKFeedCommand(cfg, cfg.mtkFeedCommand)
	}
	if cfg.mtkReadPartitions {
		if cfg.device == "" {
			return errors.New("-mtk-read-partitions requires -device /dev/cu.usbmodem... or another MTK VCOM serial device")
		}
		return readPartitionsMTKFeed(cfg)
	}
	if cfg.mtkAnalyzePreload != "" {
		return analyzePreloaderCommand(cfg, cfg.mtkAnalyzePreload)
	}
	if cfg.mtkDecodeLK != "" {
		return decodeLKCommand(cfg)
	}

	// Direct raw BROM execution shorthand (no feed protocol, no eMMC write).
	//   ./flash -address 0x110000 [payload.bin] -device /dev/cu... [-yes]
	//   ./flash -address 0x1f40000 -device /dev/cu... -yes
	// Does SEND_DA (if payload) + JUMP_DA, then reads raw serial bridge output.
	// This revives the convenient top-level form (in addition to the `run` subcommand).
	// We only trigger on explicit address/entry + serial device when feed was not forced.
	if isSerialDevicePath(cfg.device) &&
		(strings.TrimSpace(cfg.mtkPayloadAddr) != "" || strings.TrimSpace(cfg.mtkPayloadEntry) != "") &&
		cfg.mtkFeedPayload == "" {
		bin := cfg.image
		if bin != "" {
			if !fileExists(bin) {
				if abs, aerr := filepath.Abs(bin); aerr == nil && fileExists(abs) {
					bin = abs
				} else {
					return fmt.Errorf("payload binary not found: %s", bin)
				}
			}
			return rawExecutePayload(cfg, bin)
		}
		return rawJumpToAddress(cfg)
	}

	// Default serial VCOM + main image path to the native MVIIFlash feed payload mechanism.
	// `./flash myimage.img -device /dev/cu...` (or positional) writes the image to eMMC via the feed protocol.
	// -address/-mtk-payload-addr here overrides *where the feed payload stub itself is loaded* (advanced).
	// For direct BROM load+exec or bare jump+bridge read (payload optional), use -address with a serial device
	// (or the `run` subcommand).
	if cfg.device != "" && isSerialDevicePath(cfg.device) && cfg.mtkFeedPayload == "" {
		cfg.mtkFeedPayload = "auto"
	}

	// If user passed -address that looks like a large eMMC/scatter offset (common confusion with -raw-offset),
	// and we are on auto feed, don't use it as the *stub* load address; it would break SEND_DA for the small payload.
	if strings.EqualFold(cfg.mtkFeedPayload, "auto") {
		if a := strings.TrimSpace(cfg.mtkPayloadAddr); a != "" {
			if n, err := parseMTKNumber(a, ""); err == nil && n >= 0x1000000 {
				fmt.Printf("Note: -address 0x%x ignored for feed stub load (high value looks like eMMC offset; use -raw-offset for the image). Stub uses default SRAM addr.\n", n)
				cfg.mtkPayloadAddr = ""
				cfg.mtkPayloadEntry = ""
			}
		}
	}

	if cfg.mtkFeedPayload != "" && shouldUseDefaultMTKFeedBootChain(cfg) {
		return finishWithBootFlag(cfg, feedDefaultMVIIBundleMTKSerial(cfg))
	}

	image, err := resolveImage(cfg)
	if err != nil {
		return err
	}
	info, err := os.Stat(image)
	if err != nil {
		return err
	}
	if info.IsDir() {
		return fmt.Errorf("%s is a directory, not a flash image", image)
	}
	if cfg.mtkFeedPayload != "" {
		if cfg.device == "" {
			return errors.New("-mtk-feed-payload requires -device /dev/cu.usbmodem... or another MTK VCOM serial device")
		}
		if shouldUseDefaultMTKFeedBundle(cfg) {
			return finishWithBootFlag(cfg, feedDefaultMVIIBundleMTKSerial(cfg))
		}
		return finishWithBootFlag(cfg, feedPayloadMTKSerial(cfg, image, info))
	}

	backend := strings.ToLower(strings.TrimSpace(cfg.backend))
	if backend == "" {
		backend = "auto"
	}
	if backend == "raw" {
		backend = "raw-block"
	}
	if backend == "mtk" {
		backend = "mtkclient"
	}
	if backend == "auto" && isSerialDevicePath(cfg.device) {
		backend = "mtk-serial"
	}
	if (cfg.rawOffset != "" || cfg.rawLength != "") && backend == "auto" {
		if cfg.device == "" {
			return errors.New("-raw-offset/-raw-length now use the Go raw block writer; pass -backend=raw-block -device /dev/diskN")
		}
		backend = "raw-block"
	}
	if backend == "auto" {
		backend, err = chooseBackend(cfg)
		if err != nil {
			return err
		}
	}
	if backend == "mtkclient" {
		return mtkclientRemovedError("-backend=mtkclient")
	}
	if backend == "raw-block" && isSerialDevicePath(cfg.device) {
		return fmt.Errorf("%s is a serial/CDC device, not a block device; use -backend=mtk-serial for detection or a /dev/diskN device for -backend=raw-block", cfg.device)
	}
	if (cfg.rawOffset != "" || cfg.rawLength != "") && backend != "raw-block" && backend != "mtk-serial" {
		return fmt.Errorf("-raw-offset/-raw-length are supported only with -backend=raw-block or -backend=mtk-serial")
	}
	// The two agents that can arm the flag are the live console and the MTK feed
	// payload. Neither exists here, and silently dropping the mode would be the
	// worst outcome: the board would come back in whatever mode it was already in
	// while the invocation reported success.
	if cfg.bootFlag != "" && backend != "mtk-serial" {
		return fmt.Errorf("-flag %s needs either the live debug console or a board in BROM on the "+
			"MTK serial path; the %s backend cannot arm the boot flag", cfg.bootFlag, backend)
	}

	fmt.Printf("MVII ARM flash image: %s (%d bytes)\n", image, info.Size())
	if backend == "raw-block" {
		if cfg.rawOffset != "" {
			fmt.Printf("Target: J36 Ultra / MediaTek MT6592, raw block device: %s, raw offset: %s\n", cfg.device, cfg.rawOffset)
		} else {
			fmt.Printf("Target: J36 Ultra / MediaTek MT6592, raw block device: %s\n", cfg.device)
		}
	} else if backend == "mtk-serial" {
		fmt.Printf("Target: J36 Ultra / MediaTek MT6592, serial device: %s\n", cfg.device)
	} else {
		fmt.Printf("Target: J36 Ultra / MediaTek MT6592, partition: %s\n", effectivePartition(cfg, backend))
	}

	switch backend {
	case "fastboot":
		return flashWithFastboot(cfg, image)
	case "raw-block":
		return flashRawBlock(cfg, image)
	case "mtk-serial":
		return finishWithBootFlag(cfg, flashWithMTKSerial(cfg, image))
	default:
		return fmt.Errorf("unknown -backend %q; expected auto, fastboot, raw-block, or mtk-serial", cfg.backend)
	}
}

// finishWithBootFlag arms a deferred -flag once the writes it had to wait for
// are done.
//
// It runs after the flash rather than before it because the flag is a one-shot
// eMMC sector: the reset at the end of a flash would consume it before the LK
// that reads it ever ran. Nothing is armed if the flash failed -- resetting into
// a mode served by a half-written image is worse than leaving the board where it
// is, in BROM, where the next attempt can reach it.
func finishWithBootFlag(cfg config, err error) error {
	if err != nil || cfg.bootFlag == "" {
		return err
	}
	if err := applyBootFlagOverMTKFeed(cfg, cfg.bootFlag); err != nil {
		return err
	}
	return reportBootFlagReset(cfg.bootFlag, cfg.dbgConsole || cfg.dbgRun != "", cfg.dbgRun)
}

func parseFlags() (config, error) {
	// Start with a fresh FlagSet on the global CommandLine so that repeated calls
	// to parseFlags (as happens in tests) do not panic on re-registration of flags.
	// Production use invokes parseFlags once per process.
	flag.CommandLine = flag.NewFlagSet(os.Args[0], flag.ContinueOnError)

	exe, err := os.Executable()
	if err != nil {
		return config{}, err
	}
	defaultRoot := filepath.Dir(exe)
	if root := os.Getenv("MVII_FLASH_ROOT"); root != "" {
		defaultRoot = root
	}
	cfg := config{
		root:                 defaultRoot,
		target:               "arm",
		backend:              "auto",
		partition:            "boot",
		adbReboot:            "edl",
		prepare:              true,
		mtkFeedBundle:        false,
		mtkFeedLiveParts:     true,
		mtkFeedBootPreloader: false,
		mtkFeedFollowTimeout: "2m",
	}
	cfg.partitionExplicit = hasFlag(os.Args[1:], "partition")
	flag.StringVar(&cfg.root, "root", cfg.root, "MVII ARM package root; defaults to the directory containing ./flash")
	flag.StringVar(&cfg.target, "target", cfg.target, "flash target; only arm/J36 Ultra is currently supported")
	flag.StringVar(&cfg.image, "image", cfg.image, "image to flash (positional arg also accepted)")
	flag.StringVar(&cfg.backend, "backend", cfg.backend, "flashing backend: auto, fastboot, raw-block, or mtk-serial")
	flag.StringVar(&cfg.device, "device", cfg.device, "serial VCOM for MTK feed (e.g. /dev/cu.usbmodemXXXX) or block device")
	flag.StringVar(&cfg.serial, "serial", cfg.serial, "fastboot serial when multiple devices")
	flag.StringVar(&cfg.tool, "tool", cfg.tool, "fastboot binary name/path")
	flag.StringVar(&cfg.partition, "partition", cfg.partition, "fastboot partition name")
	flag.StringVar(&cfg.rawOffset, "raw-offset", "", "raw offset for eMMC writes (feed path defaults LK->UBOOT, arm.img->BOOTIMG)")
	flag.StringVar(&cfg.rawLength, "raw-length", "", "raw transfer length; for feed, auto/minimal rounds image up to 512B")
	flag.StringVar(&cfg.preloader, "preloader", "", "optional preloader for DRAM init in some serial paths")
	flag.StringVar(&cfg.daLoader, "da-loader", "", "legacy DA loader (only for -backend=mtk-serial without feed)")
	flag.StringVar(&cfg.mtkDRAM, "mtk-dram", "", "legacy DA DRAM profile: auto, preloader, mt6592-standard, mt6592-lpddr2, mt6592-lpddr3, or mt6592-da-default")
	flag.StringVar(&cfg.mtkPacketSize, "mtk-packet-size", "", "legacy DA packet size override")
	flag.StringVar(&cfg.mtkDumpPreloader, "mtk-dump-preloader", "", "dump eMMC BOOT1 preloader to a file or directory; use 'auto' for the default path")
	flag.BoolVar(&cfg.mtkBootPreloader, "mtk-boot-preloader", cfg.mtkBootPreloader, "boot the supplied stock preloader from SRAM without writing eMMC")
	flag.StringVar(&cfg.mtkWritePreloader, "mtk-write-preloader", "", "write a stock preloader image to eMMC BOOT1 through the native feed payload")
	flag.BoolVar(&cfg.mtkResetToBROM, "mtk-reset-to-brom", cfg.mtkResetToBROM, "reset a preloader-mode target back into BROM USB-download mode")
	flag.BoolVar(&cfg.mtkReadBootStatus, "mtk-read-boot-status", cfg.mtkReadBootStatus, "read the MVII boot-status sector through the native feed payload")
	flag.BoolVar(&cfg.mtkSelftestWrite, "mtk-selftest-write", cfg.mtkSelftestWrite, "write/read/restore the MVII boot-status sector to verify feed eMMC writes")
	flag.BoolVar(&cfg.mtkReadPartitions, "mtk-read-partitions", cfg.mtkReadPartitions, "read live MTK/GPT/MBR partition information through the native feed payload")
	flag.StringVar(&cfg.mtkAnalyzePreload, "mtk-analyze-preloader", "", "analyze a stock preloader image; use 'auto' to locate one")
	flag.StringVar(&cfg.mtkDecodeLK, "mtk-decode-lk", "", "decode an LK image; use 'auto' to locate one")
	flag.StringVar(&cfg.mtkPreloadProfile, "mtk-preloader-profile", "", "MTK preloader profile JSON overriding LK/BOOTIMG offsets")
	flag.StringVar(&cfg.mtkPreloadProfileOut, "mtk-preloader-profile-out", "", "write an editable MTK preloader profile JSON during -mtk-analyze-preloader")
	flag.StringVar(&cfg.mtkPayloadAddr, "mtk-payload-addr", "", "RAM load addr for the *feed payload stub* (e.g. MVIIFlash.bin at 0x110000) or for 'run'; not eMMC offset")
	flag.StringVar(&cfg.mtkPayloadEntry, "mtk-payload-entry", "", "entry point for the feed stub or raw run")
	flag.StringVar(&cfg.mtkPayloadAddr, "address", cfg.mtkPayloadAddr, "RAM load addr alias (for feed stub or with 'run' subcommand; use -raw-offset for image eMMC location)")
	flag.StringVar(&cfg.mtkPayloadEntry, "entry", cfg.mtkPayloadEntry, "entry point alias")
	flag.StringVar(&cfg.mtkFeedPayload, "mtk-feed-payload", "", "explicit MVIIFlash payload binary; 'auto' finds boot/MVIIFlash.bin under root")
	flag.StringVar(&cfg.mtkFeedChunkSize, "mtk-feed-chunk-size", "", "native feed chunk size; defaults to the payload-supported size")
	flag.StringVar(&cfg.mtkFeedPart, "mtk-feed-part", "", "native feed eMMC part: user, boot1, boot2, or numeric part id")
	flag.StringVar(&cfg.mtkFeedCommand, "mtk-feed-command", "", "send a live MVIIFlash command: help, probe, keys, display, sd, usb, wifi, \"wifi bind\", audio, speaker-noise, dsi, \"panel [argb]\", \"peek <addr> [words]\", \"poke <addr> <value>\", \"bl <pct>\", \"pmicr <reg> [count]\", \"pmicw <reg> <value>\", or \"led <r> <g> <b>\"")
	flag.StringVar(&cfg.mtkScatter, "scatter", "", "MTK scatter file (e.g. MT6592_Android_scatter.txt) used to name/locate partitions when reading; does not trigger a flash")
	flag.StringVar(&cfg.mtkFlashScatter, "mtk-flash-scatter", "", "MTK scatter file (e.g. MT6572_Android_scatter.txt) to flash via native feed; all downloadable EMMC_USER images are written")
	flag.BoolVar(&cfg.mtkFeedBundle, "mtk-feed-bundle", cfg.mtkFeedBundle, "force the default MVII boot-chain feed sequence")
	flag.StringVar(&cfg.upload, "upload", cfg.upload, uploadFlagUsage())
	flag.BoolVar(&cfg.mtkFeedLiveParts, "mtk-feed-live-parts", cfg.mtkFeedLiveParts, "probe live partitions before bundled feed writes")
	flag.BoolVar(&cfg.mtkFeedHandoff, "mtk-feed-handoff", cfg.mtkFeedHandoff, "disabled: flashing never mirrors streamed images into DRAM")
	flag.BoolVar(&cfg.mtkFeedReboot, "mtk-feed-reboot", cfg.mtkFeedReboot, "after feed write, watchdog reset")
	flag.BoolVar(&cfg.mtkFeedReuse, "mtk-feed-reuse", cfg.mtkFeedReuse, "reuse already-running MVIIFlash payload (skip re-upload)")
	flag.BoolVar(&cfg.mtkFeedBootPreloader, "mtk-feed-boot-preloader", cfg.mtkFeedBootPreloader, "after a feed reboot request, boot the stock preloader from RAM")
	flag.BoolVar(&cfg.mtkFeedFollowBoot, "mtk-feed-follow-boot", cfg.mtkFeedFollowBoot, "follow post-flash serial boot logs after the feed completes")
	flag.StringVar(&cfg.mtkFeedFollowTimeout, "mtk-feed-follow-timeout", cfg.mtkFeedFollowTimeout, "post-flash serial follow timeout, or 0/none/forever")
	flag.BoolVar(&cfg.mtkRunStage1, "mtk-run-stage1", cfg.mtkRunStage1, "after feed payload is resident, run the stage1 display bring-up in-process and stream live breadcrumbs over VCOM (no eMMC write, no reboot)")
	flag.BoolVar(&cfg.mtkHWProbe, "mtk-hw-probe", cfg.mtkHWProbe, "send the live MVIIFlash probe command for display, keys/GPIO, MSDC, USB, WiFi hints, and audio AFE")
	flag.BoolVar(&cfg.mtkSpeakerTest, "mtk-speaker-test", cfg.mtkSpeakerTest, "send the live MVIIFlash speaker-noise command (conservative AFE pulse)")
	flag.BoolVar(&cfg.mtkFeedMenu, "mtk-feed-menu", cfg.mtkFeedMenu, "open the two-entry BROM menu (debug mode / boot)")
	/* No backquotes in these usage strings: Go's flag package treats a backquoted
	 * word as the flag's value name, which turned "-dbg" into "-dbg debug mode". */
	flag.BoolVar(&cfg.dbgConsole, "dbg", cfg.dbgConsole, "attach the interactive live debug console on a booted board (arm it first with the BROM menu's 'debug mode')")
	flag.StringVar(&cfg.dbgRun, "dbg-run", cfg.dbgRun, "run a ';'-separated batch of live-console commands and print one transcript, e.g. -dbg-run \"bat;batscan;isink\"")
	flag.StringVar(&cfg.bootFlag, "flag", cfg.bootFlag, "set what the NEXT boot does and reset so it takes effect: debug (come back in the live console), boot (run the kernel), or brom (come back in BROM download mode). Uses the live console if the board is already in it; otherwise the flash proceeds as usual and the flag is armed by the BROM payload at the end, in place of the flash's own reset. Combine with -dbg or -dbg-run to re-attach automatically once the board is back")
	flag.BoolVar(&cfg.yes, "yes", false, "skip interactive confirmation")
	flag.BoolVar(&cfg.prepare, "prepare", cfg.prepare, "auto-run sibling ./prepare when default image missing")
	flag.BoolVar(&cfg.reboot, "reboot", false, "reboot after fastboot flash")
	flag.BoolVar(&cfg.listOnly, "list", false, "list backends and fastboot devices")
	if err := flag.CommandLine.Parse(os.Args[1:]); err != nil {
		return config{}, err
	}

	// Remember whether the user explicitly passed -mtk-feed-reboot so the feed
	// path can safely default it on for BOOTIMG/UBOOT-slot images (which exist to
	// be booted) without overriding an explicit -mtk-feed-reboot=false.
	flag.Visit(func(f *flag.Flag) {
		if f.Name == "mtk-feed-reboot" {
			cfg.mtkFeedRebootSet = true
		}
	})

	// Support positional image arg, e.g. `./flash boot.img -device /dev/cu.usbmodemXXXX -yes`
	// or a scatter file to flash: `./flash MT6572_Android_scatter.txt -device /dev/cu... -yes`
	// (or with -image / -mtk-flash-scatter). Use `run` + -address for loading+exec or bare jump.
	if cfg.image == "" && cfg.mtkFlashScatter == "" {
		for _, a := range flag.Args() {
			if a == "" || strings.HasPrefix(a, "-") {
				continue
			}
			// accept if it exists on disk (absolute or relative)
			if fileExists(a) {
				if isMTKScatterFile(a) {
					cfg.mtkFlashScatter = a
				} else {
					cfg.image = a
				}
				break
			}
			if abs, err := filepath.Abs(a); err == nil && fileExists(abs) {
				if isMTKScatterFile(abs) {
					cfg.mtkFlashScatter = abs
				} else {
					cfg.image = abs
				}
				break
			}
		}
	}
	// If -image pointed at a scatter file (or positional was classified as image but is scatter), promote it.
	if cfg.image != "" && cfg.mtkFlashScatter == "" && isMTKScatterFile(cfg.image) {
		cfg.mtkFlashScatter = cfg.image
		cfg.image = ""
	}

	root, err := filepath.Abs(cfg.root)
	if err != nil {
		return config{}, err
	}
	cfg.root = root
	if cfg.mtkclientRoot != "" {
		mtkclientRoot, err := filepath.Abs(cfg.mtkclientRoot)
		if err != nil {
			return config{}, err
		}
		cfg.mtkclientRoot = mtkclientRoot
	}
	target, err := normalizeTarget(cfg.target)
	if err != nil {
		return config{}, err
	}
	cfg.target = target
	cfg.partition = strings.TrimSpace(cfg.partition)
	if cfg.partition == "" {
		return config{}, errors.New("-partition must not be empty")
	}
	cfg.rawOffset = strings.TrimSpace(cfg.rawOffset)
	cfg.rawLength = strings.TrimSpace(cfg.rawLength)
	cfg.mtkDRAM = strings.TrimSpace(cfg.mtkDRAM)
	cfg.mtkPacketSize = strings.TrimSpace(cfg.mtkPacketSize)
	cfg.mtkDumpPreloader = strings.TrimSpace(cfg.mtkDumpPreloader)
	cfg.mtkWritePreloader = strings.TrimSpace(cfg.mtkWritePreloader)
	cfg.mtkFlashScatter = strings.TrimSpace(cfg.mtkFlashScatter)
	cfg.mtkScatter = strings.TrimSpace(cfg.mtkScatter)
	cfg.mtkFeedPayload = strings.TrimSpace(cfg.mtkFeedPayload)
	cfg.mtkAnalyzePreload = strings.TrimSpace(cfg.mtkAnalyzePreload)
	cfg.mtkDecodeLK = strings.TrimSpace(cfg.mtkDecodeLK)
	cfg.mtkPreloadProfile = strings.TrimSpace(cfg.mtkPreloadProfile)
	cfg.mtkPreloadProfileOut = strings.TrimSpace(cfg.mtkPreloadProfileOut)
	cfg.mtkPayloadAddr = strings.TrimSpace(cfg.mtkPayloadAddr)
	cfg.mtkPayloadEntry = strings.TrimSpace(cfg.mtkPayloadEntry)
	cfg.mtkFeedChunkSize = strings.TrimSpace(cfg.mtkFeedChunkSize)
	cfg.mtkFeedPart = strings.TrimSpace(cfg.mtkFeedPart)
	cfg.mtkFeedCommand = strings.TrimSpace(cfg.mtkFeedCommand)
	cfg.mtkFeedFollowTimeout = strings.TrimSpace(cfg.mtkFeedFollowTimeout)
	if cfg.mtkFeedHandoff {
		return config{}, mtkFeedHandoffDisabledError()
	}
	if cfg.mtkFeedFollowBoot {
		cfg.mtkFeedReboot = true
	}
	if cfg.daLoader != "" {
		daLoader, err := filepath.Abs(cfg.daLoader)
		if err != nil {
			return config{}, err
		}
		cfg.daLoader = daLoader
	}
	if cfg.preloader != "" {
		preloader, err := filepath.Abs(cfg.preloader)
		if err != nil {
			return config{}, err
		}
		cfg.preloader = preloader
	}
	if cfg.mtkFlashScatter != "" {
		scatter, err := filepath.Abs(cfg.mtkFlashScatter)
		if err != nil {
			return config{}, err
		}
		cfg.mtkFlashScatter = scatter
	}
	if cfg.mtkScatter != "" {
		scatter, err := filepath.Abs(cfg.mtkScatter)
		if err != nil {
			return config{}, err
		}
		cfg.mtkScatter = scatter
	}
	if cfg.mtkFeedPayload != "" && !strings.EqualFold(cfg.mtkFeedPayload, "auto") {
		payload, err := filepath.Abs(cfg.mtkFeedPayload)
		if err != nil {
			return config{}, err
		}
		cfg.mtkFeedPayload = payload
	}
	return cfg, nil
}

func hasFlag(args []string, name string) bool {
	short := "-" + name
	long := "--" + name
	for _, arg := range args {
		if arg == short || arg == long || strings.HasPrefix(arg, short+"=") || strings.HasPrefix(arg, long+"=") {
			return true
		}
	}
	return false
}

// reorderArgsWithFlagsFirst moves all flag tokens (including the value token when
// separated by space) to the front so that Go's flag.Parse sees options even if
// a positional filename was given first on the command line.
func reorderArgsWithFlagsFirst(argv []string) []string {
	if len(argv) <= 1 {
		return argv
	}
	prog := argv[0]
	args := argv[1:]
	var flags, operands []string
	i := 0
	for i < len(args) {
		a := args[i]
		if a == "--" {
			// everything after -- is operands
			operands = append(operands, args[i+1:]...)
			break
		}
		if strings.HasPrefix(a, "-") && a != "-" {
			flags = append(flags, a)
			// if it doesn't contain = , and next exists and doesn't start with -, consume as its value
			if !strings.Contains(a, "=") && i+1 < len(args) && !strings.HasPrefix(args[i+1], "-") {
				i++
				flags = append(flags, args[i])
			}
		} else {
			operands = append(operands, a)
		}
		i++
	}
	return append([]string{prog}, append(flags, operands...)...)
}

func effectivePartition(cfg config, backend string) string {
	if backend == "mtkclient" && !cfg.partitionExplicit && strings.EqualFold(cfg.partition, "boot") {
		return "bootimg"
	}
	return cfg.partition
}

func mtkclientRemovedError(feature string) error {
	return fmt.Errorf("%s depended on mtkclient, which is no longer packaged; expose the target storage as a block device and use -backend=raw-block -device /dev/diskN", feature)
}

// runRawPayload implements `flash run -address 0x... [payload.bin] -device /dev/cu...`
// With a payload: does direct BROM SEND_DA + JUMP_DA.
// Without a payload: just JUMP_DA to the address then reads raw serial bridge output
// (equivalent to the prior direct -mtk-run-stage1 behavior for cold stage1 debug).
func runRawPayload(tail []string) error {
	fs := flag.NewFlagSet("flash run", flag.ContinueOnError)
	var (
		device string
		addr   string
		entry  string
		yes    bool
		root   string
		image  string // fallback if someone uses -image with run
	)
	fs.StringVar(&device, "device", "", "MTK VCOM serial, e.g. /dev/cu.usbmodem141300")
	fs.StringVar(&addr, "address", "", "target address (load addr with payload, or jump target if no payload)")
	fs.StringVar(&entry, "entry", "", "entry point address (defaults to load/jump address)")
	fs.BoolVar(&yes, "yes", false, "skip confirmation")
	fs.StringVar(&root, "root", "", "package root (rarely needed)")
	fs.StringVar(&image, "image", "", "payload binary (positional also accepted; optional for bare jump)")
	if err := fs.Parse(tail); err != nil {
		return err
	}
	pos := fs.Args()
	bin := ""
	if image != "" {
		bin = image
	}
	if bin == "" && len(pos) > 0 {
		bin = pos[0]
	}
	if bin == "" {
		for _, a := range pos {
			if !strings.HasPrefix(a, "-") {
				bin = a
				break
			}
		}
	}
	if bin != "" {
		if !fileExists(bin) {
			if abs, aerr := filepath.Abs(bin); aerr == nil && fileExists(abs) {
				bin = abs
			} else {
				return fmt.Errorf("payload binary not found: %s", bin)
			}
		}
	}

	cfg := config{
		device:          device,
		mtkPayloadAddr:  addr,
		mtkPayloadEntry: entry,
		yes:             yes,
		root:            root,
		image:           bin,
		prepare:         false,
	}
	if cfg.root == "" {
		if exe, err := os.Executable(); err == nil {
			cfg.root = filepath.Dir(exe)
		}
	}
	if cfg.device == "" {
		return errors.New("-device /dev/cu.usbmodem... is required for run")
	}
	if !isSerialDevicePath(cfg.device) {
		return fmt.Errorf("%s does not look like a serial VCOM device", cfg.device)
	}
	if bin != "" {
		return rawExecutePayload(cfg, bin)
	}
	return rawJumpToAddress(cfg)
}

func runRead(tail []string) error {
	fs := flag.NewFlagSet("flash read", flag.ContinueOnError)
	var (
		device     string
		partitions bool
		address    string
		length     string
		output     string
		part       string // read a named partition, e.g. UBOOT, BOOTIMG, etc.
		reuse      bool
		root       string
		scatter    string
	)
	fs.StringVar(&device, "device", "", "MTK VCOM serial device, e.g. /dev/cu.usbmodem141300")
	fs.BoolVar(&partitions, "partitions", false, "read and print the live eMMC USER partition table (PMT + MBR/EBR fallback)")
	fs.StringVar(&address, "address", "", "eMMC USER offset to read (hex or decimal)")
	fs.StringVar(&length, "length", "", "number of bytes to read (default 512)")
	fs.StringVar(&output, "output", "", "write read data to this file instead of hexdump to stdout")
	fs.StringVar(&part, "part", "", "read entire named partition (e.g. UBOOT, BOOTIMG, data); prints offset first")
	fs.BoolVar(&reuse, "reuse", false, "reuse resident MVIIFlash payload if present (faster subsequent reads)")
	fs.StringVar(&root, "root", "", "MVII package root (for locating feed payload)")
	fs.StringVar(&scatter, "scatter", "", "MTK scatter file (e.g. MT6592_Android_scatter.txt) to name partitions and resolve -part names")

	if err := fs.Parse(tail); err != nil {
		return err
	}

	if device == "" {
		return errors.New("-device /dev/cu.usbmodem... is required")
	}
	if !isSerialDevicePath(device) {
		return fmt.Errorf("%s does not look like a serial VCOM device", device)
	}

	cfg := config{
		device:         device,
		root:           root,
		mtkFeedReuse:   reuse,
		mtkFeedPayload: "", // will be set to auto inside readers
		mtkScatter:     strings.TrimSpace(scatter),
		prepare:        false,
	}
	if cfg.root == "" {
		if exe, err := os.Executable(); err == nil {
			cfg.root = filepath.Dir(exe)
		}
	}
	if cfg.mtkScatter != "" {
		if abs, err := filepath.Abs(cfg.mtkScatter); err == nil {
			cfg.mtkScatter = abs
		}
	}

	switch {
	case partitions:
		// Use the existing implementation (it forces the feed payload and prints nicely).
		cfg.device = device
		return readPartitionsMTKFeed(cfg)

	case part != "":
		return readNamedPartition(cfg, part, output, reuse)

	case address != "":
		return readAddressViaFeed(cfg, address, length, output, reuse)

	default:
		return errors.New("read requires one of: -partitions, -address 0x..., or -part NAME")
	}
}

func readAddressViaFeed(cfg config, addrStr, lenStr, outPath string, reuse bool) error {
	offset, err := parseMTKNumber(addrStr, "-address")
	if err != nil {
		return err
	}

	var length uint64 = 0x200 // default one sector
	if lenStr != "" {
		length, err = parseMTKNumber(lenStr, "-length")
		if err != nil {
			return err
		}
	}
	if length == 0 || length > 64*1024*1024 {
		return fmt.Errorf("-length 0x%x is invalid (must be >0 and reasonable)", length)
	}

	// Force feed payload
	feedCfg := cfg
	if strings.TrimSpace(feedCfg.mtkFeedPayload) == "" {
		feedCfg.mtkFeedPayload = "auto"
	}
	payload, err := loadMTKFeedPayload(feedCfg)
	if err != nil {
		return err
	}

	fmt.Printf("Reading eMMC USER at 0x%x, length 0x%x via MVII feed payload\n", offset, length)
	fmt.Printf("Feed payload: %s (%s) load 0x%x\n", filepath.Base(payload.Path), payload.Kind, payload.LoadAddr)

	client, err := connectMTKSerialForFeedWithOptions(feedCfg.device, reuse)
	if err != nil {
		return err
	}
	defer func() { _ = client.port.Close() }()

	hello := client.feedPayloadHello
	if client.feedPayloadReady {
		fmt.Println("Reusing existing MVIIFlash payload.")
	} else {
		if err := client.disableMT6592Watchdog(); err != nil {
			fmt.Printf("Warning: disable watchdog: %v\n", err)
		}
		if err := client.sendDA(payload.LoadAddr, 0, payload.Data); err != nil {
			return fmt.Errorf("send feed payload to 0x%x: %w", payload.LoadAddr, err)
		}
		if err := client.jumpDA(payload.EntryAddr); err != nil {
			fmt.Printf("JUMP_DA: %v (continuing)\n", err)
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

	stopReset := installMTKFeedInterruptReset(client)
	defer stopReset()

	data, _, err := client.readMTKFeedBytes(1, offset, uint32(length), mtkLegacyEMMCPartUser, fmt.Sprintf("read@0x%x", offset))
	if err != nil {
		return err
	}

	if outPath != "" {
		if err := os.WriteFile(outPath, data, 0o644); err != nil {
			return fmt.Errorf("write %s: %w", outPath, err)
		}
		fmt.Printf("Wrote 0x%x bytes to %s\n", len(data), outPath)
	} else {
		// Pretty hexdump for small reads; for large, suggest -output
		if len(data) > 4096 {
			fmt.Printf("Read 0x%x bytes (first 4k shown; use -output to save full binary):\n", len(data))
			fmt.Printf("%s", hex.Dump(data[:4096]))
		} else {
			fmt.Printf("Read 0x%x bytes at 0x%x:\n", len(data), offset)
			fmt.Printf("%s", hex.Dump(data))
		}
	}
	return nil
}

func readNamedPartition(cfg config, name, outPath string, reuse bool) error {
	// First get the live table using existing logic, but without exiting.
	// We'll temporarily capture by calling the low-level after ensuring payload.
	feedCfg := cfg
	if strings.TrimSpace(feedCfg.mtkFeedPayload) == "" {
		feedCfg.mtkFeedPayload = "auto"
	}
	payload, err := loadMTKFeedPayload(feedCfg)
	if err != nil {
		return err
	}

	client, err := connectMTKSerialForFeedWithOptions(feedCfg.device, reuse)
	if err != nil {
		return err
	}
	defer func() { _ = client.port.Close() }()

	if !client.feedPayloadReady {
		if err := client.disableMT6592Watchdog(); err != nil {
			fmt.Printf("Warning: %v\n", err)
		}
		if err := client.sendDA(payload.LoadAddr, 0, payload.Data); err != nil {
			return fmt.Errorf("send payload: %w", err)
		}
		if err := client.jumpDA(payload.EntryAddr); err != nil {
			fmt.Printf("JUMP warning: %v\n", err)
		}
		if _, err := client.waitMTKFeedHello(); err != nil {
			return err
		}
	}

	stop := installMTKFeedInterruptReset(client)
	defer stop()

	parts, _, err := client.readMTKFeedLivePartitions(1)
	if err != nil {
		return err
	}
	printMTKLivePartitions(parts)

	p, ok := findMTKLivePartition(parts, name)
	if !ok {
		// try a few common aliases
		if p2, ok2 := findMTKLivePartition(parts, "UBOOT"); name == "LK" || name == "lk" {
			p, ok = p2, ok2
		}
	}
	if !ok {
		// Fall back to scatter (gives protect_f, protect_s, 2, 3, 4 etc.)
		if sp := tryLoadScatterPartitions(cfg); len(sp) > 0 {
			p, ok = findMTKLivePartition(sp, name)
		}
	}
	if !ok {
		return fmt.Errorf("partition %q not found in live table or scatter; run with -partitions to list", name)
	}

	fmt.Printf("Reading partition %s: offset=0x%x size=0x%x\n", p.Name, p.Offset, p.Size)

	readLen := uint32(p.Size)
	if readLen == 0 {
		return fmt.Errorf("partition %s has zero size", p.Name)
	}

	data, _, err := client.readMTKFeedBytes(2, p.Offset, readLen, mtkLegacyEMMCPartUser, p.Name)
	if err != nil {
		return err
	}

	if outPath == "" {
		// default output name
		outPath = strings.ToLower(p.Name) + ".bin"
	}
	if err := os.WriteFile(outPath, data, 0o644); err != nil {
		return err
	}
	fmt.Printf("Wrote %s (%d bytes) to %s\n", p.Name, len(data), outPath)
	return nil
}

func rawExecutePayload(cfg config, binPath string) error {
	if cfg.device == "" || !isSerialDevicePath(cfg.device) {
		return errors.New("raw payload execution requires -device pointing at a serial VCOM (e.g. /dev/cu.usbmodem...)")
	}
	addrStr := strings.TrimSpace(cfg.mtkPayloadAddr)
	if addrStr == "" {
		addrStr = strings.TrimSpace(os.Getenv("MVII_MTK_PAYLOAD_ADDR"))
	}
	if addrStr == "" {
		return errors.New("raw payload run requires -address 0xNNNNNN (MT6592 feed payloads commonly live at 0x110000)")
	}
	loadAddr, err := parseMTKNumber(addrStr, "-address")
	if err != nil {
		return err
	}
	if loadAddr > 0xffffffff {
		return fmt.Errorf("-address 0x%x is outside 32-bit MT6592 space", loadAddr)
	}

	entryStr := strings.TrimSpace(cfg.mtkPayloadEntry)
	if entryStr == "" {
		entryStr = strings.TrimSpace(os.Getenv("MVII_MTK_PAYLOAD_ENTRY"))
	}
	entryAddr := loadAddr
	if entryStr != "" {
		parsed, err := parseMTKNumber(entryStr, "-entry")
		if err != nil {
			return err
		}
		entryAddr = parsed
	}

	data, err := os.ReadFile(binPath)
	if err != nil {
		return err
	}
	if len(data) == 0 {
		return fmt.Errorf("%s is empty", binPath)
	}
	if len(data) > mtkFeedMaxPayloadBytes {
		return fmt.Errorf("payload too large: %d bytes (limit ~16 MiB for BROM upload)", len(data))
	}

	if !cfg.yes {
		fmt.Printf("Load %s (%s) to 0x%x and jump to entry 0x%x via %s?\n",
			binPath, formatBytes(uint64(len(data))), loadAddr, entryAddr, cfg.device)
		fmt.Print("Type exactly 'FLASH J36 ULTRA' to continue: ")
		line, rerr := bufio.NewReader(os.Stdin).ReadString('\n')
		if rerr != nil {
			return rerr
		}
		if strings.TrimSpace(line) != "FLASH J36 ULTRA" {
			return errors.New("confirmation did not match")
		}
	}

	client, err := connectMTKSerialWithOptions(cfg.device, mtkSerialConnectOptions{})
	if err != nil {
		return err
	}
	defer func() {
		if client.port != nil {
			_ = client.port.Close()
		}
	}()

	if err := client.disableMT6592Watchdog(); err != nil {
		fmt.Printf("Warning: could not disable watchdog: %v\n", err)
	}
	fmt.Printf("Uploading %s (%s) to 0x%x...\n", filepath.Base(binPath), formatBytes(uint64(len(data))), loadAddr)
	if err := client.sendDA(uint32(loadAddr), 0, data); err != nil {
		return fmt.Errorf("SEND_DA to 0x%x: %w", loadAddr, err)
	}
	fmt.Printf("Jumping to 0x%x...\n", entryAddr)
	if err := client.jumpDA(uint32(entryAddr)); err != nil {
		fmt.Printf("JUMP_DA: %v (proceeding to capture output anyway)\n", err)
	}
	fmt.Println("Payload started. Reading bridge output (Ctrl-C to stop):")
	// hand off FD to follower for raw capture / reconnects
	if client.port != nil {
		_ = client.port.Close()
		client.port = nil
	}
	return followRawSerialBridge(cfg.device)
}

// rawJumpToAddress performs a bare JUMP_DA (no SEND_DA/upload) to the given
// address and then pumps raw serial bytes (the "bridge" output) to stdout.
// Used for ./flash run -address 0x... -device ... with no payload binary.
func rawJumpToAddress(cfg config) error {
	if cfg.device == "" || !isSerialDevicePath(cfg.device) {
		return errors.New("run requires -device pointing at a serial VCOM (e.g. /dev/cu.usbmodem...)")
	}
	addrStr := strings.TrimSpace(cfg.mtkPayloadAddr)
	if addrStr == "" {
		addrStr = strings.TrimSpace(os.Getenv("MVII_MTK_PAYLOAD_ADDR"))
	}
	if addrStr == "" {
		return errors.New("run requires -address 0xNNNNNN when no payload binary is given")
	}
	jumpAddr, err := parseMTKNumber(addrStr, "-address")
	if err != nil {
		return err
	}
	if jumpAddr > 0xffffffff {
		return fmt.Errorf("-address 0x%x is outside 32-bit MT6592 space", jumpAddr)
	}

	entryStr := strings.TrimSpace(cfg.mtkPayloadEntry)
	if entryStr == "" {
		entryStr = strings.TrimSpace(os.Getenv("MVII_MTK_PAYLOAD_ENTRY"))
	}
	entryAddr := jumpAddr
	if entryStr != "" {
		parsed, err := parseMTKNumber(entryStr, "-entry")
		if err != nil {
			return err
		}
		entryAddr = parsed
	}

	if !cfg.yes {
		fmt.Printf("Jump to 0x%x (entry 0x%x) via %s and capture serial bridge output?\n",
			jumpAddr, entryAddr, cfg.device)
		fmt.Print("Type exactly 'FLASH J36 ULTRA' to continue: ")
		line, rerr := bufio.NewReader(os.Stdin).ReadString('\n')
		if rerr != nil {
			return rerr
		}
		if strings.TrimSpace(line) != "FLASH J36 ULTRA" {
			return errors.New("confirmation did not match")
		}
	}

	client, err := connectMTKSerialWithOptions(cfg.device, mtkSerialConnectOptions{})
	if err != nil {
		return err
	}
	// Defer not used for the port because we explicitly hand off to follower which manages its own opens.
	// Close the protocol port after jump so raw follower can take over cleanly.
	defer func() {
		if client.port != nil {
			_ = client.port.Close()
		}
	}()

	if err := client.disableMT6592Watchdog(); err != nil {
		fmt.Printf("Warning: could not disable watchdog: %v\n", err)
	}
	fmt.Printf("Jumping to 0x%x (no payload upload)...\n", entryAddr)
	if err := client.jumpDA(uint32(entryAddr)); err != nil {
		fmt.Printf("JUMP_DA: %v (proceeding to capture output anyway)\n", err)
	}
	fmt.Println("Jump issued. Reading bridge output (Ctrl-C to stop):")

	// Close here so followRawSerialBridge can manage its own opens/reconnects without
	// the higher level client owning the FD.
	if client.port != nil {
		_ = client.port.Close()
		client.port = nil
	}
	return followRawSerialBridge(cfg.device)
}

// followRawSerialBridge opens (and reopens on disconnect) the serial device in raw
// mode and copies all received bytes to stdout. This is how post-jump "bridge output"
// (e.g. UART logs from a just-jumped stage1 or payload) gets displayed.
func followRawSerialBridge(device string) error {
	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, os.Interrupt)
	defer signal.Stop(sigCh)

	buf := make([]byte, 1024)
	var received uint64
	var port *mtkSerialPort
	var opened string
	nextOpen := time.Time{}

	for {
		select {
		case <-sigCh:
			if port != nil {
				_ = port.Close()
			}
			fmt.Printf("\n[bridge] stopped (0x%x bytes)\n", received)
			return nil
		default:
		}

		if port == nil {
			if !nextOpen.IsZero() && time.Now().Before(nextOpen) {
				time.Sleep(10 * time.Millisecond)
				continue
			}
			var err error
			port, opened, err = openMTKFollowSerial(device)
			if err != nil {
				nextOpen = time.Now().Add(200 * time.Millisecond)
				continue
			}
			_ = opened // quiet; device bytes are the payload
		}

		n, readErr := port.file.Read(buf)
		if n > 0 {
			received += uint64(n)
			if _, werr := os.Stdout.Write(buf[:n]); werr != nil {
				_ = port.Close()
				return werr
			}
		}
		if readErr != nil {
			if isWouldBlock(readErr) || errors.Is(readErr, io.EOF) {
				time.Sleep(2 * time.Millisecond)
				continue
			}
			// transient error; reopen
			_ = port.Close()
			port = nil
			opened = ""
			nextOpen = time.Now().Add(100 * time.Millisecond)
			continue
		}
		if n == 0 {
			time.Sleep(2 * time.Millisecond)
		}
	}
}

func normalizeTarget(target string) (string, error) {
	switch strings.ToLower(strings.TrimSpace(target)) {
	case "", "arm", "armv7", "j36", "j36-ultra", "mediatek-j36-ultra", "mt6592":
		return "arm", nil
	default:
		return "", fmt.Errorf("unknown -target %q; ./flash currently supports only arm/J36 Ultra", target)
	}
}

func resolveImage(cfg config) (string, error) {
	if cfg.image != "" {
		image, err := filepath.Abs(cfg.image)
		if err != nil {
			return "", err
		}
		if !fileExists(image) {
			return "", fmt.Errorf("image %s does not exist", image)
		}
		return image, nil
	}

	for _, candidate := range defaultImageCandidates(cfg.root) {
		if fileExists(candidate) {
			return candidate, nil
		}
	}

	if cfg.prepare {
		prepare := filepath.Join(cfg.root, executableName("prepare"))
		if fileExists(prepare) {
			fmt.Println("Default ARM image is missing; running ./prepare -target=arm first.")
			cmd := exec.Command(prepare, "-target=arm", "-root", cfg.root)
			cmd.Stdout = os.Stdout
			cmd.Stderr = os.Stderr
			cmd.Stdin = os.Stdin
			if err := cmd.Run(); err != nil {
				return "", fmt.Errorf("prepare ARM image: %w", err)
			}
			for _, candidate := range defaultImageCandidates(cfg.root) {
				if fileExists(candidate) {
					return candidate, nil
				}
			}
		}
	}

	return "", fmt.Errorf("missing %s or %s; build mvii-armv7-pc or run ./prepare -target=arm",
		filepath.Join(cfg.root, "boot.img"),
		filepath.Join(cfg.root, "lk.bin"))
}

func defaultImageCandidates(root string) []string {
	return []string{
		filepath.Join(root, "boot.img"),
		filepath.Join(root, "lk.bin"),
	}
}

func chooseBackend(cfg config) (string, error) {
	if cfg.tool != "" {
		name := strings.ToLower(filepath.Base(cfg.tool))
		switch {
		case strings.Contains(name, "fastboot"):
			return "fastboot", nil
		case strings.Contains(name, "mtk"):
			return "", mtkclientRemovedError("-tool")
		default:
			return "", errors.New("-tool with -backend=auto is ambiguous; pass -backend=fastboot or -backend=raw-block")
		}
	}
	if isSerialDevicePath(cfg.device) {
		return "mtk-serial", nil
	}
	if cfg.device != "" {
		return "raw-block", nil
	}
	if _, err := resolveFastboot(cfg); err == nil {
		devices, _ := fastbootDevices(cfg)
		if len(devices) > 0 {
			return "fastboot", nil
		}
	}
	return "", errors.New("no fastboot device detected; pass -backend=raw-block -device /dev/diskN to use the Go raw block writer")
}

func listBackends(cfg config) error {
	fmt.Println("MVII ARM flash helper")
	fmt.Println()
	if tool, err := resolveFastboot(cfg); err == nil {
		fmt.Printf("fastboot: %s\n", tool.name)
		devices, err := fastbootDevices(cfg)
		if err != nil {
			fmt.Printf("  devices: %v\n", err)
		} else if len(devices) == 0 {
			fmt.Println("  devices: none")
		} else {
			for _, dev := range devices {
				fmt.Printf("  device: %s\n", dev)
			}
		}
	} else {
		fmt.Println("fastboot: not found")
	}

	fmt.Println("mtkclient: removed from this package")
	fmt.Println("raw-block: available with -backend=raw-block -device /dev/diskN")
	fmt.Println("mtk-serial (native feed): primary path for J36 Ultra")
	fmt.Println("  ./flash lk-or-img.bin -device /dev/cu.usbmodemXXXX   # feed via auto MVIIFlash payload")
	fmt.Println("  ./flash run -address 0x110000 [payload.bin] -device /dev/cu.usbmodemXXXX   # raw load+jump, or bare jump+bridge output if no payload")
	fmt.Println("  -address / -entry also work for raw payload uploads without the 'run' verb")
	fmt.Println("  -mtk-feed-payload auto  (or explicit path) forces the resident feed protocol engine")
	fmt.Println("  -mtk-feed-reboot / -mtk-feed-reuse for advanced feed control")
	fmt.Println("  -raw-offset / -raw-length still supported for explicit eMMC locations")
	return nil
}

func flashWithFastboot(cfg config, image string) error {
	tool, err := resolveFastboot(cfg)
	if err != nil {
		return err
	}
	partition := effectivePartition(cfg, "fastboot")
	devices, err := fastbootDevices(cfg)
	if err != nil {
		return err
	}
	if len(devices) == 0 {
		return errors.New("no fastboot device detected; boot the J36 Ultra into fastboot or use -backend=raw-block -device /dev/diskN")
	}
	if len(devices) > 1 && cfg.serial == "" {
		return fmt.Errorf("multiple fastboot devices detected (%s); pass -serial", strings.Join(devices, ", "))
	}
	selected := devices[0]
	if cfg.serial != "" {
		selected = cfg.serial
	}

	if err := confirmFlash(cfg, fmt.Sprintf("fastboot device %s", selected), image, partition); err != nil {
		return err
	}

	args := append([]string{}, tool.args...)
	if cfg.serial != "" {
		args = append(args, "-s", cfg.serial)
	}
	args = append(args, "flash", partition, image)
	if err := runStreaming(tool.name, args...); err != nil {
		return err
	}
	if cfg.reboot {
		rebootArgs := append([]string{}, tool.args...)
		if cfg.serial != "" {
			rebootArgs = append(rebootArgs, "-s", cfg.serial)
		}
		rebootArgs = append(rebootArgs, "reboot")
		if err := runStreaming(tool.name, rebootArgs...); err != nil {
			return err
		}
	}
	fmt.Println("Fastboot flash complete.")
	return nil
}

func flashWithMTKClient(cfg config, image string) error {
	tool, err := resolveMTKClient(cfg)
	if err != nil {
		return err
	}
	partition := effectivePartition(cfg, "mtkclient")
	imageInfo, err := os.Stat(image)
	if err != nil {
		return err
	}
	if err := checkMTKClientReady(tool); err != nil {
		return fmt.Errorf("%w\nInstall Python dependencies with: ./flash -target=arm -install-mtk-deps", err)
	}
	fmt.Println("MTK mode expects the J36 Ultra powered off, connected through OTG.")
	fmt.Println("The helper will try `adb reboot edl` first when an online ADB device is present.")
	fmt.Println("If mtkclient waits, hold the board's boot/download key combo while reconnecting OTG.")
	if cfg.rawOffset != "" {
		rawLengthText := cfg.rawLength
		if rawLengthText == "" {
			rawLengthText = fmt.Sprintf("0x%x", imageInfo.Size())
		}
		if _, err := parseMTKNumber(cfg.rawOffset, "-raw-offset"); err != nil {
			return err
		}
		rawLength, err := parseMTKNumber(rawLengthText, "-raw-length")
		if err != nil {
			return err
		}
		length := fmt.Sprintf("0x%x", rawLength)
		flashImage, cleanup, err := prepareRawFlashImage(image, imageInfo.Size(), rawLength)
		if err != nil {
			return err
		}
		defer cleanup()
		if err := confirmRawFlash(cfg, "MediaTek BROM/preloader via mtkclient", image, cfg.rawOffset, length); err != nil {
			return err
		}
		if err := maybeADBReboot(cfg); err != nil {
			return err
		}
		writeTool := tool
		cleanupWriteTool := func() {}
		if shimmedTool, cleanup, err := legacyMTKWriteShim(tool); err == nil {
			writeTool = shimmedTool
			cleanupWriteTool = cleanup
		} else {
			fmt.Printf("mtkclient legacy write shim unavailable: %v; using mtkclient directly.\n", err)
		}
		defer cleanupWriteTool()
		text, err := runMTKClientRaw(writeTool, "wo", cfg.rawOffset, length, flashImage)
		if err != nil {
			return err
		}
		if strings.Contains(text, "Couldn't send sdmmc_write_data header") {
			return errors.New("mtkclient legacy DA rejected the eMMC write header; reconnect in BROM mode and retry")
		}
		if strings.Contains(text, "Failed to write") {
			return errors.New("mtkclient raw-offset write failed")
		}
	} else {
		if err := confirmFlash(cfg, "MediaTek BROM/preloader via mtkclient", image, partition); err != nil {
			return err
		}
		if err := maybeADBReboot(cfg); err != nil {
			return err
		}
		args := append([]string{}, tool.args...)
		args = append(args, "w", partition, image)
		if _, err := runMTKClient(tool, partition, args[len(tool.args):]...); err != nil {
			return err
		}
	}
	fmt.Println("mtkclient flash complete.")
	return nil
}

func printMTKPartitions(cfg config) error {
	tool, err := resolveMTKClient(cfg)
	if err != nil {
		return err
	}
	if err := checkMTKClientReady(tool); err != nil {
		return fmt.Errorf("%w\nInstall Python dependencies with: ./flash -target=arm -install-mtk-deps", err)
	}
	if err := maybeADBReboot(cfg); err != nil {
		return err
	}
	args := append([]string{}, tool.args...)
	args = append(args, "printgpt")
	if _, err := runMTKClient(tool, "", args[len(tool.args):]...); err != nil {
		return err
	}
	return nil
}

type partitionProbe struct {
	Name    string
	Kind    string
	Summary string
	Boot    bool
}

type partitionSpan struct {
	Name    string
	Sector  uint64
	Sectors uint64
}

type rawGap struct {
	Start  uint64
	Length uint64
	Before string
	After  string
}

type rawBootCandidate struct {
	Offset uint64
	Probe  partitionProbe
	Gap    rawGap
}

func detectMTKBootPartition(cfg config) error {
	tool, err := resolveMTKClient(cfg)
	if err != nil {
		return err
	}
	if err := checkMTKClientReady(tool); err != nil {
		return fmt.Errorf("%w\nInstall Python dependencies with: ./flash -target=arm -install-mtk-deps", err)
	}
	candidates := splitCSV(cfg.candidates)
	if len(candidates) == 0 {
		return errors.New("-candidates must name at least one partition")
	}
	tmp, err := os.MkdirTemp("", "mvii-mtk-boot-probe-")
	if err != nil {
		return err
	}
	defer os.RemoveAll(tmp)

	var files []string
	for _, candidate := range candidates {
		files = append(files, filepath.Join(tmp, sanitizeFilename(candidate)+".bin"))
	}
	fmt.Printf("Sampling %d partition headers: %s\n", len(candidates), strings.Join(candidates, ", "))
	if err := maybeADBReboot(cfg); err != nil {
		return err
	}
	args := append([]string{}, tool.args...)
	args = append(args, "r", strings.Join(candidates, ","), strings.Join(files, ","), "--length", "0x10000")
	text, err := runMTKClientRaw(tool, args[len(tool.args):]...)
	if err != nil {
		return err
	}
	spans := parseDumpedPartitionSpans(text, candidates, files)

	var probes []partitionProbe
	var bootLike []partitionProbe
	for i, candidate := range candidates {
		probe := inspectPartitionHeader(candidate, files[i])
		probes = append(probes, probe)
		if probe.Boot {
			bootLike = append(bootLike, probe)
		}
	}

	fmt.Println()
	fmt.Println("Partition header probe:")
	for _, probe := range probes {
		if probe.Summary != "" {
			fmt.Printf("  %s: %s - %s\n", probe.Name, probe.Kind, probe.Summary)
		} else {
			fmt.Printf("  %s: %s\n", probe.Name, probe.Kind)
		}
	}
	fmt.Println()
	switch len(bootLike) {
	case 0:
		fmt.Println("No Android boot-image header was found in the sampled partitions.")
		rawBoots, err := scanHiddenBootGaps(tool, tmp, spans)
		if err != nil {
			return err
		}
		reportRawBootCandidates(rawBoots)
	case 1:
		fmt.Printf("Likely boot partition: %s\n", bootLike[0].Name)
		fmt.Printf("Flash with: ./flash -target=arm -backend=mtkclient -partition %s\n", bootLike[0].Name)
	default:
		fmt.Printf("Multiple boot-like partitions found: %s\n", partitionNames(bootLike))
		fmt.Println("One is probably boot and one may be recovery. Verify before flashing, then pass -partition <name>.")
	}
	return nil
}

func parseDumpedPartitionSpans(text string, candidates, files []string) []partitionSpan {
	fileToName := make(map[string]string, len(files))
	for i, file := range files {
		if i < len(candidates) {
			fileToName[filepath.Clean(file)] = candidates[i]
		}
	}
	var spans []partitionSpan
	for _, match := range dumpedSectorRE.FindAllStringSubmatch(text, -1) {
		if len(match) != 4 {
			continue
		}
		sector, err := strconv.ParseUint(match[1], 10, 64)
		if err != nil {
			continue
		}
		sectors, err := strconv.ParseUint(match[2], 10, 64)
		if err != nil {
			continue
		}
		name := fileToName[filepath.Clean(strings.TrimSpace(match[3]))]
		if name == "" {
			name = strings.TrimSuffix(filepath.Base(strings.TrimSpace(match[3])), filepath.Ext(match[3]))
		}
		spans = append(spans, partitionSpan{Name: name, Sector: sector, Sectors: sectors})
	}
	return spans
}

func hiddenGapsFromSpans(spans []partitionSpan) []rawGap {
	if len(spans) == 0 {
		return nil
	}
	sorted := append([]partitionSpan(nil), spans...)
	sort.Slice(sorted, func(i, j int) bool {
		return sorted[i].Sector < sorted[j].Sector
	})
	var gaps []rawGap
	var cursor uint64
	before := "start of user area"
	for _, span := range sorted {
		if span.Sectors == 0 {
			continue
		}
		if span.Sector > cursor {
			start := cursor * mtkSectorSize
			length := (span.Sector - cursor) * mtkSectorSize
			if length >= 0x10000 {
				gaps = append(gaps, rawGap{Start: start, Length: length, Before: before, After: span.Name})
			}
		}
		if end := span.Sector + span.Sectors; end > cursor {
			cursor = end
			before = span.Name
		}
	}
	return gaps
}

func scanHiddenBootGaps(tool commandSpec, tmp string, spans []partitionSpan) ([]rawBootCandidate, error) {
	gaps := hiddenGapsFromSpans(spans)
	if len(gaps) == 0 {
		fmt.Println("The mtkclient output did not expose raw partition spans, so hidden-gap scanning is unavailable.")
		fmt.Println("Retry with a broader list, for example: ./flash -target=arm -detect-boot-partition -candidates protect_f,protect_s,2,3,4,5,data")
		return nil, nil
	}
	fmt.Println()
	fmt.Println("Scanning unlabeled raw gaps between reported partitions.")
	var found []rawBootCandidate
	for i, gap := range gaps {
		scanLen := gap.Length
		truncated := false
		if scanLen > maxRawGapScanBytes {
			scanLen = maxRawGapScanBytes
			truncated = true
		}
		gapFile := filepath.Join(tmp, fmt.Sprintf("raw-gap-%02d-0x%x.bin", i, gap.Start))
		if truncated {
			fmt.Printf("  gap 0x%x..0x%x (%s) between %s and %s; scanning first %s\n",
				gap.Start, gap.Start+gap.Length, formatBytes(gap.Length), gap.Before, gap.After, formatBytes(scanLen))
		} else {
			fmt.Printf("  gap 0x%x..0x%x (%s) between %s and %s\n",
				gap.Start, gap.Start+gap.Length, formatBytes(gap.Length), gap.Before, gap.After)
		}
		args := append([]string{}, tool.args...)
		args = append(args, "ro", fmt.Sprintf("0x%x", gap.Start), fmt.Sprintf("0x%x", scanLen), gapFile)
		text, err := runMTKClientRaw(tool, args[len(tool.args):]...)
		if err != nil {
			return found, err
		}
		if strings.Contains(text, "Failed to dump offset") {
			return found, fmt.Errorf("mtkclient failed to dump hidden gap at 0x%x", gap.Start)
		}
		data, err := os.ReadFile(gapFile)
		if err != nil {
			return found, err
		}
		for _, offsetInGap := range androidBootHeaderOffsets(data) {
			rawOffset := gap.Start + uint64(offsetInGap)
			probe := androidBootProbe(fmt.Sprintf("raw@0x%x", rawOffset), data[offsetInGap:])
			found = append(found, rawBootCandidate{Offset: rawOffset, Probe: probe, Gap: gap})
		}
	}
	return found, nil
}

func androidBootHeaderOffsets(data []byte) []int {
	magic := []byte("ANDROID!")
	var offsets []int
	for pos := 0; pos < len(data); {
		idx := bytes.Index(data[pos:], magic)
		if idx < 0 {
			break
		}
		offset := pos + idx
		offsets = append(offsets, offset)
		pos = offset + len(magic)
	}
	return offsets
}

func reportRawBootCandidates(candidates []rawBootCandidate) {
	fmt.Println()
	switch len(candidates) {
	case 0:
		fmt.Println("No Android boot-image header was found in the visible partitions or hidden raw gaps.")
		fmt.Println("Do not flash protect_f/protect_s/data or the ext numeric partitions as boot.")
		fmt.Println("Retry with any additional mtkclient partition names shown by -print-partitions, or inspect a full flash dump.")
	case 1:
		candidate := candidates[0]
		fmt.Printf("Likely hidden boot image: raw offset 0x%x\n", candidate.Offset)
		fmt.Printf("  %s\n", candidate.Probe.Summary)
		fmt.Printf("Flash with: ./flash -target=arm -backend=mtkclient -raw-offset 0x%x\n", candidate.Offset)
	default:
		fmt.Println("Multiple Android boot images were found in hidden raw gaps:")
		for _, candidate := range candidates {
			fmt.Printf("  raw offset 0x%x: %s\n", candidate.Offset, candidate.Probe.Summary)
		}
		fmt.Printf("Likely boot image: raw offset 0x%x\n", candidates[0].Offset)
		if len(candidates) >= 2 {
			fmt.Printf("Likely recovery image: raw offset 0x%x\n", candidates[1].Offset)
		}
		fmt.Println("Verify before flashing if the stock layout is unusual.")
		if length := inferredRawBootLength(candidates); length > 0 {
			fmt.Printf("Flash likely boot with: ./flash -target=arm -backend=mtkclient -raw-offset 0x%x -raw-length 0x%x\n", candidates[0].Offset, length)
		} else {
			fmt.Printf("Flash likely boot with: ./flash -target=arm -backend=mtkclient -raw-offset 0x%x\n", candidates[0].Offset)
		}
	}
}

func inferredRawBootLength(candidates []rawBootCandidate) uint64 {
	if len(candidates) < 2 || candidates[1].Offset <= candidates[0].Offset {
		return 0
	}
	if candidates[0].Gap.Start != candidates[1].Gap.Start || candidates[0].Gap.Length != candidates[1].Gap.Length {
		return 0
	}
	return candidates[1].Offset - candidates[0].Offset
}

func splitCSV(value string) []string {
	var out []string
	for _, part := range strings.Split(value, ",") {
		part = strings.TrimSpace(part)
		if part != "" {
			out = append(out, part)
		}
	}
	return out
}

func sanitizeFilename(value string) string {
	var b strings.Builder
	for _, r := range value {
		if (r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z') || (r >= '0' && r <= '9') || r == '-' || r == '_' {
			b.WriteRune(r)
		} else {
			b.WriteByte('_')
		}
	}
	if b.Len() == 0 {
		return "partition"
	}
	return b.String()
}

func inspectPartitionHeader(name, path string) partitionProbe {
	data, err := os.ReadFile(path)
	if err != nil {
		return partitionProbe{Name: name, Kind: "not read"}
	}
	for len(data) > 0 && data[len(data)-1] == 0 {
		data = data[:len(data)-1]
	}
	if len(data) == 0 {
		return partitionProbe{Name: name, Kind: "empty/zero-filled"}
	}
	if bytes.HasPrefix(data, []byte("ANDROID!")) {
		return androidBootProbe(name, data)
	}
	if len(data) > 0x438 && data[0x438] == 0x53 && data[0x439] == 0xef {
		return partitionProbe{Name: name, Kind: "ext filesystem", Summary: "ext superblock detected"}
	}
	if bytes.HasPrefix(data, []byte{0x88, 0x16, 0x88, 0x58}) {
		return partitionProbe{Name: name, Kind: "MediaTek payload", Summary: "MTK image header at offset 0"}
	}
	return partitionProbe{Name: name, Kind: "unknown", Summary: fmt.Sprintf("first bytes %s", hexPrefix(data, 16))}
}

func androidBootProbe(name string, data []byte) partitionProbe {
	get := func(off int) uint32 {
		if len(data) < off+4 {
			return 0
		}
		return binary.LittleEndian.Uint32(data[off : off+4])
	}
	board := cString(data, 48, 16)
	cmdline := cString(data, 64, 512)
	parts := []string{
		fmt.Sprintf("kernel=%s", formatBytes(uint64(get(8)))),
		fmt.Sprintf("ramdisk=%s", formatBytes(uint64(get(16)))),
		fmt.Sprintf("second=%s", formatBytes(uint64(get(24)))),
		fmt.Sprintf("page=%d", get(36)),
	}
	if board != "" {
		parts = append(parts, "board="+board)
	}
	if cmdline != "" {
		if len(cmdline) > 72 {
			cmdline = cmdline[:72] + "..."
		}
		parts = append(parts, "cmdline="+cmdline)
	}
	return partitionProbe{Name: name, Kind: "Android boot image", Summary: strings.Join(parts, ", "), Boot: true}
}

func cString(data []byte, off, length int) string {
	if len(data) <= off {
		return ""
	}
	end := off + length
	if len(data) < end {
		end = len(data)
	}
	raw := data[off:end]
	if idx := bytes.IndexByte(raw, 0); idx >= 0 {
		raw = raw[:idx]
	}
	return strings.TrimSpace(string(raw))
}

func hexPrefix(data []byte, n int) string {
	if len(data) < n {
		n = len(data)
	}
	var b strings.Builder
	for i := 0; i < n; i++ {
		if i > 0 {
			b.WriteByte(' ')
		}
		fmt.Fprintf(&b, "%02x", data[i])
	}
	return b.String()
}

func formatBytes(value uint64) string {
	switch {
	case value >= 1024*1024:
		return fmt.Sprintf("%.1f MiB", float64(value)/(1024*1024))
	case value >= 1024:
		return fmt.Sprintf("%.1f KiB", float64(value)/1024)
	default:
		return fmt.Sprintf("%d B", value)
	}
}

func partitionNames(probes []partitionProbe) string {
	names := make([]string, 0, len(probes))
	for _, probe := range probes {
		names = append(names, probe.Name)
	}
	return strings.Join(names, ", ")
}

type rawBlockFlashPlan struct {
	inputDevice string
	diskDevice  string
	writeDevice string
	offset      uint64
	length      uint64
	imageSize   int64
}

func flashRawBlock(cfg config, image string) error {
	info, err := os.Stat(image)
	if err != nil {
		return err
	}
	plan, err := makeRawBlockFlashPlan(cfg, info.Size())
	if err != nil {
		return err
	}
	if err := validateRawBlockDevice(plan); err != nil {
		return err
	}
	if err := confirmRawBlockDeviceFlash(cfg, plan, image); err != nil {
		return err
	}
	if err := prepareRawBlockDevice(plan); err != nil {
		return err
	}
	expectedHash, err := writeRawBlockImage(image, plan)
	if err != nil {
		return err
	}
	settleRawBlockDevice(plan)
	if err := verifyRawBlockImage(plan, expectedHash); err != nil {
		return err
	}
	finishRawBlockDevice(plan)
	fmt.Printf("Raw block flash complete: %s\n", plan.writeDevice)
	return nil
}

func flashWithMTKSerial(cfg config, image string) error {
	if cfg.device == "" {
		return errors.New("-backend=mtk-serial requires -device /dev/cu.usbmodem... or another MTK VCOM serial device")
	}
	return flashWithMTKSerialDA(cfg, image)
}

func makeRawBlockFlashPlan(cfg config, imageSize int64) (rawBlockFlashPlan, error) {
	if cfg.device == "" {
		return rawBlockFlashPlan{}, errors.New("-backend=raw-block requires -device /dev/diskN or another block device")
	}
	if imageSize < 0 {
		return rawBlockFlashPlan{}, fmt.Errorf("image size is invalid: %d", imageSize)
	}
	imageLength := uint64(imageSize)
	offset := uint64(0)
	if cfg.rawOffset != "" {
		parsed, err := parseMTKNumber(cfg.rawOffset, "-raw-offset")
		if err != nil {
			return rawBlockFlashPlan{}, err
		}
		offset = parsed
	}
	length := imageLength
	if cfg.rawLength != "" {
		parsed, err := parseMTKNumber(cfg.rawLength, "-raw-length")
		if err != nil {
			return rawBlockFlashPlan{}, err
		}
		length = parsed
	}
	if length < imageLength {
		return rawBlockFlashPlan{}, fmt.Errorf("-raw-length 0x%x is smaller than image size 0x%x", length, imageLength)
	}
	if offset > uint64(1<<63-1) {
		return rawBlockFlashPlan{}, fmt.Errorf("-raw-offset 0x%x is too large for this host", offset)
	}
	if length > uint64(1<<63-1) {
		return rawBlockFlashPlan{}, fmt.Errorf("-raw-length 0x%x is too large for this host", length)
	}

	device := strings.TrimSpace(cfg.device)
	return rawBlockFlashPlan{
		inputDevice: device,
		diskDevice:  normalizeDiskutilDevice(device),
		writeDevice: normalizeBlockDevice(device),
		offset:      offset,
		length:      length,
		imageSize:   imageSize,
	}, nil
}

func validateRawBlockDevice(plan rawBlockFlashPlan) error {
	if plan.inputDevice == "" {
		return errors.New("no raw block device specified")
	}
	if isSerialDevicePath(plan.inputDevice) {
		return fmt.Errorf("%s is a serial/CDC device, not a block device; use a /dev/diskN device for -backend=raw-block, or use -backend=mtk-serial with -da-loader", plan.inputDevice)
	}
	switch runtime.GOOS {
	case "windows":
		if !strings.HasPrefix(plan.inputDevice, `\\.\PhysicalDrive`) {
			return fmt.Errorf("invalid Windows raw disk path %q; expected \\\\.\\PhysicalDriveN", plan.inputDevice)
		}
	default:
		if !strings.HasPrefix(plan.diskDevice, "/dev/") {
			return fmt.Errorf("invalid block device path %q; expected /dev/diskN, /dev/rdiskN, or another /dev node", plan.inputDevice)
		}
		if _, err := os.Stat(plan.diskDevice); err != nil {
			return fmt.Errorf("device %s is not available: %w", plan.diskDevice, err)
		}
		if plan.writeDevice != plan.diskDevice {
			if _, err := os.Stat(plan.writeDevice); err != nil {
				return fmt.Errorf("raw device %s is not available: %w", plan.writeDevice, err)
			}
		}
	}
	switch runtime.GOOS {
	case "darwin":
		return rejectDarwinInternalDisk(plan.diskDevice)
	case "linux":
		return rejectLinuxNonRemovableDisk(plan.diskDevice)
	default:
		return nil
	}
}

func confirmRawBlockDeviceFlash(cfg config, plan rawBlockFlashPlan, image string) error {
	fmt.Println()
	if plan.offset == 0 {
		fmt.Printf("About to raw-write %s to %s (%s).\n", image, plan.writeDevice, formatBytes(plan.length))
	} else {
		fmt.Printf("About to raw-write %s to %s at offset 0x%x, length 0x%x (%s).\n",
			image, plan.writeDevice, plan.offset, plan.length, formatBytes(plan.length))
	}
	fmt.Println("This uses direct Go block I/O, unmounts the target first, and verifies the bytes after writing.")
	fmt.Println("This can make the target device unbootable if the block device or raw offset is wrong.")
	if cfg.yes {
		return nil
	}
	fmt.Print("Type exactly 'FLASH J36 ULTRA' to continue: ")
	line, err := bufio.NewReader(os.Stdin).ReadString('\n')
	if err != nil {
		return err
	}
	if strings.TrimSpace(line) != "FLASH J36 ULTRA" {
		return errors.New("confirmation did not match; leaving the device untouched")
	}
	return nil
}

func prepareRawBlockDevice(plan rawBlockFlashPlan) error {
	switch runtime.GOOS {
	case "darwin":
		return prepareRawBlockDeviceDarwin(plan)
	case "linux":
		prepareRawBlockDeviceLinux(plan)
		return nil
	default:
		return nil
	}
}

func prepareRawBlockDeviceDarwin(plan rawBlockFlashPlan) error {
	args := []string{"unmountDisk", "force", plan.diskDevice}
	if !darwinWholeDisk(plan.diskDevice) {
		args = []string{"unmount", "force", plan.diskDevice}
	}
	out, err := exec.Command("diskutil", args...).CombinedOutput()
	if err == nil || harmlessUnmountOutput(string(out)) {
		runBestEffortCommand("sync")
		return nil
	}
	text := strings.TrimSpace(string(out))
	if text == "" {
		return fmt.Errorf("diskutil %s failed: %w", strings.Join(args, " "), err)
	}
	return fmt.Errorf("diskutil %s failed: %w: %s", strings.Join(args, " "), err, firstLine(text))
}

func prepareRawBlockDeviceLinux(plan rawBlockFlashPlan) {
	unmountLinuxDeviceTree(plan.diskDevice)
	runBestEffortCommand("blockdev", "--flushbufs", plan.diskDevice)
	runBestEffortCommand("sync")
}

func writeRawBlockImage(image string, plan rawBlockFlashPlan) ([]byte, error) {
	src, err := os.Open(image)
	if err != nil {
		return nil, err
	}
	defer src.Close()

	dst, err := os.OpenFile(plan.writeDevice, os.O_WRONLY, 0)
	if err != nil {
		return nil, fmt.Errorf("open %s for write: %w; rerun with sudo if this is the intended target", plan.writeDevice, err)
	}
	defer dst.Close()
	if plan.offset != 0 {
		if _, err := dst.Seek(int64(plan.offset), io.SeekStart); err != nil {
			return nil, fmt.Errorf("seek %s to 0x%x: %w", plan.writeDevice, plan.offset, err)
		}
	}

	expected := sha256.New()
	buf := make([]byte, rawBlockChunkSize)
	written := uint64(0)
	lastProgress := time.Time{}
	fmt.Printf("Writing %s to %s", formatBytes(plan.length), plan.writeDevice)
	if plan.offset != 0 {
		fmt.Printf(" at 0x%x", plan.offset)
	}
	fmt.Println()
	for {
		n, readErr := src.Read(buf)
		if n > 0 {
			chunk := buf[:n]
			if err := writeAll(dst, chunk); err != nil {
				return nil, fmt.Errorf("write %s: %w", plan.writeDevice, err)
			}
			_, _ = expected.Write(chunk)
			written += uint64(n)
			printRawBlockProgress(written, plan.length, &lastProgress)
		}
		if readErr == io.EOF {
			break
		}
		if readErr != nil {
			return nil, fmt.Errorf("read %s: %w", image, readErr)
		}
	}

	if written > plan.length {
		return nil, fmt.Errorf("internal raw write length error: wrote 0x%x bytes for length 0x%x", written, plan.length)
	}
	if pad := plan.length - written; pad > 0 {
		zeroes := make([]byte, rawBlockChunkSize)
		for pad > 0 {
			n := len(zeroes)
			if uint64(n) > pad {
				n = int(pad)
			}
			chunk := zeroes[:n]
			if err := writeAll(dst, chunk); err != nil {
				return nil, fmt.Errorf("zero-pad %s: %w", plan.writeDevice, err)
			}
			_, _ = expected.Write(chunk)
			written += uint64(n)
			pad -= uint64(n)
			printRawBlockProgress(written, plan.length, &lastProgress)
		}
	}

	if err := dst.Sync(); err != nil {
		return nil, fmt.Errorf("sync %s: %w", plan.writeDevice, err)
	}
	printRawBlockProgress(plan.length, plan.length, &lastProgress)
	return expected.Sum(nil), nil
}

func writeAll(dst *os.File, data []byte) error {
	for len(data) > 0 {
		n, err := dst.Write(data)
		if err != nil {
			return err
		}
		if n == 0 {
			return io.ErrShortWrite
		}
		data = data[n:]
	}
	return nil
}

func printRawBlockProgress(done, total uint64, last *time.Time) {
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
	fmt.Printf("\r  wrote %s / %s (%.1f%%)", formatBytes(done), formatBytes(total), percent)
	if done >= total {
		fmt.Println()
	}
	*last = now
}

func settleRawBlockDevice(plan rawBlockFlashPlan) {
	runBestEffortCommand("sync")
	switch runtime.GOOS {
	case "darwin":
		args := []string{"unmountDisk", "force", plan.diskDevice}
		if !darwinWholeDisk(plan.diskDevice) {
			args = []string{"unmount", "force", plan.diskDevice}
		}
		runBestEffortCommand("diskutil", args...)
	case "linux":
		runBestEffortCommand("blockdev", "--flushbufs", plan.diskDevice)
	}
	time.Sleep(2 * time.Second)
}

func verifyRawBlockImage(plan rawBlockFlashPlan, expectedHash []byte) error {
	fmt.Printf("Verifying %s from %s", formatBytes(plan.length), plan.writeDevice)
	if plan.offset != 0 {
		fmt.Printf(" at 0x%x", plan.offset)
	}
	fmt.Println()
	var lastHash []byte
	var lastErr error
	for attempt := 1; attempt <= 2; attempt++ {
		got, err := hashRawBlockRange(plan)
		if err == nil && bytes.Equal(got, expectedHash) {
			fmt.Printf("Verification passed: SHA256 %x\n", got)
			return nil
		}
		lastHash = got
		lastErr = err
		if attempt == 1 {
			fmt.Println("Verification did not pass on the first read; settling the device and retrying.")
			settleRawBlockDevice(plan)
		}
	}
	if lastErr != nil {
		return fmt.Errorf("verification failed: %w", lastErr)
	}
	return fmt.Errorf("verification failed: expected %x, got %x", expectedHash, lastHash)
}

func hashRawBlockRange(plan rawBlockFlashPlan) ([]byte, error) {
	src, err := os.Open(plan.writeDevice)
	if err != nil {
		return nil, fmt.Errorf("open %s for verify: %w", plan.writeDevice, err)
	}
	defer src.Close()
	if plan.offset != 0 {
		if _, err := src.Seek(int64(plan.offset), io.SeekStart); err != nil {
			return nil, fmt.Errorf("seek %s to 0x%x for verify: %w", plan.writeDevice, plan.offset, err)
		}
	}
	h := sha256.New()
	limited := io.LimitReader(src, int64(plan.length))
	n, err := io.CopyBuffer(h, limited, make([]byte, rawBlockChunkSize))
	if err != nil {
		return nil, fmt.Errorf("read %s for verify: %w", plan.writeDevice, err)
	}
	if uint64(n) != plan.length {
		return nil, fmt.Errorf("read %s for verify: got %s, want %s", plan.writeDevice, formatBytes(uint64(n)), formatBytes(plan.length))
	}
	return h.Sum(nil), nil
}

func finishRawBlockDevice(plan rawBlockFlashPlan) {
	switch runtime.GOOS {
	case "darwin":
		runBestEffortCommand("diskutil", "eject", plan.diskDevice)
	case "linux":
		runBestEffortCommand("partprobe", plan.diskDevice)
		runBestEffortCommand("eject", plan.diskDevice)
	}
}

func rejectDarwinInternalDisk(device string) error {
	if allowInternalRawDisk() {
		return nil
	}
	out, err := exec.Command("diskutil", "info", device).CombinedOutput()
	if err != nil {
		text := strings.TrimSpace(string(out))
		if text == "" {
			return fmt.Errorf("diskutil info %s failed: %w", device, err)
		}
		return fmt.Errorf("diskutil info %s failed: %w: %s", device, err, firstLine(text))
	}
	info := string(out)
	if strings.EqualFold(diskutilField(info, "Internal"), "Yes") ||
		strings.EqualFold(diskutilField(info, "Device Location"), "Internal") {
		return fmt.Errorf("%s appears to be an internal macOS disk; refusing raw flash (set MVII_FLASH_ALLOW_INTERNAL_DISK=1 to override)", device)
	}
	return nil
}

func diskutilField(info, name string) string {
	prefix := name + ":"
	for _, line := range strings.Split(info, "\n") {
		line = strings.TrimSpace(line)
		if strings.HasPrefix(line, prefix) {
			return strings.TrimSpace(strings.TrimPrefix(line, prefix))
		}
	}
	return ""
}

func rejectLinuxNonRemovableDisk(device string) error {
	if allowInternalRawDisk() {
		return nil
	}
	base := linuxBaseBlockDevice(device)
	if base == "" {
		return nil
	}
	removable, err := os.ReadFile(filepath.Join("/sys/block", base, "removable"))
	if err != nil {
		return nil
	}
	if strings.TrimSpace(string(removable)) == "0" {
		return fmt.Errorf("/dev/%s is not marked removable; refusing raw flash (set MVII_FLASH_ALLOW_INTERNAL_DISK=1 to override)", base)
	}
	return nil
}

func allowInternalRawDisk() bool {
	switch strings.ToLower(strings.TrimSpace(os.Getenv("MVII_FLASH_ALLOW_INTERNAL_DISK"))) {
	case "1", "true", "yes", "on":
		return true
	default:
		return false
	}
}

func linuxBaseBlockDevice(device string) string {
	base := filepath.Base(device)
	if base == "." || base == string(filepath.Separator) {
		return ""
	}
	if strings.HasPrefix(base, "mmcblk") || strings.HasPrefix(base, "nvme") {
		if idx := strings.LastIndex(base, "p"); idx > 0 && allDigits(base[idx+1:]) {
			return base[:idx]
		}
		return base
	}
	for len(base) > 0 && base[len(base)-1] >= '0' && base[len(base)-1] <= '9' {
		base = base[:len(base)-1]
	}
	return base
}

func allDigits(value string) bool {
	if value == "" {
		return false
	}
	for _, r := range value {
		if r < '0' || r > '9' {
			return false
		}
	}
	return true
}

func unmountLinuxDeviceTree(device string) {
	mounts, err := os.ReadFile("/proc/mounts")
	if err != nil {
		return
	}
	var sources []string
	for _, line := range strings.Split(string(mounts), "\n") {
		fields := strings.Fields(line)
		if len(fields) < 2 {
			continue
		}
		source := fields[0]
		if source == device || strings.HasPrefix(source, device) {
			sources = append(sources, source)
		}
	}
	for _, source := range uniqueStrings(sources) {
		runBestEffortCommand("udisksctl", "unmount", "-b", source, "--no-user-interaction")
		runBestEffortCommand("umount", source)
		runBestEffortCommand("umount", "-l", source)
	}
}

func runBestEffortCommand(name string, args ...string) {
	if _, err := exec.LookPath(name); err != nil {
		return
	}
	_ = exec.Command(name, args...).Run()
}

func harmlessUnmountOutput(text string) bool {
	lower := strings.ToLower(text)
	return strings.Contains(lower, "not currently mounted") ||
		strings.Contains(lower, "was already unmounted") ||
		strings.Contains(lower, "not mounted")
}

func darwinWholeDisk(device string) bool {
	base := filepath.Base(device)
	base = strings.TrimPrefix(base, "r")
	if !strings.HasPrefix(base, "disk") {
		return false
	}
	return allDigits(strings.TrimPrefix(base, "disk"))
}

func resolveFastboot(cfg config) (commandSpec, error) {
	if cfg.tool != "" {
		return commandSpec{name: cfg.tool}, nil
	}
	path, err := exec.LookPath(executableName("fastboot"))
	if err != nil {
		return commandSpec{}, err
	}
	return commandSpec{name: path}, nil
}

func resolveMTKClient(cfg config) (commandSpec, error) {
	if cfg.tool != "" {
		return pythonAwareTool(cfg.tool)
	}
	if candidate, ok := resolveBundledMTKClient(cfg); ok {
		return pythonAwareTool(candidate)
	}
	for _, name := range []string{"mtk", "mtk.py"} {
		path, err := exec.LookPath(name)
		if err == nil {
			return pythonAwareTool(path)
		}
	}
	return commandSpec{}, errors.New("mtkclient not found")
}

func resolveBundledMTKClient(cfg config) (string, bool) {
	for _, candidate := range mtkclientCandidates(cfg) {
		if fileExists(candidate) {
			return candidate, true
		}
	}
	return "", false
}

func mtkclientCandidates(cfg config) []string {
	var candidates []string
	if cfg.mtkclientRoot != "" {
		candidates = append(candidates, filepath.Join(cfg.mtkclientRoot, "mtk.py"))
	}
	candidates = append(candidates,
		filepath.Join(cfg.root, "tools", "mtkclient", "mtk.py"),
		filepath.Join(cfg.root, "mtkclient", "mtk.py"),
	)
	for _, base := range []string{cfg.root, currentWorkingDirectory()} {
		candidates = append(candidates, findUpwardCandidates(base, filepath.Join("Hardware", "Virtua", "mtkclient", "mtk.py"))...)
	}
	return uniqueStrings(candidates)
}

func findUpwardCandidates(start, rel string) []string {
	if start == "" {
		return nil
	}
	start, err := filepath.Abs(start)
	if err != nil {
		return nil
	}
	var candidates []string
	cur := start
	for {
		candidates = append(candidates, filepath.Join(cur, rel))
		parent := filepath.Dir(cur)
		if parent == cur {
			break
		}
		cur = parent
	}
	return candidates
}

func uniqueStrings(values []string) []string {
	seen := make(map[string]bool, len(values))
	var out []string
	for _, value := range values {
		if value == "" {
			continue
		}
		clean := filepath.Clean(value)
		if seen[clean] {
			continue
		}
		seen[clean] = true
		out = append(out, clean)
	}
	return out
}

func pythonAwareTool(path string) (commandSpec, error) {
	if strings.HasSuffix(strings.ToLower(path), ".py") {
		python, err := pythonForMTKScript(path)
		if err != nil {
			return commandSpec{}, err
		}
		return commandSpec{name: python, args: []string{path}}, nil
	}
	return commandSpec{name: path}, nil
}

func pythonForMTKScript(path string) (string, error) {
	root := filepath.Dir(path)
	for _, candidate := range []string{
		filepath.Join(root, ".venv", "bin", "python3"),
		filepath.Join(root, ".venv", "bin", "python"),
		filepath.Join(root, ".venv", "Scripts", "python.exe"),
	} {
		if fileExists(candidate) {
			return candidate, nil
		}
	}
	python, err := exec.LookPath(executableName("python3"))
	if err != nil {
		python, err = exec.LookPath(executableName("python"))
		if err != nil {
			return "", errors.New("python is needed to run mtk.py")
		}
	}
	return python, nil
}

func installMTKClientDeps(cfg config) error {
	mtkScript, ok := resolveBundledMTKClient(cfg)
	if !ok {
		if cfg.tool == "" {
			return errors.New("no bundled mtkclient checkout found; pass -mtkclient-root or rebuild the ARM package")
		}
		if !strings.HasSuffix(strings.ToLower(cfg.tool), ".py") {
			return errors.New("-install-mtk-deps requires a source checkout mtk.py, not an installed mtk executable")
		}
		mtkScript = cfg.tool
	}
	root := filepath.Dir(mtkScript)
	venv := filepath.Join(root, ".venv")
	python, err := exec.LookPath(executableName("python3"))
	if err != nil {
		python, err = exec.LookPath(executableName("python"))
		if err != nil {
			return errors.New("python is needed to create the mtkclient venv")
		}
	}
	fmt.Printf("Creating mtkclient venv at %s\n", venv)
	if err := runStreaming(python, "-m", "venv", venv); err != nil {
		return err
	}
	venvPython, err := pythonForMTKScript(mtkScript)
	if err != nil {
		return err
	}
	packages := []string{
		"pyusb",
		"pycryptodome",
		"pycryptodomex",
		"colorama",
		"pyserial",
	}
	args := append([]string{"-m", "pip", "install"}, packages...)
	fmt.Println("Installing mtkclient CLI Python dependencies.")
	if err := runStreaming(venvPython, args...); err != nil {
		return err
	}
	tool, err := pythonAwareTool(mtkScript)
	if err != nil {
		return err
	}
	if err := checkMTKClientReady(tool); err != nil {
		return err
	}
	fmt.Println("mtkclient dependency setup complete.")
	return nil
}

func checkMTKClientReady(tool commandSpec) error {
	if len(tool.args) == 0 || !strings.HasSuffix(strings.ToLower(tool.args[0]), ".py") {
		return nil
	}
	ctx, cancel := context.WithTimeout(context.Background(), 8*time.Second)
	defer cancel()
	args := append([]string{}, tool.args...)
	args = append(args, "--help")
	cmd := exec.CommandContext(ctx, tool.name, args...)
	out, err := cmd.CombinedOutput()
	if ctx.Err() == context.DeadlineExceeded {
		return errors.New("mtkclient dependency check timed out")
	}
	if err != nil {
		text := strings.TrimSpace(string(out))
		if text == "" {
			text = err.Error()
		}
		return fmt.Errorf("mtkclient cannot start: %s", mtkclientStartupSummary(text))
	}
	return nil
}

func firstLine(text string) string {
	for _, line := range strings.Split(text, "\n") {
		line = strings.TrimSpace(line)
		if line != "" {
			return line
		}
	}
	return text
}

func mtkclientStartupSummary(text string) string {
	lines := strings.Split(text, "\n")
	for i := len(lines) - 1; i >= 0; i-- {
		line := strings.TrimSpace(lines[i])
		if strings.Contains(line, "ModuleNotFoundError:") || strings.Contains(line, "ImportError:") {
			return line
		}
	}
	return firstLine(text)
}

func fastbootDevices(cfg config) ([]string, error) {
	tool, err := resolveFastboot(cfg)
	if err != nil {
		return nil, err
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	args := append([]string{}, tool.args...)
	args = append(args, "devices")
	cmd := exec.CommandContext(ctx, tool.name, args...)
	out, err := cmd.Output()
	if ctx.Err() == context.DeadlineExceeded {
		return nil, errors.New("fastboot devices timed out")
	}
	if err != nil {
		return nil, err
	}
	var devices []string
	for _, line := range strings.Split(string(out), "\n") {
		fields := strings.Fields(line)
		if len(fields) == 0 {
			continue
		}
		if cfg.serial != "" && fields[0] != cfg.serial {
			continue
		}
		devices = append(devices, fields[0])
	}
	return devices, nil
}

func maybeADBReboot(cfg config) error {
	mode := strings.ToLower(strings.TrimSpace(cfg.adbReboot))
	switch mode {
	case "", "none", "off", "false", "no":
		return nil
	}
	adb, err := exec.LookPath(executableName("adb"))
	if err != nil {
		fmt.Printf("adb: not found; continuing with manual MTK connection for reboot mode %q.\n", mode)
		return nil
	}
	devices, err := adbOnlineDevices(adb)
	if err != nil {
		fmt.Printf("adb: device check failed: %v; continuing with manual MTK connection.\n", err)
		return nil
	}
	if len(devices) == 0 {
		fmt.Printf("adb: no online device; continuing with manual MTK connection for reboot mode %q.\n", mode)
		return nil
	}
	fmt.Printf("adb: rebooting %s to %s mode.\n", devices[0], mode)
	ctx, cancel := context.WithTimeout(context.Background(), 12*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, adb, "reboot", mode)
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	cmd.Stdin = os.Stdin
	if err := cmd.Run(); err != nil {
		fmt.Printf("adb: reboot %s failed: %v; continuing with manual MTK connection.\n", mode, err)
		return nil
	}
	time.Sleep(2 * time.Second)
	return nil
}

func adbOnlineDevices(adb string) ([]string, error) {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, adb, "devices")
	out, err := cmd.Output()
	if ctx.Err() == context.DeadlineExceeded {
		return nil, errors.New("adb devices timed out")
	}
	if err != nil {
		return nil, err
	}
	var devices []string
	for _, line := range strings.Split(string(out), "\n") {
		fields := strings.Fields(line)
		if len(fields) >= 2 && fields[1] == "device" {
			devices = append(devices, fields[0])
		}
	}
	return devices, nil
}

func confirmFlash(cfg config, destination, image, partition string) error {
	fmt.Println()
	fmt.Printf("About to flash %s to %s partition %q.\n", image, destination, partition)
	fmt.Println("This can make the target device unbootable if the destination is wrong.")
	if cfg.yes {
		return nil
	}
	fmt.Print("Type exactly 'FLASH J36 ULTRA' to continue: ")
	line, err := bufio.NewReader(os.Stdin).ReadString('\n')
	if err != nil {
		return err
	}
	if strings.TrimSpace(line) != "FLASH J36 ULTRA" {
		return errors.New("confirmation did not match; leaving the device untouched")
	}
	return nil
}

func confirmRawFlash(cfg config, destination, image, offset, length string) error {
	fmt.Println()
	fmt.Printf("About to flash %s to %s raw offset %s, length %s.\n", image, destination, offset, length)
	fmt.Println("This can make the target device unbootable if the raw offset is wrong.")
	if cfg.yes {
		return nil
	}
	fmt.Print("Type exactly 'FLASH J36 ULTRA' to continue: ")
	line, err := bufio.NewReader(os.Stdin).ReadString('\n')
	if err != nil {
		return err
	}
	if strings.TrimSpace(line) != "FLASH J36 ULTRA" {
		return errors.New("confirmation did not match; leaving the device untouched")
	}
	return nil
}

func readLineFromStdin() (string, error) {
	return bufio.NewReader(os.Stdin).ReadString('\n')
}

func prepareRawFlashImage(image string, imageSize int64, rawLength uint64) (string, func(), error) {
	if rawLength > uint64(1<<63-1) {
		return "", nil, fmt.Errorf("-raw-length 0x%x is too large for this host", rawLength)
	}
	if rawLength < uint64(imageSize) {
		return "", nil, fmt.Errorf("-raw-length 0x%x is smaller than image size 0x%x", rawLength, imageSize)
	}
	if rawLength == uint64(imageSize) {
		return image, func() {}, nil
	}
	src, err := os.Open(image)
	if err != nil {
		return "", nil, err
	}
	defer src.Close()
	tmp, err := os.CreateTemp("", "mvii-raw-flash-*.img")
	if err != nil {
		return "", nil, err
	}
	cleanup := func() {
		os.Remove(tmp.Name())
	}
	if _, err := io.Copy(tmp, src); err != nil {
		tmp.Close()
		cleanup()
		return "", nil, err
	}
	if err := tmp.Truncate(int64(rawLength)); err != nil {
		tmp.Close()
		cleanup()
		return "", nil, err
	}
	if err := tmp.Close(); err != nil {
		cleanup()
		return "", nil, err
	}
	fmt.Printf("Padded raw flash image to %s for slot-sized write.\n", formatBytes(rawLength))
	return tmp.Name(), cleanup, nil
}

func parseMTKNumber(value, flagName string) (uint64, error) {
	if value == "" {
		return 0, fmt.Errorf("%s must not be empty", flagName)
	}
	base := 10
	digits := value
	if strings.HasPrefix(strings.ToLower(value), "0x") {
		base = 16
		digits = value[2:]
	}
	if digits == "" {
		return 0, fmt.Errorf("%s must be a decimal or 0x-prefixed number", flagName)
	}
	parsed, err := strconv.ParseUint(digits, base, 64)
	if err != nil {
		return 0, fmt.Errorf("%s must be a decimal or 0x-prefixed number: %w", flagName, err)
	}
	return parsed, nil
}

func legacyMTKWriteShim(tool commandSpec) (commandSpec, func(), error) {
	if len(tool.args) == 0 || !strings.HasSuffix(strings.ToLower(tool.args[0]), ".py") {
		return commandSpec{}, nil, errors.New("selected mtkclient is not a Python mtk.py checkout")
	}
	mtkScript := tool.args[0]
	tmp, err := os.CreateTemp("", "mvii-mtk-write-shim-*.py")
	if err != nil {
		return commandSpec{}, nil, err
	}
	cleanup := func() {
		os.Remove(tmp.Name())
	}
	if _, err := tmp.WriteString(legacyMTKWriteShimSource); err != nil {
		tmp.Close()
		cleanup()
		return commandSpec{}, nil, err
	}
	if err := tmp.Close(); err != nil {
		cleanup()
		return commandSpec{}, nil, err
	}
	args := []string{tmp.Name(), mtkScript}
	args = append(args, tool.args[1:]...)
	return commandSpec{name: tool.name, args: args}, cleanup, nil
}

const legacyMTKWriteShimSource = `#!/usr/bin/env python3
import os
import runpy
import sys

if len(sys.argv) < 2:
    raise SystemExit("usage: mvii-mtk-write-shim.py /path/to/mtk.py <mtk args...>")

mtk_script = os.path.abspath(sys.argv[1])
sys.argv = [mtk_script] + sys.argv[2:]
mtk_root = os.path.dirname(mtk_script)
if mtk_root not in sys.path:
    sys.path.insert(0, mtk_root)

from mtkclient.Library.DA.legacy.dalegacy_lib import DALegacy

_orig_sdmmc_write_data = DALegacy.sdmmc_write_data

def _mvii_sdmmc_write_data(self, *args, **kwargs):
    try:
        self.info("MVII legacy write preflight: using SDMMC_WRITE_DATA parttype from header")
    except Exception:
        pass
    return _orig_sdmmc_write_data(self, *args, **kwargs)

DALegacy.sdmmc_write_data = _mvii_sdmmc_write_data

runpy.run_path(mtk_script, run_name="__main__")
`

func runStreaming(name string, args ...string) error {
	cmd := exec.Command(name, args...)
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	cmd.Stdin = os.Stdin
	if err := cmd.Run(); err != nil {
		var buf bytes.Buffer
		fmt.Fprintf(&buf, "%s", name)
		for _, arg := range args {
			fmt.Fprintf(&buf, " %s", arg)
		}
		return fmt.Errorf("%s failed: %w", buf.String(), err)
	}
	return nil
}

func runMTKClient(tool commandSpec, partition string, args ...string) (string, error) {
	text, err := runMTKClientRaw(tool, args...)
	if err != nil {
		return text, err
	}
	if summary := mtkclientCommandFailure(text, partition); summary != "" {
		return text, errors.New(summary)
	}
	return text, nil
}

func runMTKClientRaw(tool commandSpec, args ...string) (string, error) {
	fullArgs := append([]string{}, tool.args...)
	fullArgs = append(fullArgs, args...)
	cmd := exec.Command(tool.name, fullArgs...)
	var output bytes.Buffer
	writer := io.MultiWriter(os.Stdout, &output)
	cmd.Stdout = writer
	cmd.Stderr = writer
	cmd.Stdin = os.Stdin
	cmd.Env = append(os.Environ(), "PYTHONUNBUFFERED=1", "PYTHONDONTWRITEBYTECODE=1")
	err := cmd.Run()
	text := output.String()
	if err != nil {
		return text, fmt.Errorf("%s failed: %w", commandString(tool.name, fullArgs), err)
	}
	return text, nil
}

func commandString(name string, args []string) string {
	var b strings.Builder
	b.WriteString(name)
	for _, arg := range args {
		b.WriteByte(' ')
		b.WriteString(arg)
	}
	return b.String()
}

func mtkclientCommandFailure(text, partition string) string {
	switch {
	case strings.Contains(text, "Couldn't detect partition"):
		available := parseAvailablePartitions(text)
		if available != "" {
			return fmt.Sprintf("mtkclient could not find partition %q. Available partitions: %s. Run ./flash -target=arm -detect-boot-partition -candidates %s, then retry with -partition <name> after verifying the boot partition.", partition, available, csvForShell(available))
		}
		return fmt.Sprintf("mtkclient could not find partition %q. Run ./flash -target=arm -detect-boot-partition, then retry with -partition <name> after verifying the boot partition.", partition)
	case strings.Contains(text, "Couldn't get gpt") || strings.Contains(text, "Error reading gpt"):
		return "mtkclient could not read the partition table; run ./flash -target=arm -print-partitions and check the device mode/cable."
	default:
		return ""
	}
}

func parseAvailablePartitions(text string) string {
	idx := strings.Index(text, "Available partitions:")
	if idx < 0 {
		return ""
	}
	var parts []string
	for _, line := range strings.Split(text[idx:], "\n")[1:] {
		line = strings.TrimSpace(line)
		if line == "" {
			break
		}
		if strings.Contains(line, " - ") {
			line = strings.TrimSpace(line[strings.LastIndex(line, " - ")+3:])
		}
		if strings.Contains(line, ":") || strings.Contains(line, "Progress") {
			break
		}
		fields := strings.Fields(line)
		if len(fields) == 1 {
			parts = append(parts, fields[0])
		}
	}
	return strings.Join(parts, ", ")
}

func csvForShell(value string) string {
	parts := splitCSV(value)
	return strings.Join(parts, ",")
}

func normalizeBlockDevice(device string) string {
	if runtime.GOOS == "darwin" && strings.HasPrefix(device, "/dev/disk") {
		return strings.Replace(device, "/dev/disk", "/dev/rdisk", 1)
	}
	return device
}

func normalizeDiskutilDevice(device string) string {
	if runtime.GOOS == "darwin" && strings.HasPrefix(device, "/dev/rdisk") {
		return strings.Replace(device, "/dev/rdisk", "/dev/disk", 1)
	}
	return device
}

func isSerialDevicePath(device string) bool {
	device = strings.TrimSpace(device)
	if device == "" {
		return false
	}
	base := filepath.Base(device)
	lower := strings.ToLower(device)
	baseLower := strings.ToLower(base)
	switch {
	case strings.HasPrefix(lower, "/dev/cu."), strings.HasPrefix(lower, "/dev/tty."):
		return true
	case strings.HasPrefix(baseLower, "ttyusb"), strings.HasPrefix(baseLower, "ttyacm"):
		return true
	case runtime.GOOS == "windows" && strings.HasPrefix(baseLower, "com") && allDigits(strings.TrimPrefix(baseLower, "com")):
		return true
	default:
		return false
	}
}

func executableName(name string) string {
	if runtime.GOOS == "windows" {
		return name + ".exe"
	}
	return name
}

func fileExists(path string) bool {
	st, err := os.Stat(path)
	return err == nil && !st.IsDir()
}

// isMTKScatterFile reports whether the path names a usable MTK scatter file
// (by name hint + successful parse, or by parse producing actionable entries).
// Used to let `./flash foo_scatter.txt` directly trigger scatter flashing.
func isMTKScatterFile(p string) bool {
	if p == "" {
		return false
	}
	base := strings.ToLower(filepath.Base(p))
	hasScatterName := strings.Contains(base, "scatter")
	entries, err := parseMTKScatterFile(p)
	if err != nil || len(entries) == 0 {
		return false
	}
	if hasScatterName {
		return true
	}
	// For files without "scatter" in the name, require real placement data to avoid
	// misclassifying random key:value text files.
	for _, e := range entries {
		if e.LinearStart != 0 || e.IsDownload {
			return true
		}
	}
	return false
}

func currentWorkingDirectory() string {
	wd, err := os.Getwd()
	if err != nil {
		return ""
	}
	return wd
}
