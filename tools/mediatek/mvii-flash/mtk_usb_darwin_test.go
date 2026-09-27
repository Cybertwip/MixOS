//go:build darwin

package main

import "testing"

func TestIsMTKUSBFlashDevice(t *testing.T) {
	allow := [][2]uint16{
		{0x0e8d, 0x0003}, // MTK BROM
		{0x0e8d, 0x2000}, // MTK preloader
		{0x0e8d, 0x6000}, // MTK preloader
		{0x22d9, 0x0006}, // OPPO preloader
		{0x1004, 0x6000}, // LG preloader
	}
	for _, id := range allow {
		if !isMTKUSBFlashDevice(id[0], id[1]) {
			t.Errorf("isMTKUSBFlashDevice(0x%04x:0x%04x) = false, want true", id[0], id[1])
		}
	}
	deny := [][2]uint16{
		{0x0e8d, 0x4d56}, // MVII debug console, never a BROM
		{0x22d9, 0x0003}, // OPPO, wrong PID
		{0x1004, 0x0006}, // LG, wrong PID
		{0x1234, 0x5678}, // stranger
	}
	for _, id := range deny {
		if isMTKUSBFlashDevice(id[0], id[1]) {
			t.Errorf("isMTKUSBFlashDevice(0x%04x:0x%04x) = true, want false", id[0], id[1])
		}
	}
}
