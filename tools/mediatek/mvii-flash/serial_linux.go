//go:build linux

package main

import (
	"syscall"
	"unsafe"
)

const mtkLinuxTCSBRK = 0x5409

func configureMTKSerialFD(fd int) error {
	var termios syscall.Termios
	if err := ioctlTermios(fd, syscall.TCGETS, &termios); err != nil {
		return err
	}
	termios.Iflag &^= syscall.IGNPAR | syscall.ISTRIP | syscall.IXON | syscall.IXOFF | syscall.IXANY
	termios.Oflag = 0
	termios.Lflag &^= syscall.ICANON | syscall.ECHO | syscall.ECHOE | syscall.ECHOK | syscall.ECHONL | syscall.ISIG
	termios.Cflag &^= syscall.CSIZE | syscall.PARENB | syscall.CSTOPB
	termios.Cflag |= syscall.CS8 | syscall.CREAD | syscall.CLOCAL
	termios.Cc[syscall.VMIN] = 0
	termios.Cc[syscall.VTIME] = 1
	termios.Ispeed = syscall.B115200
	termios.Ospeed = syscall.B115200
	return ioctlTermios(fd, syscall.TCSETS, &termios)
}

func ioctlTermios(fd int, request uintptr, termios *syscall.Termios) error {
	_, _, errno := syscall.Syscall(syscall.SYS_IOCTL, uintptr(fd), request, uintptr(unsafe.Pointer(termios)))
	if errno != 0 {
		return errno
	}
	return nil
}

func drainMTKSerialFD(fd int) error {
	_, _, errno := syscall.Syscall(syscall.SYS_IOCTL, uintptr(fd), mtkLinuxTCSBRK, 1)
	if errno != 0 {
		return errno
	}
	return nil
}

func setMTKSerialControlLines(fd int, dtr, rts bool) error {
	// Assert the requested lines (DTR/RTS). This tells the CDC-ACM device
	// the host serial port is open, which some MTK BROM/preloaders require
	// to keep the VCOM endpoint configured for the full command sequence.
	setMask := uint32(0)
	if dtr {
		setMask |= uint32(syscall.TIOCM_DTR)
	}
	if rts {
		setMask |= uint32(syscall.TIOCM_RTS)
	}
	if setMask == 0 {
		return nil
	}
	_, _, errno := syscall.Syscall(syscall.SYS_IOCTL, uintptr(fd), syscall.TIOCMBIS, uintptr(unsafe.Pointer(&setMask)))
	if errno != 0 {
		return errno
	}
	return nil
}
