//go:build darwin

package main

/*
#cgo pkg-config: libusb-1.0
#include <libusb.h>
#include <stdlib.h>
*/
import "C"

import (
	"errors"
	"fmt"
	"strings"
	"sync"
	"time"
	"unsafe"
)

// The MediaTek BROM/preloader VCOM is a USB CDC-ACM device. On macOS the kernel
// AppleUSBCDC driver binds it and exposes /dev/cu.usbmodem*, but that kernel
// path injects control/interrupt traffic the MT6592 BROM does not tolerate, so
// the device re-enumerates a beat after the handshake ("device not configured").
//
// This transport bypasses the tty driver entirely: it opens the device with
// libusb, detaches the kernel driver (macOS device capture, requires sudo) and
// talks to the BROM over its raw bulk IN/OUT endpoints, exactly like mtkclient.
const mtkUSBVendorID = 0x0e8d

var (
	errMTKUSBTimeout  = errors.New("usb bulk timeout")
	errMTKUSBClosed   = errors.New("usb port closed")
	errMTKUSBNotFound = errors.New("no MediaTek USB device found (VID 0x0e8d)")
)

var (
	libusbOnce    sync.Once
	libusbCtx     *C.libusb_context
	libusbInitErr error
)

func libusbInit() error {
	libusbOnce.Do(func() {
		var ctx *C.libusb_context
		if rc := C.libusb_init(&ctx); rc != 0 {
			libusbInitErr = fmt.Errorf("libusb_init failed: %s", libusbErrName(rc))
			return
		}
		libusbCtx = ctx
	})
	return libusbInitErr
}

func libusbErrName(rc C.int) string {
	return C.GoString(C.libusb_error_name(rc))
}

type mtkUSBPort struct {
	handle *C.libusb_device_handle
	iface  C.int
	epIn   C.uchar
	epOut  C.uchar
	mu     sync.Mutex
	closed bool
	rbuf   []byte // bytes already pulled from the device but not yet consumed
}

// usbReadChunk is the scratch size for a single bulk IN. It is a multiple of the
// common bulk max-packet sizes (64 and 512) so a full-size packet can never
// overflow the request buffer; a short packet ends the transfer early.
const usbReadChunk = 16384

// openMTKUSBPort finds the first MediaTek (VID 0x0e8d) device, claims its bulk
// data interface (detaching the kernel CDC driver) and returns a transport.
func openMTKUSBPort() (*mtkUSBPort, error) {
	if err := libusbInit(); err != nil {
		return nil, err
	}
	var list **C.libusb_device
	n := C.libusb_get_device_list(libusbCtx, &list)
	if n < 0 {
		return nil, fmt.Errorf("libusb_get_device_list: %s", libusbErrName(C.int(n)))
	}
	defer C.libusb_free_device_list(list, 1)

	devs := unsafe.Slice(list, int(n))
	var lastErr error
	sawConsole := false
	for _, dev := range devs {
		var desc C.struct_libusb_device_descriptor
		if rc := C.libusb_get_device_descriptor(dev, &desc); rc != 0 {
			continue
		}
		if uint16(desc.idVendor) != mtkUSBVendorID {
			continue
		}
		// Never the live debug console, even though it is a 0x0e8d device and
		// "first 0x0e8d wins" would take it.
		//
		// This is the host half of the bridge magic, and it is the half that
		// actually caused the damage. A board serving the console is 0x0e8d/
		// 0x4d56 and sits on the bus for the entire length of a debugging
		// session; any `flash` invocation in that window -- a flash, a
		// -mtk-read-boot-status, anything not -dbg -- came here, took the
		// console as if it were a BROM, claimed its interface and drove the
		// A0 0A 50 05 handshake into it. Claiming alone re-configures the
		// device, which drops the console's link, and that used to end the
		// session outright. The console now survives both (it swallows the
		// probe bytes and re-waits instead of exiting), but the right fix is
		// for the host not to knock on that door at all: the console is not a
		// BROM and no amount of handshaking will make it one.
		if uint16(desc.idProduct) == mviiDebugConsolePID {
			sawConsole = true
			continue
		}
		port, err := openMTKUSBDevice(dev)
		if err != nil {
			lastErr = err
			continue
		}
		return port, nil
	}
	if lastErr != nil {
		return nil, lastErr
	}
	if sawConsole {
		return nil, errors.New("the only MediaTek device here is the MVII debug console (0x0e8d:0x4d56), which is not a BROM — " +
			"the board is booted and serving the console, so either attach to it with -dbg, or send it `flag brom` " +
			"(./flash -target=arm -flag brom) to reset it into download mode")
	}
	return nil, errMTKUSBNotFound
}

