//go:build !darwin

package main

import (
	"errors"
	"time"
)

// The libusb raw-bulk transport is only needed (and only implemented) on macOS,
// where the kernel CDC-ACM driver destabilises the MT6592 BROM. On other
// platforms the tty serial path is used, so this is a no-op hook.
func tryConnectMTKUSB(string, mtkSerialConnectOptions) (*mtkSerialClient, bool, error) {
	return nil, false, nil
}

func tryReconnectMTKUSBBROM(string, time.Duration, time.Duration) (*mtkSerialClient, bool, error) {
	return nil, false, nil
}

// The live debug console is raw bulk on a vendor-class device, so it needs the
// libusb transport rather than a tty. Only macOS has that transport here; on
// other hosts the console is reachable with any generic libusb tool against
// 0x0e8d:0x4d56, but this binary does not implement it.
func openMVIIDebugPort() (mviiDebugPort, error) {
	return nil, errors.New("the MVII live debug console needs the libusb transport, which is only built on macOS in this tool")
}
