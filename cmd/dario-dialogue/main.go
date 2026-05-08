// dario-dialogue — Go replacement for chain_dialogue.py.
//
// Modes:
//   chain      one voice; KK injects between sentences for `depth` rounds
//   dialogue   interactive REPL: user → voice (with KK) → user → ...
//   duet       two voices, two goroutines, channel-coordinated turns
//   trialogue  three voices, three goroutines, round-robin
//
// Each utterance is also POSTed to the running dario equation REPL
// (./dario --web 3001) when one is reachable, so the C field absorbs
// the conversation and the next prompt can be primed by field-words.
//
// Usage examples:
//
//   ./dario-dialogue --mode chain --voice leo --topic "What is resonance?"
//   ./dario-dialogue --mode dialogue --voice arianna
//   ./dario-dialogue --mode duet --voice leo --voice2 yent --topic "soul formula"
//   ./dario-dialogue --mode duet --voice yent --voice2 resonance-yent --topic "what is yent"
//   ./dario-dialogue --mode trialogue --voice leo --voice2 yent --voice3 arianna --topic "patterns"
//
// The duet/trialogue goroutines are coordinated by a single `turn chan TurnMsg`:
// a voice's worker blocks on its inbound mailbox, runs inference, then
// hands the floor to the next worker. KK absorption and dario field
// POSTs happen inside the worker, before the next turn is dispatched.
package main

import (
	"bufio"
	"context"
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"github.com/ariannamethod/dario/internal/dario"
	"github.com/ariannamethod/dario/internal/kk"
	"github.com/ariannamethod/dario/internal/voices"
)

func main() {
	if err := run(os.Args[1:]); err != nil {
		fmt.Fprintln(os.Stderr, "error:", err)
		os.Exit(1)
	}
}

type config struct {
	Mode       string
	Voice      string
	Voice2     string
	Voice3     string
	Topic      string
	Depth      int
	MaxTokens  int
	Temp       float64
	TopK       int
	RepPenalty float64
	Knowledge  knowledgeFlag
	Save       string
	WeightsDir string
	Field      string // dario --web base URL
	Timeout    time.Duration
}

type knowledgeFlag []string

func (k *knowledgeFlag) String() string     { return strings.Join(*k, ",") }
func (k *knowledgeFlag) Set(v string) error { *k = append(*k, v); return nil }

func run(argv []string) error {
	fs := flag.NewFlagSet("dario-dialogue", flag.ContinueOnError)
	fs.Usage = printUsage(fs)

	cfg := config{}
	fs.StringVar(&cfg.Mode, "mode", "chain", "")
	fs.StringVar(&cfg.Voice, "voice", "leo", "")
	fs.StringVar(&cfg.Voice2, "voice2", "yent", "")
	fs.StringVar(&cfg.Voice3, "voice3", "arianna", "")
	fs.StringVar(&cfg.Topic, "topic", "What is resonance?", "")
	// Aliases for compatibility with the Python CLI.
	fs.StringVar(&cfg.Topic, "prompt", "What is resonance?", "")
	fs.StringVar(&cfg.Topic, "seed", "What is resonance?", "")
	fs.IntVar(&cfg.Depth, "depth", 6, "")
	fs.IntVar(&cfg.MaxTokens, "max-segment", 200, "")
	fs.IntVar(&cfg.MaxTokens, "max-tokens", 200, "")
	fs.Float64Var(&cfg.Temp, "temperature", -1, "")
	fs.Float64Var(&cfg.Temp, "temp", -1, "")
	fs.IntVar(&cfg.TopK, "top-k", -1, "")
	fs.IntVar(&cfg.TopK, "topk", -1, "")
	fs.Float64Var(&cfg.RepPenalty, "rep-penalty", -1, "")
	fs.Var(&cfg.Knowledge, "knowledge", "knowledge file (repeatable)")
	fs.StringVar(&cfg.Save, "save", "", "")
	fs.StringVar(&cfg.WeightsDir, "weights-dir", "", "")
	fs.StringVar(&cfg.Field, "field", "http://localhost:3001",
		"dario equation REPL base URL (--web)")
	fs.DurationVar(&cfg.Timeout, "timeout", 2*time.Minute, "")

	if err := fs.Parse(argv); err != nil {
		return err
	}
	if cfg.WeightsDir != "" {
		// Honour --weights-dir by setting the env var the voices package reads.
		_ = os.Setenv("DARIO_WEIGHTS_DIR", cfg.WeightsDir)
	}

	scriptDir, err := scriptDirOrCWD()
	if err != nil {
		return err
	}

	binPath, err := dario.FindBinary(scriptDir)
	if err != nil {
		return err
	}

	// Build runner state.
	st, err := newSession(cfg, scriptDir, binPath)
	if err != nil {
		return err
	}

	// Default knowledge files mirror chain_dialogue.py's defaults.
	if len(cfg.Knowledge) == 0 {
		cfg.Knowledge = knowledgeFlag{
			filepath.Join(scriptDir, "dario_essay.txt"),
			filepath.Join(scriptDir, "leo_expanded.txt"),
		}
	}
	for _, p := range cfg.Knowledge {
		if _, err := os.Stat(p); err != nil {
			fmt.Fprintf(os.Stderr, "[kk] WARN: %s not found, skipping\n", p)
			continue
		}
		n, err := st.kk.Ingest(p)
		if err != nil {
			fmt.Fprintf(os.Stderr, "[kk] %s: %v\n", p, err)
			continue
		}
		fmt.Fprintf(os.Stderr, "[kk] %s: %d chunks\n", filepath.Base(p), n)
	}
	if st.kk.NumChunks() == 0 {
		fmt.Fprintln(os.Stderr, "[kk] WARNING: no knowledge loaded")
	} else {
		fmt.Fprintf(os.Stderr, "[kk] %d chunks ready\n", st.kk.NumChunks())
	}

	switch cfg.Mode {
	case "chain":
		return st.chainMode()
	case "dialogue":
		return st.dialogueMode()
	case "duet":
		return st.duetMode()
	case "trialogue":
		return st.trialogueMode()
	default:
		return fmt.Errorf("unknown --mode %q (chain|dialogue|duet|trialogue)", cfg.Mode)
	}
}

