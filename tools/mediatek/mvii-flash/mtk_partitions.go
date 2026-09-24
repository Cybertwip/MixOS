package main

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"path/filepath"
	"strings"
	"unicode/utf16"
)

type mtkLivePartition struct {
	Name   string
	Offset uint64
	Size   uint64
	Source string
}

type mbrPartitionEntry struct {
	Index    int
	Type     byte
	Offset   uint64
	Size     uint64
	Extended bool
}

func readPartitionsMTKFeed(cfg config) error {
	feedCfg := cfg
	if strings.TrimSpace(feedCfg.mtkFeedPayload) == "" {
		feedCfg.mtkFeedPayload = "auto"
	}
	payload, err := loadMTKFeedPayload(feedCfg)
	if err != nil {
		return err
	}

	fmt.Printf("Using MTK feed payload for live partition read: %s (%s, 0x%x bytes)\n",
		payload.Path, payload.Kind, len(payload.Data))
	fmt.Printf("Loading payload to 0x%x, entry 0x%x\n", payload.LoadAddr, payload.EntryAddr)
	fmt.Println("Reading live eMMC USER partition metadata; no flash writes will be performed.")

	client, err := connectMTKSerialForFeedReadOnly(feedCfg.device)
	if err != nil {
		return err
	}
	defer func() {
		_ = client.port.Close()
	}()
	hello := client.feedPayloadHello
	if client.feedPayloadReady {
		fmt.Println("Reusing existing MVIIFlash payload; no reset or payload upload needed.")
	} else {
		if err := client.disableMT6592Watchdog(); err != nil {
			fmt.Printf("Warning: could not disable MT6592 watchdog before feed payload boot: %v\n", err)
		}
		if err := client.sendDA(payload.LoadAddr, 0, payload.Data); err != nil {
			return fmt.Errorf("send feed payload to 0x%x: %w", payload.LoadAddr, err)
		}
		if err := client.jumpDA(payload.EntryAddr); err != nil {
			fmt.Printf("JUMP_DA reported %v - waiting for the feed payload protocol anyway.\n", err)
		}

		hello, err = client.waitMTKFeedHello()
		if err != nil {
			return err
		}
	}
	if hello.Name != "" {
		fmt.Printf("Feed payload ready: %s\n", hello.Name)
	} else {
		fmt.Println("Feed payload ready.")
	}
	stopInterruptReset := installMTKFeedInterruptReset(client)
	defer stopInterruptReset()

	parts, seq, err := client.readMTKFeedLivePartitions(1)
	if err != nil {
		return err
	}

	// Augment with full list from scatter file (if present in the package root).
	// This gives the real names the user expects (protect_f, protect_s, 2, 3, 4, ...)
	// even when the live eMMC has no (or unfindable) MTK PMT name table.
	if sparts := tryLoadScatterPartitions(cfg); len(sparts) > 0 {
		for _, sp := range sparts {
			replaced := false
			for i := range parts {
				if parts[i].Offset == sp.Offset {
					// The scatter has the authoritative name (e.g. PROTECT_F vs the
					// generic MBR2 we derived from the on-disk MBR), so prefer it.
					parts[i].Name = sp.Name
					if sp.Size != 0 {
						parts[i].Size = sp.Size
					}
					parts[i].Source = sp.Source
					replaced = true
					break
				}
			}
			if !replaced {
				parts = append(parts, sp)
			}
		}
	}

	printMTKLivePartitions(parts)
	_ = seq
	fmt.Println("Leaving MVIIFlash payload running for more read-only probes (use -reuse on next read for speed). Power-cycle the board to return to BROM if needed.")
	return nil
}

