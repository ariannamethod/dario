// Package kk implements the Knowledge Kernel used by dario-dialogue and
// dario-forum: a charged-chunk store with FTS-like text retrieval plus
// emotional resonance scoring.
//
// This is a pure-Go re-implementation of the Python KnowledgeKernel +
// ForumKK + ChunkMeta in chain_dialogue.py and forum.py. SQLite/FTS5
// is replaced with a simple word-inverted-index because we don't want
// the cgo dependency for a few hundred chunks.
package kk

import (
	"math"
	"os"
	"regexp"
	"sort"
	"strings"
	"sync"
)

// Emotion fingerprint dimensions, in the canonical order used by both
// chain_dialogue.py and forum.py. The order is load-bearing because we
// store fingerprints as []float64 indexed by Emotions[].
var Emotions = [...]string{
	"trauma", "joy", "grief", "resonance",
	"desire", "void", "rage", "tenderness",
}

// anchor is one (word → emotion + mass) entry. The full table mirrors
// ChunkMeta.ANCHORS in forum.py.
type anchor struct {
	emotion string
	mass    float64
}

var anchors = map[string]anchor{
	"death": {"trauma", 0.95}, "pain": {"trauma", 0.85}, "war": {"trauma", 0.90},
	"loss": {"grief", 0.80}, "memory": {"grief", 0.60}, "silence": {"grief", 0.50},
	"love": {"tenderness", 0.90}, "beauty": {"tenderness", 0.70}, "light": {"tenderness", 0.60},
	"fire": {"rage", 0.85}, "break": {"rage", 0.70}, "destroy": {"rage", 0.80},
	"dream": {"desire", 0.75}, "want": {"desire", 0.65}, "seek": {"desire", 0.60},
	"void": {"void", 0.90}, "nothing": {"void", 0.80}, "empty": {"void", 0.75},
	"joy": {"joy", 0.85}, "laugh": {"joy", 0.70}, "sing": {"joy", 0.65},
	"echo": {"resonance", 0.80}, "pattern": {"resonance", 0.75}, "wave": {"resonance", 0.70},
	"rhythm": {"resonance", 0.85}, "field": {"resonance", 0.65}, "frequency": {"resonance", 0.80},
	"god": {"tenderness", 0.60}, "prayer": {"tenderness", 0.70}, "sacred": {"tenderness", 0.75},
	"gold": {"joy", 0.60}, "sun": {"joy", 0.55}, "star": {"joy", 0.50},
	"code": {"resonance", 0.60}, "equation": {"resonance", 0.70}, "formula": {"resonance", 0.65},
	"refuse": {"rage", 0.75}, "no": {"rage", 0.50}, "resist": {"rage", 0.70},
}

// emoIndex maps an emotion name to its slot in the fingerprint vector.
var emoIndex = func() map[string]int {
	m := make(map[string]int, len(Emotions))
	for i, e := range Emotions {
		m[e] = i
	}
	return m
}()

var wordRe = regexp.MustCompile(`[a-zA-Z]{3,}`)
var sentenceSplit = regexp.MustCompile(`(?:[.!?])\s+`)

// ChunkMeta is the per-chunk emotional fingerprint, mirroring the Python
// ChunkMeta class. It carries `Charge` (8-dim, sums to ≈1), `Mass` (a
// 0..1 emotional weight), and the dominant emotion's name.
type ChunkMeta struct {
	Text     string
	Mass     float64
	Charge   [len(Emotions)]float64
	Dominant string
	NWords   int
}

