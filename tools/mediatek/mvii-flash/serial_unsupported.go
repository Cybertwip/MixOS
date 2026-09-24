//go:build !darwin && !linux

package main

import (
	"errors"
	"os"
	"time"
)

// The field is never set -- openMTKSerialPort always fails here -- but the follow
// loops in main.go and mtk_feed.go read straight from it, so the type has to carry
// it for those files to compile on a platform without a serial backend.
type mtkSerialPort struct {
	file *os.File
}

func openMTKSerialPort(device string, baud int) (*mtkSerialPort, error) {
	return nil, errors.New("mtk-serial is currently implemented on macOS and Linux")
}

func (p *mtkSerialPort) Close() error {
	return nil
}

func (p *mtkSerialPort) ReadExact(n int, timeout time.Duration) ([]byte, error) {
	return nil, errors.New("mtk-serial is unsupported on this platform")
}

func (p *mtkSerialPort) WriteAll(data []byte, timeout time.Duration) error {
	return errors.New("mtk-serial is unsupported on this platform")
}

func (p *mtkSerialPort) DiscardInput(timeout time.Duration) error {
	return nil
}

// Nothing here can produce a would-block read, so every error the follow loops see
// is a real one and none of them should be retried.
func isWouldBlock(err error) bool {
	return false
}
