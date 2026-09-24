package main

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

type mtkGFHRecord struct {
	Offset uint64
	Type   uint16
	Size   uint16
	Name   string
}

type mtkPreloaderFileInfo struct {
	Offset        uint64
	FileType      uint16
	FlashDev      byte
	SigType       byte
	LoadAddr      uint32
	FileLen       uint32
	MaxSize       uint32
	ContentOffset uint32
	SigLen        uint32
	JumpOffset    uint32
	Attr          uint32
}

type mtkPreloaderPartitionTable struct {
	Offset      uint64
	AddressBase uint64
	Partitions  []mtkPreloaderHardcodedPartition
}

type mtkPreloaderHardcodedPartition struct {
	Name   string
	Offset uint64
	Size   uint64
	Source string
}

func analyzePreloaderCommand(cfg config, path string) error {
	resolved, err := resolvePreloaderAnalyzePath(cfg, path)
	if err != nil {
		return err
	}
	data, err := os.ReadFile(resolved)
	if err != nil {
		return fmt.Errorf("read preloader %s: %w", resolved, err)
	}
	fmt.Printf("MediaTek preloader analysis: %s (%s)\n", resolved, formatBytes(uint64(len(data))))
	printPreloaderBootHeader(data)
	printPreloaderBRLYT(data)
	printPreloaderGFH(data)
	printPreloaderBootStrings(data)
	table := printPreloaderHardcodedPartitions(data)
	if strings.TrimSpace(cfg.mtkPreloadProfileOut) != "" {
		profile := buildMTKPreloaderProfileFromAnalysis(resolved, data, table)
		if err := writeMTKPreloaderProfile(cfg.mtkPreloadProfileOut, profile); err != nil {
			return err
		}
		fmt.Printf("  wrote editable preloader profile: %s\n", cfg.mtkPreloadProfileOut)
	}
	return nil
}

func resolvePreloaderAnalyzePath(cfg config, path string) (string, error) {
	if !strings.EqualFold(strings.TrimSpace(path), "auto") {
		return path, nil
	}
	candidates := []string{}
	if cfg.preloader != "" {
		candidates = append(candidates, cfg.preloader)
	}
	candidates = append(candidates,
		filepath.Join(cfg.root, "tools", "mtk-da", "preloader_j36ultra.bin"),
		filepath.Join(cfg.root, "tools", "mtk-da", "preloader.bin"),
		filepath.Join("Hardware", "Virtua", "loader", "mtk-da", "preloader_j36ultra.bin"),
	)
	for _, candidate := range candidates {
		if fileExists(candidate) {
			return candidate, nil
		}
	}
	return "", fmt.Errorf("could not find a preloader for -mtk-analyze-preloader auto; pass -preloader or an explicit path")
}

func printPreloaderBootHeader(data []byte) {
	if len(data) < 0x20 {
		return
	}
	magic := trimNULASCII(data[0:12])
	fmt.Printf("  boot header: magic=%q version=0x%x declared-size=0x%x\n",
		magic, binary.LittleEndian.Uint32(data[12:16]), binary.LittleEndian.Uint32(data[16:20]))
}

func printPreloaderBRLYT(data []byte) {
	pos := bytes.Index(data, []byte("BRLYT"))
	if pos < 0 || pos+0x24 > len(data) {
		fmt.Println("  BRLYT: not found")
		return
	}
	fmt.Printf("  BRLYT @0x%x: header-size=0x%x version=0x%x region0 offset=0x%x length=0x%x\n",
		pos,
		binary.LittleEndian.Uint32(data[pos+4:pos+8]),
		binary.LittleEndian.Uint32(data[pos+8:pos+12]),
		binary.LittleEndian.Uint32(data[pos+12:pos+16]),
		binary.LittleEndian.Uint32(data[pos+16:pos+20]))
}