// openMVIIDebugPort finds the MVII live debug console specifically, by VID:PID.
//
// It cannot share openMTKUSBPort's "first 0x0e8d device wins" rule, because in
// practice both devices are 0x0e8d: 0x0e8d/0x2000-ish is the board sitting in
// BROM, and 0x0e8d/0x4d56 is the board booted and serving the console. Matching
// on the PID is what makes "you are still in BROM" a clear diagnostic instead of
// a confusing framing error.
func openMVIIDebugPort() (*mtkUSBPort, error) {
	if err := libusbInit(); err != nil {
		return nil, err
	}
	var list **C.libusb_device
	n := C.libusb_get_device_list(libusbCtx, &list)
	if n < 0 {
		return nil, fmt.Errorf("libusb_get_device_list: %s", libusbErrName(C.int(n)))
	}
	defer C.libusb_free_device_list(list, 1)

	devs := unsafe.Slice(list, int(n))
	var lastErr error
	sawBROM := false
	for _, dev := range devs {
		var desc C.struct_libusb_device_descriptor
		if rc := C.libusb_get_device_descriptor(dev, &desc); rc != 0 {
			continue
		}
		if uint16(desc.idVendor) != mtkUSBVendorID {
			continue
		}
		if uint16(desc.idProduct) != mviiDebugConsolePID {
			sawBROM = true
			continue
		}
		port, err := openMTKUSBDevice(dev)
		if err != nil {
			lastErr = err
			continue
		}
		return port, nil
	}
	if lastErr != nil {
		return nil, lastErr
	}
	if sawBROM {
		// Reporting the observed PID matters: 0x0e8d/0x2000 is the BROM, and a
		// board that has *just been flashed* is sitting in the resident
		// MVIIFlash payload on that same PID, which looks identical here but
		// needs a completely different response from the operator. Naming the
		// number lets them tell which one they are looking at.
		return nil, errors.New("found a MediaTek device but not the MVII console (0x0e8d:0x4d56) — the board is in BROM or still running the resident flash payload; power-cycle it while holding down any face or shoulder button, then retry")
	}
	return nil, errMVIIDebugNotFound
}