// session bundles everything two or three voices need to share.
type session struct {
	cfg       config
	scriptDir string
	binPath   string
	kk        *kk.Kernel
	field     *dario.FieldClient
	fieldUp   bool

	// Resolved Voice + weights for each handle.
	voices map[string]resolvedVoice
}

type resolvedVoice struct {
	V       voices.Voice
	Weights string
	Temp    float64
	TopK    int
	RepPen  float64
}

func newSession(cfg config, scriptDir, binPath string) (*session, error) {
	st := &session{
		cfg:       cfg,
		scriptDir: scriptDir,
		binPath:   binPath,
		kk:        kk.New(),
		field:     dario.NewFieldClient(cfg.Field),
		voices:    make(map[string]resolvedVoice),
	}
	// Probe field availability quickly so we can degrade gracefully.
	probeCtx, cancel := context.WithTimeout(context.Background(), 500*time.Millisecond)
	defer cancel()
	st.fieldUp = st.field.Available(probeCtx)
	if st.fieldUp {
		fmt.Fprintf(os.Stderr, "[field] %s — alive (KK absorption enabled)\n", cfg.Field)
	} else {
		fmt.Fprintf(os.Stderr, "[field] %s — not reachable, KK-only mode\n", cfg.Field)
	}

	// Resolve voices upfront so all errors surface before any inference.
	needed := []string{cfg.Voice}
	switch cfg.Mode {
	case "duet":
		needed = append(needed, cfg.Voice2)
	case "trialogue":
		needed = append(needed, cfg.Voice2, cfg.Voice3)
	}
	for _, name := range needed {
		if _, ok := st.voices[name]; ok {
			continue
		}
		v, err := voices.Lookup(name)
		if err != nil {
			return nil, err
		}
		w, err := voices.ResolveWeights(v, "", scriptDir)
		if err != nil {
			return nil, err
		}
		rv := resolvedVoice{V: v, Weights: w, Temp: v.Temp, TopK: v.TopK, RepPen: v.RepPenalty}
		// CLI overrides apply to all voices, mirroring the Python CLI.
		if cfg.Temp > 0 {
			rv.Temp = cfg.Temp
		}
		if cfg.TopK >= 0 {
			rv.TopK = cfg.TopK
		}
		if cfg.RepPenalty > 0 {
			rv.RepPen = cfg.RepPenalty
		}
		st.voices[name] = rv
		fmt.Fprintf(os.Stderr, "[voice] %s (%s) — %s\n", v.Name, v.Backend, v.Desc)
		fmt.Fprintf(os.Stderr, "[weights] %s\n", w)
	}
	return st, nil
}

