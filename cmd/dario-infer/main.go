// dario-infer — Go replacement for dario_infer.py.
//
// It is a thin spawner around the C ./infer_v4 binary: pick a voice
// (which selects the right weights file and sampling defaults), build
// the canonical Q/A prompt, run inference, print the generated text.
//
// Unlike the Python original this binary does NOT need tiktoken or any
// external tokenizer file: infer_v4 carries its own BPE tables baked
// into the C source (janus_v4_bpe_merges.h, leo_bpe_merges.h), so it
// accepts a raw text prompt directly.
//
// Usage:
//
//	./dario-infer --voice leo "What is resonance?"
//	./dario-infer --voice arianna --temp 0.5 "Tell me about the Method"
//	./dario-infer --voice yent --max-tokens 100 "Who are you?"
//	./dario-infer --voice resonance-yent --temp 0.7 "What lives between the lines?"
//	./dario-infer --voice leo24m --weights /path/to/some.bin "..."
package main

import (
	"context"
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/ariannamethod/dario/internal/dario"
	"github.com/ariannamethod/dario/internal/voices"
)

func main() {
	if err := run(os.Args[1:]); err != nil {
		fmt.Fprintln(os.Stderr, "error:", err)
		os.Exit(1)
	}
}

func run(argv []string) error {
	fs := flag.NewFlagSet("dario-infer", flag.ContinueOnError)
	fs.Usage = func() {
		fmt.Fprintf(os.Stderr, `dario-infer: Go wrapper around the C ./infer_v4 binary.

usage:
  dario-infer [flags] "prompt text"

flags:
  --voice NAME       voice to speak through (default leo)
                     valid: %s
  --weights PATH     override the resolved weights file
  --temp F           sampling temperature (default = voice default)
  --topk N           top-k cutoff (0 = disabled, default = voice default)
  --max-tokens N     max generation length (default 80)
  --seed N           RNG seed for infer_v4 (default 0 = wall clock)
  --binary PATH      override path to the infer_v4 binary
  --timeout DUR      kill inference after this long (default 2m)
  --raw              print raw infer_v4 stdout (do not strip stats)
  --help

env:
  DARIO_WEIGHTS_DIR  search root for weight files when --weights is unset
  DARIO_HOME         search root for infer_v4 when --binary is unset

`, strings.Join(voices.Names(), ", "))
	}
	var (
		voice     = fs.String("voice", "leo", "")
		weights   = fs.String("weights", "", "")
		tempF     = fs.Float64("temp", -1, "")
		topK      = fs.Int("topk", -1, "")
		maxTokens = fs.Int("max-tokens", 80, "")
		seed      = fs.Int("seed", 0, "")
		binary    = fs.String("binary", "", "")
		timeout   = fs.Duration("timeout", 2*time.Minute, "")
		raw       = fs.Bool("raw", false, "")
		// Python's `--max` / `-n` / `-v` shorthands kept for drop-in compatibility.
	)
	fs.IntVar(maxTokens, "max", 80, "")
	fs.IntVar(maxTokens, "n", 80, "")
	fs.Float64Var(tempF, "t", -1, "")
	fs.StringVar(voice, "v", "leo", "")

	if err := fs.Parse(argv); err != nil {
		return err
	}

	prompt := strings.Join(fs.Args(), " ")
	if prompt == "" {
		prompt = "What is resonance?"
	}

	v, err := voices.Lookup(*voice)
	if err != nil {
		return err
	}

	scriptDir, err := scriptDirOrCWD()
	if err != nil {
		return err
	}

	binPath := *binary
	if binPath == "" {
		binPath, err = dario.FindBinary(scriptDir)
		if err != nil {
			return err
		}
	}

	weightsPath, err := voices.ResolveWeights(v, *weights, scriptDir)
	if err != nil {
		return err
	}

	temp := v.Temp
	if *tempF >= 0 {
		temp = *tempF
	}
	tk := v.TopK
	if *topK >= 0 {
		tk = *topK
	}

	// Build the same Q/A wrapping that dario_infer.py used (so the model
	// receives the prompt format it was trained on).
	full := fmt.Sprintf("Q: %s\nA:", prompt)

	ctx := context.Background()
	res, err := dario.Run(ctx, binPath, dario.InferRequest{
		Voice:       v,
		WeightsPath: weightsPath,
		Prompt:      full,
		MaxTokens:   *maxTokens,
		Temp:        temp,
		TopK:        tk,
		Seed:        *seed,
		Timeout:     *timeout,
	})
	if err != nil {
		return err
	}

	if *raw {
		fmt.Print(res.Raw)
		return nil
	}

	fmt.Printf("\n[%s] %s\n", v.Name, res.Text)
	if res.Stats != "" {
		fmt.Printf("\n%s\n", res.Stats)
	}
	return nil
}

// scriptDirOrCWD returns the directory of the running binary, falling
// back to the current working directory when the executable path can't
// be resolved (e.g. running via `go run`).
func scriptDirOrCWD() (string, error) {
	if exe, err := os.Executable(); err == nil {
		// Resolve symlinks so $HOME/bin/dario-infer points at the real cmd dir.
		if real, err := filepath.EvalSymlinks(exe); err == nil {
			exe = real
		}
		// Common layout: bin/dario-infer alongside the C dario.c at repo root.
		repo := filepath.Dir(filepath.Dir(exe))
		if hasInferV4(repo) {
			return repo, nil
		}
		// Fall back to the directory containing the binary.
		if hasInferV4(filepath.Dir(exe)) {
			return filepath.Dir(exe), nil
		}
	}
	cwd, err := os.Getwd()
	if err != nil {
		return "", err
	}
	return cwd, nil
}

func hasInferV4(dir string) bool {
	_, err := os.Stat(filepath.Join(dir, "infer_v4"))
	return err == nil
}
