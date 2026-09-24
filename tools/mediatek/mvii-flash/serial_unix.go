//go:build darwin || linux

package main

import (
	"errors"
	"fmt"
	"io"
	"os"
	"syscall"
	"time"
)

type mtkSerialPort struct {
	file *os.File
}

func openMTKSerialPort(device string, baud int) (*mtkSerialPort, error) {
	if baud != 115200 {
		return nil, fmt.Errorf("unsupported MTK serial baud %d; expected 115200", baud)
	}
	fd, err := syscall.Open(device, syscall.O_RDWR|syscall.O_NOCTTY|syscall.O_NONBLOCK, 0)
	if err != nil {
		return nil, err
	}
	if err := configureMTKSerialFD(fd); err != nil {
		_ = syscall.Close(fd)
		return nil, err
	}
	if err := syscall.SetNonblock(fd, true); err != nil {
		_ = syscall.Close(fd)
		return nil, err
	}
	// Best-effort: assert DTR/RTS so the device keeps the VCOM configured
	// through the BROM handshake + first commands (GET_HW_CODE etc).
	//
	// On some hosts the opposite is true: the SET_CONTROL_LINE_STATE control
	// transfer the macOS CDC driver emits for this disturbs the MT6592 BROM and
	// it re-enumerates a beat after the handshake (the "device not configured"
	// drop on the first command). MVII_MTK_NO_CONTROL_LINES=1 skips it so that
	// theory can be tested without a rebuild.
	if !envFlag("MVII_MTK_NO_CONTROL_LINES") {
		_ = setMTKSerialControlLines(fd, true, true)
	}
	return &mtkSerialPort{file: os.NewFile(uintptr(fd), device)}, nil
}

func (p *mtkSerialPort) Close() error {
	if p == nil || p.file == nil {
		return nil
	}
	return p.file.Close()
}

func (p *mtkSerialPort) ReadExact(n int, timeout time.Duration) ([]byte, error) {
	if n < 0 {
		return nil, fmt.Errorf("invalid read length %d", n)
	}
	buf := make([]byte, n)
	read := 0
	deadline := time.Now().Add(timeout)
	for read < n {
		count, err := p.file.Read(buf[read:])
		if count > 0 {
			read += count
			continue
		}
		if err != nil {
			if isWouldBlock(err) {
				if time.Now().After(deadline) {
					return nil, fmt.Errorf("serial read timeout after %s: got %d/%d bytes", timeout, read, n)
				}
				time.Sleep(2 * time.Millisecond)
				continue
			}
			if errors.Is(err, io.EOF) && time.Now().Before(deadline) {
				time.Sleep(2 * time.Millisecond)
				continue
			}
			return nil, err
		}
		if time.Now().After(deadline) {
			return nil, fmt.Errorf("serial read timeout after %s: got %d/%d bytes", timeout, read, n)
		}
		time.Sleep(2 * time.Millisecond)
	}
	return buf, nil
}

func (p *mtkSerialPort) WriteAll(data []byte, timeout time.Duration) error {
	deadline := time.Now().Add(timeout)
	for len(data) > 0 {
		count, err := p.file.Write(data)
		if count > 0 {
			data = data[count:]
			continue
		}
		if err != nil {
			if isWouldBlock(err) {
				if time.Now().After(deadline) {
					return fmt.Errorf("serial write timeout after %s with %d bytes pending", timeout, len(data))
				}
				time.Sleep(2 * time.Millisecond)
				continue
			}
			return err
		}
		if time.Now().After(deadline) {
			return fmt.Errorf("serial write timeout after %s with %d bytes pending", timeout, len(data))
		}
		time.Sleep(2 * time.Millisecond)
	}
	return nil
}

func (p *mtkSerialPort) DiscardInput(timeout time.Duration) error {
	if timeout <= 0 {
		// Fast non-blocking drain of any immediately available bytes.
		buf := make([]byte, 512)
		for {
			_, err := p.file.Read(buf)
			if err != nil {
				if isWouldBlock(err) || errors.Is(err, io.EOF) {
					return nil
				}
				return err
			}
		}
	}
	deadline := time.Now().Add(timeout)
	buf := make([]byte, 512)
	quiet := 0
	for time.Now().Before(deadline) {
		_, err := p.file.Read(buf)
		if err != nil {
			if isWouldBlock(err) || errors.Is(err, io.EOF) {
				quiet++
				if quiet >= 2 {
					return nil
				}
				time.Sleep(1 * time.Millisecond)
				continue
			}
			return err
		}
		quiet = 0
	}
	return nil
}

func isWouldBlock(err error) bool {
	return errors.Is(err, syscall.EAGAIN) || errors.Is(err, syscall.EWOULDBLOCK)
}