// NewChunkMeta computes the fingerprint of `text` exactly the way
// ChunkMeta._compute does: anchor counts + 4-token spread.
func NewChunkMeta(text string) ChunkMeta {
	m := ChunkMeta{Text: text}
	words := wordRe.FindAllString(strings.ToLower(text), -1)
	m.NWords = len(words)
	if m.NWords == 0 {
		return m
	}
	totalMass := 0.0
	for _, w := range words {
		if a, ok := anchors[w]; ok {
			m.Charge[emoIndex[a.emotion]] += a.mass
			totalMass += a.mass
		}
	}
	for i, w := range words {
		a, ok := anchors[w]
		if !ok {
			continue
		}
		idx := emoIndex[a.emotion]
		lo, hi := i-4, i+5
		if lo < 0 {
			lo = 0
		}
		if hi > len(words) {
			hi = len(words)
		}
		for j := lo; j < hi; j++ {
			if j == i {
				continue
			}
			dist := j - i
			if dist < 0 {
				dist = -dist
			}
			m.Charge[idx] += a.mass * 0.3 / float64(dist)
		}
	}
	total := 1e-8
	for _, c := range m.Charge {
		total += c
	}
	for k := range m.Charge {
		m.Charge[k] /= total
	}
	mass := totalMass / math.Max(float64(len(words)), 1) * 5
	if mass > 1 {
		mass = 1
	}
	m.Mass = mass
	maxIdx, maxV := 0, m.Charge[0]
	for k := 1; k < len(m.Charge); k++ {
		if m.Charge[k] > maxV {
			maxV, maxIdx = m.Charge[k], k
		}
	}
	m.Dominant = Emotions[maxIdx]
	return m
}

// ResonanceWith returns the cosine similarity between this chunk's
// charge and another fingerprint vector.
func (m ChunkMeta) ResonanceWith(other [len(Emotions)]float64) float64 {
	var dot, na, nb float64
	for i := range m.Charge {
		dot += m.Charge[i] * other[i]
		na += m.Charge[i] * m.Charge[i]
		nb += other[i] * other[i]
	}
	na = math.Sqrt(na) + 1e-8
	nb = math.Sqrt(nb) + 1e-8
	return dot / (na * nb)
}

// chunk is one row in the kernel: text + source label + fingerprint.
type chunk struct {
	id      int
	text    string
	source  string
	meta    ChunkMeta
	tokens  map[string]struct{} // distinct lowercase word tokens for FTS
	forwds  []string            // ordered tokens (used for AND-overlap dedup)
	used    bool                // marked when picked as injection
}

// Kernel is the unified KK shared by both dialogue and forum modes.
//
// The dialogue mode used a richer "FTS5 + dedup + injection extraction"
// pipeline than forum's "fingerprint-only" version; we collapse them
// into one struct and let the caller decide whether to take the path
// `Pick` (chain/dialogue style) or `QueryInjection` (forum style).
type Kernel struct {
	mu             sync.Mutex
	nextID         int
	chunks         []*chunk
	byToken        map[string][]int // word → list of chunk IDs
	used           map[int]struct{} // ids already returned by Pick (per-session)
	currentCharge  [len(Emotions)]float64
}

// New creates an empty kernel with a uniform initial charge.
func New() *Kernel {
	k := &Kernel{
		byToken: make(map[string][]int),
		used:    make(map[int]struct{}),
	}
	for i := range k.currentCharge {
		k.currentCharge[i] = 1.0 / float64(len(Emotions))
	}
	return k
}

// NumChunks returns the count of chunks currently held.
func (k *Kernel) NumChunks() int {
	k.mu.Lock()
	defer k.mu.Unlock()
	return len(k.chunks)
}

// Reset clears the per-session "used" set without dropping any chunks.
// Mirrors KnowledgeKernel.reset() in chain_dialogue.py.
func (k *Kernel) Reset() {
	k.mu.Lock()
	defer k.mu.Unlock()
	k.used = make(map[int]struct{})
}

// CurrentState returns a snapshot of the organism's current 8-emotion
// fingerprint. Used by /api/kk and /api/forum.
func (k *Kernel) CurrentState() map[string]float64 {
	k.mu.Lock()
	defer k.mu.Unlock()
	out := make(map[string]float64, len(Emotions))
	for i, e := range Emotions {
		out[e] = round(k.currentCharge[i], 3)
	}
	return out
}

func round(x float64, d int) float64 {
	p := math.Pow10(d)
	return math.Round(x*p) / p
}

