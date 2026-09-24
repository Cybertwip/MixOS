# MediaTek flashing tool

This directory contains a standalone copy of PowerEngine's `Deployment/cmd/mvii-flash` Go source, with its `go.mod` and `go.sum`, for the J36 Ultra MT6592. The original PowerEngine directory was left in place. Run `./tools/mediatek/flash -h` from the dArkOS checkout, or build the CLI with `cd tools/mediatek && go build -o /path/to/flash ./mvii-flash`. Go 1.23 or newer is required. On macOS, install `libusb-1.0` and `pkg-config` for the native USB transport.

This is the flashing client source, not a firmware image or an MT6592 download agent. Its flashing commands require their usual board-specific inputs and a connected device. The first `go run` may download the dependencies in `go.mod`.