func (c *mtkSerialClient) readMTKFeedLivePartitions(seq uint32) ([]mtkLivePartition, uint32, error) {
	const tailRead = uint32(0x10000)

	// Discover actual eMMC USER size if possible. This is critical because the
	// hardcoded j36 size may not match this particular board's eMMC, causing
	// the tail PMT probe to read past the end or the wrong place.
	actualSize, dseq := c.discoverEMMCUserSize(seq)
	seq = dseq
	if actualSize > (64 * 1024 * 1024) {
		// use discovered for tail calc
	}

	// Try multiple possible tail locations. Use discovered size if we have a
	// plausible one, plus the known j36 value and a few others.
	baseTail := j36UltraEMMCUserBytes - uint64(tailRead)
	if actualSize > (64 * 1024 * 1024) {
		baseTail = actualSize - uint64(tailRead)
	}
	candidateTails := []uint64{
		baseTail,
		j36UltraEMMCUserBytes - uint64(tailRead),
		0xe7000000 - uint64(tailRead),
		0x100000000 - uint64(tailRead),
		0x80000000 - uint64(tailRead),
	}
	seen := map[uint64]bool{}
	for _, t := range candidateTails {
		if t < 0x1000000 || seen[t] {
			continue
		}
		seen[t] = true
		tailData, nextSeq, err := c.readMTKFeedBytes(seq, t, tailRead, mtkLegacyEMMCPartUser, fmt.Sprintf("eMMC USER PMT tail@0x%x", t))
		seq = nextSeq
		if err == nil {
			if parts := parseMTKPMTPartitions(tailData, t); len(parts) != 0 {
				fmt.Printf("Live PMT detected near eMMC USER tail (0x%x).\n", t)
				return parts, seq, nil
			}
		}
	}

	// Also do a broader backward scan from the (discovered or known) end in 1MB
	// steps over the last ~32MB. This catches cases where PMT is not exactly at
	// the very last 64k or the size guess is slightly off.
	searchEnd := j36UltraEMMCUserBytes
	if actualSize > searchEnd {
		searchEnd = actualSize
	}
	for off := searchEnd - uint64(tailRead); off > searchEnd-0x2000000 && off > 0x1000000; off -= 0x100000 {
		if seen[off] {
			continue
		}
		seen[off] = true
		tailData, nextSeq, err := c.readMTKFeedBytes(seq, off, tailRead, mtkLegacyEMMCPartUser, fmt.Sprintf("eMMC USER PMT scan@0x%x", off))
		seq = nextSeq
		if err == nil {
			if parts := parseMTKPMTPartitions(tailData, off); len(parts) != 0 {
				fmt.Printf("Live PMT detected during scan (0x%x).\n", off)
				return parts, seq, nil
			}
		}
	}

	// Scan the low area (first 8 MiB) for PMT magic as well. On some boards
	// a copy of the PMT or the table is stored early in USER.
	for off := uint64(0); off < 0x800000; off += 0x10000 {
		if seen[off] {
			continue
		}
		seen[off] = true
		lowData, nextSeq, err := c.readMTKFeedBytes(seq, off, tailRead, mtkLegacyEMMCPartUser, fmt.Sprintf("low PMT scan@0x%x", off))
		seq = nextSeq
		if err == nil {
			if parts := parseMTKPMTPartitions(lowData, off); len(parts) != 0 {
				fmt.Printf("Live PMT detected in low area (0x%x).\n", off)
				return parts, seq, nil
			}
		}
	}

	// Read a conservative head area (small enough to avoid geometry errors on some
	// payload/BROM handoffs and PIO modes). 0x4000 is enough for MBR + GPT header.
	const headRead = uint32(0x4000)
	head, nextSeq, err := c.readMTKFeedBytes(seq, 0, headRead, mtkLegacyEMMCPartUser, "eMMC USER head (MBR/GPT/PMT search)")
	if err != nil {
		// Some payloads (especially right after BROM reset into "USER PIO") reject
		// reads at LBA 0 with "geometry is invalid" even for small lengths.
		// This is not fatal for partition discovery — we fall back to the
		// well-known scatter offsets for UBOOT/BOOTIMG which is what you
		// actually need for flashing on this target.
		if strings.Contains(err.Error(), "geometry is invalid") ||
			strings.Contains(err.Error(), "READ offset/length") {
			fmt.Printf("Warning: payload rejected read at offset 0 (%v). Using scatter fallbacks for known UBOOT/BOOTIMG locations.\n", err)
			head = nil
			seq = nextSeq
		} else {
			return nil, nextSeq, err
		}
	} else {
		seq = nextSeq
	}

	// Search the head we actually got (may be nil or tiny on geometry errors).
	if len(head) > 0 {
		if parts := parseMTKPMTPartitions(head, 0); len(parts) != 0 {
			fmt.Println("Live PMT detected in head area.")
			return parts, seq, nil
		}
	}

	// Try GPT from whatever we have.
	var gptParts []mtkLivePartition
	if len(head) > 0 {
		gptParts = parseGPTPartitions(head)
	}
	if len(gptParts) == 0 && len(head) >= 1024 && bytes.Equal(head[512:512+8], []byte("EFI PART")) {
		// Try to fetch entries separately (targeted read, not at 0).
		hdr := head[512:1024]
		partLba := binary.LittleEndian.Uint64(hdr[72:80])
		numParts := binary.LittleEndian.Uint32(hdr[80:84])
		partEntrySz := binary.LittleEndian.Uint32(hdr[84:88])
		if partEntrySz == 128 && numParts > 0 && partLba > 0 {
			entriesLen := numParts * 128
			if entriesLen > 0x8000 {
				entriesLen = 0x8000
			}
			entriesOff := partLba * 512
			entriesData, n2, rerr := c.readMTKFeedBytes(seq, entriesOff, entriesLen, mtkLegacyEMMCPartUser, "GPT partition entries")
			seq = n2
			if rerr == nil {
				fake := make([]byte, 1024+len(entriesData))
				copy(fake[512:1024], hdr)
				copy(fake[1024:], entriesData)
				gptParts = parseGPTPartitions(fake)
			}
		}
	}
	if len(gptParts) != 0 {
		fmt.Println("GPT partition table detected.")
		return gptParts, seq, nil
	}

	mbrBuf := head
	if len(mbrBuf) > 0x4000 {
		mbrBuf = mbrBuf[:0x4000]
	}
	parts, nextSeq := c.readMBRLikePartitions(seq, mbrBuf)
	seq = nextSeq

	// (MBR content inspection for friendlier names could be added here using
	// in-memory versions of the header probes if desired.)

	// Always supplement with the known good scatter offsets for this target.
	// This is the most important information for flashing LK / boot images.
	parts = append(parts,
		mtkLivePartition{Name: "UBOOT", Offset: j36UltraScatterLKOffset, Size: j36UltraScatterLKSize, Source: "scatter-fallback"},
		mtkLivePartition{Name: "BOOTIMG", Offset: j36UltraScatterBootImageOffset, Size: j36UltraScatterBootImageSize, Source: "scatter-fallback"},
	)

	if len(parts) == 0 {
		// Should not happen now because of the append above.
		return nil, seq, fmt.Errorf("no partition information available")
	}
	if len(head) == 0 || !bytes.Equal(head[510:512], []byte{0x55, 0xaa}) {
		fmt.Println("No live PMT name table found; using scatter fallbacks + any MBR data that was readable.")
	} else {
		fmt.Println("No live PMT name table found; sector 0 has an MBR-style table without MediaTek partition names.")
	}
	return parts, seq, nil
}