func printPreloaderGFH(data []byte) {
	records := scanPreloaderGFH(data)
	if len(records) == 0 {
		fmt.Println("  GFH: no records found")
		return
	}
	fmt.Println("  GFH records:")
	for _, rec := range records {
		fmt.Printf("    @0x%05x type=0x%04x size=0x%04x %s\n", rec.Offset, rec.Type, rec.Size, rec.Name)
		if rec.Name == "FILE_INFO" {
			if info, ok := parsePreloaderGFHFileInfo(data, rec.Offset); ok {
				printPreloaderGFHFileInfo(info)
			}
		}
	}
}

func scanPreloaderGFH(data []byte) []mtkGFHRecord {
	var out []mtkGFHRecord
	for pos := 0; pos+8 <= len(data); pos++ {
		if data[pos] != 'M' || data[pos+1] != 'M' || data[pos+2] != 'M' {
			continue
		}
		size := binary.LittleEndian.Uint16(data[pos+4 : pos+6])
		typ := binary.LittleEndian.Uint16(data[pos+6 : pos+8])
		if size < 8 || int(pos)+int(size) > len(data) {
			continue
		}
		name := ""
		if typ == 0 && int(pos)+20 <= len(data) {
			name = trimNULASCII(data[pos+8 : pos+20])
		}
		out = append(out, mtkGFHRecord{Offset: uint64(pos), Type: typ, Size: size, Name: name})
		pos += int(size) - 1
	}
	return out
}

func parsePreloaderGFHFileInfo(data []byte, offset uint64) (mtkPreloaderFileInfo, bool) {
	if offset+0x38 > uint64(len(data)) {
		return mtkPreloaderFileInfo{}, false
	}
	raw := data[offset : offset+0x38]
	return mtkPreloaderFileInfo{
		Offset:        offset,
		FileType:      binary.LittleEndian.Uint16(raw[24:26]),
		FlashDev:      raw[26],
		SigType:       raw[27],
		LoadAddr:      binary.LittleEndian.Uint32(raw[28:32]),
		FileLen:       binary.LittleEndian.Uint32(raw[32:36]),
		MaxSize:       binary.LittleEndian.Uint32(raw[36:40]),
		ContentOffset: binary.LittleEndian.Uint32(raw[40:44]),
		SigLen:        binary.LittleEndian.Uint32(raw[44:48]),
		JumpOffset:    binary.LittleEndian.Uint32(raw[48:52]),
		Attr:          binary.LittleEndian.Uint32(raw[52:56]),
	}, true
}

func findPreloaderGFHFileInfo(data []byte) (mtkPreloaderFileInfo, bool) {
	for _, rec := range scanPreloaderGFH(data) {
		if rec.Name == "FILE_INFO" {
			return parsePreloaderGFHFileInfo(data, rec.Offset)
		}
	}
	return mtkPreloaderFileInfo{}, false
}

func printPreloaderGFHFileInfo(info mtkPreloaderFileInfo) {
	fmt.Printf("      file-info: type=0x%x flash-dev=0x%x sig=0x%x load=0x%08x file-len=0x%x max=0x%x content=0x%x sig-len=0x%x jump=0x%x attr=0x%x\n",
		info.FileType, info.FlashDev, info.SigType, info.LoadAddr, info.FileLen, info.MaxSize, info.ContentOffset, info.SigLen, info.JumpOffset, info.Attr)
	fmt.Printf("      implied entry: 0x%08x\n", info.LoadAddr+info.JumpOffset)
}

func printPreloaderBootStrings(data []byte) {
	needles := []string{"UBOOT", "BOOTIMG", "RECOVERY", "ANDROID", "SEC_RO", "SECCFG"}
	fmt.Println("  boot-chain strings:")
	for _, needle := range needles {
		offsets := findASCIIOffsets(data, needle, 6)
		if len(offsets) == 0 {
			continue
		}
		fmt.Printf("    %-8s %s\n", needle, formatHexOffsets(offsets))
	}
	if bytes.Contains(data, []byte(`load "%s" from 0x%llx (dev) to 0x%x (mem)`)) {
		fmt.Println("  loader model: partition-name based load path is present; live PMT/partition offsets matter.")
	}
	fmt.Println("  suggested MVII profile:")
	fmt.Println("    lk-partition: UBOOT")
	fmt.Println("    bootimg-partition: BOOTIMG")
	fmt.Printf("    scatter fallback UBOOT: 0x%x\n", j36UltraScatterLKOffset)
	fmt.Printf("    scatter fallback BOOTIMG: 0x%x\n", j36UltraScatterBootImageOffset)
}

