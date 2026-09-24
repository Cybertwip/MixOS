//go:build gui

// The MVII flashing window.
//
// This is the same program as ./flash, built from the same package with
// `-tags gui`: the scatter parser, the MTK feed payload loader and the eMMC
// write loop below are literally the ones the console tool uses, not a second
// implementation of them. That was the whole reason to put the window in this
// package rather than in a tool of its own -- a flasher that agrees with the
// command line only most of the time is worse than no flasher at all.
//
// It deliberately ignores os.Args, and it offers exactly one thing to choose: the
// scatter file. Everything else a flash needs -- which payload, which offsets,
// which transport -- is either in the scatter or discovered, so there is nothing
// left for the window to ask about and it does not ask. Putting the board into
// BROM/preloader download mode is the operator's business and happens before any
// of this is any use, which is why there is no button for it either.
//
// The flashing code reports through fmt.Printf, so this file borrows os.Stdout
// for the duration of a run and turns what comes out of it into the log pane.
// That is the one piece of trickery here, and it is what keeps the shared code
// free of a logging abstraction it would otherwise need only for this window.
package main

import (
	"bytes"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"time"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/app"
	"fyne.io/fyne/v2/container"
	"fyne.io/fyne/v2/dialog"
	"fyne.io/fyne/v2/storage"
	"fyne.io/fyne/v2/widget"
)

const guiBuild = true

const (
	// The two scatter files the ARM package build emits into boot/. The
	// hypervisor one names the debug LK, the boot image and the assets; the
	// other names the release LK and the assets.
	guiScatterRelease    = "mvii.txt"
	guiScatterHypervisor = "mvii.hypervisor.txt"

	guiLogMaxLines      = 4000
	guiLogRefreshPeriod = 100 * time.Millisecond

	// How long to wait for the board's VCOM node to appear on platforms that
	// reach it through a tty. A flash is normally started with the board
	// already in BROM, but "plug it in now" is a reasonable thing to allow.
	guiDeviceWait = 90 * time.Second
)

/* ------------------------------------------------------------------------- */
/* The log                                                                   */
/* ------------------------------------------------------------------------- */

// guiLog is the model behind the log pane: a bounded list of lines, with the
// last one possibly unterminated.
//
// The unterminated case is not a detail. Two of the messages that matter most
// during a flash -- "Waiting for MTK serial device" and the handshake's row of
// dots -- are printed without a newline and extended in place, and a log that
// only ever appended on '\n' would show neither until the wait was already
// over. So a partial line is stored like any other and rewritten as it grows.
type guiLog struct {
	mu      sync.Mutex
	lines   []string
	partial bool
	dirty   bool
}

// set writes one line. When the tail line is still unterminated it is replaced
// rather than followed, which is what makes in-place progress work.
func (l *guiLog) set(text string, terminated bool) {
	l.mu.Lock()
	defer l.mu.Unlock()
	if l.partial && len(l.lines) > 0 {
		l.lines[len(l.lines)-1] = text
	} else {
		l.lines = append(l.lines, text)
	}
	l.partial = !terminated
	if len(l.lines) > guiLogMaxLines {
		l.lines = append([]string(nil), l.lines[len(l.lines)-guiLogMaxLines:]...)
	}
	l.dirty = true
}

// terminate closes off a partial tail line so the next write appends instead of
// overwriting it.
func (l *guiLog) terminate() {
	l.mu.Lock()
	l.partial = false
	l.mu.Unlock()
}

// printf is the window's own voice, as opposed to captured output.
func (l *guiLog) printf(format string, args ...any) {
	l.terminate()
	text := strings.TrimRight(fmt.Sprintf(format, args...), "\n")
	for _, line := range strings.Split(text, "\n") {
		l.set(line, true)
	}
}

func (l *guiLog) count() int {
	l.mu.Lock()
	defer l.mu.Unlock()
	return len(l.lines)
}

func (l *guiLog) line(index int) string {
	l.mu.Lock()
	defer l.mu.Unlock()
	if index < 0 || index >= len(l.lines) {
		return ""
	}
	return l.lines[index]
}