// Ingest reads `path`, splits on blank lines (matching Python's
// `text.split('\n\n')`), and stores chunks ≥ 50 chars long. The first
// 600 chars of each chunk are kept (matching chain_dialogue.py).
func (k *Kernel) Ingest(path string) (int, error) {
	raw, err := os.ReadFile(path)
	if err != nil {
		return 0, err
	}
	source := lastSegment(path)
	parts := strings.Split(string(raw), "\n\n")
	added := 0
	for _, p := range parts {
		s := strings.TrimSpace(p)
		if len(s) <= 50 {
			continue
		}
		if len(s) > 600 {
			s = s[:600]
		}
		k.insert(s, source)
		added++
	}
	return added, nil
}

// IngestText is like Ingest but takes the raw bytes directly. Useful for
// stream-of-conversation absorption.
func (k *Kernel) IngestText(text, source string) int {
	parts := strings.Split(text, "\n\n")
	added := 0
	for _, p := range parts {
		s := strings.TrimSpace(p)
		if len(s) <= 50 {
			continue
		}
		if len(s) > 600 {
			s = s[:600]
		}
		k.insert(s, source)
		added++
	}
	return added
}

// Absorb mirrors KnowledgeKernel.absorb / ForumKK.absorb: takes a model
// utterance, pulls out properly capitalized sentences ≥40 chars, and
// inserts them with simple AND-overlap dedup against existing chunks.
//
// Returns the number of new chunks added.
func (k *Kernel) Absorb(text, source string) int {
	if len(strings.TrimSpace(text)) < 30 {
		return 0
	}
	sents := splitSentences(text)
	added := 0
	for _, s := range sents {
		s = strings.TrimSpace(s)
		if len(s) < 40 {
			continue
		}
		if len(s) > 600 {
			s = s[:600]
		}
		// Must start uppercase (filter out fragments).
		if !isUpper(s[0]) {
			continue
		}
		if len(strings.Fields(s)) < 6 {
			continue
		}
		// Dedup: if an existing chunk shares ≥5 distinct ≥4-letter words,
		// drop this sentence (would echo).
		if k.duplicateExists(s) {
			continue
		}
		k.insert(s, source)
		added++
	}
	return added
}

// insert adds a chunk and indexes its tokens. Caller must NOT hold mu.
func (k *Kernel) insert(text, source string) {
	k.mu.Lock()
	defer k.mu.Unlock()
	c := &chunk{
		id:     k.nextID,
		text:   text,
		source: source,
		meta:   NewChunkMeta(text),
	}
	k.nextID++
	tokens := make(map[string]struct{})
	for _, w := range wordRe.FindAllString(strings.ToLower(text), -1) {
		tokens[w] = struct{}{}
	}
	c.tokens = tokens
	for w := range tokens {
		k.byToken[w] = append(k.byToken[w], c.id)
	}
	k.chunks = append(k.chunks, c)
}

func (k *Kernel) duplicateExists(s string) bool {
	words := uniqWords(s)
	if len(words) < 3 {
		return true // not enough signal — skip
	}
	probe := words
	if len(probe) > 5 {
		probe = probe[:5]
	}
	k.mu.Lock()
	defer k.mu.Unlock()
	// Walk inverted index for the smallest probe-word's posting list,
	// then verify the rest by intersection.
	if len(probe) == 0 {
		return false
	}
	smallest := probe[0]
	smallestN := len(k.byToken[smallest])
	for _, w := range probe[1:] {
		if n := len(k.byToken[w]); n < smallestN {
			smallest = w
			smallestN = n
		}
	}
	for _, id := range k.byToken[smallest] {
		c := k.lookupLocked(id)
		if c == nil {
			continue
		}
		all := true
		for _, w := range probe {
			if _, ok := c.tokens[w]; !ok {
				all = false
				break
			}
		}
		if all {
			return true
		}
	}
	return false
}

func (k *Kernel) lookupLocked(id int) *chunk {
	for _, c := range k.chunks {
		if c.id == id {
			return c
		}
	}
	return nil
}

// QueryResult is one match returned by the kernel. Both Pick and
// QueryInjection rebuild this internally; we expose it for callers
// that want to inspect ranking.
type QueryResult struct {
	ID     int
	Text   string
	Source string
	Score  float64
}