func openMTKUSBDevice(dev *C.libusb_device) (*mtkUSBPort, error) {
	// An unconfigured device has no *active* configuration, and asking for one
	// legitimately returns NOT_FOUND. That is the normal state of this console
	// on macOS: it is a vendor-class device with no matching kernel driver, and
	// macOS does not select a configuration for devices it cannot bind, so the
	// board sat fully enumerated (address assigned, all descriptors read) while
	// this function rejected it before even opening it. Fall back to
	// configuration index 0, which comes from the cached descriptors and is
	// readable whether or not anything is active; the configuration gets
	// selected for real after libusb_open, below.
	var cfg *C.struct_libusb_config_descriptor
	rcActive := C.libusb_get_active_config_descriptor(dev, &cfg)
	if rcActive != 0 {
		if rc := C.libusb_get_config_descriptor(dev, 0, &cfg); rc != 0 {
			return nil, fmt.Errorf("get config descriptor (active: %s, index 0: %s)",
				libusbErrName(rcActive), libusbErrName(rc))
		}
	}
	wantConfig := C.int(cfg.bConfigurationValue)
	defer C.libusb_free_config_descriptor(cfg)

	ifaceNum := -1
	var epIn, epOut C.uchar
	ifaces := unsafe.Slice(cfg._interface, int(cfg.bNumInterfaces))
	for _, iface := range ifaces {
		alts := unsafe.Slice(iface.altsetting, int(iface.num_altsetting))
		for _, alt := range alts {
			var bin, bout C.uchar
			haveIn, haveOut := false, false
			eps := unsafe.Slice(alt.endpoint, int(alt.bNumEndpoints))
			for _, ep := range eps {
				if int(ep.bmAttributes)&0x03 != int(C.LIBUSB_TRANSFER_TYPE_BULK) {
					continue
				}
				if int(ep.bEndpointAddress)&0x80 != 0 {
					bin = ep.bEndpointAddress
					haveIn = true
				} else {
					bout = ep.bEndpointAddress
					haveOut = true
				}
			}
			if haveIn && haveOut {
				ifaceNum = int(alt.bInterfaceNumber)
				epIn, epOut = bin, bout
				break
			}
		}
		if ifaceNum >= 0 {
			break
		}
	}
	if ifaceNum < 0 {
		return nil, errors.New("MediaTek USB device exposes no bulk IN/OUT interface")
	}

	var handle *C.libusb_device_handle
	if rc := C.libusb_open(dev, &handle); rc != 0 {
		return nil, fmt.Errorf("libusb_open MediaTek device: %s (run the flasher with sudo)", libusbErrName(rc))
	}
	// Detach the macOS kernel CDC driver so we can own the interface. This is
	// the macOS "device capture" path and requires root.
	C.libusb_set_auto_detach_kernel_driver(handle, 1)
	// Select the configuration ourselves. Claiming an interface on a device with
	// no active configuration cannot work, and on macOS nothing else is going to
	// do this for a driverless vendor device. libusb short-circuits this when the
	// requested configuration is already active, so it is free in that case; a
	// failure here is not fatal on its own, and claim_interface below gives the
	// better diagnostic if the device really is unusable.
	C.libusb_set_configuration(handle, wantConfig)
	if rc := C.libusb_claim_interface(handle, C.int(ifaceNum)); rc != 0 {
		C.libusb_close(handle)
		return nil, fmt.Errorf("claim MediaTek interface %d: %s (the macOS CDC driver holds the VCOM; run the flasher with sudo)", ifaceNum, libusbErrName(rc))
	}
	return &mtkUSBPort{handle: handle, iface: C.int(ifaceNum), epIn: epIn, epOut: epOut}, nil
}

// isMTKUSBAccessError reports whether the failure is the macOS kernel holding
// the interface (i.e. the tool needs to run as root), which is not worth
// retrying for the whole handshake window.
func isMTKUSBAccessError(err error) bool {
	if err == nil {
		return false
	}
	msg := strings.ToLower(err.Error())
	return strings.Contains(msg, "sudo") || strings.Contains(msg, "access") || strings.Contains(msg, "busy")
}

func (p *mtkUSBPort) bulk(ep C.uchar, buf []byte, timeoutMs C.uint) (int, error) {
	var transferred C.int
	var ptr *C.uchar
	if len(buf) > 0 {
		ptr = (*C.uchar)(unsafe.Pointer(&buf[0]))
	}
	rc := C.libusb_bulk_transfer(p.handle, ep, ptr, C.int(len(buf)), &transferred, timeoutMs)
	n := int(transferred)
	switch rc {
	case 0:
		return n, nil
	case C.LIBUSB_ERROR_TIMEOUT:
		return n, errMTKUSBTimeout
	case C.LIBUSB_ERROR_NO_DEVICE, C.LIBUSB_ERROR_IO, C.LIBUSB_ERROR_PIPE, C.LIBUSB_ERROR_NOT_FOUND, C.LIBUSB_ERROR_OTHER:
		// Surface a message isDeviceGoneError recognises so the upper layers can
		// trigger their reconnect logic.
		return n, fmt.Errorf("usb device not configured: %s", libusbErrName(rc))
	default:
		return n, fmt.Errorf("libusb bulk transfer: %s", libusbErrName(rc))
	}
}