// takeDirty reports whether anything changed since the last call, and clears
// the flag. The pane repaints on a timer rather than per line: a chunked eMMC
// write prints faster than a UI can usefully redraw.
func (l *guiLog) takeDirty() bool {
	l.mu.Lock()
	defer l.mu.Unlock()
	dirty := l.dirty
	l.dirty = false
	return dirty
}

func (l *guiLog) text() string {
	l.mu.Lock()
	defer l.mu.Unlock()
	return strings.Join(l.lines, "\n")
}

/* ------------------------------------------------------------------------- */
/* Borrowing stdout                                                          */
/* ------------------------------------------------------------------------- */

// guiCapture points os.Stdout and os.Stderr at a pipe and feeds every line that
// comes out of it into a guiLog.
//
// This works because nothing in this package holds a buffered writer over
// os.Stdout: every report goes through fmt.Printf and friends, which read the
// variable at call time. Only one flash runs at a time, so swapping a process
// global is contained.
type guiCapture struct {
	log     *guiLog
	reader  *os.File
	writer  *os.File
	prevOut *os.File
	prevErr *os.File
	done    chan struct{}
}

func guiCaptureOutput(log *guiLog) (*guiCapture, error) {
	reader, writer, err := os.Pipe()
	if err != nil {
		return nil, err
	}
	capture := &guiCapture{
		log:     log,
		reader:  reader,
		writer:  writer,
		prevOut: os.Stdout,
		prevErr: os.Stderr,
		done:    make(chan struct{}),
	}
	os.Stdout = writer
	os.Stderr = writer
	go capture.pump()
	return capture, nil
}

func (c *guiCapture) pump() {
	defer close(c.done)
	buf := make([]byte, 4096)
	var pending []byte
	for {
		n, err := c.reader.Read(buf)
		if n > 0 {
			pending = append(pending, buf[:n]...)
			for {
				index := bytes.IndexByte(pending, '\n')
				if index < 0 {
					break
				}
				c.log.set(guiVisibleLine(pending[:index]), true)
				pending = pending[index+1:]
			}
			// A carriage return means everything before it was overwritten in
			// place, which is how the serial waits report progress. Drop it so
			// the pane shows what the terminal would have shown.
			if index := bytes.LastIndexByte(pending, '\r'); index >= 0 {
				pending = append([]byte(nil), pending[index+1:]...)
			}
			if len(pending) > 0 {
				c.log.set(string(pending), false)
			}
		}
		if err != nil {
			if len(pending) > 0 {
				c.log.set(guiVisibleLine(pending), true)
			}
			return
		}
	}
}

func (c *guiCapture) close() {
	os.Stdout = c.prevOut
	os.Stderr = c.prevErr
	_ = c.writer.Close()
	<-c.done
	_ = c.reader.Close()
	c.log.terminate()
}

func guiVisibleLine(line []byte) string {
	if index := bytes.LastIndexByte(line, '\r'); index >= 0 {
		line = line[index+1:]
	}
	return string(line)
}

/* ------------------------------------------------------------------------- */
/* Finding the scatter and the board                                         */
/* ------------------------------------------------------------------------- */

// guiScatterSearchDirs lists where a generated scatter is expected to be. The
// binary is built into the package's boot/ directory beside the images, so its
// own directory comes first; boot/ under it covers a copy left at the package
// root, and the working directory covers running it from a checkout.
func guiScatterSearchDirs() []string {
	var dirs []string
	seen := map[string]bool{}
	add := func(dir string) {
		if dir == "" || seen[dir] {
			return
		}
		seen[dir] = true
		dirs = append(dirs, dir)
	}
	if exe, err := os.Executable(); err == nil {
		if resolved, err := filepath.EvalSymlinks(exe); err == nil {
			exe = resolved
		}
		dir := filepath.Dir(exe)
		add(dir)
		add(filepath.Join(dir, "boot"))
	}
	if wd, err := os.Getwd(); err == nil {
		add(wd)
		add(filepath.Join(wd, "boot"))
	}
	return dirs
}

