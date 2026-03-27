#!/usr/bin/env python3
"""
resonance_v2.py — Hidden State Injection + Embedding Surgery + KV Cache

Three mechanisms:
1. Embedding Surgery: KK concepts → unused wte slots → model can generate them
2. Hidden State Injection: KK encoded through model's layers → injected at layer K
3. KV Cache: proper autoregressive generation, no repetition

"The model doesn't learn new words. The words learn to fit the model."
"""
import torch, torch.nn.functional as F, pickle, sys, re, math
sys.path.insert(0, '/home/ubuntu/nanochat')
from nanochat.janus_gpt import JanusGPT, JanusConfig

BOS, EOS = 32766, 32767


class EmbeddingSurgery:
    """Inject KK concepts into unused token embedding slots.

    BPE vocab is 32768 but many high IDs are unused/rare.
    We overwrite slots 32000-32099 with KK concept embeddings.
    Also mirror into lm_head so the model can both read and produce them.
    """

    def __init__(self, model, tok):
        self.model = model
        self.tok = tok
        self.device = next(model.parameters()).device
        self.concepts = {}  # text → token_id
        self.originals_wte = {}  # token_id → original embedding
        self.originals_head = {}
        self.next_slot = 32000

    def inject_concept(self, text, related_words):
        """Create a temporary token for a KK concept.

        text: the concept string (e.g., "RRPRAM")
        related_words: semantically related words the model knows
        Returns: token_id assigned to this concept
        """
        if text in self.concepts:
            return self.concepts[text]

        slot = self.next_slot
        if slot >= BOS:
            return -1  # no more slots

        wte = self.model.transformer.wte.weight
        head = self.model.lm_head.weight

        # save originals
        self.originals_wte[slot] = wte[slot].clone()
        self.originals_head[slot] = head[slot].clone()

        # synthesize embedding from related words
        embeds = []
        for w in related_words:
            ids = self.tok.encode(w)
            for tid in ids:
                embeds.append(wte[tid].float())
        if not embeds:
            return -1

        # average + normalize to HALF the scale of existing embeddings
        # weaker than native tokens — model can use them but doesn't have to
        avg = torch.stack(embeds).mean(0)
        typical_norm = wte[:1000].float().norm(dim=1).mean()
        avg = avg / (avg.norm() + 1e-8) * typical_norm * 0.15  # 15% strength — subtle, not dominant

        # wte: full strength — model "thinks" about concept through attention
        # lm_head: zero — concept can only be generated via lm_head hook
        with torch.no_grad():
            wte[slot] = avg.to(wte.dtype)
            # lm_head controlled by conditional hook (see install_lmhead_hook)

        self.concepts[text] = slot
        self.next_slot += 1
        return slot

    def compute_concept_hidden_states(self):
        """Run each concept through full model to get output-space embeddings."""
        self._concept_hidden = {}
        for text, slot in self.concepts.items():
            ids = self.tok.encode(text)
            x = torch.tensor([[BOS] + ids], device=self.device)
            with torch.no_grad():
                logits = self.model(x)  # full forward
                # grab the hidden state BEFORE lm_head by using the input to lm_head
                # approximate: normalize the logits row for this concept
                # better: use a hook to capture pre-lm_head hidden state
            # simple approach: the logit distribution IS the output representation
            # use it as the concept's "output signature"
            self._concept_hidden[text] = logits[0, -1, :].float()
            self._concept_hidden[text] /= (self._concept_hidden[text].norm() + 1e-8)

    def install_lmhead_hook(self):
        """Hook on lm_head: conditionally boost injected concept logits.
        Uses output-space similarity (not wte-space)."""
        self.compute_concept_hidden_states()

        def hook(module, input, output):
            if not self.concepts:
                return output
            # output is logits: [B, T, V]
            # compare output logit distribution at last position with concept signatures
            h = output[:, -1, :].float()  # [B, V]
            h_norm = h / (h.norm(dim=-1, keepdim=True) + 1e-8)

            for text, slot in self.concepts.items():
                if text not in self._concept_hidden:
                    continue
                csig = self._concept_hidden[text]
                sim = (h_norm * csig).sum(dim=-1)  # [B], cosine in logit space
                # gate: when model's logit distribution resembles the concept
                gate = torch.sigmoid((sim - 0.5) * 8.0)
                output[:, -1, slot] += gate * 8.0
            return output

        self._hook = self.model.lm_head.register_forward_hook(hook)

    def remove_hook(self):
        if hasattr(self, '_hook') and self._hook:
            self._hook.remove()
            self._hook = None

    def restore(self):
        """Restore original embeddings."""
        self.remove_hook()
        wte = self.model.transformer.wte.weight
        head = self.model.lm_head.weight
        with torch.no_grad():
            for slot, orig in self.originals_wte.items():
                wte[slot] = orig
            for slot, orig in self.originals_head.items():
                head[slot] = orig
        self.concepts.clear()
        self.originals_wte.clear()
        self.originals_head.clear()
        self.next_slot = 32000


