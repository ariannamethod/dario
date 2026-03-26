/*
 * dario_leo.c — Janus model + Dario field + Knowledge Kernel
 *
 * End-to-end: model generates → KK resonates → field modulates → model continues.
 * Leo voice, 24M Janus (RRPRAM + Echo + 3-way gate), BPE 2048.
 *
 * Build:
 *   cc dario_leo.c kk_kernel.c -O2 -lm -lsqlite3 -o dario_leo
 *
 * Run:
 *   ./dario_leo janus_bpe_leo_d12.bin [document.txt] [prompt]
 *
 * by Arianna Method. 2026.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "kk_kernel.h"
#include "leo_bpe_merges.h"

/* ═══════════════════════════════════════════════════════════════════
 * BPE TOKENIZER — encode/decode with exported merges
 * ═══════════════════════════════════════════════════════════════════ */

static int bpe_encode(const char *text, int text_len, int *out, int max_tokens) {
    /* start with bytes */
    int n = 0;
    for (int i = 0; i < text_len && n < max_tokens; i++)
        out[n++] = (unsigned char)text[i];
    /* apply merges in order */
    for (int m = 0; m < BPE_MERGES && n > 1; m++) {
        int a = bpe_merges[m][0], b = bpe_merges[m][1];
        int new_tok = 256 + m;
        int j = 0;
        for (int i = 0; i < n; i++) {
            if (i < n-1 && out[i] == a && out[i+1] == b) {
                out[j++] = new_tok;
                i++; /* skip next */
            } else {
                out[j++] = out[i];
            }
        }
        n = j;
    }
    return n;
}

/* Build decode table: token_id → string */
static char bpe_decode_table[BPE_VOCAB][16];
static int bpe_decode_len[BPE_VOCAB];

static void bpe_init_decode(void) {
    /* bytes 0-255 */
    for (int i = 0; i < 256; i++) {
        bpe_decode_table[i][0] = (char)i;
        bpe_decode_len[i] = 1;
    }
    /* merges: token 256+m = concat(decode[a], decode[b]) */
    for (int m = 0; m < BPE_MERGES; m++) {
        int tok = 256 + m;
        int a = bpe_merges[m][0], b = bpe_merges[m][1];
        int la = bpe_decode_len[a], lb = bpe_decode_len[b];
        if (la + lb < 16) {
            memcpy(bpe_decode_table[tok], bpe_decode_table[a], la);
            memcpy(bpe_decode_table[tok] + la, bpe_decode_table[b], lb);
            bpe_decode_len[tok] = la + lb;
        }
    }
}