func (p *mtkUSBPort) ReadExact(n int, timeout time.Duration) ([]byte, error) {
	if n < 0 {
		return nil, fmt.Errorf("invalid read length %d", n)
	}
	if n == 0 {
		return []byte{}, nil
	}
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.closed {
		return nil, errMTKUSBClosed
	}
	out := make([]byte, 0, n)
	scratch := make([]byte, usbReadChunk)
	deadline := time.Now().Add(timeout)
	for len(out) < n {
		// Serve from the leftover buffer first.
		if len(p.rbuf) > 0 {
			take := n - len(out)
			if take > len(p.rbuf) {
				take = len(p.rbuf)
			}
			out = append(out, p.rbuf[:take]...)
			p.rbuf = p.rbuf[take:]
			if len(p.rbuf) == 0 {
				p.rbuf = nil
			}
			continue
		}
		remaining := time.Until(deadline)
		if remaining <= 0 {
			return nil, fmt.Errorf("usb read timeout after %s: got %d/%d bytes", timeout, len(out), n)
		}
		ms := remaining / time.Millisecond
		if ms <= 0 {
			ms = 1
		}
		if ms > 200 {
			ms = 200 // cap per-call so the deadline is re-checked promptly
		}
		got, err := p.bulk(p.epIn, scratch, C.uint(ms))
		if got > 0 {
			p.rbuf = append(p.rbuf, scratch[:got]...)
		}
		if err != nil {
			if errors.Is(err, errMTKUSBTimeout) {
				continue
			}
			return nil, err
		}
	}
	return out, nil
}

func (p *mtkUSBPort) WriteAll(data []byte, timeout time.Duration) error {
	if len(data) == 0 {
		return nil
	}
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.closed {
		return errMTKUSBClosed
	}
	deadline := time.Now().Add(timeout)
	for len(data) > 0 {
		remaining := time.Until(deadline)
		if remaining <= 0 {
			return fmt.Errorf("usb write timeout after %s with %d bytes pending", timeout, len(data))
		}
		ms := remaining / time.Millisecond
		if ms <= 0 {
			ms = 1
		}
		if ms > 1000 {
			ms = 1000
		}
		got, err := p.bulk(p.epOut, data, C.uint(ms))
		if got > 0 {
			data = data[got:]
		}
		if err != nil {
			if errors.Is(err, errMTKUSBTimeout) {
				continue
			}
			return err
		}
	}
	return nil
}

// ReadSome returns whatever the device has sent, up to len(buf), and reports 0
// with no error when the window closed quietly. That is the difference between
// this and ReadExact: the debug console is a stream of unpredictable length, so
// "nothing arrived" is the normal case between commands and must not be an
// error the caller has to distinguish from a real fault.
func (p *mtkUSBPort) ReadSome(buf []byte, timeout time.Duration) (int, error) {
	if len(buf) == 0 {
		return 0, nil
	}
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.closed {
		return 0, errMTKUSBClosed
	}
	if len(p.rbuf) > 0 {
		n := copy(buf, p.rbuf)
		p.rbuf = p.rbuf[n:]
		if len(p.rbuf) == 0 {
			p.rbuf = nil
		}
		return n, nil
	}
	ms := timeout / time.Millisecond
	if ms <= 0 {
		ms = 1
	}
	got, err := p.bulk(p.epIn, buf, C.uint(ms))
	if err != nil && !errors.Is(err, errMTKUSBTimeout) {
		return got, err
	}
	return got, nil
}

