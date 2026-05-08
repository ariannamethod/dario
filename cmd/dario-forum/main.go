// dario-forum — Go replacement for forum.py.
//
// A small HTTP server that:
//   - holds one shared Knowledge Kernel
//   - accepts POST /api/forum {question, voice} → spawns ./infer_v4 for the
//     chosen voice and returns the generated text + KK injection + state
//   - serves dario.html / chatbot.html / forum.html from disk
//
// Per-request inference runs on a goroutine; concurrent /api/forum
// requests get parallel infer_v4 subprocesses (one per voice). The KK
// is protected by its own mutex inside internal/kk.
package main

import (
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"net/http"
	"os"
	"path/filepath"
	"strings"
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

type knowledgeFlag []string

func (k *knowledgeFlag) String() string     { return strings.Join(*k, ",") }
func (k *knowledgeFlag) Set(v string) error { *k = append(*k, v); return nil }

func run(argv []string) error {
	fs := flag.NewFlagSet("dario-forum", flag.ContinueOnError)
	port := fs.Int("port", 8800, "HTTP listen port")
	host := fs.String("host", "0.0.0.0", "HTTP listen address")
	field := fs.String("field", "http://localhost:3001",
		"dario equation REPL base URL for KK absorption")
	timeout := fs.Duration("timeout", 2*time.Minute, "per-inference timeout")
	var knowledge knowledgeFlag
	fs.Var(&knowledge, "knowledge", "knowledge file (repeatable)")

	if err := fs.Parse(argv); err != nil {
		return err
	}

	scriptDir, err := scriptDirOrCWD()
	if err != nil {
		return err
	}
	binPath, err := dario.FindBinary(scriptDir)
	if err != nil {
		return err
	}

	if len(knowledge) == 0 {
		knowledge = knowledgeFlag{
			filepath.Join(scriptDir, "dario_essay.txt"),
			filepath.Join(scriptDir, "leo_expanded.txt"),
		}
	}

	srv := newServer(scriptDir, binPath, *field, *timeout)

	for _, p := range knowledge {
		if _, err := os.Stat(p); err != nil {
			fmt.Fprintf(os.Stderr, "[kk] WARN: %s missing, skipping\n", p)
			continue
		}
		n, err := srv.kk.Ingest(p)
		if err != nil {
			fmt.Fprintf(os.Stderr, "[kk] %s: %v\n", p, err)
			continue
		}
		fmt.Fprintf(os.Stderr, "[kk] %s: %d chunks\n", filepath.Base(p), n)
	}

	// Resolve all known voices upfront so /api/voices reflects what's
	// actually playable, and so we can return a 503 before infer_v4
	// runs if the weights are missing.
	for _, name := range voices.Names() {
		v, _ := voices.Lookup(name)
		w, err := voices.ResolveWeights(v, "", scriptDir)
		if err != nil {
			fmt.Fprintf(os.Stderr, "[voices] %s — %v (skipped)\n", name, err)
			continue
		}
		srv.voices[name] = resolved{V: v, Weights: w}
		fmt.Fprintf(os.Stderr, "[voices] %s — %s\n", name, w)
	}

	addr := fmt.Sprintf("%s:%d", *host, *port)
	fmt.Fprintf(os.Stderr, "\n[forum] http://%s\n", addr)
	fmt.Fprintf(os.Stderr, "[forum] %d chunks loaded\n", srv.kk.NumChunks())
	fmt.Fprintf(os.Stderr, "[forum] %d voices ready: %v\n", len(srv.voices), keys(srv.voices))
	fmt.Fprintf(os.Stderr, "[forum] POST /api/forum {question, voice}\n")
	fmt.Fprintf(os.Stderr, "[forum] GET  /api/voices  /api/kk\n")
	return http.ListenAndServe(addr, srv.mux())
}

type resolved struct {
	V       voices.Voice
	Weights string
}

type server struct {
	scriptDir string
	binPath   string
	timeout   time.Duration
	kk        *kk.Kernel
	field     *dario.FieldClient
	fieldUp   bool
	voices    map[string]resolved
}

func newServer(scriptDir, binPath, fieldURL string, timeout time.Duration) *server {
	s := &server{
		scriptDir: scriptDir,
		binPath:   binPath,
		timeout:   timeout,
		kk:        kk.New(),
		field:     dario.NewFieldClient(fieldURL),
		voices:    make(map[string]resolved),
	}
	probe, cancel := context.WithTimeout(context.Background(), 500*time.Millisecond)
	defer cancel()
	s.fieldUp = s.field.Available(probe)
	if s.fieldUp {
		fmt.Fprintf(os.Stderr, "[field] %s — alive\n", fieldURL)
	} else {
		fmt.Fprintf(os.Stderr, "[field] %s — not reachable\n", fieldURL)
	}
	return s
}

func (s *server) mux() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/api/forum", s.handleForum)
	mux.HandleFunc("/api/voices", s.handleVoices)
	mux.HandleFunc("/api/kk", s.handleKK)
	mux.HandleFunc("/", s.handleStatic)
	return withCORS(mux)
}

