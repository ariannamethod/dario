/*
 * infer_v4.c — Janus v4 176M C inference (notorch-accelerated)
 *
 * Low-rank RRPRAM (wr_a[H,E,R] x wr_b[H,R,T]) + Echo + 3-way gate
 * RoPE, non-parametric RMSNorm, nanochat residual lambdas
 * V=32768 E=640 H=10 D=64 B=20 M=1664 T=1024 R=64
 *
 * Build: make infer_v4   (links to ariannamethod/notorch.c with BLAS)
 * Run:   ./infer_v4 janus_v4_leo.bin "Q: What is resonance?"
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "ariannamethod/notorch.h"

static int V,E,H,D,B,M,T,R;

/* ── BLAS-accelerated matmul via notorch ── */
static void mm_t(float *C, const float *A, const float *BT, int m, int k, int n) {
    /* C[m,n] = A[m,k] @ BT[n,k]^T — BT is stored transposed */
    nt_blas_mmT(C, A, BT, m, k, n);
}

/* notorch ops — used directly for single-vector operations */
static void rmsnorm(float *o, const float *x, int n) {
    float ss = 0;
    for (int i = 0; i < n; i++) ss += x[i] * x[i];
    float inv = 1.0f / sqrtf(ss / n + 1e-5f);
    for (int i = 0; i < n; i++) o[i] = x[i] * inv;
}

static void softmax_f(float *x, int n) {
    float mx = x[0];
    for (int i = 1; i < n; i++) if (x[i] > mx) mx = x[i];
    float s = 0;
    for (int i = 0; i < n; i++) { x[i] = expf(x[i] - mx); s += x[i]; }
    for (int i = 0; i < n; i++) x[i] /= s;
}

static float siluf(float x) { return x > -20 ? x / (1 + expf(-x)) : 0; }

static void rope_pos(float *q, float *k, int pos, int dim) {
    /* Split-half convention (nanochat/Janus v4): pairs (i, i+D/2), base=100000 */
    int half = dim / 2;
    for (int i = 0; i < half; i++) {
        float freq = 1.0f / powf(100000.0f, (float)(2*i) / (float)dim);
        float val = pos * freq;
        float cv = cosf(val), sv = sinf(val);
        float q0 = q[i], q1 = q[i + half];
        q[i]        = q0 * cv + q1 * sv;
        q[i + half] = q0 * (-sv) + q1 * cv;
        float k0 = k[i], k1 = k[i + half];
        k[i]        = k0 * cv + k1 * sv;
        k[i + half] = k0 * (-sv) + k1 * cv;
    }
}

/* QK-norm: RMSNorm + scale (from nanochat) */
static void qk_norm(float *q, float *k, int dim) {
    rmsnorm(q, q, dim);
    rmsnorm(k, k, dim);
    for (int i = 0; i < dim; i++) { q[i] *= 1.2f; k[i] *= 1.2f; }
}

/* Weight layout: header(8i) + resid_l(20) + x0_l(20) + smear_l(1) + backout_l(1) + smear_g(24)
 * + wte[V,E] + B * (cq ck cv wr_a wr_b wvr wj gate cproj wg wu wd) + head[V,E] */
#define MBL 24
typedef struct {
    float *resid_l, *x0_l, *smear_l, *backout_l, *smear_g;
    float *wte;
    struct {
        float *cq, *ck, *cv, *wr_a, *wr_b, *wvr, *wj, *gate, *cproj;
        float *wg, *wu, *wd;
    } b[MBL];
    float *head;
} Weights;