func (p *mtkUSBPort) DiscardInput(timeout time.Duration) error {
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.closed {
		return errMTKUSBClosed
	}
	p.rbuf = nil // drop anything already buffered
	buf := make([]byte, usbReadChunk)
	if timeout <= 0 {
		// Fast non-blocking drain: read until a tiny-timeout read returns nothing.
		for {
			got, err := p.bulk(p.epIn, buf, 3)
			if err != nil && !errors.Is(err, errMTKUSBTimeout) {
				return nil // ignore drops while draining
			}
			if got == 0 {
				return nil
			}
		}
	}
	deadline := time.Now().Add(timeout)
	quiet := 0
	for time.Now().Before(deadline) {
		ms := time.Until(deadline) / time.Millisecond
		if ms <= 0 {
			break
		}
		if ms > 20 {
			ms = 20
		}
		got, err := p.bulk(p.epIn, buf, C.uint(ms))
		if err != nil && !errors.Is(err, errMTKUSBTimeout) {
			return nil
		}
		if got == 0 {
			quiet++
			if quiet >= 2 {
				return nil
			}
			continue
		}
		quiet = 0
	}
	return nil
}

func (p *mtkUSBPort) Close() error {
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.closed || p.handle == nil {
		return nil
	}
	p.closed = true
	C.libusb_release_interface(p.handle, p.iface)
	C.libusb_close(p.handle)
	p.handle = nil
	return nil
}

// mtkUSBTransportEnabled reports whether the libusb transport should be used
// instead of the kernel tty driver. It is the default on macOS (the tty path is
// unreliable for the MT6592 BROM); MVII_MTK_TTY=1 forces the old path.
func mtkUSBTransportEnabled() bool {
	if envFlag("MVII_MTK_TTY") {
		return false
	}
	return true
}

func tryConnectMTKUSB(device string, options mtkSerialConnectOptions) (*mtkSerialClient, bool, error) {
	if !mtkUSBTransportEnabled() {
		return nil, false, nil
	}
	client, err := connectMTKUSB(device, options)
	return client, true, err
}

// tryReconnectMTKUSBBROM reopens the BROM over libusb after a preloader→BROM
// watchdog reset, replacing the tty reconnect (which hits "resource busy" once
// libusb has captured the device). It handshakes and probes until the target
// reports BROM or the reconnect window expires.
func tryReconnectMTKUSBBROM(device string, commandTimeout, writeTimeout time.Duration) (*mtkSerialClient, bool, error) {
	if !mtkUSBTransportEnabled() {
		return nil, false, nil
	}
	timeout := envDuration("MVII_MTK_BROM_RECONNECT_TIMEOUT",
		envDuration("MVII_MTK_SERIAL_RECONNECT_TIMEOUT", mtkSerialReconnectTimeout))
	deadline := time.Now().Add(timeout)
	fmt.Printf("Reopening MediaTek BROM over libusb after preloader reset (timeout %s)\n", timeout)
	var lastErr error
	lastReport := time.Time{}
	for time.Now().Before(deadline) {
		port, err := openMTKUSBPort()
		if err != nil {
			if isMTKUSBAccessError(err) {
				return nil, true, err
			}
			lastErr = err
			if now := time.Now(); now.Sub(lastReport) >= 2*time.Second {
				fmt.Printf("Waiting for MediaTek BROM over libusb (%s left)\n", time.Until(deadline).Round(time.Second))
				lastReport = now
			}
			time.Sleep(200 * time.Millisecond)
			continue
		}
		client := &mtkSerialClient{
			port:           port,
			device:         mtkUSBDeviceLabel(device),
			commandTimeout: commandTimeout,
			writeTimeout:   writeTimeout,
		}
		_ = client.port.DiscardInput(10 * time.Millisecond)
		handshakeDeadline := time.Now().Add(3 * time.Second)
		if handshakeDeadline.After(deadline) {
			handshakeDeadline = deadline
		}
		if err := client.handshake(handshakeDeadline); err != nil {
			_ = port.Close()
			lastErr = err
			time.Sleep(200 * time.Millisecond)
			continue
		}
		_ = client.port.DiscardInput(0)
		if err := client.probeMT6592(); err != nil {
			_ = port.Close()
			lastErr = err
			time.Sleep(200 * time.Millisecond)
			continue
		}
		if !client.isBROM {
			_ = port.Close()
			lastErr = errors.New("target returned to preloader instead of BROM")
			time.Sleep(500 * time.Millisecond)
			continue
		}
		fmt.Println("Reconnected to MediaTek BROM over libusb.")
		return client, true, nil
	}
	if lastErr == nil {
		lastErr = errors.New("no BROM handshake over libusb")
	}
	return nil, true, fmt.Errorf("MediaTek BROM reconnect over libusb timed out: %w", lastErr)
}

