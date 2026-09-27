package main

import (
	"strings"
	"testing"
)

func TestResolveUARTMemOp(t *testing.T) {
	dev := "/dev/cu.usbmodemFAKE"
	rows := []struct {
		name       string
		cfg        config
		write      bool
		want       uartMemOp
		wantErrSub string
	}{
		{"read default words", config{device: dev, mtkPayloadAddr: "0x201000"}, false, uartMemOp{addr: 0x201000, words: 1}, ""},
		{"read decimal", config{device: dev, mtkPayloadAddr: "2101248", mtkWords: "4"}, false, uartMemOp{addr: 0x201000, words: 4}, ""},
		{"write", config{device: dev, mtkPayloadAddr: "0x1001a100", mtkValue: "0x444cfffd"}, true, uartMemOp{addr: 0x1001a100, words: 1, value: 0x444cfffd}, ""},
		{"no device", config{mtkPayloadAddr: "0x201000"}, false, uartMemOp{}, "-device"},
		{"block device", config{device: "/dev/disk2", mtkPayloadAddr: "0x201000"}, false, uartMemOp{}, "-device"},
		{"no address", config{device: dev}, false, uartMemOp{}, "-address"},
		{"bad address", config{device: dev, mtkPayloadAddr: "0xzz"}, false, uartMemOp{}, "-address"},
		{"no value", config{device: dev, mtkPayloadAddr: "0x1001a100"}, true, uartMemOp{}, "-mtk-value"},
		{"bad value", config{device: dev, mtkPayloadAddr: "0x1001a100", mtkValue: "nope"}, true, uartMemOp{}, "-mtk-value"},
		{"zero words", config{device: dev, mtkPayloadAddr: "0x201000", mtkWords: "0"}, false, uartMemOp{}, "-mtk-words"},
		{"too many words", config{device: dev, mtkPayloadAddr: "0x201000", mtkWords: "257"}, false, uartMemOp{}, "-mtk-words"},
		{"bad words", config{device: dev, mtkPayloadAddr: "0x201000", mtkWords: "lots"}, false, uartMemOp{}, "-mtk-words"},
	}
	for _, row := range rows {
		got, err := resolveUARTMemOp(row.cfg, row.write)
		if row.wantErrSub != "" {
			if err == nil || !strings.Contains(err.Error(), row.wantErrSub) {
				t.Fatalf("%s: error = %v, want %q inside", row.name, err, row.wantErrSub)
			}
			continue
		}
		if err != nil {
			t.Fatalf("%s: error = %v, want nil", row.name, err)
		}
		if got != row.want {
			t.Fatalf("%s: op = %+v, want %+v", row.name, got, row.want)
		}
	}
}
