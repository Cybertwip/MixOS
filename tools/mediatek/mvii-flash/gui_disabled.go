//go:build !gui

package main

// The console build of this program. `guiBuild` is a constant, so the `if
// guiBuild` in main() folds away and the GUI never influences this binary --
// and, more to the point, a file behind `//go:build gui` is not scanned for
// imports, so the default `go build ./cmd/mvii-flash` that CMake runs never
// resolves or downloads Fyne. The console tool keeps exactly the dependencies
// it had before the GUI existed.
const guiBuild = false

// Unreachable here; it exists so that main() type-checks in both builds.
func runFlashGUI() int { return 0 }