// guiResolveScatter finds a generated scatter to open with, or "" if no built
// package is beside this binary. The release scatter wins where both are present,
// because that is the one an ARM package is built to flash; the hypervisor one is
// picked up only when it is the only scatter in the directory, which is what a
// hypervisor-only build leaves behind. Either way the entry above is editable and
// Browse overrides it, so this is a starting point rather than a decision.
func guiResolveScatter() string {
	for _, dir := range guiScatterSearchDirs() {
		for _, name := range []string{guiScatterRelease, guiScatterHypervisor} {
			candidate := filepath.Join(dir, name)
			if fileExists(candidate) {
				return candidate
			}
		}
	}
	return ""
}

// guiScatterImageCount answers whether a scatter can be flashed at all, and with
// how many images, using the same parser and the same planner the console tool
// runs. A package this accepts is one ./flash accepts.
//
// The failure it exists to catch is a half-built package: the scatter is there
// because CMake writes it at configure time, but an image it names is not. That
// has to be refused before the board is opened, not two images into a write. What
// gets written is not listed here -- the flash path itself prints the plan as it
// goes, into the log, which is where detail belongs.
func guiScatterImageCount(path string) (int, error) {
	entries, err := parseMTKScatterFile(path)
	if err != nil {
		return 0, err
	}
	items, _, err := planMTKScatterFlash(path, entries)
	if err != nil {
		return 0, err
	}
	if len(items) == 0 {
		return 0, fmt.Errorf("%s has no downloadable EMMC_USER images to flash", path)
	}
	return len(items), nil
}

// guiFlashDevice names the transport for this run.
//
// On macOS the answer is "no name at all": the kernel CDC-ACM driver
// destabilises the MT6592 BROM, so this tool talks to it over libusb, which
// finds the board by vendor ID and never looks at a device path. Elsewhere the
// tty path is used and a node has to exist, so this waits for one to show up
// instead of failing on a board that is a second away from being plugged in.
func guiFlashDevice(log *guiLog) (string, error) {
	switch runtime.GOOS {
	case "darwin":
		return "", nil
	case "linux":
		deadline := time.Now().Add(guiDeviceWait)
		announced := false
		for {
			if device := guiFirstSerialCandidate(); device != "" {
				return device, nil
			}
			if !announced {
				log.printf("Waiting for the board's USB serial node (%s)...", strings.Join(guiSerialGlobs(), ", "))
				announced = true
			}
			if time.Now().After(deadline) {
				return "", fmt.Errorf("no MediaTek USB serial device appeared under %s within %s; "+
					"power the board off and hold the download combo while connecting it",
					strings.Join(guiSerialGlobs(), " or "), guiDeviceWait)
			}
			time.Sleep(250 * time.Millisecond)
		}
	default:
		// serial_unsupported.go covers everything that is not darwin or linux,
		// so there is no transport to open here. Saying so up front beats
		// letting the write path discover it a hundred lines in.
		return "", fmt.Errorf("flashing is implemented on macOS and Linux; %s has no MediaTek serial transport in this build yet",
			runtime.GOOS)
	}
}

// guiSerialGlobs lists where this host's VCOM node shows up. Only the Linux pair
// is ever consulted -- on macOS guiFlashDevice answers over libusb and never
// looks at a path -- but the macOS pair belongs with its platform rather than in
// the caller, so that a build which does use the tty path there does not have to
// go looking for where the names went.
func guiSerialGlobs() []string {
	if runtime.GOOS == "darwin" {
		return []string{"/dev/cu.usbmodem*", "/dev/tty.usbmodem*"}
	}
	return []string{"/dev/ttyACM*", "/dev/ttyUSB*"}
}

// guiTransportLine is the window's subtitle: the board, and how this build
// reaches it. It is the only line in the window that differs per platform, so it
// is worth saying out loud rather than leaving the operator to infer it from a
// failure.
func guiTransportLine() string {
	switch runtime.GOOS {
	case "darwin":
		return "J36 Ultra · MT6592 · libusb, found by vendor ID"
	case "linux":
		return "J36 Ultra · MT6592 · USB serial on " + strings.Join(guiSerialGlobs(), " or ")
	default:
		return "J36 Ultra · MT6592 · no serial transport on " + runtime.GOOS + " in this build"
	}
}