static void bpe_decode_token(int tok, char *out, int *out_len) {
    if (tok >= 0 && tok < BPE_VOCAB) {
        memcpy(out, bpe_decode_table[tok], bpe_decode_len[tok]);
        *out_len = bpe_decode_len[tok];
    } else {
        out[0] = '?';
        *out_len = 1;
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * JANUS MODEL — Triple Attention (QKV + RRPRAM + Echo)
 * ═══════════════════════════════════════════════════════════════════ */

static int V, xE, xH, xD, BLK, xM, MT;
#define E xE
#define H xH
#define D xD
#define M xM

static void mm(float *C, const float *A, const float *B, int m, int k, int n) {
    for (int i = 0; i < m; i++)
        for (int j = 0; j < n; j++) {
            float s = 0;
            for (int p = 0; p < k; p++) s += A[i*k+p] * B[p*n+j];
            C[i*n+j] = s;
        }
}

static void mm_t(float *C, const float *A, const float *B, int m, int k, int n) {
    for (int i = 0; i < m; i++)
        for (int j = 0; j < n; j++) {
            float s = 0;
            for (int p = 0; p < k; p++) s += A[i*k+p] * B[j*k+p];
            C[i*n+j] = s;
        }
}

static void rmsnorm(float *out, const float *x, const float *w, int T, int dim) {
    for (int t = 0; t < T; t++) {
        float ss = 0;
        for (int i = 0; i < dim; i++) ss += x[t*dim+i] * x[t*dim+i];
        float inv = 1.0f / sqrtf(ss/dim + 1e-5f);
        for (int i = 0; i < dim; i++) out[t*dim+i] = w[i] * x[t*dim+i] * inv;
    }
}

static void softmax_f(float *x, int n) {
    float mx = x[0];
    for (int i = 1; i < n; i++) if (x[i] > mx) mx = x[i];
    float s = 0;
    for (int i = 0; i < n; i++) { x[i] = expf(x[i] - mx); s += x[i]; }
    for (int i = 0; i < n; i++) x[i] /= s;
}

static float siluf(float x) { return x > -20 ? x/(1+expf(-x)) : 0; }

#define MAX_BLK 24
typedef struct {
    float *tok_emb, *pos_emb;
    struct {
        float *rms1, *wq, *wk, *wv, *wr, *wvr, *wj, *gate, *wo;
        float *rms2, *wg, *wu, *wd;
    } b[MAX_BLK];
    float *rms_f, *head;
} Weights;

static int param_count(void) {
    int s = V*E + MT*E;
    for (int i = 0; i < BLK; i++)
        s += E + E*E + E*E + E*E + H*E*MT + E*E + E*E + H*3 + E*E + E + M*E + M*E + E*M;
    s += E + V*E;
    return s;
}

static void assign_weights(Weights *w, float *p) {
    w->tok_emb = p; p += V*E;
    w->pos_emb = p; p += MT*E;
    for (int i = 0; i < BLK; i++) {
        w->b[i].rms1 = p; p += E;
        w->b[i].wr = p;   p += H*E*MT;
        w->b[i].gate = p; p += H*3;
        w->b[i].wq = p;   p += E*E;
        w->b[i].wk = p;   p += E*E;
        w->b[i].wv = p;   p += E*E;
        w->b[i].wvr = p;  p += E*E;
        w->b[i].wj = p;   p += E*E;
        w->b[i].wo = p;   p += E*E;
        w->b[i].rms2 = p; p += E;
        w->b[i].wg = p;   p += M*E;
        w->b[i].wu = p;   p += M*E;
        w->b[i].wd = p;   p += E*M;
    }
    w->rms_f = p; p += E;
    w->head = p;
}

/* Forward pass — returns logits AND hidden state for KK resonance */
static void janus_forward(Weights *w, int *tok, int T, float *logits, float *hidden_out) {
    float *x = calloc(T*E, 4);
    float *rn = calloc(T*E, 4);
    float sc = 1.0f / sqrtf((float)D);

    for (int t = 0; t < T; t++)
        for (int e = 0; e < E; e++)
            x[t*E+e] = w->tok_emb[tok[t]*E+e] + w->pos_emb[t*E+e];

    float *cat = calloc(T*E, 4);
    float *ao = calloc(T*E, 4);
    float *r1 = calloc(T*E, 4);
    float *mg = calloc(T*M, 4);
    float *mu = calloc(T*M, 4);
    float *mo = calloc(T*E, 4);

    for (int bl = 0; bl < BLK; bl++) {
        rmsnorm(rn, x, w->b[bl].rms1, T, E);

        float *qa = calloc(T*E, 4);
        float *ka = calloc(T*E, 4);
        float *va = calloc(T*E, 4);
        float *vra = calloc(T*E, 4);
        mm_t(qa, rn, w->b[bl].wq, T, E, E);
        mm_t(ka, rn, w->b[bl].wk, T, E, E);
        mm_t(va, rn, w->b[bl].wv, T, E, E);
        mm_t(vra, rn, w->b[bl].wvr, T, E, E);

        float *echo = calloc(T*E, 4);
        mm_t(echo, rn, w->b[bl].wj, T, E, E);
        float *eback = calloc(T*E, 4);
        mm(eback, echo, w->b[bl].wj, T, E, E);

        float *jsc = calloc(T, 4);
        for (int t = 0; t < T; t++) {
            float s = 0;
            for (int e = 0; e < E; e++) s += rn[t*E+e] * eback[t*E+e];
            jsc[t] = s / sqrtf((float)E);
        }
        float *jat = calloc(T*T, 4);
        for (int i = 0; i < T; i++) {
            for (int j = 0; j < T; j++)
                jat[i*T+j] = (j > i) ? -1e9f : jsc[i] * jsc[j];
            softmax_f(jat + i*T, T);
        }

        float gs[16][3];
        for (int h = 0; h < H; h++) {
            gs[h][0] = w->b[bl].gate[h*3+0];
            gs[h][1] = w->b[bl].gate[h*3+1];
            gs[h][2] = w->b[bl].gate[h*3+2];
            softmax_f(gs[h], 3);
        }

        memset(cat, 0, T*E*4);
        float *at = calloc(T*T, 4);
        float *ho = calloc(T*D, 4);

        for (int h = 0; h < H; h++) {
            float *q = calloc(T*D, 4), *k = calloc(T*D, 4), *v = calloc(T*D, 4);
            for (int t = 0; t < T; t++)
                for (int d = 0; d < D; d++) {
                    q[t*D+d] = qa[t*E + h*D + d];
                    k[t*D+d] = ka[t*E + h*D + d];
                    v[t*D+d] = va[t*E + h*D + d];
                }

            for (int i = 0; i < T; i++) {
                for (int j = 0; j < T; j++) {
                    if (j > i) { at[i*T+j] = -1e9f; continue; }
                    float s = 0;
                    for (int d = 0; d < D; d++) s += q[i*D+d] * k[j*D+d];
                    at[i*T+j] = s * sc;
                }
                softmax_f(at + i*T, T);
            }
            mm(ho, at, v, T, T, D);

            float *wr_h = w->b[bl].wr + h*E*MT;
            float *rrp_sc = calloc(MT, 4);
            for (int j = 0; j < T; j++) {
                float s = 0;
                for (int e = 0; e < E; e++) s += rn[j*E+e] * wr_h[e*MT+j];
                rrp_sc[j] = s * sc;
            }
            float *ra = calloc(T*T, 4);
            for (int i = 0; i < T; i++) {
                for (int j = 0; j < T; j++)
                    ra[i*T+j] = (j > i) ? -1e9f : rrp_sc[j];
                softmax_f(ra + i*T, T);
            }
            float *rv = calloc(T*D, 4);
            for (int t = 0; t < T; t++)
                for (int d = 0; d < D; d++)
                    rv[t*D+d] = vra[t*E + h*D + d];
            float *ro = calloc(T*D, 4);
            mm(ro, ra, rv, T, T, D);

            float *jv = calloc(T*D, 4);
            for (int t = 0; t < T; t++)
                for (int d = 0; d < D; d++)
                    jv[t*D+d] = echo[t*E + h*D + d];
            float *jo = calloc(T*D, 4);
            mm(jo, jat, jv, T, T, D);

            for (int t = 0; t < T; t++)
                for (int d = 0; d < D; d++)
                    cat[t*E + h*D + d] = gs[h][0]*ho[t*D+d]
                                       + gs[h][1]*ro[t*D+d]
                                       + gs[h][2]*jo[t*D+d];
            free(q); free(k); free(v); free(ra); free(rv); free(ro);
            free(jv); free(jo); free(rrp_sc);
        }

        mm_t(ao, cat, w->b[bl].wo, T, E, E);
        for (int i = 0; i < T*E; i++) r1[i] = x[i] + ao[i];

        rmsnorm(rn, r1, w->b[bl].rms2, T, E);
        mm_t(mg, rn, w->b[bl].wg, T, E, M);
        mm_t(mu, rn, w->b[bl].wu, T, E, M);
        for (int i = 0; i < T*M; i++) mg[i] = siluf(mg[i]) * mu[i];
        mm_t(mo, mg, w->b[bl].wd, T, M, E);
        for (int i = 0; i < T*E; i++) x[i] = r1[i] + mo[i];

        free(qa); free(ka); free(va); free(vra);
        free(echo); free(eback); free(jsc); free(jat);
        free(at); free(ho);
    }

    rmsnorm(rn, x, w->rms_f, T, E);
    mm_t(logits, rn, w->head, T, E, V);

    /* export last hidden state for KK resonance */
    if (hidden_out)
        memcpy(hidden_out, rn + (T-1)*E, E * sizeof(float));

    free(x); free(rn); free(cat); free(ao); free(r1);
    free(mg); free(mu); free(mo);
}

/* ═══════════════════════════════════════════════════════════════════
 * MAIN — model + KK + Dario field
 * ═══════════════════════════════════════════════════════════════════ */

int main(int argc, char **argv) {
    srand(time(NULL));

    if (argc < 2) {
        printf("dario_leo — Janus + Knowledge Kernel + Dario Field\n\n");
        printf("usage: %s weights.bin [document.txt] [prompt]\n", argv[0]);
        printf("  weights.bin   — janus_bpe_leo_d12.bin (24M, BPE 2048)\n");
        printf("  document.txt  — optional: ingest into KK for resonance\n");
        printf("  prompt        — optional: generation seed (default: interactive)\n");
        return 1;
    }

    /* Load model */
    const char *weights_path = argv[1];
    const char *doc_path = argc > 2 ? argv[2] : NULL;
    const char *prompt = argc > 3 ? argv[3] : "Q: What is resonance?";

    FILE *f = fopen(weights_path, "rb");
    if (!f) { printf("ERROR: cannot open %s\n", weights_path); return 1; }
    int hdr[7];
    fread(hdr, 4, 7, f);
    V = hdr[0]; xE = hdr[1]; xH = hdr[2]; xD = hdr[3];
    BLK = hdr[4]; xM = hdr[5]; MT = hdr[6];
    printf("[janus] V=%d E=%d H=%d D=%d B=%d M=%d T=%d\n", V, E, H, D, BLK, M, MT);

    int np = param_count();
    printf("[janus] %d params (%.1fM)\n", np, np/1e6);
    float *data = malloc(np * sizeof(float));

    /* detect f16 marker: 8th int == 16 means f16 weights */
    int dtype_marker = 0;
    long pos = ftell(f);
    fread(&dtype_marker, 4, 1, f);
    if (dtype_marker == 16) {
        printf("[janus] f16 weights — loading and converting\n");
        unsigned short *f16 = malloc(np * sizeof(unsigned short));
        fread(f16, sizeof(unsigned short), np, f);
        /* f16 → f32 conversion (IEEE 754) */
        for (int i = 0; i < np; i++) {
            unsigned int h = f16[i];
            unsigned int sign = (h >> 15) & 1;
            unsigned int exp = (h >> 10) & 0x1F;
            unsigned int mant = h & 0x3FF;
            unsigned int f32_bits;
            if (exp == 0) {
                if (mant == 0) f32_bits = sign << 31;
                else { exp = 1; while (!(mant & 0x400)) { mant <<= 1; exp--; }
                    mant &= 0x3FF; f32_bits = (sign<<31)|((exp+127-15)<<23)|(mant<<13); }
            } else if (exp == 31) {
                f32_bits = (sign<<31)|0x7F800000|(mant<<13);
            } else {
                f32_bits = (sign<<31)|((exp+127-15)<<23)|(mant<<13);
            }
            memcpy(&data[i], &f32_bits, 4);
        }
        free(f16);
    } else {
        /* f32: rewind and read */
        fseek(f, pos, SEEK_SET);
        fread(data, sizeof(float), np, f);
    }
    fclose(f);

    Weights w;
    assign_weights(&w, data);
    bpe_init_decode();
    printf("[janus] loaded. Leo voice.\n");

    /* KK setup */
    kk_ctx *kk = NULL;
    if (doc_path) {
        const char *db_path = "/tmp/dario_leo_kk.db";
        remove(db_path);
        kk = kk_open(db_path);
        if (kk) {
            kk_set_namespace(kk, "leo", "public", "Leo's knowledge");
            int chunks = kk_ingest_file(kk, doc_path, "leo", "public");
            kk_stats stats;
            kk_get_stats(kk, &stats);
            printf("[kk] ingested %s → %d chunks (metaweights built)\n", doc_path, stats.chunks);
        }
    }

    /* Encode prompt */
    int ctx[4096];
    int len = bpe_encode(prompt, strlen(prompt), ctx, 4096);
    printf("[prompt] \"%s\" → %d tokens\n\n", prompt, len);

    /* Generation loop */
    float hidden[1024]; /* max embedding dim */
    printf("--- generation ---\n");

    for (int step = 0; step < 200; step++) {
        int T = len < MT ? len : MT;
        int *tok = ctx + (len > MT ? len - MT : 0);
        float *lg = calloc(T * V, 4);

        janus_forward(&w, tok, T, lg, hidden);
        float *last = lg + (T-1)*V;

        /* KK resonance: hidden state → find knowledge → inject into logits */
        if (kk && step % 3 == 0) {
            /* project hidden[E] → KK embedding */
            float kk_emb[KK_META_AFFINITY_DIM];
            int stride = E / KK_META_AFFINITY_DIM;
            if (stride < 1) stride = 1;
            for (int i = 0; i < KK_META_AFFINITY_DIM; i++) {
                float s = 0;
                for (int j = 0; j < stride && i*stride+j < E; j++)
                    s += hidden[i*stride+j];
                kk_emb[i] = s / stride;
            }
            float emax = 0;
            for (int i = 0; i < KK_META_AFFINITY_DIM; i++)
                if (fabsf(kk_emb[i]) > emax) emax = fabsf(kk_emb[i]);
            if (emax > 0)
                for (int i = 0; i < KK_META_AFFINITY_DIM; i++) kk_emb[i] /= emax;

            /* decode last N tokens as text query */
            char last_text[256] = {0};
            int lt = 0;
            for (int i = (len > 8 ? len - 8 : 0); i < len && lt < 250; i++) {
                char buf[16]; int bl;
                bpe_decode_token(ctx[i], buf, &bl);
                for (int b = 0; b < bl && lt < 250; b++) last_text[lt++] = buf[b];
            }

            kk_result *res = NULL;
            int nres = kk_query_resonant(kk, last_text, kk_emb, KK_META_AFFINITY_DIM,
                                         "public", "leo", 3, KK_PROFILE_TINY, &res);
            if (nres > 0) {
                /* INJECT: encode resonating chunk text into BPE tokens,
                 * boost those token logits proportional to resonance score.
                 * This is how KK knowledge flows into generation. */
                for (int ri = 0; ri < nres; ri++) {
                    if (!res[ri].text) continue;
                    float boost = (float)res[ri].resonance * 2.0f;
                    int chunk_toks[512];
                    int cn = bpe_encode(res[ri].text, strlen(res[ri].text) < 200 ? strlen(res[ri].text) : 200,
                                       chunk_toks, 512);
                    for (int ci = 0; ci < cn; ci++) {
                        int tid = chunk_toks[ci];
                        if (tid >= 0 && tid < V)
                            last[tid] += boost;
                    }
                }
                /* trace */
                if (res[0].rrpram_resonance > 0.1) {
                    fprintf(stderr, "\r[kk step=%d res=%.2f rrpram=%.2f] ",
                            step, res[0].resonance, res[0].rrpram_resonance);
                }
                kk_free_results(res, nres);
            }
        }

        /* Temperature sampling */
        float temp = 0.8f;
        for (int i = 0; i < V; i++) last[i] /= temp;
        softmax_f(last, V);

        float r = (float)rand() / RAND_MAX, cum = 0;
        int next = 0;
        for (int i = 0; i < V; i++) { cum += last[i]; if (cum >= r) { next = i; break; } }

        /* Decode and print */
        char dec[16]; int dl;
        bpe_decode_token(next, dec, &dl);
        for (int i = 0; i < dl; i++) putchar(dec[i]);
        fflush(stdout);

        if (len < 4096) ctx[len++] = next;
        free(lg);
    }

    printf("\n\n--- done ---\n");
    if (kk) kk_close(kk);
    free(data);
    return 0;
}