func withCORS(h http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Access-Control-Allow-Origin", "*")
		w.Header().Set("Access-Control-Allow-Methods", "POST, GET, OPTIONS")
		w.Header().Set("Access-Control-Allow-Headers", "Content-Type")
		if r.Method == http.MethodOptions {
			w.WriteHeader(http.StatusNoContent)
			return
		}
		h.ServeHTTP(w, r)
	})
}

// ---- /api/forum ----

type forumRequest struct {
	Question  string `json:"question"`
	Voice     string `json:"voice"`
	MaxTokens int    `json:"max_tokens"`
}

type forumResponse struct {
	Voice          string             `json:"voice"`
	Text           string             `json:"text"`
	Injection      string             `json:"injection"`
	KKChunks       int                `json:"kk_chunks"`
	Backend        string             `json:"backend"`
	Desc           string             `json:"desc"`
	EmotionalState map[string]float64 `json:"emotional_state"`
	Stats          string             `json:"stats,omitempty"`
}

func (s *server) handleForum(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST required", http.StatusMethodNotAllowed)
		return
	}
	var req forumRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, fmt.Sprintf("bad json: %v", err), http.StatusBadRequest)
		return
	}
	if req.Voice == "" {
		req.Voice = "leo"
	}
	rv, ok := s.voices[req.Voice]
	if !ok {
		http.Error(w, fmt.Sprintf("unknown voice %q (have %v)", req.Voice, keys(s.voices)),
			http.StatusBadRequest)
		return
	}
	if req.Question == "" {
		http.Error(w, "missing 'question'", http.StatusBadRequest)
		return
	}
	if req.MaxTokens <= 0 {
		req.MaxTokens = 200
	}

	// Absorb the user question into KK so the conversation grows.
	s.kk.Absorb(req.Question, "user")

	// KK injection — a sentence that resonates with the question.
	injection := s.kk.QueryInjection(req.Question)

	prompt := fmt.Sprintf("Q: %s\nA:", req.Question)
	if injection != "" {
		prompt = fmt.Sprintf("Q: %s\nA: %s ", req.Question, injection)
	}

	ctx, cancel := context.WithTimeout(r.Context(), s.timeout)
	defer cancel()

	res, err := dario.Run(ctx, s.binPath, dario.InferRequest{
		Voice: rv.V, WeightsPath: rv.Weights,
		Prompt: prompt, MaxTokens: req.MaxTokens,
		Temp: rv.V.Temp, TopK: rv.V.TopK,
		Timeout: s.timeout,
	})
	if err != nil {
		http.Error(w, fmt.Sprintf("inference failed: %v", err),
			http.StatusInternalServerError)
		return
	}
	text := strings.TrimSpace(res.Text)

	if text != "" {
		s.kk.Absorb(text, fmt.Sprintf("%s_forum", rv.V.Name))
		// Bridge back into the dario field if it's running.
		if s.fieldUp {
			ctx2, cancel2 := context.WithTimeout(context.Background(), 5*time.Second)
			s.field.Chat(ctx2, text)
			cancel2()
		}
	}

	resp := forumResponse{
		Voice: rv.V.Name, Text: text, Injection: injection,
		KKChunks: s.kk.NumChunks(), Backend: rv.V.Backend, Desc: rv.V.Desc,
		EmotionalState: s.kk.CurrentState(), Stats: res.Stats,
	}
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(resp)
}

// ---- /api/voices ----

func (s *server) handleVoices(w http.ResponseWriter, _ *http.Request) {
	out := make(map[string]map[string]string, len(s.voices))
	for name, rv := range s.voices {
		out[name] = map[string]string{
			"desc":    rv.V.Desc,
			"backend": rv.V.Backend,
		}
	}
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(out)
}

// ---- /api/kk ----

func (s *server) handleKK(w http.ResponseWriter, _ *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]interface{}{
		"chunks":          s.kk.NumChunks(),
		"emotional_state": s.kk.CurrentState(),
	})
}

// ---- static files ----

func (s *server) handleStatic(w http.ResponseWriter, r *http.Request) {
	path := r.URL.Path
	switch path {
	case "/", "/forum", "/forum.html":
		s.serveFile(w, "forum.html")
	case "/chat", "/chatbot", "/chatbot.html":
		s.serveFile(w, "chatbot.html")
	case "/dario", "/dario.html":
		s.serveFile(w, "dario.html")
	default:
		http.NotFound(w, r)
	}
}

func (s *server) serveFile(w http.ResponseWriter, name string) {
	candidates := []string{
		filepath.Join(s.scriptDir, name),
		filepath.Join(s.scriptDir, "dario", name),
		name,
	}
	for _, p := range candidates {
		if data, err := os.ReadFile(p); err == nil {
			w.Header().Set("Content-Type", "text/html; charset=utf-8")
			w.Write(data)
			return
		}
	}
	http.Error(w, name+" not found", http.StatusNotFound)
}

// ---- helpers ----

func keys(m map[string]resolved) []string {
	out := make([]string, 0, len(m))
	for k := range m {
		out = append(out, k)
	}
	return out
}

func scriptDirOrCWD() (string, error) {
	if exe, err := os.Executable(); err == nil {
		if real, err := filepath.EvalSymlinks(exe); err == nil {
			exe = real
		}
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