func printPreloaderHardcodedPartitions(data []byte) mtkPreloaderPartitionTable {
	table := scanPreloaderHardcodedPartitions(data)
	if len(table.Partitions) == 0 {
		fmt.Println("  hardcoded partition table: not found")
		return table
	}
	fmt.Printf("  hardcoded partition table @0x%x (address base 0x%x):\n", table.Offset, table.AddressBase)
	for _, part := range table.Partitions {
		fmt.Printf("    %-16s start=0x%08x size=0x%08x\n", part.Name, part.Offset, part.Size)
	}
	return table
}

func scanPreloaderHardcodedPartitions(data []byte) mtkPreloaderPartitionTable {
	bases := preloaderAddressBaseCandidates(data)
	nameOffsets := findASCIIOffsets(data, "PRELOADER", 64)
	best := mtkPreloaderPartitionTable{}
	bestScore := 0
	for _, base := range bases {
		for _, nameOffset := range nameOffsets {
			if base+nameOffset > 0xffffffff {
				continue
			}
			var ptr [4]byte
			binary.LittleEndian.PutUint32(ptr[:], uint32(base+nameOffset))
			for start := 0; start < len(data); {
				pos := bytes.Index(data[start:], ptr[:])
				if pos < 0 {
					break
				}
				tableOffset := uint64(start + pos)
				parts := parsePreloaderPartitionTable(data, tableOffset, base)
				score := scorePreloaderPartitionTable(parts)
				if score > bestScore {
					bestScore = score
					best = mtkPreloaderPartitionTable{Offset: tableOffset, AddressBase: base, Partitions: parts}
				}
				start += pos + 4
			}
		}
	}
	return best
}

func preloaderAddressBaseCandidates(data []byte) []uint64 {
	candidates := []uint64{0x200d00}
	if info, ok := findPreloaderGFHFileInfo(data); ok && info.LoadAddr != 0 {
		load := uint64(info.LoadAddr)
		candidates = append(candidates, load)
		if load >= info.Offset {
			candidates = append(candidates, load-info.Offset)
		}
		if info.ContentOffset != 0 && load >= uint64(info.ContentOffset) {
			candidates = append(candidates, load-uint64(info.ContentOffset))
		}
	}
	return uniqueUint64s(candidates)
}

func parsePreloaderPartitionTable(data []byte, offset, base uint64) []mtkPreloaderHardcodedPartition {
	var parts []mtkPreloaderHardcodedPartition
	for i := 0; i < 128; i++ {
		entry := offset + uint64(i*24)
		if entry+24 > uint64(len(data)) {
			break
		}
		raw := data[entry : entry+24]
		namePtr := uint64(binary.LittleEndian.Uint32(raw[0:4]))
		if namePtr < base {
			break
		}
		nameOffset := namePtr - base
		if nameOffset >= uint64(len(data)) {
			break
		}
		nameEnd := nameOffset + 64
		if nameEnd > uint64(len(data)) {
			nameEnd = uint64(len(data))
		}
		name := strings.ToUpper(trimNULASCII(data[nameOffset:nameEnd]))
		if name == "" || len(name) > 31 {
			break
		}
		start := uint64(binary.LittleEndian.Uint32(raw[4:8]))
		size := uint64(binary.LittleEndian.Uint32(raw[8:12]))
		parts = append(parts, mtkPreloaderHardcodedPartition{
			Name:   name,
			Offset: start,
			Size:   size,
			Source: fmt.Sprintf("preloader-table@0x%x", offset),
		})
		if name == "FAT" || name == "USRDATA" {
			break
		}
	}
	if scorePreloaderPartitionTable(parts) == 0 {
		return nil
	}
	return parts
}

