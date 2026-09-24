package main

import (
	"fmt"
	"time"
)

func resetMTKTargetToBROM(cfg config) error {
	fmt.Println("Requesting MT6592 BROM recovery reset. No eMMC write will be performed.")
	client, err := connectMTKSerialWithOptions(cfg.device, mtkSerialConnectOptions{recoverFeedPayload: true})
	if err != nil {
		return err
	}
	if err := client.probeMT6592(); err != nil {
		_ = client.port.Close()
		return err
	}
	if client.isBROM {
		fmt.Printf("Target is already in BROM mode on %s.\n", client.device)
		_ = client.port.Close()
		return nil
	}

	oldDevice := client.device
	commandTimeout := client.commandTimeout
	writeTimeout := client.writeTimeout
	fmt.Printf("Target is in preloader mode (BL=0x%02x); resetting to BROM.\n", client.blVersion)
	if err := client.resetPreloaderToBROM(); err != nil {
		_ = client.port.Close()
		return err
	}
	_ = client.port.Close()
	time.Sleep(1200 * time.Millisecond)

	bromClient, err := reconnectMTKSerialBROMAfterPreloaderReset(oldDevice, commandTimeout, writeTimeout)
	if err != nil {
		return err
	}
	defer bromClient.port.Close()
	fmt.Printf("Target is now in BROM mode on %s.\n", bromClient.device)
	return nil
}