static void assign(Weights *w, float *p) {
    /* Order matches PyTorch state_dict (from janus_gpt_v4_lowrank.py) */
    w->resid_l = p; p += B;           /* resid_lambdas [20] */
    w->x0_l = p; p += B;              /* x0_lambdas [20] */
    w->smear_l = p; p += 1;           /* smear_lambda [1] */
    w->backout_l = p; p += 1;         /* backout_lambda [1] */
    w->wte = p; p += V * E;           /* transformer.wte.weight [V, E] */
    for (int i = 0; i < B; i++) {
        w->b[i].wr_a = p; p += H*E*R; /* attn.wr_a [H, E, R] */
        w->b[i].wr_b = p; p += H*R*T; /* attn.wr_b [H, R, T] */
        w->b[i].gate = p; p += H*3;   /* attn.gate [H, 3] */
        w->b[i].cq = p; p += E*E;     /* attn.c_q.weight [E, E] */
        w->b[i].ck = p; p += E*E;     /* attn.c_k.weight [E, E] */
        w->b[i].cv = p; p += E*E;     /* attn.c_v.weight [E, E] */
        w->b[i].wvr = p; p += E*E;    /* attn.wvr.weight [E, E] */
        w->b[i].wj = p; p += E*E;     /* attn.wj.weight [E, E] */
        w->b[i].cproj = p; p += E*E;  /* attn.c_proj.weight [E, E] */
        w->b[i].wg = p; p += M*E;     /* mlp.w_gate.weight [M, E] */
        w->b[i].wu = p; p += M*E;     /* mlp.w_up.weight [M, E] */
        w->b[i].wd = p; p += E*M;     /* mlp.w_down.weight [E, M] */
    }
    w->head = p; p += V * E;          /* lm_head.weight [V, E] */
    w->smear_g = p;                    /* smear_gate.weight [1, 24] */
}

/* KV cache for autoregressive generation */
static float *kv_k; /* [B, seqlen, E] */
static float *kv_v; /* [B, seqlen, E] */
static float *kv_vr; /* [B, seqlen, E] */
static float *kv_rrpram_mid; /* [B, H, R] — accumulated RRPRAM intermediate */
static int kv_len;

static void kv_init(int max_seq) {
    kv_k = calloc((size_t)B * max_seq * E, sizeof(float));
    kv_v = calloc((size_t)B * max_seq * E, sizeof(float));
    kv_vr = calloc((size_t)B * max_seq * E, sizeof(float));
    kv_rrpram_mid = calloc((size_t)B * H * R, sizeof(float));
    kv_len = 0;
}

/* Parallel prefill: process all prompt tokens through each block together.
 * Matches Python's parallel attention exactly. After prefill, KV cache is populated
 * and generation continues with forward_token (autoregressive). */
