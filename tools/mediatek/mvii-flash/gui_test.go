//go:build gui

package main

import (
	"image/color"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"

	"fyne.io/fyne/v2/theme"
)

// A scatter shaped like the ones the ARM package build generates, minus the
// header block the parser does not need for this.
const guiTestScatter = `- partition_index: SYS1
  partition_name: UBOOT
  file_name: lk-release.bin
  is_download: true
  type: NORMAL_ROM
  linear_start_addr: 0x1d40000
  physical_start_addr: 0x1d40000
  partition_size: 0x200000
  region: EMMC_USER
  storage: HW_STORAGE_EMMC
  boundary_check: true
  is_reserved: false
  operation_type: UPDATE
  reserve: 0x00

- partition_index: SYS3
  partition_name: LOGO
  file_name: assets.bin
  is_download: true
  type: NORMAL_ROM
  linear_start_addr: 0x38c0000
  physical_start_addr: 0x38c0000
  partition_size: 0x800000
  region: EMMC_USER
  storage: HW_STORAGE_EMMC
  boundary_check: true
  is_reserved: false
  operation_type: UPDATE
  reserve: 0x00
`

// guiWriteTestPackage lays out a directory shaped like a built ARM package: the
// named scatters, and the images they name.
func guiWriteTestPackage(t *testing.T, scatters ...string) string {
	t.Helper()
	dir := t.TempDir()
	for _, scatter := range scatters {
		if err := os.WriteFile(filepath.Join(dir, scatter), []byte(guiTestScatter), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	for name, size := range map[string]int{"lk-release.bin": 0x200000, "assets.bin": 0x1000} {
		if err := os.WriteFile(filepath.Join(dir, name), make([]byte, size), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	return dir
}

// The count is what the confirmation says out loud, and it has to be the
// planner's answer rather than a re-read of the file -- so that the number in the
// dialog is the number of writes the press is about to perform.
func TestGUIScatterImageCountCountsEveryFlashableImage(t *testing.T) {
	dir := guiWriteTestPackage(t, guiScatterRelease)
	images, err := guiScatterImageCount(filepath.Join(dir, guiScatterRelease))
	if err != nil {
		t.Fatal(err)
	}
	if images != 2 {
		t.Fatalf("guiScatterImageCount = %d, want the 2 downloadable images the scatter names", images)
	}
}

// A half-built package is the likely failure here: the scatter exists because
// CMake writes it at configure time, but an image it names does not. The window
// must refuse before it opens the board rather than flash the subset.
func TestGUIScatterImageCountRefusesWhenNoImagesArePresent(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, guiScatterRelease)
	if err := os.WriteFile(path, []byte(guiTestScatter), 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := guiScatterImageCount(path); err == nil {
		t.Fatal("guiScatterImageCount accepted a scatter whose images are all missing")
	}
}

// Nothing in the window asks which build is meant, so the opening scatter is
// resolved rather than chosen: the release one where both are present, and the
// hypervisor one when a hypervisor-only build is all there is. Only the working
// directory is exercisable in a test (the executable is the test binary), which
// is the same lookup with a different starting point.
func TestGUIResolveScatterPrefersTheReleaseBuild(t *testing.T) {
	both := guiWriteTestPackage(t, guiScatterRelease, guiScatterHypervisor)
	hypervisorOnly := guiWriteTestPackage(t, guiScatterHypervisor)

	previous, err := os.Getwd()
	if err != nil {
		t.Fatal(err)
	}
	defer os.Chdir(previous)

	for dir, want := range map[string]string{both: guiScatterRelease, hypervisorOnly: guiScatterHypervisor} {
		if err := os.Chdir(dir); err != nil {
			t.Fatal(err)
		}
		if got := filepath.Base(guiResolveScatter()); got != want {
			t.Fatalf("%s resolved %q, want %q", dir, got, want)
		}
	}
}

// Progress during a flash is printed without newlines and rewritten in place;
// the log pane has to show that as it happens, not after it ends. This is the
// capture path's whole reason for existing, so it is worth pinning.
func TestGUILogRewritesPartialLinesAndHonoursCarriageReturns(t *testing.T) {
	log := &guiLog{}
	capture, err := guiCaptureOutput(log)
	if err != nil {
		t.Fatal(err)
	}
	// os.Stdout is a global; restore it before any t.Fatalf so a failure does
	// not take the test binary's own output with it.
	os.Stdout.WriteString("Waiting for MTK serial device")
	os.Stdout.WriteString(".")
	os.Stdout.WriteString("..\n")
	os.Stdout.WriteString("chunk 1/4\rchunk 4/4\n")
	os.Stdout.WriteString("trailing without newline")
	capture.close()

	lines := strings.Split(log.text(), "\n")
	want := []string{"Waiting for MTK serial device...", "chunk 4/4", "trailing without newline"}
	if len(lines) != len(want) {
		t.Fatalf("captured %d lines, want %d: %q", len(lines), len(want), lines)
	}
	for i := range want {
		if lines[i] != want[i] {
			t.Fatalf("line %d = %q, want %q", i, lines[i], want[i])
		}
	}
}

// The line the log prints above a run claims the window is a front for ./flash and
// nothing more. If it ever named a flag the console tool does not register, that
// claim would be a lie and the line would not be pasteable, so every flag it can
// emit is checked against the source that declares them.
func TestGUIFlashCommandLineNamesOnlyFlagsTheConsoleToolRegisters(t *testing.T) {
	source, err := os.ReadFile("main.go")
	if err != nil {
		t.Fatal(err)
	}
	cfg, err := guiFlashConfig(filepath.Join(t.TempDir(), guiScatterRelease), "/dev/ttyACM0")
	if err != nil {
		t.Fatal(err)
	}
	line := guiFlashCommandLine(cfg, cfg.mtkFlashScatter)
	flags := 0
	for _, field := range strings.Fields(line) {
		if !strings.HasPrefix(field, "-") {
			continue
		}
		flags++
		declaration := strconv.Quote(strings.TrimPrefix(field, "-"))
		if !strings.Contains(string(source), declaration) {
			t.Fatalf("the command line names %s, which parseFlags does not register:\n%s", field, line)
		}
	}
	if flags == 0 {
		t.Fatalf("the command line names no flags at all:\n%s", line)
	}
}

// That line has to be pasteable, which means the scatter path has to survive a
// shell and the flags have to be the ones the press will actually use -- which is
// why it is rendered from the config rather than from a format string.
func TestGUIFlashCommandLineRendersThePastableEquivalent(t *testing.T) {
	dir := t.TempDir()
	scatter := filepath.Join(dir, "a package", guiScatterRelease)
	cfg, err := guiFlashConfig(scatter, "/dev/ttyACM0")
	if err != nil {
		t.Fatal(err)
	}
	line := guiFlashCommandLine(cfg, scatter)
	for _, want := range []string{
		"./flash -mtk-flash-scatter ",
		"'" + scatter + "'", // the space in the directory name has to be quoted
		"-device /dev/ttyACM0",
		"-root '" + filepath.Dir(scatter) + "'",
		"-yes",
	} {
		if !strings.Contains(line, want) {
			t.Fatalf("command line is missing %q:\n%s", want, line)
		}
	}
}

// The panel is meant to look like the metal the board is sitting on: dim,
// neutral, and in the middle of the range at both ends -- no black page and no
// white one, in either theme variant. That is a look, so it cannot be tested, but
// the three properties it rests on are numbers and they can be.
func TestGUIThemeIsMidGreyMetalInBothVariants(t *testing.T) {
	surfaces := map[string]color.NRGBA{
		"page":        guiMetalPalette.page,
		"panel":       guiMetalPalette.panel,
		"input":       guiMetalPalette.input,
		"hover":       guiMetalPalette.hover,
		"pressed":     guiMetalPalette.pressed,
		"border":      guiMetalPalette.border,
		"separator":   guiMetalPalette.separator,
		"disabledBox": guiMetalPalette.disabledBox,
		"accent":      guiMetalPalette.accent,
	}
	for name, c := range surfaces {
		low, high := min(c.R, min(c.G, c.B)), max(c.R, max(c.G, c.B))
		// Midpoint: nothing is allowed near either end of the range.
		if low < 0x30 || high > 0xc0 {
			t.Errorf("%s is %#02x%02x%02x, which leaves the mid range; the panel must not go black or white",
				name, c.R, c.G, c.B)
		}
		// Neutral: a grey with a tint in it stops reading as metal.
		if high-low > 0x10 {
			t.Errorf("%s is %#02x%02x%02x, too saturated to be a grey", name, c.R, c.G, c.B)
		}
	}

	// The FLASH key is accented by being the brightest surface in the window
	// rather than by being a different hue, so it has to stay brighter than the
	// controls around it.
	if guiMetalPalette.accent.G <= guiMetalPalette.input.G {
		t.Error("the accent is no brighter than an ordinary control; the FLASH key would disappear into the panel")
	}

	// A light-mode host must not repaint any of this.
	metal := guiTheme{}
	if metal.Color(theme.ColorNameBackground, theme.VariantLight) !=
		metal.Color(theme.ColorNameBackground, theme.VariantDark) {
		t.Error("the page colour follows the host's light/dark setting; the metal palette is meant to be the only one")
	}
}

// The window is a scatter picker and a FLASH key, and that is the whole of it: no
// partition read-back, no boot status, no probe, no reset-to-BROM -- the operator
// puts the board in download mode themselves. It is worth pinning because the
// pressure is all one way: every one of those is a two-line addition to a file
// that is already holding the transport open.
func TestGUIExposesTheFlashAndNothingElse(t *testing.T) {
	source, err := os.ReadFile("gui.go")
	if err != nil {
		t.Fatal(err)
	}
	text := string(source)
	for _, absent := range []string{
		"readPartitionsMTKFeed",
		"readBootStatusMTKFeed",
		"resetMTKTargetToBROM",
		"runMTKFeedCommand",
		"widget.NewCheck",
	} {
		if strings.Contains(text, absent) {
			t.Fatalf("the window has grown a %s; it is meant to front -mtk-flash-scatter only", absent)
		}
	}
	if got := strings.Count(text, "widget.NewButton"); got != 2 {
		t.Fatalf("the window builds %d buttons, want exactly 2 (FLASH and Browse)", got)
	}
	if !strings.Contains(text, `widget.NewButton("FLASH"`) {
		t.Fatal(`the flash key is not labelled "FLASH"`)
	}
}

// The window has no stdin to answer confirmMTKScatterFlash's prompt, and the
// scatter it flashes always rewrites the bootloader slot.
func TestGUIFlashConfigConfirmsAndResets(t *testing.T) {
	dir := guiWriteTestPackage(t, guiScatterRelease)
	scatter := filepath.Join(dir, guiScatterRelease)
	cfg, err := guiFlashConfig(scatter, "")
	if err != nil {
		t.Fatal(err)
	}
	if !cfg.yes {
		t.Fatal("cfg.yes is false; the flash would block on a stdin prompt nobody can answer")
	}
	if !cfg.mtkFeedReboot || !cfg.mtkFeedRebootSet {
		t.Fatal("a scatter flash must reset the board into what it just wrote")
	}
	if cfg.root != dir {
		t.Fatalf("cfg.root = %q, want the scatter's own directory %q", cfg.root, dir)
	}
	if cfg.mtkFeedPayload != "auto" {
		t.Fatalf("cfg.mtkFeedPayload = %q, want auto", cfg.mtkFeedPayload)
	}
}