func mtkUSBDeviceLabel(device string) string {
	if strings.TrimSpace(device) != "" {
		return device
	}
	return fmt.Sprintf("libusb:%04x", mtkUSBVendorID)
}

func connectMTKUSB(device string, options mtkSerialConnectOptions) (*mtkSerialClient, error) {
	timeout := envDuration("MVII_MTK_SERIAL_HANDSHAKE_TIMEOUT", mtkSerialHandshakeTimeout)
	deadline := time.Now().Add(timeout)
	commandTimeout := envDuration("MVII_MTK_SERIAL_TIMEOUT", mtkSerialCommandTimeout)
	writeTimeout := envDuration("MVII_MTK_SERIAL_WRITE_TIMEOUT", envDuration("MVII_MTK_USB_WRITE_TIMEOUT", mtkSerialWriteTimeout))
	openHandshake := envDuration("MVII_MTK_SERIAL_OPEN_HANDSHAKE_TIMEOUT", mtkSerialOpenHandshake)
	fmt.Printf("Opening MediaTek USB device via libusb (VID 0x%04x, timeout %s)\n", mtkUSBVendorID, timeout)
	var lastErr error
	lastReport := time.Time{}
	for time.Now().Before(deadline) {
		port, err := openMTKUSBPort()
		if err != nil {
			if isMTKUSBAccessError(err) {
				return nil, fmt.Errorf("%w", err)
			}
			lastErr = err
			if now := time.Now(); now.Sub(lastReport) >= 2*time.Second {
				fmt.Printf("Waiting for MediaTek USB device (%s left)\n", time.Until(deadline).Round(time.Second))
				lastReport = now
			}
			time.Sleep(200 * time.Millisecond)
			continue
		}

		client := &mtkSerialClient{
			port:           port,
			device:         mtkUSBDeviceLabel(device),
			commandTimeout: commandTimeout,
			writeTimeout:   writeTimeout,
		}
		_ = client.port.DiscardInput(10 * time.Millisecond)
		fmt.Println("Opened MediaTek USB device. Performing BROM handshake.")
		handshakeDeadline := time.Now().Add(openHandshake)
		if handshakeDeadline.After(deadline) {
			handshakeDeadline = deadline
		}
		if err := client.handshake(handshakeDeadline); err != nil {
			// The handshake only works against a fresh BROM. If a previous run
			// left the MVIIFlash feed payload resident it speaks the feed frame
			// protocol instead, so probe for that before giving up.
			if options.reuseFeedPayload {
				if hello, recovered, _ := client.probeMTKFeedPayloadQuick(); recovered {
					client.feedPayloadReady = true
					client.feedPayloadHello = hello
					if hello.Name != "" {
						fmt.Printf("Existing MVIIFlash payload detected over USB: %s; reusing it.\n", hello.Name)
					} else {
						fmt.Println("Existing MVIIFlash payload detected over USB; reusing it.")
					}
					return client, nil
				}
			}
			if options.recoverFeedPayload {
				if recovered, _ := client.requestMTKFeedPayloadResetQuick(); recovered {
					_ = port.Close()
					// Let the payload watchdog-reset into a clean MediaTek loader
					// path, then reopen on the next loop iteration.
					time.Sleep(2500 * time.Millisecond)
					continue
				}
			}
			_ = port.Close()
			lastErr = err
			time.Sleep(200 * time.Millisecond)
			continue
		}
		fmt.Println("MTK USB handshake successful.")
		_ = client.port.DiscardInput(0)
		return client, nil
	}
	if lastErr == nil {
		lastErr = errors.New("no MediaTek USB handshake response")
	}
	return nil, fmt.Errorf("MediaTek USB connect timed out: %w", lastErr)
}