func (c *mtkSerialClient) discoverEMMCUserSize(seq uint32) (uint64, uint32) {
	// Binary search for the highest offset where we can successfully read a sector
	// via the feed payload. This lets us compute a good tail for PMT search
	// without relying only on the hardcoded j36 value.
	const blk = uint64(512)
	low := uint64(0)
	high := uint64(0x200000000) // 8 GiB upper bound guess
	lastGood := uint64(0)
	probes := 0
	maxProbes := 40 // don't go crazy over slow serial
	for low <= high && probes < maxProbes {
		probes++
		mid := low + (high-low)/2
		off := (mid / blk) * blk
		_, nseq, err := c.readMTKFeedBytes(seq, off, 512, mtkLegacyEMMCPartUser, "size probe")
		seq = nseq
		if err == nil {
			lastGood = off + blk
			low = off + blk
		} else {
			if high < blk {
				break
			}
			high = off - blk
		}
	}
	return lastGood, seq
}

func (c *mtkSerialClient) readMTKFeedBytes(seq uint32, offset uint64, length uint32, part byte, label string) ([]byte, uint32, error) {
	read := buildMTKFeedReadPayload(offset, length, uint32(part), mtkSerialBlockSize)
	if err := c.writeMTKFeedFrame(mtkFeedFrameRead, seq, read); err != nil {
		return nil, seq, fmt.Errorf("send feed READ for %s: %w", label, err)
	}
	data, err := c.waitMTKFeedReadDataAt(seq, label, offset, length)
	if err != nil {
		return nil, seq + 1, err
	}
	return data, seq + 1, nil
}