func scorePreloaderPartitionTable(parts []mtkPreloaderHardcodedPartition) int {
	score := 0
	seen := map[string]bool{}
	for _, part := range parts {
		switch part.Name {
		case "PRELOADER", "MBR", "EBR1", "UBOOT", "BOOTIMG", "RECOVERY", "ANDROID", "CACHE", "USRDATA", "FAT":
			if !seen[part.Name] {
				score++
				seen[part.Name] = true
			}
		}
	}
	if seen["UBOOT"] {
		score += 4
	}
	if seen["BOOTIMG"] {
		score += 4
	}
	if len(parts) > 64 {
		score -= len(parts) - 64
	}
	if score < 3 {
		return 0
	}
	return score
}

func buildMTKPreloaderProfileFromAnalysis(source string, data []byte, table mtkPreloaderPartitionTable) *mtkPreloaderProfile {
	profile := &mtkPreloaderProfile{
		Target:           "j36-ultra",
		Source:           source,
		HWCode:           mtkHexUint64(mtkHWCodeMT6592),
		EMMCUserBytes:    mtkHexUint64(j36UltraEMMCUserBytes),
		LKPartition:      "UBOOT",
		BootimgPartition: "BOOTIMG",
		LKLoadAddress:    mtkHexUint64(0x81e00000),
		Stage2Address:    mtkHexUint64(0x81e20000),
		Notes: []string{
			"Editable MVII model of the MediaTek preloader load path; this is not a full DRAM-training BOOT1 replacement.",
			"Feed writes use this profile when -mtk-preloader-profile is passed.",
		},
	}
	if len(table.Partitions) != 0 {
		for _, part := range table.Partitions {
			if part.Name == "PRELOADER" || part.Size == 0 {
				continue
			}
			profile.Partitions = append(profile.Partitions, mtkPreloaderProfilePartition{
				Name:   part.Name,
				Offset: mtkHexUint64(part.Offset),
				Size:   mtkHexUint64(part.Size),
				Source: part.Source,
			})
		}
	} else {
		profile.Partitions = []mtkPreloaderProfilePartition{
			{Name: "UBOOT", Offset: mtkHexUint64(j36UltraScatterLKOffset), Size: mtkHexUint64(j36UltraScatterLKSize), Source: "scatter-fallback"},
			{Name: "BOOTIMG", Offset: mtkHexUint64(j36UltraScatterBootImageOffset), Size: mtkHexUint64(j36UltraScatterBootImageSize), Source: "scatter-fallback"},
		}
	}
	if info, ok := findPreloaderGFHFileInfo(data); ok && info.LoadAddr != 0 {
		profile.Notes = append(profile.Notes, fmt.Sprintf("Stock preloader SRAM load=0x%x entry=0x%x.", info.LoadAddr, info.LoadAddr+info.JumpOffset))
	}
	return profile
}

func uniqueUint64s(values []uint64) []uint64 {
	seen := map[uint64]bool{}
	out := make([]uint64, 0, len(values))
	for _, value := range values {
		if seen[value] {
			continue
		}
		seen[value] = true
		out = append(out, value)
	}
	return out
}

func findASCIIOffsets(data []byte, needle string, limit int) []uint64 {
	var out []uint64
	n := []byte(needle)
	for start := 0; start < len(data); {
		pos := bytes.Index(data[start:], n)
		if pos < 0 {
			break
		}
		out = append(out, uint64(start+pos))
		if len(out) >= limit {
			break
		}
		start += pos + len(n)
	}
	return out
}

func formatHexOffsets(offsets []uint64) string {
	parts := make([]string, 0, len(offsets))
	for _, offset := range offsets {
		parts = append(parts, fmt.Sprintf("0x%x", offset))
	}
	return strings.Join(parts, ", ")
}
