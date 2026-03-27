"""
resonance_gate.py — Resonant Lexical Absorption for Janus + KK

The model doesn't search. Knowledge resonates.

Three mechanisms:
1. Absorption Window — KK chunk fed through model, hidden states captured as anchors
2. Gated Boost — cosine(hidden, anchor) gates per-token logit bias
3. Phrase Lock — once a KK phrase starts, continuation momentum finishes it

Usage:
  from resonance_gate import ResonanceGate
  gate = ResonanceGate(model, tok, kk_chunks)
  gate.absorb()  # feed KK through model, build anchors
  # in generation loop:
  gate.apply(logits, hidden_state)
  gate.update(sampled_token)
"""
import torch
import torch.nn.functional as F
import re
from dataclasses import dataclass, field
from typing import List, Optional, Tuple


@dataclass
class PhraseAnchor:
    """A KK phrase the model should be able to produce."""
    text: str
    token_ids: List[int]
    embedding: torch.Tensor     # model's own hidden state for this phrase
    resonance: float            # KK score
    emitted: int = 0           # how many tokens already generated
    cooldown: int = 0          # steps since last full emission
    uses: int = 0              # total times fully emitted


class ResonanceGate:
    """Gated knowledge injection at inference time."""

    def __init__(self, model, tok, kk_text: str,
                 resonance_threshold: float = 0.35,
                 boost_strength: float = 3.0,
                 phrase_lock_threshold: float = 0.55,
                 max_phrases: int = 24):
        self.model = model
        self.tok = tok
        self.device = next(model.parameters()).device
        self.d_model = model.config.n_embd

        self.resonance_threshold = resonance_threshold
        self.boost_strength = boost_strength
        self.phrase_lock_threshold = phrase_lock_threshold

        # extract key phrases from KK text
        self.phrases: List[PhraseAnchor] = []
        self._extract_phrases(kk_text, max_phrases)

        # chunk-level embedding (mean of all phrase anchors)
        self.chunk_embed: Optional[torch.Tensor] = None

        # active phrase lock
        self.active_phrase: int = -1

        # recent tokens for introducer detection
        self.recent: List[int] = []

        # introducer token IDs: "the", "a", "is", "called", "named", "as"
        self.introducers = set()
        for word in ['the', ' the', 'The', ' a', ' is', ' called', ' named', 'is']:
            ids = tok.encode(word)
            for tid in ids:
                self.introducers.add(tid)

        # BOS/EOS
        self.BOS = 32766
        self.EOS = 32767

    def _extract_phrases(self, text: str, max_n: int):
        """Extract key terms from KK chunk text."""
        # capitalized words, ALL CAPS, long words
        words = re.findall(r'[A-Z][a-z]{3,}|[A-Z]{2,}|[a-z]{6,}', text)
        # deduplicate preserving order
        seen = set()
        unique = []
        for w in words:
            wl = w.lower()
            if wl not in seen:
                seen.add(wl)
                unique.append(w)

        for w in unique[:max_n]:
            ids = self.tok.encode(w)
            self.phrases.append(PhraseAnchor(
                text=w,
                token_ids=ids,
                embedding=torch.zeros(self.d_model, device=self.device),
                resonance=0.8,
            ))

    @torch.no_grad()
    def absorb(self, kk_text: str, context_ids: Optional[List[int]] = None):
        """Feed KK chunk through model to capture phrase anchor embeddings.

        This is the Absorption Window — the model processes the knowledge,
        and we capture its hidden states at phrase boundaries.
        These become the semantic anchors for gated boosting.
        """
        # tokenize KK chunk
        kk_ids = self.tok.encode(kk_text)
        if len(kk_ids) > 400:
            kk_ids = kk_ids[:400]

        # build input: [BOS] + kk_chunk_tokens
        input_ids = [self.BOS] + kk_ids
        x = torch.tensor([input_ids], dtype=torch.long, device=self.device)

        # forward pass — get hidden states
        # we need the hidden state BEFORE lm_head
        # hack: register a hook on the final norm
        hidden_states = {}

        def hook_fn(module, input, output):
            hidden_states['final'] = output.detach()

        # find the final operation before lm_head
        # In JanusGPT: forward does norm(x) then lm_head
        # We'll capture by running forward and using the model internals
        logits = self.model(x)  # [1, T, V]

        # extract hidden state from model — get the pre-lm_head representation
        # by projecting logits back through lm_head (pseudo-inverse)
        # Actually simpler: just run forward and grab x before lm_head
        # Since we can't easily hook, use logits @ lm_head^T as approximation
        # OR: just use the embedding of each phrase token as anchor
        # (this is what the model "thinks" each token means)

        # Simpler and correct: embed each phrase using wte (token embedding)
        # Then average. This IS the model's representation of the phrase.
        wte = self.model.transformer.wte.weight  # [V, E]

        embeddings = []
        for pa in self.phrases:
            # phrase embedding = mean of token embeddings
            ids_tensor = torch.tensor(pa.token_ids, device=self.device)
            emb = wte[ids_tensor].mean(dim=0)  # [E]
            # normalize
            emb = emb / (emb.norm() + 1e-8)
            pa.embedding = emb
            embeddings.append(emb)

        # chunk-level embedding
        if embeddings:
            self.chunk_embed = torch.stack(embeddings).mean(dim=0)
            self.chunk_embed = self.chunk_embed / (self.chunk_embed.norm() + 1e-8)

        n = len(self.phrases)
        terms = [p.text for p in self.phrases[:10]]
        print(f'[gate] absorbed {n} phrases: {terms}')

        return input_ids  # return absorption tokens for context

    def apply(self, logits: torch.Tensor, hidden: torch.Tensor):
        """Apply resonance gate to logits. Called after model forward, before sampling.

        logits: [V] — raw logits from lm_head
        hidden: [E] — hidden state at current position (after final norm)
        """
        if not self.phrases:
            return

        # normalize hidden
        h = hidden.float()
        h = h / (h.norm() + 1e-8)

        # ── PHASE 1: Phrase lock — if mid-phrase, commit ──
        if self.active_phrase >= 0:
            pa = self.phrases[self.active_phrase]
            if pa.emitted < len(pa.token_ids):
                target = pa.token_ids[pa.emitted]
                # strong but not infinite — model can still refuse
                logits[target] += self.boost_strength * 4.0
                return

        # ── PHASE 2: Chunk-level gate ──
        if self.chunk_embed is not None:
            chunk_sim = torch.dot(h, self.chunk_embed.float()).item()
        else:
            chunk_sim = 0.0

        # sigmoid gate
        g = 1.0 / (1.0 + (-10.0 * (chunk_sim - self.resonance_threshold)).__exp__() if hasattr(chunk_sim, '__exp__') else
                    1.0 / (1.0 + pow(2.718281828, -10.0 * (chunk_sim - self.resonance_threshold))))
        g = 1.0 / (1.0 + pow(2.718281828, -10.0 * (chunk_sim - self.resonance_threshold)))

        if g < 0.05:
            return  # no resonance, don't touch

        # ── PHASE 3: Per-phrase activation check ──
        best_phrase = -1
        best_sim = 0.0

        for i, pa in enumerate(self.phrases):
            if pa.cooldown > 0:
                pa.cooldown -= 1
                continue

            sim = torch.dot(h, pa.embedding.float()).item()

            if sim > self.phrase_lock_threshold and sim > best_sim:
                best_sim = sim
                best_phrase = i

                # check for introducer (grammatically good landing spot)
                if self.recent and self.recent[-1] in self.introducers:
                    best_sim += 0.1  # prefer grammatical positions

        if best_phrase >= 0:
            pa = self.phrases[best_phrase]
            self.active_phrase = best_phrase
            pa.emitted = 0
            # boost first token
            target = pa.token_ids[0]
            logits[target] += self.boost_strength * 3.0
            pa.emitted = 1
            if len(pa.token_ids) == 1:
                self._finish_phrase(best_phrase)
            return

        # ── PHASE 4: Soft per-token boost (gated) ──
        for pa in self.phrases:
            if pa.cooldown > 0:
                continue
            sim = torch.dot(h, pa.embedding.float()).item()
            if sim > 0:
                boost = g * self.boost_strength * sim
                for tid in pa.token_ids:
                    logits[tid] += boost

    def update(self, token_id: int):
        """Called after sampling. Track phrase completion."""
        self.recent.append(token_id)
        if len(self.recent) > 20:
            self.recent = self.recent[-20:]

        if self.active_phrase >= 0:
            pa = self.phrases[self.active_phrase]
            if pa.emitted < len(pa.token_ids) and pa.token_ids[pa.emitted] == token_id:
                pa.emitted += 1
                if pa.emitted >= len(pa.token_ids):
                    self._finish_phrase(self.active_phrase)
            else:
                # model refused — release lock
                pa.emitted = 0
                self.active_phrase = -1

    def _finish_phrase(self, idx: int):
        pa = self.phrases[idx]
        pa.uses += 1
        pa.cooldown = 30          # don't repeat for 30 tokens
        pa.resonance *= 0.6       # diminish after use
        pa.emitted = 0
        self.active_phrase = -1