static void prefill_batch(Weights *w, int *toks, int n, float *logits, float *hidden) {
    float *xs = calloc((size_t)n * E, sizeof(float));  /* [n, E] hidden states */
    float *x0s = calloc((size_t)n * E, sizeof(float)); /* [n, E] original embeddings */
    float sc = 1.0f / sqrtf((float)D);

    /* Embed all tokens + NORM (nanochat: x = norm(wte(idx))) */
    for (int p = 0; p < n; p++) {
        for (int e = 0; e < E; e++)
            xs[p*E+e] = w->wte[toks[p]*E+e];
        rmsnorm(xs + p*E, xs + p*E, E);  /* norm BEFORE everything */
    }

    /* Smear: mix previous token embedding into current.
     * gate = smear_lambda * sigmoid(smear_gate @ x[:, :24])
     * x[t] += gate[t] * x[t-1]  (for t >= 1) */
    float smear_l = *w->smear_l;
    if (smear_l > 1e-6f) {
        for (int p = 1; p < n; p++) {
            float dot = 0;
            for (int d = 0; d < 24; d++) dot += w->smear_g[d] * xs[p*E+d];
            float gate = smear_l / (1.0f + expf(-dot));
            for (int e = 0; e < E; e++) xs[p*E+e] += gate * xs[(p-1)*E+e];
        }
    }

    /* x0 = x AFTER norm + smear (nanochat line 602: x0 = x) */
    memcpy(x0s, xs, (size_t)n * E * sizeof(float));

    int backout_layer = B / 2;
    float *x_backout = calloc((size_t)n * E, sizeof(float));

    for (int bl = 0; bl < B; bl++) {
        /* Residual scaling: x = resid_lambda * x + x0_lambda * x0 */
        float rl = w->resid_l[bl], x0l = w->x0_l[bl];
        for (int i = 0; i < n * E; i++)
            xs[i] = rl * xs[i] + x0l * x0s[i];

        /* Norm all positions */
        float *rns = calloc((size_t)n * E, sizeof(float));
        for (int p = 0; p < n; p++)
            rmsnorm(rns + p*E, xs + p*E, E);

        /* QKV projections for all positions: [n, E] @ [E, E]^T = [n, E] */
        float *qa = calloc((size_t)n*E, 4), *ka = calloc((size_t)n*E, 4);
        float *va = calloc((size_t)n*E, 4), *vra = calloc((size_t)n*E, 4);
        nt_blas_mmT(qa, rns, w->b[bl].cq, n, E, E);
        nt_blas_mmT(ka, rns, w->b[bl].ck, n, E, E);
        nt_blas_mmT(va, rns, w->b[bl].cv, n, E, E);
        nt_blas_mmT(vra, rns, w->b[bl].wvr, n, E, E);

        /* RoPE + QK-norm per position per head */
        for (int p = 0; p < n; p++)
            for (int h = 0; h < H; h++) {
                rope_pos(qa + p*E + h*D, ka + p*E + h*D, p, D);
                qk_norm(qa + p*E + h*D, ka + p*E + h*D, D);
            }

        /* Store K, V, Vr in cache for later autoregressive generation */
        for (int p = 0; p < n; p++) {
            size_t off = ((size_t)bl * T + p) * E;
            memcpy(kv_k + off, ka + p*E, E * sizeof(float));
            memcpy(kv_v + off, va + p*E, E * sizeof(float));
            memcpy(kv_vr + off, vra + p*E, E * sizeof(float));
        }

        /* Echo: [n, E] @ [E, E]^T */
        float *echo = calloc((size_t)n*E, 4);
        nt_blas_mmT(echo, rns, w->b[bl].wj, n, E, E);

        /* Gate softmax (same for all positions) */
        float gs[16][3];
        for (int h = 0; h < H; h++) {
            gs[h][0]=w->b[bl].gate[h*3]; gs[h][1]=w->b[bl].gate[h*3+1]; gs[h][2]=w->b[bl].gate[h*3+2];
            softmax_f(gs[h], 3);
        }

        /* Per-head attention (parallel over all positions) */
        float *cat = calloc((size_t)n*E, 4);
        for (int h = 0; h < H; h++) {
            /* Content attention: [n, n] scores, causal mask */
            float *scores = calloc((size_t)n*n, 4);
            for (int i = 0; i < n; i++)
                for (int j = 0; j <= i; j++) {
                    float s = 0;
                    float *qi = qa + i*E + h*D;
                    float *kj_p = ka + j*E + h*D;
                    for (int d = 0; d < D; d++) s += qi[d] * kj_p[d];
                    scores[i*n+j] = s * sc;
                }
            /* Softmax per row (causal) */
            for (int i = 0; i < n; i++) {
                for (int j = i+1; j < n; j++) scores[i*n+j] = -1e30f;
                softmax_f(scores + i*n, n);
            }
            /* Weighted sum of V */
            for (int i = 0; i < n; i++) {
                float c_out[128] = {0};
                for (int j = 0; j < n; j++)
                    for (int d = 0; d < D; d++)
                        c_out[d] += scores[i*n+j] * va[j*E + h*D + d];

                /* RRPRAM (broadcast pattern) */
                float *wr_a_h = w->b[bl].wr_a + h*E*R;
                float *wr_b_h = w->b[bl].wr_b + h*R*T;
                /* intermediate = sum_t sum_e x[t,e] * wr_a[h,e,r] for t=0..n-1 */
                float mid[128] = {0};
                for (int t = 0; t < n; t++)
                    for (int r = 0; r < R; r++)
                        for (int e = 0; e < E; e++)
                            mid[r] += rns[t*E+e] * wr_a_h[e*R+r];
                /* scores = mid @ wr_b * sc, broadcast */
                float r_scores[2048];
                for (int j = 0; j < n; j++) {
                    float s = 0;
                    for (int r = 0; r < R; r++) s += mid[r] * wr_b_h[r*T+j];
                    r_scores[j] = s * sc;
                }
                /* RRPRAM attention: attn[i,j] = softmax(r_scores[j] for j<=i) */
                float r_attn[2048];
                for (int j = 0; j <= i; j++) r_attn[j] = r_scores[j];
                for (int j = i+1; j < n; j++) r_attn[j] = -1e30f;
                softmax_f(r_attn, n);
                float r_out[128] = {0};
                for (int j = 0; j < n; j++)
                    for (int d = 0; d < D; d++)
                        r_out[d] += r_attn[j] * vra[j*E + h*D + d];

                /* Echo (simplified - gate is ~0 so minimal impact) */
                float *e_h = echo + i*E + h*D;

                /* Blend */
                for (int d = 0; d < D; d++)
                    cat[i*E + h*D + d] = gs[h][0]*c_out[d] + gs[h][1]*r_out[d] + gs[h][2]*e_h[d];
            }
            free(scores);
        }

        /* Output projection: [n, E] @ [E, E]^T + residual */
        float *ao = calloc((size_t)n*E, 4);
        nt_blas_mmT(ao, cat, w->b[bl].cproj, n, E, E);
        for (int i = 0; i < n*E; i++) xs[i] += ao[i];

        if (bl == backout_layer) memcpy(x_backout, xs, (size_t)n*E*4);

        /* MLP: norm → gate/up → silu*up → down + residual */
        float *rn2s = calloc((size_t)n*E, 4);
        for (int p = 0; p < n; p++) rmsnorm(rn2s + p*E, xs + p*E, E);
        float *mg = calloc((size_t)n*M, 4), *mu = calloc((size_t)n*M, 4), *mo = calloc((size_t)n*E, 4);
        nt_blas_mmT(mg, rn2s, w->b[bl].wg, n, E, M);
        nt_blas_mmT(mu, rn2s, w->b[bl].wu, n, E, M);
        for (int i = 0; i < n*M; i++) mg[i] = siluf(mg[i]) * mu[i];
        nt_blas_mmT(mo, mg, w->b[bl].wd, n, M, E);
        for (int i = 0; i < n*E; i++) xs[i] += mo[i];

        free(rns); free(qa); free(ka); free(va); free(vra);
        free(echo); free(cat); free(ao); free(rn2s); free(mg); free(mu); free(mo);
    }

    /* Backout */
    float bl_val = *w->backout_l;
    for (int i = 0; i < n*E; i++) xs[i] -= bl_val * x_backout[i];

    /* Final norm + head for last position */
    float rn_final[1024];
    rmsnorm(rn_final, xs + (n-1)*E, E);
    if (hidden) memcpy(hidden, rn_final, E * sizeof(float));
    mm_t(logits, rn_final, w->head, 1, E, V);
    for (int i = 0; i < V; i++) logits[i] = 15.0f * tanhf(logits[i] / 15.0f);


    free(xs); free(x0s); free(x_backout);
}

