package main

import (
	"bytes"
	"strings"
	"testing"
	"time"
)

func TestResidentJumpAfterExplicitReadRefusal(t *testing.T) {
	const entry = uint32(0x201000)
	for _, status := range []uint16{0, 0x2001} {
		responses := []byte{mtkCmdRead32}
		responses = append(responses, be32(entry)...)
		responses = append(responses, be32(4)...)
		responses = append(responses, be16(0x1001)...)
		responses = append(responses, mtkCmdJumpDA)
		responses = append(responses, be32(entry)...)
		responses = append(responses, be16(status)...)
		port := &scriptPort{pending: responses}
		client := &mtkSerialClient{port: port, commandTimeout: time.Millisecond, writeTimeout: time.Millisecond}
		err := probeResidentPreloaderJump(client, entry, make([]byte, 16))
		if (err == nil) != (status == 0) {
			t.Fatalf("status 0x%x: unexpected result %v", status, err)
		}
		if status != 0 && !strings.Contains(err.Error(), "JUMP_DA status 0x2001") {
			t.Fatal(err)
		}
		want := []byte{mtkCmdRead32}
		want = append(want, be32(entry)...)
		want = append(want, be32(4)...)
		want = append(want, mtkCmdJumpDA)
		want = append(want, be32(entry)...)
		if got := bytes.Join(port.writes, nil); !bytes.Equal(got, want) {
			t.Fatalf("unexpected commands %x; want only READ32 and one JUMP_DA: %x", got, want)
		}
	}
}

func TestResidentJumpStopsOnUnreadableOrMismatchedEntry(t *testing.T) {
	const entry = uint32(0x201000)
	responses := []byte{mtkCmdRead32}
	responses = append(responses, be32(entry)...)
	responses = append(responses, be32(4)...)
	responses = append(responses, be16(0)...)
	for i := 0; i < 4; i++ {
		responses = append(responses, be32(0xdeadbeef)...)
	}
	responses = append(responses, be16(0)...)
	for _, reply := range [][]byte{nil, responses} {
		port := &scriptPort{pending: reply}
		client := &mtkSerialClient{port: port, commandTimeout: time.Millisecond, writeTimeout: time.Millisecond}
		if err := probeResidentPreloaderJump(client, entry, make([]byte, 16)); err == nil {
			t.Fatal("expected read/mismatch failure")
		}
		for _, write := range port.writes {
			if bytes.Equal(write, []byte{mtkCmdJumpDA}) {
				t.Fatal("jump sent after failed or mismatched read")
			}
		}
	}
}
