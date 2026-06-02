// Package dario wraps the C ./infer_v4 binary and the dario equation REPL's
// HTTP API. It is the only package that knows about subprocess invocation
// and the line-based stdout protocol of infer_v4.
package dario

import (
	"bytes"
	"context"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"github.com/ariannamethod/dario/internal/voices"
)

// InferRequest captures one call to ./infer_v4.
type InferRequest struct {
	Voice       voices.Voice
	WeightsPath string  // resolved absolute path
	Prompt      string  // user-facing prompt (e.g. "What is resonance?")
	MaxTokens   int     // generation length cap
	Temp        float64 // sampling temperature (>0)
	TopK        int     // top-k cutoff (0 = disabled)
	Seed        int     // RNG seed (0 = use binary's wall-clock default)
	RepPenalty  float64 // repetition penalty (0 = binary default 1.3)
	ChatTokens  bool    // wrap prompt in Janus BOS/USER/ASST chat tokens (SFT voices)
	Timeout     time.Duration
}

// InferResult is the parsed output of one ./infer_v4 invocation.
type InferResult struct {
	Text  string // generated text, with the surrounding stats stripped
	Stats string // last "[janus..." stats line, if present
	Raw   string // raw stdout for debugging
	Cmd   string // the command line that was actually run
}

// FindBinary locates the infer_v4 binary. It looks first in `scriptDir`,
// then on $PATH, then in $DARIO_HOME (if set).
func FindBinary(scriptDir string) (string, error) {
	candidates := []string{filepath.Join(scriptDir, "infer_v4")}
	if home := os.Getenv("DARIO_HOME"); home != "" {
		candidates = append(candidates, filepath.Join(home, "infer_v4"))
	}
	for _, c := range candidates {
		if _, err := os.Stat(c); err == nil {
			return c, nil
		}
	}
	if p, err := exec.LookPath("infer_v4"); err == nil {
		return p, nil
	}
	return "", fmt.Errorf("infer_v4 not found (looked in %v)", candidates)
}

// Run spawns ./infer_v4 with the canonical CLI:
//
//	./infer_v4 weights.bin "prompt" max_tokens temp [seed] [top_k]
//
// and parses the "--- generation ---" delimiter the way dario_infer.py
// did. The C binary writes the user-facing text to stdout between
// "--- generation ---\n" and a terminal "[janus..." stats line.
func Run(ctx context.Context, binary string, req InferRequest) (*InferResult, error) {
	if req.MaxTokens <= 0 {
		req.MaxTokens = 80
	}
	if req.Temp <= 0 {
		req.Temp = 0.6
	}
	if req.Timeout <= 0 {
		req.Timeout = 120 * time.Second
	}

	args := []string{
		req.WeightsPath,
		req.Prompt,
		strconv.Itoa(req.MaxTokens),
		strconv.FormatFloat(req.Temp, 'f', -1, 64),
	}
	// infer_v4 CLI is positional: [seed] [top_k] both optional. To pass
	// top_k we must also pass seed. Use the supplied seed or 0 (= binary
	// uses wall clock).
	if req.TopK > 0 || req.Seed != 0 {
		args = append(args, strconv.Itoa(req.Seed))
		if req.TopK > 0 {
			args = append(args, strconv.Itoa(req.TopK))
		}
	}
	// P0.5 flags (scanned by infer_v4 regardless of position).
	if req.RepPenalty > 0 {
		args = append(args, "--rep-penalty", strconv.FormatFloat(req.RepPenalty, 'f', -1, 64))
	}
	if req.ChatTokens {
		args = append(args, "--chat-tokens")
	}

	rctx, cancel := context.WithTimeout(ctx, req.Timeout)
	defer cancel()

	cmd := exec.CommandContext(rctx, binary, args...)
	var stdout, stderr bytes.Buffer
	cmd.Stdout = &stdout
	cmd.Stderr = &stderr

	cmdline := binary + " " + strings.Join(args, " ")
	if err := cmd.Run(); err != nil {
		return nil, fmt.Errorf("infer_v4 failed: %w\nstderr: %s\ncmd: %s",
			err, stderr.String(), cmdline)
	}

	raw := stdout.String()
	res := &InferResult{Raw: raw, Cmd: cmdline}

	const sep = "--- generation ---\n"
	if idx := strings.Index(raw, sep); idx >= 0 {
		gen := raw[idx+len(sep):]
		// Trim trailing stats line ("[janus-v4] N tokens, X tok/s ...").
		gen = strings.TrimRight(gen, "\n")
		lines := strings.Split(gen, "\n")
		if n := len(lines); n > 0 && strings.HasPrefix(strings.TrimSpace(lines[n-1]), "[janus") {
			res.Stats = strings.TrimSpace(lines[n-1])
			lines = lines[:n-1]
		}
		res.Text = strings.TrimSpace(strings.Join(lines, "\n"))
	} else {
		// Fallback: emit the whole stdout as text. Useful when infer_v4
		// crashes mid-generation.
		res.Text = strings.TrimSpace(raw)
	}
	return res, nil
}
