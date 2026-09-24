package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

type mtkHexUint64 uint64

type mtkPreloaderProfile struct {
	Target           string                         `json:"target,omitempty"`
	Source           string                         `json:"source,omitempty"`
	HWCode           mtkHexUint64                   `json:"hw_code,omitempty"`
	EMMCUserBytes    mtkHexUint64                   `json:"emmc_user_bytes,omitempty"`
	LKPartition      string                         `json:"lk_partition,omitempty"`
	BootimgPartition string                         `json:"bootimg_partition,omitempty"`
	LKLoadAddress    mtkHexUint64                   `json:"lk_load_address,omitempty"`
	Stage2Address    mtkHexUint64                   `json:"stage2_address,omitempty"`
	Partitions       []mtkPreloaderProfilePartition `json:"partitions,omitempty"`
	Notes            []string                       `json:"notes,omitempty"`
}

type mtkPreloaderProfilePartition struct {
	Name   string       `json:"name"`
	Offset mtkHexUint64 `json:"offset"`
	Size   mtkHexUint64 `json:"size"`
	Source string       `json:"source,omitempty"`
}

func (v *mtkHexUint64) UnmarshalJSON(raw []byte) error {
	raw = bytes.TrimSpace(raw)
	if len(raw) == 0 || bytes.Equal(raw, []byte("null")) {
		*v = 0
		return nil
	}
	text := string(raw)
	if raw[0] == '"' {
		var s string
		if err := json.Unmarshal(raw, &s); err != nil {
			return err
		}
		text = strings.TrimSpace(s)
	}
	if text == "" {
		*v = 0
		return nil
	}
	n, err := strconv.ParseUint(text, 0, 64)
	if err != nil {
		return fmt.Errorf("parse %q as u64: %w", text, err)
	}
	*v = mtkHexUint64(n)
	return nil
}

func (v mtkHexUint64) MarshalJSON() ([]byte, error) {
	return []byte(strconv.Quote(fmt.Sprintf("0x%x", uint64(v)))), nil
}

func loadMTKPreloaderProfile(path string) (*mtkPreloaderProfile, error) {
	resolved, err := resolveMTKPreloaderProfilePath(path)
	if err != nil {
		return nil, err
	}
	raw, err := os.ReadFile(resolved)
	if err != nil {
		return nil, fmt.Errorf("read preloader profile %s: %w", resolved, err)
	}
	var profile mtkPreloaderProfile
	if err := json.Unmarshal(raw, &profile); err != nil {
		return nil, fmt.Errorf("parse preloader profile %s: %w", resolved, err)
	}
	if profile.Source == "" {
		profile.Source = resolved
	}
	return &profile, nil
}

func resolveMTKPreloaderProfilePath(path string) (string, error) {
	path = strings.TrimSpace(path)
	if path == "" {
		return "", errorsNewNoPreloaderProfile()
	}
	if !strings.EqualFold(path, "auto") {
		return path, nil
	}
	candidates := []string{
		"mtk-preloader-profile.json",
		"preloader-profile.json",
		filepath.Join("tools", "mtk-da", "mtk-preloader-profile.json"),
		filepath.Join("tools", "mtk-da", "preloader-profile.json"),
	}
	for _, candidate := range candidates {
		if fileExists(candidate) {
			return candidate, nil
		}
	}
	return "", errorsNewNoPreloaderProfile()
}

func errorsNewNoPreloaderProfile() error {
	return fmt.Errorf("no MTK preloader profile found; pass -mtk-preloader-profile /path/to/profile.json")
}

func configuredMTKPreloaderPartitions(cfg config) ([]mtkLivePartition, error) {
	profile, err := configuredMTKPreloaderProfile(cfg)
	if profile == nil || err != nil {
		return nil, err
	}
	parts := profileLivePartitions(profile)
	if len(parts) == 0 {
		return nil, fmt.Errorf("preloader profile %s does not define any partitions", profile.Source)
	}
	return parts, nil
}

func configuredMTKPreloaderProfile(cfg config) (*mtkPreloaderProfile, error) {
	path := strings.TrimSpace(cfg.mtkPreloadProfile)
	if path == "" {
		return nil, nil
	}
	if !strings.EqualFold(path, "auto") && !filepath.IsAbs(path) && !fileExists(path) && cfg.root != "" {
		candidate := filepath.Join(cfg.root, path)
		if fileExists(candidate) {
			path = candidate
		}
	}
	profile, err := loadMTKPreloaderProfile(path)
	if err != nil {
		return nil, err
	}
	return profile, nil
}

func profileLivePartitions(profile *mtkPreloaderProfile) []mtkLivePartition {
	if profile == nil {
		return nil
	}
	source := profile.Source
	if source == "" {
		source = "preloader-profile"
	}
	parts := make([]mtkLivePartition, 0, len(profile.Partitions))
	for _, part := range profile.Partitions {
		name := strings.ToUpper(strings.TrimSpace(part.Name))
		offset := uint64(part.Offset)
		size := uint64(part.Size)
		if name == "" || size == 0 {
			continue
		}
		partSource := part.Source
		if partSource == "" {
			partSource = source
		}
		parts = append(parts, mtkLivePartition{Name: name, Offset: offset, Size: size, Source: partSource})
	}
	return parts
}