// QueryInjection mirrors ForumKK.query: returns ONE injection sentence
// (≤100 chars by default) chosen by FTS-overlap + emotional resonance.
//
// Mutates the kernel's currentCharge toward the picked chunk (organism
// drifts toward the topic it's thinking about). Returns "" when nothing
// matches.
func (k *Kernel) QueryInjection(text string) string {
	candidates := k.fts(text, 5, "", false)
	if len(candidates) == 0 {
		return ""
	}
	queryMeta := NewChunkMeta(text)
	type scored struct {
		c     *chunk
		score float64
	}
	out := make([]scored, 0, len(candidates))
	k.mu.Lock()
	for _, id := range candidates {
		c := k.lookupLocked(id)
		if c == nil {
			continue
		}
		res := c.meta.ResonanceWith(queryMeta.Charge)
		orgRes := c.meta.ResonanceWith(k.currentCharge)
		s := res*0.6 + orgRes*0.4 + c.meta.Mass*0.2
		out = append(out, scored{c, s})
	}
	k.mu.Unlock()
	if len(out) == 0 {
		return ""
	}
	sort.Slice(out, func(i, j int) bool { return out[i].score > out[j].score })
	best := out[0].c
	chunkText := stripQA(best.text)
	injection := pickShortSentence(chunkText, 15, 100)
	// Drift current charge toward the picked chunk.
	k.mu.Lock()
	for i := range k.currentCharge {
		k.currentCharge[i] = 0.8*k.currentCharge[i] + 0.2*best.meta.Charge[i]
	}
	k.mu.Unlock()
	return injection
}

// PickOpts are the dialogue-mode search options.
type PickOpts struct {
	TopK         int    // default 5
	PreferSource string // bonus rank for chunks from this source (e.g. "dario_essay.txt")
	ExcludeModel bool   // skip chunks absorbed from model output
}

// Pick mirrors KnowledgeKernel.pick_next: returns ONE injection text
// suitable for prepending to the next inference call, plus the source
// chunk text. Uses session-local "used" set to avoid repeats.
func (k *Kernel) Pick(text string, opts PickOpts) (injection, fullChunk string) {
	if opts.TopK <= 0 {
		opts.TopK = 5
	}
	results := k.rank(text, opts)
	if len(results) == 0 {
		return "", ""
	}
	for _, r := range results {
		k.mu.Lock()
		k.used[r.ID] = struct{}{}
		k.mu.Unlock()
		stripped := stripQA(r.Text)
		inj := extractInjection(stripped, 100)
		inj = strings.ReplaceAll(inj, "\n", " ")
		inj = strings.ReplaceAll(inj, "  ", " ")
		inj = strings.TrimSpace(inj)
		if !endsAtSentence(inj) {
			if cut := lastSentenceBoundary(inj); cut > 15 {
				inj = inj[:cut+1]
			} else {
				continue // no boundary, skip
			}
		}
		if inj == "" || !isUpper(inj[0]) || len(strings.Fields(inj)) < 4 {
			continue
		}
		return inj, r.Text
	}
	return "", ""
}

// rank computes the dialogue-mode ranking (FTS-rank approximation +
// source bonuses + used-skip). It returns ordered candidates the way
// chain_dialogue.py expects.
func (k *Kernel) rank(text string, opts PickOpts) []QueryResult {
	tokens := tokenize(text)
	if len(tokens) == 0 {
		return nil
	}
	if len(tokens) > 12 {
		tokens = tokens[:12]
	}
	k.mu.Lock()
	defer k.mu.Unlock()
	// Collect candidate IDs with overlap counts.
	hits := make(map[int]int)
	for _, w := range tokens {
		for _, id := range k.byToken[w] {
			hits[id]++
		}
	}
	if len(hits) == 0 {
		return nil
	}
	type cand struct {
		c     *chunk
		score float64
	}
	cands := make([]cand, 0, len(hits))
	for id, hit := range hits {
		if _, isUsed := k.used[id]; isUsed {
			continue
		}
		c := k.lookupLocked(id)
		if c == nil {
			continue
		}
		if opts.ExcludeModel {
			s := c.source
			if strings.Contains(s, "model") || strings.Contains(s, "_turn") || strings.Contains(s, "_duet") {
				continue
			}
		}
		// Higher score = better. Approximates negative FTS rank.
		score := float64(hit)
		if opts.PreferSource != "" && c.source == opts.PreferSource {
			score += 8
		} else if strings.Contains(c.source, "essay") {
			score += 4
		} else if strings.Contains(c.source, "expanded") {
			score -= 1
		}
		cands = append(cands, cand{c, score})
	}
	if len(cands) == 0 {
		return nil
	}
	sort.Slice(cands, func(i, j int) bool { return cands[i].score > cands[j].score })
	limit := opts.TopK
	if limit > len(cands) {
		limit = len(cands)
	}
	out := make([]QueryResult, limit)
	for i := 0; i < limit; i++ {
		out[i] = QueryResult{
			ID: cands[i].c.id, Text: cands[i].c.text,
			Source: cands[i].c.source, Score: cands[i].score,
		}
	}
	return out
}