func parseMTKPMTPartitions(data []byte, baseOffset uint64) []mtkLivePartition {
	var out []mtkLivePartition
	for pos := 0; pos+8+96 <= len(data); pos += mtkSerialBlockSize {
		magic := data[pos : pos+4]
		if !bytes.Equal(magic, []byte("3vTP")) && !bytes.Equal(magic, []byte("PTv3")) &&
			!bytes.Equal(magic, []byte("MPT3")) && !bytes.Equal(magic, []byte("3TPM")) {
			continue
		}
		for entry := pos + 8; entry+96 <= len(data); entry += 96 {
			name := trimNULASCII(data[entry : entry+64])
			if name == "" {
				break
			}
			size := binary.LittleEndian.Uint64(data[entry+64 : entry+72])
			offset := binary.LittleEndian.Uint64(data[entry+80 : entry+88])
			if size == 0 || offset >= j36UltraEMMCUserBytes || offset+size < offset {
				continue
			}
			out = append(out, mtkLivePartition{
				Name:   strings.ToUpper(name),
				Offset: offset,
				Size:   size,
				Source: fmt.Sprintf("PMT@0x%x", baseOffset+uint64(pos)),
			})
		}
		if len(out) != 0 {
			return out
		}
	}
	return nil
}

func (c *mtkSerialClient) readMBRLikePartitions(seq uint32, head []byte) ([]mtkLivePartition, uint32) {
	primary, extended := parseMBRPrimaryPartitions(head)
	out := append([]mtkLivePartition(nil), primary...)
	for _, ext := range extended {
		ebrOffset := ext.Offset
		seen := map[uint64]bool{}
		for chain := 0; chain < 128; chain++ {
			if ebrOffset == 0 || ebrOffset >= j36UltraEMMCUserBytes || seen[ebrOffset] {
				break
			}
			seen[ebrOffset] = true
			ebr, nextSeq, err := c.readMTKFeedBytes(seq, ebrOffset, mtkSerialBlockSize, mtkLegacyEMMCPartUser,
				fmt.Sprintf("EBR chain at 0x%x", ebrOffset))
			seq = nextSeq
			if err != nil {
				fmt.Printf("Warning: could not read EBR chain at 0x%x: %v\n", ebrOffset, err)
				break
			}
			logical, next := parseEBRPartitions(ebr, ext.Offset, ebrOffset, chain)
			out = append(out, logical...)
			if next == 0 {
				break
			}
			ebrOffset = next
		}
	}
	return out, seq
}

func parseMBRLikePartitions(data []byte) []mtkLivePartition {
	parts, _ := parseMBRPrimaryPartitions(data)
	return parts
}

func parseMBRPrimaryPartitions(data []byte) ([]mtkLivePartition, []mbrPartitionEntry) {
	if len(data) < 512 || data[510] != 0x55 || data[511] != 0xaa {
		return nil, nil
	}
	var parts []mtkLivePartition
	var extended []mbrPartitionEntry
	for i := 0; i < 4; i++ {
		entry, ok := parseMBRPartitionEntry(data[446+i*16:446+(i+1)*16], i+1, 0)
		if !ok {
			continue
		}
		if entry.Extended {
			extended = append(extended, entry)
			continue
		}
		parts = append(parts, mtkLivePartition{
			Name:   fmt.Sprintf("MBR%d:type%02x", entry.Index, entry.Type),
			Offset: entry.Offset,
			Size:   entry.Size,
			Source: "MBR",
		})
	}
	return parts, extended
}

func parseEBRPartitions(data []byte, extendedBase uint64, ebrOffset uint64, chain int) ([]mtkLivePartition, uint64) {
	if len(data) < 512 || data[510] != 0x55 || data[511] != 0xaa {
		return nil, 0
	}
	var out []mtkLivePartition
	var next uint64
	for i := 0; i < 4; i++ {
		base := ebrOffset
		entry, ok := parseMBRPartitionEntry(data[446+i*16:446+(i+1)*16], i+1, base)
		if !ok {
			continue
		}
		if entry.Extended {
			link, ok := parseMBRPartitionEntry(data[446+i*16:446+(i+1)*16], i+1, extendedBase)
			if ok && link.Offset != ebrOffset {
				next = link.Offset
			}
			continue
		}
		out = append(out, mtkLivePartition{
			Name:   fmt.Sprintf("EBR%02d-%d:type%02x", chain+1, i+1, entry.Type),
			Offset: entry.Offset,
			Size:   entry.Size,
			Source: fmt.Sprintf("EBR@0x%x", ebrOffset),
		})
	}
	return out, next
}

