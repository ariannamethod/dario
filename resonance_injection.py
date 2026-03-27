"""
resonance_injection.py — Hidden State Knowledge Injection for Janus v4

Instead of fighting the model's output (logit boosting = dead zone),
whisper to its inner layers in its own language.

1. Encode KK text through model's OWN first K blocks → hidden state on manifold
2. Cross-attention using model's OWN Q/K projections → relevance-gated injection
3. Add to generation hidden state at layer K → remaining blocks integrate naturally
4. Dario signals modulate the gate, not the logits

"Stop arguing with the model's output. Start whispering to its inner layers."
"""
import torch
import torch.nn.functional as F
import math
from typing import Optional, List


class ResonanceInjection:
    """Inject KK knowledge into model's hidden state at intermediate layer."""

    def __init__(self, model, injection_layer: int = 10, gate_bias: float = -2.0):
        self.model = model
        self.injection_layer = injection_layer
        self.gate_bias = gate_bias
        self.device = next(model.parameters()).device
        self.dim = model.config.n_embd  # 640

        # pre-encoded KK hidden states (on model's manifold at layer K)
        self.h_kk: Optional[torch.Tensor] = None  # [S, dim]
        self.kk_len: int = 0

        # borrow Q/K from injection layer for cross-attention
        block = list(model.transformer.h.children())[injection_layer]
        self.Wq = block.attn.c_q.weight  # [dim, dim]
        self.Wk = block.attn.c_k.weight  # [dim, dim]

        # hook storage
        self._hook_handle = None
        self._injection_active = False

    @torch.no_grad()
    def encode_knowledge(self, kk_token_ids: List[int]):
        """Run KK text through model's first K blocks.

        The result is the model's OWN representation of the knowledge
        at the layer where we'll inject. It's on the learned manifold —
        the model can process it naturally.
        """
        x = torch.tensor([kk_token_ids], device=self.device)

        # get token embeddings
        h = self.model.transformer.wte(x)  # [1, S, dim]

        # normalize like the model does
        h = F.rms_norm(h, (h.size(-1),))

        # smear (nanochat feature) — skip for KK encoding, not critical

        # forward through blocks 0..K
        blocks = list(self.model.transformer.h.children())

        # need cos/sin for RoPE
        T = len(kk_token_ids)
        cos_sin = (self.model.cos[:, :T], self.model.sin[:, :T])

        x0 = h
        for i in range(self.injection_layer):
            rl = self.model.resid_lambdas[i]
            xl = self.model.x0_lambdas[i]
            h = rl * h + xl * x0
            h = blocks[i](h, cos_sin, self.model.window_sizes[i], None)

        self.h_kk = h.squeeze(0).float()  # [S, dim]
        self.kk_len = T
        print(f'[inject] encoded {T} KK tokens through {self.injection_layer} blocks')
        return self.h_kk

    @torch.no_grad()
    def compute_injection(self, h_gen: torch.Tensor) -> torch.Tensor:
        """Compute injection vector for current generation hidden state.

        Uses model's OWN Q/K projections for cross-attention.
        Returns gated injection vector on the manifold.

        h_gen: [dim] — generation hidden state at layer K
        """
        if self.h_kk is None or self.kk_len == 0:
            return torch.zeros(self.dim, device=self.device)

        h = h_gen.float().unsqueeze(0)  # [1, dim]
        kk = self.h_kk.float()  # [S, dim]

        # cross-attention: Q from generation, K from knowledge
        Q = F.linear(h, self.Wq.float())    # [1, dim]
        K = F.linear(kk, self.Wk.float())   # [S, dim]

        # attention scores
        scores = (Q @ K.T) / math.sqrt(self.dim)  # [1, S]

        # relevance = max score (how relevant is the most relevant KK token)
        max_score = scores.max().item()

        # gate: sigmoid(max_score + bias)
        # with bias=-2.0, need max_score > 2.0 to open gate above 0.5
        gate = torch.sigmoid(torch.tensor(max_score + self.gate_bias))

        if gate.item() < 0.01:
            return torch.zeros(self.dim, device=self.device)

        # softmax attention weights
        weights = F.softmax(scores, dim=-1)  # [1, S]

        # weighted sum of KK hidden states
        v_inject = weights @ kk  # [1, dim]
        v_inject = v_inject.squeeze(0)  # [dim]

        # scale by gate
        v_inject = gate * v_inject

        return v_inject.to(h_gen.dtype)

    def install_hook(self):
        """Install forward hook on the injection layer block."""
        blocks = list(self.model.transformer.h.children())
        target_block = blocks[self.injection_layer]

        def hook(module, input, output):
            if not self._injection_active:
                return output
            if self.h_kk is None:
                return output

            # output is the block's output: [B, T, dim]
            # inject into the LAST token position only (autoregressive)
            h_last = output[0, -1, :]  # [dim]
            v = self.compute_injection(h_last)
            output = output.clone()
            output[0, -1, :] = output[0, -1, :] + v
            return output

        self._hook_handle = target_block.register_forward_hook(hook)
        print(f'[inject] hook installed on block {self.injection_layer}')

    def activate(self):
        """Enable injection (call before generation loop)."""
        self._injection_active = True

    def deactivate(self):
        """Disable injection (call during prompt processing)."""
        self._injection_active = False

    def remove_hook(self):
        if self._hook_handle:
            self._hook_handle.remove()
            self._hook_handle = None


