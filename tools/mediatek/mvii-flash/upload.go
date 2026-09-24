package main

import (
	"fmt"
	"path/filepath"
	"sort"
	"strings"
)

/*
 * ── WHAT THIS RUN WRITES ──
 *
 * One flag, `-upload`, names the slice of the device this invocation is allowed
 * to touch. It replaces `-assets`, which was a boolean and therefore could only
 * ever express one of the five things you actually want to say.
 *
 *   full     lk.bin + MVIIS1.bin + boot.img + assets.bin   everything, the default
 *   lk       lk.bin                                        the bootloader alone
 *   system   MVIIS1.bin + boot.img                         the OS alone
 *   assets   assets.bin                                    the pictures alone
 *   release  lk-release.bin                                the SD-handoff bootloader
 *
 * The point is iteration time. A full write is about 7.7 MiB over a link that
 * manages roughly a megabyte a second; `-upload system' is 6.4 of that and
 * `-upload lk' is 2.1, and when the thing you changed is one driver in LK there
 * is no reason to re-stream the OS and the artwork to find out whether it
 * worked. `-upload assets' was already worth its own flag for exactly this
 * reason and this is that argument applied to the rest of the chain.
 *
 * The default is `full' and deliberately so: a bare ./flash should leave the
 * device in a state where every piece came from the same build. Partial writes
 * are a thing you ask for, not a thing you get.
 */

const (
	uploadFull    = "full"
	uploadLK      = "lk"
	uploadSystem  = "system"
	uploadAssets  = "assets"
	uploadRelease = "release"
)

var uploadTargetHelp = map[string]string{
	uploadFull:    "lk.bin, MVIIS1.bin, boot.img and assets.bin -- the whole chain",
	uploadLK:      "lk.bin only -- the MVII bootloader",
	uploadSystem:  "MVIIS1.bin and boot.img only -- the OS",
	uploadAssets:  "assets.bin only -- the LK boot pictures",
	uploadRelease: "lk-release.bin only -- the SD-handoff bootloader, in place of lk.bin",
}

func uploadTargetNames() []string {
	names := make([]string, 0, len(uploadTargetHelp))
	for name := range uploadTargetHelp {
		names = append(names, name)
	}
	sort.Strings(names)
	return names
}

func uploadFlagUsage() string {
	var b strings.Builder
	b.WriteString("what this run writes: ")
	b.WriteString(strings.Join(uploadTargetNames(), ", "))
	b.WriteString(". Defaults to ")
	b.WriteString(uploadFull)
	b.WriteString(". ")
	for _, name := range uploadTargetNames() {
		fmt.Fprintf(&b, "%s = %s; ", name, uploadTargetHelp[name])
	}
	return strings.TrimSuffix(b.String(), "; ")
}

// The selected target, with the empty flag meaning "everything". Callers that
// need to know whether the operator asked explicitly compare cfg.upload to ""
// themselves; everything downstream wants the resolved answer.
func effectiveUploadTarget(cfg config) string {
	t := strings.ToLower(strings.TrimSpace(cfg.upload))
	if t == "" {
		return uploadFull
	}
	return t
}

func validUploadTarget(target string) bool {
	_, ok := uploadTargetHelp[strings.ToLower(strings.TrimSpace(target))]
	return ok
}

func invalidUploadTargetError(target string) error {
	return fmt.Errorf("-upload %q: expected one of %s",
		target, strings.Join(uploadTargetNames(), ", "))
}

/*
 * The release bootloader: same slot as lk.bin, different build.
 *
 * It lives in UBOOT like any other LK because it IS the LK -- what makes it the
 * release build is that its boot policy hands off to the SD card instead of to
 * boot.img, so the device comes up in whatever distribution is on the card. The
 * two are mutually exclusive by construction: one UBOOT partition, one resident
 * bootloader, and `-upload release' is how you say which one is in it.
 */
func defaultMVIIReleaseLKImagePath(cfg config) string {
	return filepath.Join(cfg.root, "lk-release.bin")
}

func uploadWritesStockSlotLK(target string) bool {
	return target == uploadFull || target == uploadLK
}

func uploadWritesReleaseLK(target string) bool {
	return target == uploadRelease
}

func uploadWritesSystem(target string) bool {
	return target == uploadFull || target == uploadSystem
}

func uploadWritesAssets(target string) bool {
	return target == uploadFull || target == uploadAssets
}

// Every file the selected target needs, in write order. Used both to decide
// whether the bundled feed path applies at all and to say which file is missing
// when it does not.
func uploadRequiredImages(cfg config, target string) []string {
	var want []string
	if uploadWritesReleaseLK(target) {
		want = append(want, defaultMVIIReleaseLKImagePath(cfg))
	}
	if uploadWritesStockSlotLK(target) {
		want = append(want, defaultMVIILKImagePath(cfg))
	}
	if uploadWritesSystem(target) {
		want = append(want, defaultMVIIS1ImagePath(cfg), defaultMVIIArmImagePath(cfg))
	}
	if uploadWritesAssets(target) {
		want = append(want, defaultMVIIAssetImagePath(cfg))
	}
	return want
}