func parseMBRPartitionEntry(raw []byte, index int, baseOffset uint64) (mbrPartitionEntry, bool) {
	if len(raw) < 16 {
		return mbrPartitionEntry{}, false
	}
	partType := raw[4]
	firstLBA := binary.LittleEndian.Uint32(raw[8:12])
	sectors := binary.LittleEndian.Uint32(raw[12:16])
	if partType == 0 || sectors == 0 {
		return mbrPartitionEntry{}, false
	}
	offset := baseOffset + uint64(firstLBA)*mtkSerialBlockSize
	size := uint64(sectors) * mtkSerialBlockSize
	if offset >= j36UltraEMMCUserBytes || size == 0 {
		return mbrPartitionEntry{}, false
	}
	if !mbrPartitionTypeIsExtended(partType) && size > j36UltraEMMCUserBytes-offset {
		return mbrPartitionEntry{}, false
	}
	return mbrPartitionEntry{
		Index:    index,
		Type:     partType,
		Offset:   offset,
		Size:     size,
		Extended: mbrPartitionTypeIsExtended(partType),
	}, true
}

func mbrPartitionTypeIsExtended(partType byte) bool {
	return partType == 0x05 || partType == 0x0f || partType == 0x85
}

func applyMTKLivePartitionOffsets(plans []mtkFeedWritePlan, parts []mtkLivePartition) {
	for i := range plans {
		name := strings.ToUpper(strings.TrimSpace(plans[i].PartitionName))
		if name == "" {
			continue
		}
		part, ok := findMTKLivePartition(parts, name)
		if !ok && name == "UBOOT" {
			part, ok = findMTKLivePartition(parts, "LK")
		}
		if !ok {
			continue
		}
		if plans[i].PartitionDelta > part.Size || plans[i].TransferLength > part.Size-plans[i].PartitionDelta {
			fmt.Printf("Warning: live %s partition at 0x%x is only 0x%x bytes; keeping planned offset 0x%x for %s delta 0x%x length 0x%x.\n",
				part.Name, part.Offset, part.Size, plans[i].TargetOffset, plans[i].Label, plans[i].PartitionDelta, plans[i].TransferLength)
			continue
		}
		target := part.Offset + plans[i].PartitionDelta
		if plans[i].TargetOffset != target {
			fmt.Printf("Using live %s offset from %s: 0x%x + delta 0x%x instead of scatter fallback 0x%x for %s.\n",
				part.Name, part.Source, part.Offset, plans[i].PartitionDelta, plans[i].TargetOffset, plans[i].Label)
			plans[i].TargetOffset = target
		}
	}
}

func findMTKLivePartition(parts []mtkLivePartition, name string) (mtkLivePartition, bool) {
	name = strings.ToUpper(strings.TrimSpace(name))
	for _, part := range parts {
		if strings.EqualFold(part.Name, name) {
			return part, true
		}
	}
	return mtkLivePartition{}, false
}

func printMTKLivePartitions(parts []mtkLivePartition) {
	if len(parts) == 0 {
		fmt.Println("No live partitions found.")
		return
	}
	fmt.Println("Live eMMC USER partition map:")
	for _, part := range parts {
		fmt.Printf("  %-20s offset=0x%08x size=0x%08x (%s)\n", part.Name, part.Offset, part.Size, part.Source)
	}
	if uboot, ok := findMTKLivePartition(parts, "UBOOT"); ok {
		fmt.Printf("  preloader UBOOT target: offset=0x%x size=0x%x\n", uboot.Offset, uboot.Size)
	}
	if bootimg, ok := findMTKLivePartition(parts, "BOOTIMG"); ok {
		fmt.Printf("  Android BOOTIMG target: offset=0x%x size=0x%x\n", bootimg.Offset, bootimg.Size)
	}
}