def generate_with_injection(model, tok, prompt_ids, kk_text,
                            injection_layer=10, gate_bias=-2.0,
                            max_tokens=300, temperature=0.8, top_k=50,
                            top_p=0.92, rep_penalty=1.15):
    """Full generation pipeline with hidden state injection."""
    BOS, EOS = 32766, 32767
    device = next(model.parameters()).device

    # 1. Setup injector
    injector = ResonanceInjection(model, injection_layer, gate_bias)

    # 2. Encode KK knowledge through model's own layers
    kk_ids = tok.encode(kk_text)
    if len(kk_ids) > 300:
        kk_ids = kk_ids[:300]
    injector.encode_knowledge(kk_ids)

    # 3. Install hook
    injector.install_hook()

    # 4. Process prompt (injection OFF — don't contaminate prompt encoding)
    injector.deactivate()
    prompt_full = [BOS] + prompt_ids
    ids = torch.tensor([prompt_full], device=device)

    generated = list(prompt_full)
    out_text = []

    with torch.no_grad():
        # prefill
        logits = model(ids)[:, -1, :]

        # 5. Generate with injection ON
        injector.activate()

        for step in range(max_tokens):
            if logits.dim() == 3:
                logits = logits[0, -1, :]  # [B, T, V] → [V]
            elif logits.dim() == 2:
                logits = logits[-1, :]     # [T, V] → [V]
            logits = logits.float()

            # ban BOS
            logits[BOS] = float('-inf')

            # rep penalty
            for tid in set(generated[-50:]):
                if logits[tid] > 0:
                    logits[tid] /= rep_penalty
                else:
                    logits[tid] *= rep_penalty

            # temperature
            logits = logits / temperature

            # top-k
            if top_k > 0:
                topk_val, _ = torch.topk(logits, min(top_k, logits.size(-1)))
                logits[logits < topk_val[-1]] = float('-inf')

            probs = F.softmax(logits, dim=-1)

            # top-p
            if top_p < 1.0:
                sorted_probs, sorted_idx = probs.sort(descending=True)
                cumsum = sorted_probs.cumsum(0)
                mask = (cumsum - sorted_probs) > top_p
                sorted_probs[mask] = 0
                sorted_probs /= sorted_probs.sum()
                idx = torch.multinomial(sorted_probs, 1)
                tid = sorted_idx[idx].item()
            else:
                tid = torch.multinomial(probs, 1).item()

            if tid == EOS:
                break

            generated.append(tid)
            out_text.append(tok.decode([tid]))
            print(tok.decode([tid]), end='', flush=True)

            # next step
            next_ids = torch.tensor([[tid]], device=device)
            ids = torch.cat([ids, next_ids], dim=1)
            logits = model(ids[:, -1024:])[:, -1:, :]

    injector.deactivate()
    injector.remove_hook()
    print()
    return ''.join(out_text)
