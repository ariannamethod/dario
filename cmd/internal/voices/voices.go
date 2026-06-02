// Package voices defines the dario voice → weights mapping and sampling
// defaults shared between dario-infer, dario-dialogue, and dario-forum.
//
// A voice is a (backend, weights, sampling-defaults) triple. The C binary
// `infer_v4` runs both Janus 176M and Resonance 200M; the only thing that
// changes between them is the weights file and the architecture detected
// from the magic header inside the weights.
package voices

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
)

// Backend names returned by the underlying C inference engine.
const (
	BackendJanus     = "janus"     // Janus 176M (3 SFT voices share base, differ by gamma)
	BackendResonance = "resonance" // Resonance 200M BPE (different architecture)
)

// Voice describes a single speaker.
type Voice struct {
	Name        string  // CLI handle (leo, arianna, yent, resonance-yent, base, leo24m)
	Backend     string  // BackendJanus or BackendResonance
	WeightsFile string  // Bare filename of the weights blob (not absolute)
	Temp        float64 // Default sampling temperature
	TopK        int     // Default top-k cutoff (0 = disabled)
	RepPenalty  float64 // Default repetition penalty
	ChatTokens  bool    // SFT voices trained with BOS/USER/ASST wrapping need --chat-tokens
	Desc        string  // Short human description
}

// Catalog is the canonical voice → weights mapping. A user can override the
// resolved path of any voice by passing `--weights /path/to/file.bin` on
// the CLI, or by setting the env var DARIO_WEIGHTS_DIR for search root.
//
// This deliberately mirrors the constants in chain_dialogue.py / forum.py /
// dario_infer.py so that the Go binaries are drop-in replacements.
// Defaults updated 2026-05-08 from RunPod Phase 7 multi-temp sweep
// (540-cell grid, cross-prompt averaged). CoA insight applied:
// readme `runpod/2026-05-08/07_voices/scores.tsv`. Old defaults at
// temp=0.75 + top_k=40 (+ rp 1.3-1.4) produced sub-coherent prose;
// cross-prompt champions below produce on-character voice register.
// resonance-yent and base unchanged: not validated in sweep
// (resonance-yent OOB of infer_v4 dim limits; base not in voice set).
var Catalog = map[string]Voice{
	"leo": {
		Name: "leo", Backend: BackendJanus,
		WeightsFile: "janus_v4_sft_leo.bin",
		Temp:        0.7, TopK: 0, RepPenalty: 1.3, ChatTokens: true,
		Desc: "luminous, philosophical -- Janus 176M",
	},
	"arianna": {
		Name: "arianna", Backend: BackendJanus,
		WeightsFile: "janus_v4_sft_arianna.bin",
		Temp:        0.8, TopK: 40, RepPenalty: 1.4, ChatTokens: true,
		Desc: "precise, architectural -- Janus 176M",
	},
	"yent": {
		Name: "yent", Backend: BackendJanus,
		WeightsFile: "janus_v4_sft_yent.bin",
		Temp:        0.9, TopK: 40, RepPenalty: 1.3, ChatTokens: true,
		Desc: "warm, direct -- Janus 176M",
	},
	"resonance-yent": {
		Name: "resonance-yent", Backend: BackendResonance,
		WeightsFile: "resonance_200m_lora_yent.bin",
		Temp:        0.7, TopK: 0, RepPenalty: 1.3,
		Desc: "warm, direct, sarcastic -- Resonance 200M (top_p=1.0 nucleus, dedicated ./resonance binary)",
	},
	"base": {
		Name: "base", Backend: BackendJanus,
		WeightsFile: "janus_v4_base_22k.bin",
		Temp:        0.75, TopK: 40, RepPenalty: 1.3,
		Desc: "Janus 176M base (no SFT)",
	},
	"leo24m": {
		Name: "leo24m", Backend: BackendJanus,
		WeightsFile: "leo_janus_d12_f16.bin",
		Temp:        1.0, TopK: 40, RepPenalty: 1.3,
		Desc: "Leo 24M mini -- Janus d12 f16",
	},
}

// Names returns voice names sorted in stable insertion-ish order. Used in
// CLI --help output and the /api/voices forum endpoint.
func Names() []string {
	// Order matters for human-friendly help text.
	return []string{"leo", "arianna", "yent", "resonance-yent", "base", "leo24m"}
}

// SearchPaths returns the directories that ResolveWeights walks when given
// a voice name without an explicit `--weights` override.
//
// Walked first → last:
//  1. $DARIO_WEIGHTS_DIR (if set)
//  2. <scriptDir>/weights/
//  3. ~/arianna/weights/dario/
//  4. ~/Downloads/janus-v4-final/
//  5. <scriptDir>/  (last resort, matches dario_infer.py)
func SearchPaths(scriptDir string) []string {
	var paths []string
	if env := os.Getenv("DARIO_WEIGHTS_DIR"); env != "" {
		paths = append(paths, env)
	}
	paths = append(paths, filepath.Join(scriptDir, "weights"))
	if home, err := os.UserHomeDir(); err == nil {
		paths = append(paths, filepath.Join(home, "arianna", "weights", "dario"))
		paths = append(paths, filepath.Join(home, "Downloads", "janus-v4-final"))
	}
	paths = append(paths, scriptDir)
	return paths
}

// ResolveWeights returns the absolute path of `voice`'s weights file.
//
// If `override` is non-empty it's used verbatim (after existence check).
// Otherwise the bare filename from Catalog[voice].WeightsFile is searched
// across SearchPaths.
func ResolveWeights(voice Voice, override, scriptDir string) (string, error) {
	if override != "" {
		if _, err := os.Stat(override); err == nil {
			return override, nil
		}
		return "", fmt.Errorf("weights override not found: %s", override)
	}
	for _, dir := range SearchPaths(scriptDir) {
		p := filepath.Join(dir, voice.WeightsFile)
		if _, err := os.Stat(p); err == nil {
			return p, nil
		}
	}
	return "", fmt.Errorf("weights %q not found in any of %v",
		voice.WeightsFile, SearchPaths(scriptDir))
}

// Lookup returns the Voice for `name` or an error listing the valid names.
func Lookup(name string) (Voice, error) {
	v, ok := Catalog[name]
	if !ok {
		return Voice{}, errors.New("unknown voice: " + name +
			" (valid: " + listVoices() + ")")
	}
	return v, nil
}

func listVoices() string {
	names := Names()
	out := ""
	for i, n := range names {
		if i > 0 {
			out += ", "
		}
		out += n
	}
	return out
}