/* Forward one token at position pos, using KV cache */
static void forward_token(Weights *w, int tok, int pos, float *logits, float *hidden) {
    float x[1024]; /* E <= 1024 */
    float rn[1024], rn2[1024];
    float sc = 1.0f / sqrtf((float)D);

    /* embed + norm (nanochat: x = norm(wte(idx))) */
    for (int e = 0; e < E; e++) x[e] = w->wte[tok * E + e];
    rmsnorm(x, x, E);

    /* smear: mix previous token (from KV cache position pos-1 block 0 input) */
    /* For autoregressive, smear uses prev_embedding stored externally */
    /* TODO: full smear for autoregressive (minor effect, smear_lambda=0.32) */

    /* x0 = embedding AFTER norm+smear (nanochat line 602: x0 = x) */
    float x0[1024];
    memcpy(x0, x, E * sizeof(float));

    int backout_layer = B / 2;
    static float x_backout[1024]; /* cached mid-layer residual */

    for (int bl = 0; bl < B; bl++) {
        /* nanochat residual scaling: x = resid_lambda * x + x0_lambda * x0 (BEFORE block) */
        float rl = w->resid_l[bl];
        float x0l = w->x0_l[bl];
        for (int e = 0; e < E; e++)
            x[e] = rl * x[e] + x0l * x0[e];

        /* Block: attn(norm(x)) + x, then mlp(norm(x)) + x */
        rmsnorm(rn, x, E);

        /* QKV projections */
        float qa[1024], ka[1024], va[1024], vra[1024];
        mm_t(qa, rn, w->b[bl].cq, 1, E, E);
        mm_t(ka, rn, w->b[bl].ck, 1, E, E);
        mm_t(va, rn, w->b[bl].cv, 1, E, E);
        mm_t(vra, rn, w->b[bl].wvr, 1, E, E);

        /* RoPE + QK-norm per head */
        for (int h = 0; h < H; h++) {
            rope_pos(qa + h*D, ka + h*D, pos, D);
            qk_norm(qa + h*D, ka + h*D, D);
        }

        /* store K, V, Vr in cache */
        size_t off = ((size_t)bl * T + pos) * E;
        memcpy(kv_k + off, ka, E * sizeof(float));
        memcpy(kv_v + off, va, E * sizeof(float));
        memcpy(kv_vr + off, vra, E * sizeof(float));

        /* Echo */
        float echo_out[1024];
        mm_t(echo_out, rn, w->b[bl].wj, 1, E, E);

        /* Gate softmax */
        float gs[16][3];
        for (int h = 0; h < H; h++) {
            gs[h][0] = w->b[bl].gate[h*3];
            gs[h][1] = w->b[bl].gate[h*3+1];
            gs[h][2] = w->b[bl].gate[h*3+2];
            softmax_f(gs[h], 3);
        }

        float cat[1024];
        memset(cat, 0, E * sizeof(float));

        for (int h = 0; h < H; h++) {
            float *q_h = qa + h*D;

            /* Content attention: Q @ cached_K^T */
            float attn[2048];
            for (int j = 0; j <= pos; j++) {
                float *kj = kv_k + ((size_t)bl * T + j) * E + h*D;
                float s = 0;
                for (int d = 0; d < D; d++) s += q_h[d] * kj[d];
                attn[j] = s * sc;
            }
            softmax_f(attn, pos + 1);

            float c_out[128];
            memset(c_out, 0, D * sizeof(float));
            for (int j = 0; j <= pos; j++) {
                float *vj = kv_v + ((size_t)bl * T + j) * E + h*D;
                for (int d = 0; d < D; d++) c_out[d] += attn[j] * vj[d];
            }

            /* RRPRAM low-rank (broadcast pattern):
             * Python: intermediate[h,r] = sum_t sum_e x[t,e] * wr_a[h,e,r]
             *         score[j] = sum_r intermediate[h,r] * wr_b[h,r,j] * sc
             *         attn[i,j] = softmax(score[j] for j<=i)  — SAME score broadcast
             *
             * For autoregressive: accumulate intermediate across positions in cache.
             * rrpram_mid[bl][h][r] += sum_e xn[e] * wr_a[h,e,r] at each new position.
             */
            float *wr_a_h = w->b[bl].wr_a + h*E*R;
            float *wr_b_h = w->b[bl].wr_b + h*R*T;
            /* Accumulate current position's contribution to mid */
            float *mid_cache = kv_rrpram_mid + ((size_t)bl * H + h) * R;
            for (int r = 0; r < R; r++) {
                float s = 0;
                for (int e = 0; e < E; e++) s += rn[e] * wr_a_h[e*R+r];
                mid_cache[r] += s;
            }
            /* Score from accumulated mid */
            float r_attn[2048];
            for (int j = 0; j <= pos; j++) {
                float s = 0;
                for (int r = 0; r < R; r++) s += mid_cache[r] * wr_b_h[r*T+j];
                r_attn[j] = s * sc;
            }
            softmax_f(r_attn, pos + 1);

            float r_out[128];
            memset(r_out, 0, D * sizeof(float));
            for (int j = 0; j <= pos; j++) {
                float *vrj = kv_vr + ((size_t)bl * T + j) * E + h*D;
                for (int d = 0; d < D; d++) r_out[d] += r_attn[j] * vrj[d];
            }

            float *e_h = echo_out + h*D;

            for (int d = 0; d < D; d++)
                cat[h*D+d] = gs[h][0]*c_out[d] + gs[h][1]*r_out[d] + gs[h][2]*e_h[d];
        }

        /* Output projection + residual (x = x + attn_out) */
        float ao[1024];
        mm_t(ao, cat, w->b[bl].cproj, 1, E, E);
        for (int e = 0; e < E; e++) x[e] += ao[e];

        /* Cache mid-layer for backout */
        if (bl == backout_layer)
            memcpy(x_backout, x, E * sizeof(float));

        /* MLP: x = x + mlp(norm(x)) */
        rmsnorm(rn2, x, E);
        float mg[2048], mu[2048], mo[1024];
        mm_t(mg, rn2, w->b[bl].wg, 1, E, M);
        mm_t(mu, rn2, w->b[bl].wu, 1, E, M);
        for (int i = 0; i < M; i++) mg[i] = siluf(mg[i]) * mu[i];
        mm_t(mo, mg, w->b[bl].wd, 1, M, E);
        for (int e = 0; e < E; e++) x[e] += mo[e];

        if (pos == 7 && bl < 3) {
            float norm2 = 0;
            for (int e = 0; e < E; e++) norm2 += x[e] * x[e];
            fprintf(stderr, "  [bl=%d] x[:3]=%.4f,%.4f,%.4f norm=%.4f\n",
                    bl, x[0], x[1], x[2], sqrtf(norm2));
        }
    }

    /* Backout: subtract cached mid-layer residual */
    float bl_val = *w->backout_l;
    for (int e = 0; e < E; e++) x[e] -= bl_val * x_backout[e];

    rmsnorm(rn, x, E);
    if (hidden) memcpy(hidden, rn, E * sizeof(float));
    mm_t(logits, rn, w->head, 1, E, V);

    /* Softcap: logits = 15 * tanh(logits / 15) */
    for (int i = 0; i < V; i++)
        logits[i] = 15.0f * tanhf(logits[i] / 15.0f);

}