// fts returns up to `topK` chunk IDs matching ANY of the words in `text`.
// Used by the forum-style QueryInjection.
func (k *Kernel) fts(text string, topK int, _ string, _ bool) []int {
	tokens := tokenize(text)
	if len(tokens) == 0 {
		return nil
	}
	k.mu.Lock()
	defer k.mu.Unlock()
	hits := make(map[int]int)
	for _, w := range tokens {
		for _, id := range k.byToken[w] {
			hits[id]++
		}
	}
	if len(hits) == 0 {
		return nil
	}
	type pair struct {
		id    int
		count int
	}
	all := make([]pair, 0, len(hits))
	for id, c := range hits {
		all = append(all, pair{id, c})
	}
	sort.Slice(all, func(i, j int) bool { return all[i].count > all[j].count })
	if topK > len(all) {
		topK = len(all)
	}
	out := make([]int, topK)
	for i := 0; i < topK; i++ {
		out[i] = all[i].id
	}
	return out
}

// ===================================================================
// Helpers
// ===================================================================

func tokenize(s string) []string {
	tokens := wordRe.FindAllString(strings.ToLower(s), -1)
	// dedup, preserve order
	seen := make(map[string]struct{}, len(tokens))
	out := make([]string, 0, len(tokens))
	for _, t := range tokens {
		if _, ok := seen[t]; ok {
			continue
		}
		seen[t] = struct{}{}
		out = append(out, t)
	}
	return out
}

func uniqWords(s string) []string {
	tokens := wordRe.FindAllString(strings.ToLower(s), -1)
	seen := make(map[string]struct{}, len(tokens))
	out := make([]string, 0, len(tokens))
	for _, t := range tokens {
		if len(t) < 4 {
			continue
		}
		if _, ok := seen[t]; ok {
			continue
		}
		seen[t] = struct{}{}
		out = append(out, t)
	}
	return out
}

func splitSentences(text string) []string {
	// Python: re.split(r'(?<=[.!?])\s+', text)
	return splitOnSentencePunct(text)
}

func splitOnSentencePunct(text string) []string {
	var out []string
	start := 0
	for i := 0; i < len(text); i++ {
		c := text[i]
		if c != '.' && c != '!' && c != '?' {
			continue
		}
		j := i + 1
		// require whitespace next to consume as a boundary
		for j < len(text) && (text[j] == ' ' || text[j] == '\t' || text[j] == '\n' || text[j] == '\r') {
			j++
		}
		if j == i+1 {
			continue // no whitespace after punctuation
		}
		out = append(out, text[start:i+1])
		start = j
	}
	if start < len(text) {
		out = append(out, text[start:])
	}
	return out
}

func stripQA(s string) string {
	const nlA = "\nA:"
	if i := strings.Index(s, nlA); i >= 0 {
		return strings.TrimSpace(s[i+len(nlA):])
	}
	if strings.HasPrefix(s, "A:") {
		return strings.TrimSpace(s[2:])
	}
	if strings.HasPrefix(s, "Q:") {
		if nl := strings.IndexByte(s, '\n'); nl >= 0 {
			return strings.TrimSpace(s[nl+1:])
		}
		return strings.TrimSpace(s[2:])
	}
	return s
}