// speak runs one inference round for `voiceName` with the supplied
// prompt. The returned text is the cleaned generation, which will also
// be POSTed to the dario field if available.
func (s *session) speak(ctx context.Context, voiceName, prompt string, maxTokens int) (string, error) {
	rv := s.voices[voiceName]
	full := buildPrompt(rv.V.Backend, prompt)
	res, err := dario.Run(ctx, s.binPath, dario.InferRequest{
		Voice: rv.V, WeightsPath: rv.Weights,
		Prompt: full, MaxTokens: maxTokens,
		Temp: rv.Temp, TopK: rv.TopK, Timeout: s.cfg.Timeout,
	})
	if err != nil {
		return "", err
	}
	text := strings.TrimSpace(res.Text)
	return text, nil
}

func buildPrompt(backend, prompt string) string {
	// Both backends respond well to Q/A framing through infer_v4.
	// (The Janus chat tokens used by chain_dialogue.py only matter at
	// tokenization time; the C binary handles its own encoding.)
	_ = backend
	return fmt.Sprintf("Q: %s\nA:", prompt)
}

// absorb feeds `text` to the kernel and (if reachable) to the dario
// field via POST /api/chat. Errors are non-fatal — we just log them.
func (s *session) absorb(text, source string) {
	if text == "" {
		return
	}
	added := s.kk.Absorb(text, source)
	if added > 0 {
		fmt.Fprintf(os.Stderr, "  [kk+%d from %s]\n", added, source)
	}
	if !s.fieldUp {
		return
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if metrics, err := s.field.Chat(ctx, text); err == nil {
		fmt.Fprintf(os.Stderr,
			"  [field] dom=%s diss=%.2f res=%.2f ent=%.2f step=%d\n",
			metrics.DominantName, metrics.Dissonance,
			metrics.Resonance, metrics.Entropy, metrics.Step)
	}
}

// ===================================================================
// CHAIN MODE
// ===================================================================

type chainEntry struct {
	Step      int
	Type      string // "injection" / "kk_exhausted" / "max_tokens"
	Text      string
	Injection string
}

func (s *session) chainMode() error {
	fmt.Println(strings.Repeat("=", 60))
	fmt.Printf("  CHAIN DIALOGUE — depth %d\n", s.cfg.Depth)
	fmt.Println(strings.Repeat("=", 60))

	rv := s.voices[s.cfg.Voice]
	fmt.Printf("[seed] %s\n[voice] %s (%s)\n\n", s.cfg.Topic, rv.V.Name, rv.V.Backend)

	s.kk.Reset()
	t0 := time.Now()

	prompt := s.cfg.Topic
	prime, _ := s.kk.Pick(s.cfg.Topic, kk.PickOpts{
		PreferSource: "dario_essay.txt",
	})
	if prime != "" {
		fmt.Printf("  [prime] %s\n\n", truncate(prime, 80))
		prompt = prime + " " + s.cfg.Topic
	}

	var log []chainEntry
	var narrative []string
	ctx := context.Background()

	for step := 0; step < s.cfg.Depth; step++ {
		text, err := s.speak(ctx, s.cfg.Voice, prompt, s.cfg.MaxTokens)
		if err != nil {
			return err
		}
		if text != "" {
			fmt.Println(text)
			narrative = append(narrative, text)
		}

		// KK injection for next round, queried on (topic + tail of segment).
		queryText := s.cfg.Topic + " " + tail(text, 200)
		injection, _ := s.kk.Pick(queryText, kk.PickOpts{})

		if injection == "" {
			log = append(log, chainEntry{Step: step, Type: "kk_exhausted", Text: truncate(text, 100)})
			fmt.Printf("\n[chain] KK exhausted at step %d\n", step)
			break
		}
		log = append(log, chainEntry{Step: step, Type: "injection",
			Text: truncate(text, 100), Injection: injection})
		fmt.Printf("\n  [-> step %d] %s\n\n", step+1, truncate(injection, 80))
		prompt = injection
	}

	elapsed := time.Since(t0)
	full := strings.Join(narrative, "\n")
	fmt.Println()
	fmt.Println(strings.Repeat("=", 60))
	fmt.Println("  CHAIN COMPLETE")
	fmt.Println(strings.Repeat("=", 60))
	fmt.Printf("  steps:   %d\n  time:    %.1fs\n", len(log), elapsed.Seconds())
	for _, e := range log {
		if e.Injection != "" {
			fmt.Printf("    [%d] %s\n", e.Step+1, truncate(e.Injection, 70))
		}
	}

	if s.cfg.Save != "" {
		if err := saveTranscript(s.cfg.Save, s.cfg, full, log); err != nil {
			return err
		}
		fmt.Printf("[saved] %s\n", s.cfg.Save)
	}
	return nil
}

// ===================================================================
// DIALOGUE MODE
// ===================================================================

func (s *session) dialogueMode() error {
	fmt.Println(strings.Repeat("=", 60))
	fmt.Printf("  DIALOGUE — %s + Knowledge Kernel\n", s.cfg.Voice)
	fmt.Printf("  KK: %d chunks. Type 'quit' to exit.\n", s.kk.NumChunks())
	fmt.Println(strings.Repeat("=", 60))

	in := bufio.NewReader(os.Stdin)
	prevAnswer := ""
	turn := 0
	ctx := context.Background()

	for {
		fmt.Print("you> ")
		line, err := in.ReadString('\n')
		if err != nil {
			fmt.Println()
			break
		}
		user := strings.TrimSpace(line)
		if user == "" {
			continue
		}
		if u := strings.ToLower(user); u == "quit" || u == "exit" || u == "q" {
			break
		}
		turn++

		queryText := user
		if prevAnswer != "" {
			queryText = user + " " + tail(prevAnswer, 200)
		}
		injection, _ := s.kk.Pick(queryText, kk.PickOpts{ExcludeModel: true})
		if injection != "" {
			fmt.Printf("  [kk] %s\n", truncate(injection, 80))
		}

		prompt := user
		if injection != "" {
			prompt = injection + " " + user
		}

		text, err := s.speak(ctx, s.cfg.Voice, prompt, s.cfg.MaxTokens)
		if err != nil {
			fmt.Fprintf(os.Stderr, "infer error: %v\n", err)
			continue
		}
		fmt.Printf("\n%s> %s\n\n", s.cfg.Voice, text)

		s.absorb(text, fmt.Sprintf("%s_turn%d", s.cfg.Voice, turn))
		prevAnswer = text
	}
	fmt.Printf("[dialogue] %d turns, kk now has %d chunks\n", turn, s.kk.NumChunks())
	return nil
}

// ===================================================================
// DUET MODE  (two goroutines, channel-coordinated turns)
// ===================================================================

// turnMsg is sent on the shared turn channel: "voice X, here is your prompt".
type turnMsg struct {
	round    int
	voice    string
	prompt   string
	previous string // last speaker's full utterance (for KK + display)
}

func (s *session) duetMode() error {
	v1, v2 := s.cfg.Voice, s.cfg.Voice2
	if v1 == v2 {
		return fmt.Errorf("duet needs two distinct voices, got %s + %s", v1, v2)
	}
	fmt.Println(strings.Repeat("=", 60))
	fmt.Printf("  DUET — %s + %s\n  Topic: %s\n", v1, v2, s.cfg.Topic)
	fmt.Println(strings.Repeat("=", 60))

	s.kk.Reset()
	prime, _ := s.kk.Pick(s.cfg.Topic, kk.PickOpts{PreferSource: "dario_essay.txt"})
	startPrompt := s.cfg.Topic
	if prime != "" {
		startPrompt = prime + " " + s.cfg.Topic
		fmt.Printf("  [prime] %s\n\n", truncate(prime, 80))
	}

	// One mailbox per voice; whoever holds the floor receives a turnMsg.
	in1 := make(chan turnMsg, 1)
	in2 := make(chan turnMsg, 1)
	type spoken struct {
		voice string
		text  string
		round int
	}
	out := make(chan spoken, 4)
	done := make(chan struct{})

	totalTurns := s.cfg.Depth * 2
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	var wg sync.WaitGroup
	worker := func(name string, mailbox <-chan turnMsg) {
		defer wg.Done()
		for msg := range mailbox {
			text, err := s.speak(ctx, name, msg.prompt, s.cfg.MaxTokens)
			if err != nil {
				fmt.Fprintf(os.Stderr, "[%s] error: %v\n", name, err)
				out <- spoken{voice: name, text: "", round: msg.round}
				continue
			}
			out <- spoken{voice: name, text: text, round: msg.round}
		}
	}
	wg.Add(2)
	go worker(v1, in1)
	go worker(v2, in2)

	// Coordinator: hands the floor back-and-forth, performs absorption.
	go func() {
		defer close(done)
		// First speaker.
		in1 <- turnMsg{round: 0, voice: v1, prompt: startPrompt}
		var transcript []spoken
		for i := 0; i < totalTurns; i++ {
			sp := <-out
			fmt.Printf("\n%s> %s\n", sp.voice, sp.text)
			transcript = append(transcript, sp)

			// Absorb into KK + dario field (both happen INSIDE coordinator,
			// so there's no race on the kernel between two workers).
			s.absorb(sp.text, fmt.Sprintf("%s_duet", sp.voice))

			if i+1 >= totalTurns {
				break
			}
			next := v1
			nextMailbox := in1
			if sp.voice == v1 {
				next, nextMailbox = v2, in2
			}
			injection := s.kk.QueryInjection(sp.text + " " + s.cfg.Topic)
			prompt := s.cfg.Topic
			if injection != "" {
				prompt = injection + " " + s.cfg.Topic
				fmt.Printf("  [kk for %s] %s\n", next, truncate(injection, 60))
			}
			nextMailbox <- turnMsg{round: i + 1, voice: next,
				prompt: prompt, previous: sp.text}
		}
		close(in1)
		close(in2)

		fmt.Println()
		fmt.Println(strings.Repeat("=", 60))
		fmt.Println("  DUET TRANSCRIPT")
		fmt.Println(strings.Repeat("=", 60))
		for _, t := range transcript {
			fmt.Printf("\n%s: %s\n", t.voice, t.text)
		}
		fmt.Printf("\n[duet] %d turns, kk: %d chunks\n", len(transcript), s.kk.NumChunks())
	}()

	<-done
	wg.Wait()
	return nil
}

// ===================================================================
// TRIALOGUE MODE  (three goroutines, round-robin)
// ===================================================================

func (s *session) trialogueMode() error {
	order := []string{s.cfg.Voice, s.cfg.Voice2, s.cfg.Voice3}
	seen := map[string]bool{}
	for _, v := range order {
		if seen[v] {
			return fmt.Errorf("trialogue needs three distinct voices, got %v", order)
		}
		seen[v] = true
	}
	fmt.Println(strings.Repeat("=", 60))
	fmt.Printf("  TRIALOGUE — %s\n  Topic: %s\n", strings.Join(order, " + "), s.cfg.Topic)
	fmt.Println(strings.Repeat("=", 60))

	s.kk.Reset()
	prime, _ := s.kk.Pick(s.cfg.Topic, kk.PickOpts{PreferSource: "dario_essay.txt"})
	startPrompt := s.cfg.Topic
	if prime != "" {
		startPrompt = prime + " " + s.cfg.Topic
		fmt.Printf("  [prime] %s\n\n", truncate(prime, 80))
	}

	mailboxes := map[string]chan turnMsg{
		order[0]: make(chan turnMsg, 1),
		order[1]: make(chan turnMsg, 1),
		order[2]: make(chan turnMsg, 1),
	}
	type spoken struct {
		voice string
		text  string
		round int
	}
	out := make(chan spoken, 8)
	done := make(chan struct{})

	totalTurns := s.cfg.Depth * len(order)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	var wg sync.WaitGroup
	worker := func(name string, mailbox <-chan turnMsg) {
		defer wg.Done()
		for msg := range mailbox {
			text, err := s.speak(ctx, name, msg.prompt, s.cfg.MaxTokens)
			if err != nil {
				fmt.Fprintf(os.Stderr, "[%s] error: %v\n", name, err)
				out <- spoken{voice: name, text: "", round: msg.round}
				continue
			}
			out <- spoken{voice: name, text: text, round: msg.round}
		}
	}
	for _, v := range order {
		wg.Add(1)
		go worker(v, mailboxes[v])
	}

	go func() {
		defer close(done)
		mailboxes[order[0]] <- turnMsg{round: 0, voice: order[0], prompt: startPrompt}
		var transcript []spoken
		for i := 0; i < totalTurns; i++ {
			sp := <-out
			fmt.Printf("\n%s> %s\n", sp.voice, sp.text)
			transcript = append(transcript, sp)
			s.absorb(sp.text, fmt.Sprintf("%s_trialogue", sp.voice))
			if i+1 >= totalTurns {
				break
			}
			next := order[(i+1)%len(order)]
			injection := s.kk.QueryInjection(sp.text + " " + s.cfg.Topic)
			prompt := s.cfg.Topic
			if injection != "" {
				prompt = injection + " " + s.cfg.Topic
				fmt.Printf("  [kk for %s] %s\n", next, truncate(injection, 60))
			}
			mailboxes[next] <- turnMsg{round: i + 1, voice: next,
				prompt: prompt, previous: sp.text}
		}
		for _, m := range mailboxes {
			close(m)
		}
		fmt.Println()
		fmt.Println(strings.Repeat("=", 60))
		fmt.Println("  TRIALOGUE TRANSCRIPT")
		fmt.Println(strings.Repeat("=", 60))
		for _, t := range transcript {
			fmt.Printf("\n%s: %s\n", t.voice, t.text)
		}
		fmt.Printf("\n[trialogue] %d turns, kk: %d chunks\n", len(transcript), s.kk.NumChunks())
	}()

	<-done
	wg.Wait()
	return nil
}

// ===================================================================
// helpers
// ===================================================================

func tail(s string, n int) string {
	if len(s) <= n {
		return s
	}
	return s[len(s)-n:]
}

func truncate(s string, n int) string {
	s = strings.ReplaceAll(s, "\n", " ")
	if len(s) <= n {
		return s
	}
	return s[:n]
}

func scriptDirOrCWD() (string, error) {
	if exe, err := os.Executable(); err == nil {
		if real, err := filepath.EvalSymlinks(exe); err == nil {
			exe = real
		}
		// bin/dario-dialogue → repo root has infer_v4
		repo := filepath.Dir(filepath.Dir(exe))
		if _, err := os.Stat(filepath.Join(repo, "infer_v4")); err == nil {
			return repo, nil
		}
		if _, err := os.Stat(filepath.Join(filepath.Dir(exe), "infer_v4")); err == nil {
			return filepath.Dir(exe), nil
		}
	}
	cwd, err := os.Getwd()
	if err != nil {
		return "", err
	}
	return cwd, nil
}

func saveTranscript(path string, cfg config, narrative string, log []chainEntry) error {
	f, err := os.Create(path)
	if err != nil {
		return err
	}
	defer f.Close()
	fmt.Fprintf(f, "# Chain Dialogue — %s\n", cfg.Voice)
	fmt.Fprintf(f, "# Topic: %s\n", cfg.Topic)
	fmt.Fprintf(f, "# Depth: %d, temp=%.2f, top_k=%d, rep=%.2f\n",
		cfg.Depth, cfg.Temp, cfg.TopK, cfg.RepPenalty)
	fmt.Fprintf(f, "# Date: %s\n\n", time.Now().Format("2006-01-02 15:04"))
	fmt.Fprintln(f, narrative)
	fmt.Fprintln(f, "\n# Chain log:")
	for _, e := range log {
		fmt.Fprintf(f, "# step %d: %s", e.Step, e.Type)
		if e.Injection != "" {
			fmt.Fprintf(f, " — %s", e.Injection)
		}
		fmt.Fprintln(f)
	}
	return nil
}

func printUsage(fs *flag.FlagSet) func() {
	return func() {
		fmt.Fprintf(os.Stderr, `dario-dialogue: chain / dialogue / duet / trialogue modes for the dario voices.

usage:
  dario-dialogue --mode MODE [flags]

modes:
  chain      — one voice, KK injects between rounds
  dialogue   — interactive REPL with KK and dario field
  duet       — two voices via two goroutines + channel turn coordination
  trialogue  — three voices via three goroutines

common flags:
  --voice NAME       primary voice (default leo)
  --voice2 NAME      second voice (duet/trialogue)
  --voice3 NAME      third voice (trialogue)
  --topic TEXT       seed topic / prompt
  --depth N          rounds per voice (default 6)
  --max-tokens N     per-segment generation cap (default 200)
  --temperature F    override sampling temperature
  --top-k N          override top-k cutoff (0 = disabled)
  --rep-penalty F    override repetition penalty
  --knowledge PATH   knowledge file (repeatable; defaults dario_essay.txt + leo_expanded.txt)
  --save PATH        write transcript to file (chain mode)
  --field URL        dario --web base URL (default http://localhost:3001)
  --weights-dir DIR  override DARIO_WEIGHTS_DIR
  --timeout DUR      per-inference timeout (default 2m)

valid voices: %s
`, strings.Join(voices.Names(), ", "))
		fs.PrintDefaults()
	}
}