func trimNULASCII(raw []byte) string {
	if idx := bytes.IndexByte(raw, 0); idx >= 0 {
		raw = raw[:idx]
	}
	s := strings.TrimSpace(string(raw))
	for _, r := range s {
		if r < 0x20 || r > 0x7e {
			return ""
		}
	}
	return s
}

// tryLoadScatterPartitions loads named partitions from a scatter file in the
// given root (if any) so that -partitions and -part NAME can surface the
// names the user knows (protect_f, protect_s, 2, 3, 4, ...).
func tryLoadScatterPartitions(cfg config) []mtkLivePartition {
	var cands []string
	if cfg.mtkScatter != "" {
		cands = append(cands, cfg.mtkScatter)
	}
	if cfg.root != "" {
		cands = append(cands,
			filepath.Join(cfg.root, "MT6592_Android_scatter.txt"),
			filepath.Join(cfg.root, "scatter.txt"),
			filepath.Join(cfg.root, "MTK_Android_scatter.txt"),
		)
	}
	for _, p := range cands {
		if !fileExists(p) {
			continue
		}
		ents, err := parseMTKScatterFile(p)
		if err != nil {
			continue
		}
		var res []mtkLivePartition
		for _, e := range ents {
			// List every EMMC_USER partition, including non-download ones such as
			// PROTECT_F / PROTECT_S / NVRAM (file_name NONE). They are still real
			// partitions the user wants to see and read.
			reg := strings.ToUpper(strings.TrimSpace(e.Region))
			if reg != "EMMC_USER" {
				continue
			}
			nm := strings.ToUpper(strings.TrimSpace(e.PartitionName))
			if nm == "" {
				continue
			}
			res = append(res, mtkLivePartition{
				Name:   nm,
				Offset: e.LinearStart,
				Size:   e.PartitionSize,
				Source: "scatter",
			})
		}
		if len(res) > 0 {
			return res
		}
	}
	return nil
}

func decodeUTF16LE(b []byte) string {
	if len(b) == 0 {
		return ""
	}
	if len(b)%2 != 0 {
		b = b[:len(b)-1]
	}
	codes := make([]uint16, len(b)/2)
	for i := range codes {
		codes[i] = binary.LittleEndian.Uint16(b[i*2 : i*2+2])
	}
	for i, c := range codes {
		if c == 0 {
			codes = codes[:i]
			break
		}
	}
	return string(utf16.Decode(codes))
}

func parseGPTPartitions(data []byte) []mtkLivePartition {
	if len(data) < 1024 {
		return nil
	}
	// GPT header at LBA 1 (offset 512)
	hdr := data[512:1024]
	if !bytes.Equal(hdr[0:8], []byte("EFI PART")) {
		return nil
	}
	partLba := binary.LittleEndian.Uint64(hdr[72:80])
	numParts := binary.LittleEndian.Uint32(hdr[80:84])
	partEntrySize := binary.LittleEndian.Uint32(hdr[84:88])
	if partEntrySize != 128 {
		return nil
	}
	entriesOff := int(partLba * 512)
	if entriesOff >= len(data) {
		return nil
	}
	maxEntries := (len(data) - entriesOff) / int(partEntrySize)
	if uint32(maxEntries) > numParts {
		maxEntries = int(numParts)
	}
	var out []mtkLivePartition
	for i := 0; i < maxEntries; i++ {
		eoff := entriesOff + i*128
		if eoff+128 > len(data) {
			break
		}
		ent := data[eoff : eoff+128]
		// type GUID all zero => unused
		zero := true
		for j := 0; j < 16; j++ {
			if ent[j] != 0 {
				zero = false
				break
			}
		}
		if zero {
			continue
		}
		startLBA := binary.LittleEndian.Uint64(ent[32:40])
		endLBA := binary.LittleEndian.Uint64(ent[40:48])
		if endLBA < startLBA {
			continue
		}
		sz := (endLBA - startLBA + 1) * 512
		off := startLBA * 512
		name := decodeUTF16LE(ent[56 : 56+72])
		name = strings.ToUpper(strings.TrimSpace(name))
		if name == "" {
			name = fmt.Sprintf("GPT%d", i+1)
		}
		out = append(out, mtkLivePartition{
			Name:   name,
			Offset: off,
			Size:   sz,
			Source: "GPT",
		})
	}
	return out
}