/* BPE tokenizer — from notorch */
#include "leo_bpe_merges.h"
#include "janus_v4_bpe_merges.h"

static nt_bpe g_bpe;
static int bpe_ready = 0;

static void init_bpe(int vocab_size) {
    if (bpe_ready) return;
    if (vocab_size > 2048) {
        /* Janus v4: 32K tiktoken vocab */
        nt_bpe_init(&g_bpe, janus_v4_bpe_merges, JANUS_V4_BPE_MERGES);
    } else {
        /* Leo d12: 2048 vocab */
        nt_bpe_init(&g_bpe, bpe_merges, BPE_MERGES);
    }
    bpe_ready = 1;
}

int main(int argc, char **argv) {
    srand(time(NULL));
    if (argc < 2) { printf("usage: %s weights.bin [prompt] [max_tokens] [temp]\n", argv[0]); return 1; }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("cannot open %s\n", argv[1]); return 1; }
    /* Check for JANU magic (v4 format: 256-byte header, weights at offset 256) */
    int magic_buf[2];
    fread(magic_buf, 4, 2, f);
    int hdr[8];
    long n_params = 0;
    if (magic_buf[0] == 0x4A414E55) { /* 'UNAJ' LE = 'JANU' */
        /* v4 JANU format: magic(4) + ver(4) + V,E,H,D,B,M,T,n_params(32) + padding to 256 */
        fread(hdr, 4, 8, f);
        V=hdr[0]; E=hdr[1]; H=hdr[2]; D=hdr[3]; B=hdr[4]; M=hdr[5]; T=hdr[6];
        n_params = hdr[7];
        /* Derive R from n_params: n_params = 66 + 2*V*E + B*(6*E*E + H*R*(E+T) + 3*H + 3*M*E)
         * → R = (n_params - 66 - 2*V*E - B*(6*E*E + 3*H + 3*M*E)) / (B * H * (E + T)) */
        long fixed = 66 + 2L*V*E + (long)B*(6L*E*E + 3*H + 3L*M*E);
        R = (int)((n_params - fixed) / ((long)B * H * (E + T)));
        printf("[janus-v4] JANU format v%d, n_params=%d, R=%d (derived)\n", magic_buf[1], n_params, R);
        fseek(f, 256, SEEK_SET); /* weights start at offset 256 */
    } else {
        /* legacy format: 8 plain ints */
        hdr[0] = magic_buf[0]; hdr[1] = magic_buf[1];
        fread(hdr + 2, 4, 6, f);
        V=hdr[0]; E=hdr[1]; H=hdr[2]; D=hdr[3]; B=hdr[4]; M=hdr[5]; T=hdr[6]; R=hdr[7];
    }
    printf("[janus-v4] V=%d E=%d H=%d D=%d B=%d M=%d T=%d R=%d\n", V,E,H,D,B,M,T,R);

    /* For JANU format, n_params comes from header. For legacy, calculate. */
    long np;
    if (magic_buf[0] == 0x4A414E55) {
        np = n_params;
    } else {
        np = 66 + (long)V*E;
        for (int i = 0; i < B; i++)
            np += 6L*E*E + (long)H*E*R + (long)H*R*T + H*3 + 3L*M*E;
        np += (long)V*E;
    }
    printf("[janus-v4] %ld params (%.1fM)\n", np, np/1e6);

    float *data = malloc((size_t)np * sizeof(float));
    fread(data, sizeof(float), np, f);
    fclose(f);

    Weights w;
    assign(&w, data);
    kv_init(T);
    printf("[janus-v4] loaded. KV cache allocated (%dMB)\n", (int)((size_t)B*T*E*3*4/1024/1024));

    /* BPE tokenizer — notorch native */
    int use_bpe = (V > 256);
    if (use_bpe) {
        init_bpe(V);
        printf("[janus-v4] BPE tokenizer: vocab=%d, merges=%d\n", g_bpe.vocab_size, g_bpe.n_merges);
    }

    const char *prompt = argc > 2 ? argv[2] : "Q: What is resonance?\nA:";
    int max_gen = argc > 3 ? atoi(argv[3]) : 200;
    float temp = argc > 4 ? atof(argv[4]) : 0.6f;
    printf("prompt: \"%s\"\n\n", prompt);

    /* encode prompt */
    int ctx[4096]; int len = 0;
    /* Check if prompt is a .bin file (pre-encoded tokens) */
    int prompt_is_file = (strlen(prompt) > 4 && strcmp(prompt + strlen(prompt) - 4, ".bin") == 0);
    if (prompt_is_file) {
        FILE *pf = fopen(prompt, "rb");
        if (pf) {
            int n; fread(&n, 4, 1, pf);
            fread(ctx, 4, n, pf); len = n;
            fclose(pf);
            printf("(loaded %d tokens from file)\n", len);
        }
    } else if (use_bpe) {
        len = nt_bpe_encode(&g_bpe, prompt, strlen(prompt), ctx, 4096);
    } else {
        for (int i = 0; prompt[i] && len < 4096; i++)
            ctx[len++] = (unsigned char)prompt[i];
    }

    /* Prefill: parallel batch through all blocks (matches Python exactly) */
    float *logits = calloc(V, sizeof(float));
    float *hidden = calloc(E, sizeof(float));
    printf("prefill: %d tokens (parallel)...", len); fflush(stdout);
    prefill_batch(&w, ctx, len, logits, hidden);
    printf(" done\n\n--- generation ---\n");

    /* generate with BPE decode */
    struct timespec ts0, ts1;
    clock_gettime(CLOCK_MONOTONIC, &ts0);

    for (int step = 0; step < max_gen && len < T; step++) {
        /* Repetition penalty: penalize tokens seen in last 32 positions */
        float rep_penalty = 1.3f;
        int window = 32;
        int start = len > window ? len - window : 0;
        for (int j = start; j < len; j++) {
            int t = ctx[j];
            if (t >= 0 && t < V)
                logits[t] = logits[t] > 0 ? logits[t] / rep_penalty : logits[t] * rep_penalty;
        }

        for (int i = 0; i < V; i++) logits[i] /= temp;
        softmax_f(logits, V);

        float r = (float)rand() / RAND_MAX, cum = 0;
        int next = 0;
        for (int i = 0; i < V; i++) { cum += logits[i]; if (cum >= r) { next = i; break; } }

        /* decode token */
        if (use_bpe) {
            char decoded[64];
            int nbytes = nt_bpe_decode(&g_bpe, &next, 1, decoded, 63);
            decoded[nbytes] = '\0';
            printf("%s", decoded);
        } else {
            if (next < 256 && next > 31) putchar(next);
            else if (next == 10) putchar('\n');
            else printf("[%d]", next);
        }
        fflush(stdout);

        ctx[len] = next;
        forward_token(&w, next, len, logits, hidden);
        len++;
    }

    clock_gettime(CLOCK_MONOTONIC, &ts1);
    double elapsed = (ts1.tv_sec - ts0.tv_sec) + (ts1.tv_nsec - ts0.tv_nsec) / 1e9;
    int gen_tokens = len - (int)strlen(prompt);
    printf("\n\n[janus-v4] %d tokens, %.1f tok/s (%.2fs)\n", gen_tokens, gen_tokens / elapsed, elapsed);

    free(kv_k); free(kv_v); free(kv_vr); free(kv_rrpram_mid);
    free(data);
    return 0;
}