func profilePartitionForImage(cfg config, image string) (string, bool) {
	if isJ36LKSlotImagePath(cfg, image) {
		return "UBOOT", true
	}
	if isJ36BootSlotImagePath(cfg, image) {
		return "BOOTIMG", true
	}
	return "", false
}

func preloaderProfileTargetOffset(cfg config, image string) (uint64, bool, error) {
	role, ok := profilePartitionForImage(cfg, image)
	if !ok || strings.TrimSpace(cfg.mtkPreloadProfile) == "" {
		return 0, false, nil
	}
	profile, err := configuredMTKPreloaderProfile(cfg)
	if err != nil {
		return 0, false, err
	}
	part, name, ok := findMTKPreloaderProfilePartition(profile, role)
	if !ok {
		return 0, false, fmt.Errorf("preloader profile does not define %s for %s", name, filepath.Base(image))
	}
	return part.Offset, true, nil
}

func validateMTKPreloaderProfileFeedTarget(cfg config, image string, targetOffset, transferLength uint64) error {
	role, ok := profilePartitionForImage(cfg, image)
	if !ok || strings.TrimSpace(cfg.mtkPreloadProfile) == "" {
		return nil
	}
	profile, err := configuredMTKPreloaderProfile(cfg)
	if err != nil {
		return err
	}
	part, name, ok := findMTKPreloaderProfilePartition(profile, role)
	if !ok {
		return fmt.Errorf("preloader profile does not define %s for %s", name, filepath.Base(image))
	}
	if targetOffset != part.Offset {
		return fmt.Errorf("%s targets %s, but preloader profile puts %s at raw offset 0x%x; use -raw-offset 0x%x or update the profile",
			image, name, part.Name, part.Offset, part.Offset)
	}
	if transferLength > part.Size {
		return fmt.Errorf("%s transfer length 0x%x exceeds preloader profile %s size 0x%x",
			image, transferLength, part.Name, part.Size)
	}
	return nil
}

func applyMTKPreloaderProfileOffsets(plans []mtkFeedWritePlan, profile *mtkPreloaderProfile) {
	if profile == nil {
		return
	}
	for i := range plans {
		role := strings.ToUpper(strings.TrimSpace(plans[i].PartitionName))
		if role == "" {
			continue
		}
		part, name, ok := findMTKPreloaderProfilePartition(profile, role)
		if !ok {
			fmt.Printf("Warning: preloader profile does not define %s; keeping planned offset 0x%x for %s.\n",
				name, plans[i].TargetOffset, plans[i].Label)
			continue
		}
		if plans[i].PartitionDelta > part.Size || plans[i].TransferLength > part.Size-plans[i].PartitionDelta {
			fmt.Printf("Warning: preloader profile %s at 0x%x is only 0x%x bytes; keeping planned offset 0x%x for %s delta 0x%x length 0x%x.\n",
				part.Name, part.Offset, part.Size, plans[i].TargetOffset, plans[i].Label, plans[i].PartitionDelta, plans[i].TransferLength)
			continue
		}
		target := part.Offset + plans[i].PartitionDelta
		if plans[i].TargetOffset != target {
			fmt.Printf("Using preloader profile %s offset 0x%x + delta 0x%x instead of 0x%x for %s.\n",
				part.Name, part.Offset, plans[i].PartitionDelta, plans[i].TargetOffset, plans[i].Label)
			plans[i].TargetOffset = target
		}
	}
}

func findMTKPreloaderProfilePartition(profile *mtkPreloaderProfile, role string) (mtkLivePartition, string, bool) {
	name := profilePartitionName(profile, role)
	parts := profileLivePartitions(profile)
	part, ok := findMTKLivePartition(parts, name)
	if !ok && strings.EqualFold(name, "UBOOT") {
		part, ok = findMTKLivePartition(parts, "LK")
	}
	return part, name, ok
}

func profilePartitionName(profile *mtkPreloaderProfile, role string) string {
	role = strings.ToUpper(strings.TrimSpace(role))
	if profile != nil {
		switch role {
		case "UBOOT", "LK":
			if name := strings.ToUpper(strings.TrimSpace(profile.LKPartition)); name != "" {
				return name
			}
		case "BOOTIMG":
			if name := strings.ToUpper(strings.TrimSpace(profile.BootimgPartition)); name != "" {
				return name
			}
		}
	}
	return role
}

func writeMTKPreloaderProfile(path string, profile *mtkPreloaderProfile) error {
	if profile == nil {
		return fmt.Errorf("no preloader profile to write")
	}
	raw, err := json.MarshalIndent(profile, "", "  ")
	if err != nil {
		return err
	}
	raw = append(raw, '\n')
	if dir := filepath.Dir(path); dir != "" && dir != "." {
		if err := os.MkdirAll(dir, 0o755); err != nil {
			return fmt.Errorf("create preloader profile directory %s: %w", dir, err)
		}
	}
	if err := os.WriteFile(path, raw, 0o644); err != nil {
		return fmt.Errorf("write preloader profile %s: %w", path, err)
	}
	return nil
}