func guiFirstSerialCandidate() string {
	for _, pattern := range guiSerialGlobs() {
		matches, err := filepath.Glob(pattern)
		if err != nil {
			continue
		}
		for _, match := range matches {
			return match
		}
	}
	return ""
}

// guiBaseConfig is the config every operation in this window starts from. The
// defaults mirror parseFlags so the window and the command line behave the same,
// with one deliberate difference: yes is set, because the plan was already shown
// and confirmed here and there is no stdin behind this process to answer a
// second prompt with.
func guiBaseConfig(root string, device string) config {
	return config{
		root:                 root,
		target:               "arm",
		backend:              "auto",
		partition:            "boot",
		adbReboot:            "edl",
		prepare:              true,
		mtkFeedLiveParts:     true,
		mtkFeedFollowTimeout: "2m",
		device:               device,
		// "auto" resolves boot/MVIIFlash.bin under root.
		mtkFeedPayload: "auto",
		yes:            true,
	}
}

// guiFlashConfig builds the config a scatter flash needs.
func guiFlashConfig(scatter string, device string) (config, error) {
	root, err := filepath.Abs(filepath.Dir(scatter))
	if err != nil {
		return config{}, err
	}
	cfg := guiBaseConfig(root, device)
	cfg.mtkFlashScatter = scatter
	// A scatter always rewrites the bootloader slot, so the board has to be reset
	// to run what was just written. The console tool defaults this on for single
	// LK/boot-image writes for the same reason; a scatter is that case by
	// definition, so it is set rather than inferred.
	cfg.mtkFeedReboot = true
	cfg.mtkFeedRebootSet = true
	return cfg, nil
}

// guiFlashCommandLine renders the console invocation the FLASH key is about to be
// equivalent to, built from the config that press will actually use rather than
// from a format string. It goes into the log, once, immediately above the output
// of the run it describes -- so a log pasted out of this window says what would
// reproduce it from a terminal, and cannot drift from what the key did.
func guiFlashCommandLine(cfg config, scatter string) string {
	args := []string{"./flash", "-mtk-flash-scatter", guiShellQuote(scatter)}
	if cfg.device != "" {
		args = append(args, "-device", guiShellQuote(cfg.device))
	}
	if cfg.root != "" {
		args = append(args, "-root", guiShellQuote(cfg.root))
	}
	if cfg.yes {
		args = append(args, "-yes")
	}
	return strings.Join(args, " ")
}

// guiShellQuote quotes a path the way a shell needs it, because that log line is
// meant to be pasted and package paths have spaces in them more often than not on
// macOS.
func guiShellQuote(text string) string {
	if text == "" {
		return "''"
	}
	if !strings.ContainsAny(text, " \t\n\"'\\$`&|;<>()*?[]{}#~!") {
		return text
	}
	return "'" + strings.ReplaceAll(text, "'", `'\''`) + "'"
}

/* ------------------------------------------------------------------------- */
/* The window                                                                */
/* ------------------------------------------------------------------------- */