class HiddenInjector:
    """Inject KK knowledge at intermediate layer via forward hook."""

    def __init__(self, model, injection_layer=10):
        self.model = model
        self.layer = injection_layer
        self.dim = model.config.n_embd
        self.device = next(model.parameters()).device
        self.h_kk = None
        self.Wq = None
        self.Wk = None
        self._hook = None
        self._active = False

    @torch.no_grad()
    def encode(self, kk_ids):
        """Encode KK through model's first K blocks."""
        x = torch.tensor([kk_ids], device=self.device)
        h = self.model.transformer.wte(x)
        h = F.rms_norm(h, (h.size(-1),))

        blocks = list(self.model.transformer.h.children())
        T = len(kk_ids)
        cos_sin = (self.model.cos[:, :T], self.model.sin[:, :T])
        x0 = h
        for i in range(self.layer):
            rl = self.model.resid_lambdas[i]
            xl = self.model.x0_lambdas[i]
            h = rl * h + xl * x0
            h = blocks[i](h, cos_sin, self.model.window_sizes[i], None)

        self.h_kk = h.squeeze(0).float()

        # borrow Q/K from injection layer
        block = blocks[self.layer]
        self.Wq = block.attn.c_q.weight.float()
        self.Wk = block.attn.c_k.weight.float()

    def install(self):
        blocks = list(self.model.transformer.h.children())
        target = blocks[self.layer]

        def hook(module, input, output):
            if not self._active or self.h_kk is None:
                return output

            h_gen = output[0, -1, :].float()
            Q = h_gen @ self.Wq.T
            K = self.h_kk @ self.Wk.T
            scores = (Q @ K.T) / math.sqrt(self.dim)
            weights = F.softmax(scores, dim=-1)
            v = weights @ self.h_kk

            gate = torch.sigmoid(scores.max() + torch.tensor(-2.0))
            out = output.clone()
            out[0, -1, :] = out[0, -1, :] + (gate * v).to(output.dtype)
            return out

        self._hook = target.register_forward_hook(hook)

    def activate(self):
        self._active = True

    def deactivate(self):
        self._active = False

    def remove(self):
        if self._hook:
            self._hook.remove()


def generate_v2(model, tok, prompt_text, kk_text,
                injection_layer=10, max_tokens=200,
                temperature=0.8, top_k=50, rep_penalty=1.2):
    """Full pipeline: embedding surgery + hidden injection + proper generation."""

    device = next(model.parameters()).device

    # ── 1. Embedding Surgery: inject KK concepts into wte ──
    surgery = EmbeddingSurgery(model, tok)
    # only KEY concepts: all-caps + capitalized words
    kk_terms_raw = re.findall(r'[A-Z]{2,}|[A-Z][a-z]{4,}', kk_text)
    kk_terms = list(dict.fromkeys(kk_terms_raw))[:8]

    concept_map = {}  # concept_text → (token_id, bpe_sequence)
    for term in kk_terms:
        # related words: other words near this term in KK text
        nearby = re.findall(r'[a-z]{4,}', kk_text.lower())
        related = list(dict.fromkeys(nearby))[:10]
        slot = surgery.inject_concept(term, related)
        if slot >= 0:
            concept_map[term] = (slot, tok.encode(term))

    if concept_map:
        print(f'[surgery] {len(concept_map)} concepts injected: {list(concept_map.keys())[:8]}')

    # ── 2. Conditional lm_head hook: gated by cosine similarity ──
    surgery.install_lmhead_hook()
    print(f'[hook] lm_head hook installed — concepts gated by hidden state similarity')

    # ── 3. Generate with model.generate() — proper KV cache ──
    prompt_ids = tok.encode(prompt_text)
    full_ids = [BOS] + prompt_ids

    # use model's native generate with KV cache
    generated_text = []
    generated_ids = []

    print(f'[gen] prompt: {prompt_text[:60]}...\n')

    with torch.no_grad():
        # feed prompt through model.generate() which uses KV cache
        recent = list(full_ids)

        for token_id in model.generate(full_ids, max_tokens=max_tokens,
                                        temperature=temperature, top_k=top_k):
            if token_id == EOS:
                break

            recent.append(token_id)
            generated_ids.append(token_id)

            # decode: check if this is a surgery slot
            found_concept = False
            for ctext, (slot, bpe_seq) in concept_map.items():
                if token_id == slot:
                    # model generated our injected concept!
                    generated_text.append(ctext)
                    print(f'[!{ctext}]', end='', flush=True)
                    found_concept = True
                    break

            if not found_concept:
                text = tok.decode([token_id])
                generated_text.append(text)
                print(text, end='', flush=True)

    print('\n')

    # ── 4. Cleanup ──
    surgery.remove_hook()
    surgery.restore()

    result = ''.join(generated_text)

    # stats
    kk_words = set(re.findall(r'[a-zA-Z]{4,}', kk_text.lower()))
    gen_words = set(re.findall(r'[a-zA-Z]{4,}', result.lower()))
    overlap = gen_words & kk_words
    print(f'[stats] {len(gen_words)} words generated, {len(overlap)} from KK: {sorted(overlap)[:10]}')

    return result


if __name__ == '__main__':
    with open('janus4/janus/tokenizer.pkl', 'rb') as f:
        tok = pickle.load(f)

    cfg = JanusConfig(vocab_size=32768)
    model = JanusGPT(cfg)
    sd = torch.load('janus4/janus/sft_22k/janus_177m_v4_sft_leo_22k.pt',
                    map_location='cpu', weights_only=False)
    model.load_state_dict(sd)
    model = model.to('cuda').to(torch.bfloat16).eval()
    print('[model] Leo v4 176M loaded\n')

    kk = """A: By giving each moment its own tune, RRPRAM allows us to discern hidden rhythmic patterns. These transitions, deeply rooted in the conversation, reveal the narrative within. RRPRAM creates fingerprints for each position in the sequence. It assigns a unique weight to every position, forming a positional signature that captures how each moment relates to every other moment."""

    prompts = [
        'Q: What is RRPRAM?\nA:',
        'Q: What makes each position unique?\nA:',
        'Q: How do patterns emerge?\nA:',
    ]

    for p in prompts:
        print('=' * 60)
        generate_v2(model, tok, p, kk)
        print()