// pickShortSentence walks sentences in `s` and returns the first one
// whose length lies in (lo, hi). Falls back to `s[:hi]`.
func pickShortSentence(s string, lo, hi int) string {
	for _, sent := range splitSentences(s) {
		t := strings.TrimSpace(sent)
		if len(t) > lo && len(t) < hi {
			return t
		}
	}
	if len(s) > hi {
		return s[:hi]
	}
	return s
}

var (
	upperRunRe   = regexp.MustCompile(`[A-Z]{3,}`)
	metaphorRe   = regexp.MustCompile(`^(Like |Just as |Much like |Imagine |Think of |Consider |It's akin)`)
	concreteVbRe = regexp.MustCompile(`(?i)(creates?|assigns?|computes?|measures?|generates?|produces?|captures?|tracks?|scores?|blends?)`)
)

// extractInjection mirrors KnowledgeKernel.extract_injection: scores
// sentences, prefers short concrete capitalized definitions, rejects
// metaphor openers and questions.
func extractInjection(s string, maxLen int) string {
	sents := splitSentences(s)
	if len(sents) == 0 {
		if len(s) > maxLen {
			return s[:maxLen]
		}
		return s
	}
	type scored struct {
		score int
		neg   int // -len(s), tiebreak
		text  string
	}
	out := make([]scored, 0, len(sents))
	for i, raw := range sents {
		t := strings.TrimSpace(raw)
		if len(t) < 10 || len(t) > maxLen {
			continue
		}
		score := 0
		score += len(upperRunRe.FindAllString(t, -1)) * 4
		if i == 0 {
			score += 3
		} else if i == 1 {
			score += 1
		}
		if metaphorRe.MatchString(t) {
			score -= 4
		}
		if strings.HasPrefix(t, "Absolutely") ||
			strings.HasPrefix(t, "Yes,") ||
			strings.HasPrefix(t, "Indeed") ||
			strings.HasPrefix(t, "Of course") ||
			strings.HasPrefix(t, "Certainly") {
			score -= 3
		}
		if concreteVbRe.MatchString(t) {
			score += 2
		}
		if 40 <= len(t) && len(t) <= 80 {
			score += 1
		}
		out = append(out, scored{score, -len(t), t})
	}
	if len(out) > 0 {
		sort.SliceStable(out, func(i, j int) bool {
			if out[i].score != out[j].score {
				return out[i].score > out[j].score
			}
			return out[i].neg < out[j].neg // less negative = shorter
		})
		result := out[0].text
		if !endsAtSentence(result) {
			if cut := lastSentenceBoundary(result); cut > 20 {
				result = result[:cut+1]
			}
		}
		// Avoid injecting questions — model gets confused. Try the next
		// candidate, mirroring chain_dialogue.py.
		if strings.HasSuffix(strings.TrimSpace(result), "?") {
			for _, cand := range out[1:] {
				if !strings.HasSuffix(strings.TrimSpace(cand.text), "?") {
					return cand.text
				}
			}
		}
		return result
	}
	first := strings.TrimSpace(sents[0])
	if len(first) > maxLen {
		if cut := strings.LastIndexByte(first[:maxLen], '.'); cut > 20 {
			return first[:cut+1]
		}
	}
	if len(first) > maxLen {
		return first[:maxLen]
	}
	return first
}

func endsAtSentence(s string) bool {
	if s == "" {
		return false
	}
	c := s[len(s)-1]
	return c == '.' || c == '!' || c == '?'
}

func lastSentenceBoundary(s string) int {
	last := -1
	for i := 0; i < len(s); i++ {
		if s[i] == '.' || s[i] == '!' || s[i] == '?' {
			last = i
		}
	}
	return last
}

func isUpper(c byte) bool {
	return c >= 'A' && c <= 'Z'
}

func lastSegment(p string) string {
	if i := strings.LastIndexAny(p, `/\`); i >= 0 {
		return p[i+1:]
	}
	return p
}