func runFlashGUI() int {
	// Declared, not assumed: every UI touch from a goroutine in this file goes
	// through fyne.Do (the log repaint timer and the flash worker's completion
	// are the only two), so the toolkit's "not migrated" warning would be three
	// lines of noise on every launch. Without a packaged FyneApp.toml this is
	// the only way to say so.
	app.SetMetadata(fyne.AppMetadata{
		ID:         "com.minos.mvii.flash",
		Name:       "MVII Flash",
		Version:    "1.0.0",
		Build:      1,
		Migrations: map[string]bool{"fyneDo": true},
	})

	application := app.NewWithID("com.minos.mvii.flash")
	// The window's own theme. gui_theme.go says why it is not the stock one.
	application.Settings().SetTheme(guiTheme{})

	window := application.NewWindow("MVII Flash — J36 Ultra")

	log := &guiLog{}
	busy := false

	logList := widget.NewList(
		log.count,
		func() fyne.CanvasObject {
			label := widget.NewLabel("")
			label.TextStyle = fyne.TextStyle{Monospace: true}
			return label
		},
		func(id widget.ListItemID, object fyne.CanvasObject) {
			object.(*widget.Label).SetText(log.line(int(id)))
		},
	)

	status := newGUIStatusLine()

	scatterEntry := widget.NewEntry()
	scatterEntry.SetPlaceHolder("the scatter from a built ARM package — " + guiScatterRelease)

	var (
		browseButton *widget.Button
		flashButton  *widget.Button
	)

	// Check whatever path is in the entry and say whether it can be flashed.
	// Running on every keystroke is what makes an unbuildable or half-built
	// package visible before the key is pressed rather than during the write.
	//
	// resolve is the opening move: no scatter has been named yet, so one is looked
	// up beside this binary and typed into the entry for the operator.
	//
	// reported is what keeps this quiet. A path gets one line in the log, however
	// many times it is checked -- the opening lookup checks it twice by itself,
	// since typing it into the entry fires the change handler that lands back in
	// here.
	reported := ""
	refresh := func(resolve bool) {
		if resolve {
			if path := guiResolveScatter(); path == "" {
				status.set(guiLevelWarn, "No package found")
				log.printf("No %s beside this program. Build mvii-armv7-pc, or pick a scatter with Browse.",
					guiScatterRelease)
			} else {
				scatterEntry.SetText(path)
			}
		}
		path := strings.TrimSpace(scatterEntry.Text)
		if path == "" {
			status.set(guiLevelIdle, "No package")
			reported = ""
			return
		}
		images, err := guiScatterImageCount(path)
		announce := reported != path
		reported = path
		if err != nil {
			status.set(guiLevelBad, "Not flashable")
			// A path being typed by hand names nothing until the last character,
			// so only a scatter that is actually there gets a line. Otherwise
			// every keystroke would print a "no such file" and bury the run that
			// follows it.
			if announce && fileExists(path) {
				log.printf("%v", err)
			}
			return
		}
		status.set(guiLevelIdle, "Ready")
		if announce {
			log.printf("%s — %d images to write", path, images)
		}
	}

	scatterEntry.OnChanged = func(string) {
		if !busy {
			refresh(false)
		}
	}

	setBusy := func(value bool) {
		busy = value
		if value {
			flashButton.Disable()
			browseButton.Disable()
			scatterEntry.Disable()
			return
		}
		flashButton.Enable()
		browseButton.Enable()
		scatterEntry.Enable()
	}

	// The one operation. Borrow stdout, wait for the transport, build the config,
	// then call the same function run() dispatches to for -mtk-flash-scatter.
	// There is no second code path in here and no operation of the window's own:
	// this is that flag with a key on it.
	start := func(scatter string) {
		setBusy(true)
		status.set(guiLevelBusy, "Flashing…")
		log.printf("")

		go func() {
			err := func() (err error) {
				defer func() {
					if recovered := recover(); recovered != nil {
						err = fmt.Errorf("the flash path panicked: %v", recovered)
					}
				}()
				capture, captureErr := guiCaptureOutput(log)
				if captureErr != nil {
					return captureErr
				}
				defer capture.close()

				// Resolved here rather than on the UI goroutine because on a tty
				// platform this waits for the node to appear.
				device, deviceErr := guiFlashDevice(log)
				if deviceErr != nil {
					return deviceErr
				}
				cfg, cfgErr := guiFlashConfig(scatter, device)
				if cfgErr != nil {
					return cfgErr
				}
				log.printf("$ %s", guiFlashCommandLine(cfg, scatter))
				return flashScatterMTKFeed(cfg, scatter)
			}()

			fyne.Do(func() {
				setBusy(false)
				if err != nil {
					log.printf("flash failed: %v", err)
					status.set(guiLevelBad, "Failed")
					return
				}
				log.printf("done — the board was reset into what was just written")
				status.set(guiLevelGood, "Flashed")
			})
		}()
	}

	// cfg.yes switches off the console tool's own confirmMTKScatterFlash prompt --
	// this window has no stdin to answer it with -- so this dialog is that prompt.
	// It is the only thing in here that interrupts, and it is here because the next
	// thing that happens is an irreversible write to the bootloader slot.
	armFlash := func() {
		path := strings.TrimSpace(scatterEntry.Text)
		if path == "" {
			status.set(guiLevelWarn, "Choose a scatter first")
			return
		}
		absolute, err := filepath.Abs(path)
		if err != nil {
			status.set(guiLevelBad, "%v", err)
			log.printf("%v", err)
			return
		}
		images, err := guiScatterImageCount(absolute)
		if err != nil {
			status.set(guiLevelBad, "Not flashable")
			log.printf("%v", err)
			return
		}
		dialog.ShowConfirm("Flash "+filepath.Base(absolute)+"?",
			fmt.Sprintf("%d images go to eMMC on the attached J36 Ultra, and it is reset into them "+
				"afterwards. If this scatter does not describe the board in your hand, it will not boot.\n\n"+
				"The board has to be in BROM/preloader download mode already.", images),
			func(confirmed bool) {
				if confirmed {
					start(absolute)
				}
			}, window)
	}

	flashButton = widget.NewButton("FLASH", armFlash)
	flashButton.Importance = widget.HighImportance

	browseButton = widget.NewButton("Browse…", func() {
		open := dialog.NewFileOpen(func(reader fyne.URIReadCloser, err error) {
			if err != nil || reader == nil {
				return
			}
			defer reader.Close()
			scatterEntry.SetText(reader.URI().Path())
		}, window)
		open.SetFilter(storage.NewExtensionFileFilter([]string{".txt"}))
		for _, dir := range guiScatterSearchDirs() {
			if uri, err := storage.ListerForURI(storage.NewFileURI(dir)); err == nil {
				open.SetLocation(uri)
				break
			}
		}
		open.Resize(fyne.NewSize(760, 520))
		open.Show()
	})

	// A flash log is the first thing anyone asks for when a board does not come
	// back, so it stays copyable -- as the platform's own copy shortcut rather than
	// as a button, because the only button in this window is the one that flashes.
	window.Canvas().AddShortcut(&fyne.ShortcutCopy{}, func(fyne.Shortcut) {
		window.Clipboard().SetContent(log.text())
	})

	// Two controls and a log, in the order they are used: which package, the key
	// that writes it, and what came back. There is nothing else -- no partition
	// list, no read-backs, no reset-to-BROM button. Getting the board into BROM
	// mode is the operator's job and happens before this window is any use, and
	// everything the tool has to say once the key is down, it says in the log.
	panel := container.NewVBox(
		container.NewBorder(nil, nil,
			container.NewVBox(guiHeading("MVII Flash"), guiCaption(guiTransportLine())),
			container.NewCenter(status.widget)),
		widget.NewSeparator(),
		container.NewBorder(nil, nil, nil, browseButton, scatterEntry),
		container.NewStack(guiKeyHeight(46), flashButton),
	)

	window.SetContent(container.NewPadded(container.NewBorder(
		container.NewVBox(panel, widget.NewSeparator()),
		nil, nil, nil,
		newGUISurface(logList),
	)))

	// The pane repaints on a timer, so a long write does not turn every printed
	// line into a layout pass.
	stopRefresh := make(chan struct{})
	go func() {
		ticker := time.NewTicker(guiLogRefreshPeriod)
		defer ticker.Stop()
		for {
			select {
			case <-stopRefresh:
				return
			case <-ticker.C:
				if !log.takeDirty() {
					continue
				}
				fyne.Do(func() {
					logList.Refresh()
					logList.ScrollToBottom()
				})
			}
		}
	}()

	// Closing the window mid-write kills the process with the bootloader slot
	// half-written, which is the one way to brick the board from here.
	window.SetCloseIntercept(func() {
		if !busy {
			close(stopRefresh)
			window.Close()
			return
		}
		dialog.ShowConfirm("Quit while flashing?",
			"A write is in progress. Quitting now can leave the bootloader slot half-written and the board unbootable.",
			func(confirmed bool) {
				if confirmed {
					close(stopRefresh)
					window.Close()
				}
			}, window)
	})

	// The log opens with the same two facts the console tool prints first, so a
	// pasted log from the window and one from ./flash start the same way.
	log.printf("MVII Flash — J36 Ultra (MT6592) — %s", guiTransportLine())
	log.printf("FLASH runs ./flash -mtk-flash-scatter; put the board in BROM/preloader download mode first.")
	refresh(true)

	window.Resize(fyne.NewSize(760, 620))
	window.ShowAndRun()
	return 0
}
