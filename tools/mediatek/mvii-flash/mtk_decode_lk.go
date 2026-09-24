package main

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
)

func decodeLKCommand(cfg config) error {
	// Resolve the LK binary path
	lkPath := strings.TrimSpace(cfg.mtkDecodeLK)
	if lkPath == "" || strings.EqualFold(lkPath, "auto") {
		// Auto-discover lk.bin
		candidates := []string{
			filepath.Join("Hardware", "Virtua", "loader", "mtk-da", "lk.bin"),
			filepath.Join(cfg.root, "tools", "mtk-da", "lk.bin"),
			filepath.Join(cfg.root, "lk.bin"),
		}
		for _, candidate := range candidates {
			if fileExists(candidate) {
				lkPath = candidate
				break
			}
		}
		if lkPath == "" || strings.EqualFold(lkPath, "auto") {
			return fmt.Errorf("could not auto-discover lk.bin; pass -mtk-decode-lk /path/to/lk.bin")
		}
	}

	if !fileExists(lkPath) {
		return fmt.Errorf("LK binary not found: %s", lkPath)
	}

	// Resolve the Python decode script
	decodeScript := resolveDecodeLKScript(cfg)
	if !fileExists(decodeScript) {
		return fmt.Errorf("decode script not found: %s", decodeScript)
	}

	// Resolve python3
	python, err := resolvePython3()
	if err != nil {
		return fmt.Errorf("python3 is required for lk.bin decode: %w", err)
	}

	fmt.Printf("Decoding LK binary: %s\n", lkPath)
	fmt.Printf("Script: %s\n", decodeScript)
	fmt.Println()

	// Run the decode script
	cmd := exec.Command(python, decodeScript, lkPath)
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	cmd.Stdin = os.Stdin
	if err := cmd.Run(); err != nil {
		return fmt.Errorf("decode script failed: %w", err)
	}

	return nil
}

func resolveDecodeLKScript(cfg config) string {
	// Try to find the script relative to the flash binary root
	candidates := []string{
		filepath.Join("Hardware", "Virtua", "loader", "mtk-da", "re", "decode_lk.py"),
		filepath.Join(cfg.root, "tools", "mtk-da", "re", "decode_lk.py"),
		filepath.Join(cfg.root, "..", "Hardware", "Virtua", "loader", "mtk-da", "re", "decode_lk.py"),
	}

	// Also search upward from cwd and root
	for _, base := range []string{cfg.root, currentWorkingDirectory()} {
		candidates = append(candidates, findUpwardCandidates(base, filepath.Join("Hardware", "Virtua", "loader", "mtk-da", "re", "decode_lk.py"))...)
	}

	for _, candidate := range candidates {
		if fileExists(candidate) {
			return candidate
		}
	}

	// Fallback: the script is in the same repo as the flash tool
	// Try relative path from the flash binary's typical location
	return filepath.Join("Hardware", "Virtua", "loader", "mtk-da", "re", "decode_lk.py")
}

func resolvePython3() (string, error) {
	for _, name := range []string{"python3", "python"} {
		path, err := exec.LookPath(name)
		if err == nil {
			return path, nil
		}
	}
	return "", fmt.Errorf("python3 not found in PATH")
}