/*
 * infer_v4.c — Janus v4 176M C inference
 *
 * Low-rank RRPRAM (wr_a[H,E,R] x wr_b[H,R,T]) + Echo + 3-way gate
 * RoPE, non-parametric RMSNorm, nanochat residual lambdas
 * V=32768 E=640 H=10 D=64 B=20 M=1664 T=1024 R=64
 *
 * gcc infer_v4.c -O3 -lm -o infer_v4
 * ./infer_v4 janus_v4_leo.bin "Q: What is resonance?"
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

static int V,E,H,D,B,M,T,R;

static void mm_t(float *C, const float *A, const float *B, int m, int k, int n) {
    for (int i = 0; i < m; i++)
        for (int j = 0; j < n; j++) {
            float s = 0;
            for (int p = 0; p < k; p++) s += A[i*k+p] * B[j*k+p];
            C[i*n+j] = s;
        }
}

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
    for (int i = 0; i < dim; i += 2) {
        float freq = 1.0f / powf(10000.0f, (float)i / (float)dim);
        float val = pos * freq;
        float cv = cosf(val), sv = sinf(val);
        float q0 = q[i], q1 = q[i+1];
        q[i] = q0*cv - q1*sv; q[i+1] = q0*sv + q1*cv;
        float k0 = k[i], k1 = k[i+1];
        k[i] = k0*cv - k1*sv; k[i+1] = k0*sv + k1*cv;
    }
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
    w->resid_l = p; p += 20;
    w->x0_l = p; p += 20;
    w->smear_l = p; p += 1;
    w->backout_l = p; p += 1;
    w->smear_g = p; p += 24;
    w->wte = p; p += V * E;
    for (int i = 0; i < B; i++) {
        w->b[i].cq = p; p += E*E;
        w->b[i].ck = p; p += E*E;
        w->b[i].cv = p; p += E*E;
        w->b[i].wr_a = p; p += H*E*R;
        w->b[i].wr_b = p; p += H*R*T;
        w->b[i].wvr = p; p += E*E;
        w->b[i].wj = p; p += E*E;
        w->b[i].gate = p; p += H*3;
        w->b[i].cproj = p; p += E*E;
        w->b[i].wg = p; p += M*E;
        w->b[i].wu = p; p += M*E;
        w->b[i].wd = p; p += E*M;
    }
    w->head = p;
}

/* KV cache for autoregressive generation */
static float *kv_k; /* [B, seqlen, H, D] */
static float *kv_v; /* [B, seqlen, H, D] */
static float *kv_vr; /* [B, seqlen, H, D] */
static int kv_len;

static void kv_init(int max_seq) {
    kv_k = calloc((size_t)B * max_seq * E, sizeof(float));
    kv_v = calloc((size_t)B * max_seq * E, sizeof(float));
    kv_vr = calloc((size_t)B * max_seq * E, sizeof(float));
    kv_len = 0;
}

/* Forward one token at position pos, using KV cache */
static void forward_token(Weights *w, int tok, int pos, float *logits, float *hidden) {
    float x[1024]; /* E <= 1024 */
    float rn[1024], rn2[1024];
    float sc = 1.0f / sqrtf((float)D);

    /* embed */
    for (int e = 0; e < E; e++) x[e] = w->wte[tok * E + e];

    for (int bl = 0; bl < B; bl++) {
        rmsnorm(rn, x, E);

        /* QKV projections */
        float qa[1024], ka[1024], va[1024], vra[1024];
        mm_t(qa, rn, w->b[bl].cq, 1, E, E);
        mm_t(ka, rn, w->b[bl].ck, 1, E, E);
        mm_t(va, rn, w->b[bl].cv, 1, E, E);
        mm_t(vra, rn, w->b[bl].wvr, 1, E, E);

        /* RoPE on Q and K per head */
        for (int h = 0; h < H; h++)
            rope_pos(qa + h*D, ka + h*D, pos, D);

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
            float attn[2048]; /* max T */
            for (int j = 0; j <= pos; j++) {
                float *kj = kv_k + ((size_t)bl * T + j) * E + h*D;
                float s = 0;
                for (int d = 0; d < D; d++) s += q_h[d] * kj[d];
                attn[j] = s * sc;
            }
            softmax_f(attn, pos + 1);

            /* Content output: attn @ cached_V */
            float c_out[128]; /* D */
            memset(c_out, 0, D * sizeof(float));
            for (int j = 0; j <= pos; j++) {
                float *vj = kv_v + ((size_t)bl * T + j) * E + h*D;
                for (int d = 0; d < D; d++) c_out[d] += attn[j] * vj[d];
            }

            /* RRPRAM: low-rank score for current position
             * mid[R] = rn[E] @ wr_a[h][E,R]
             * score[j] = mid[R] . wr_b[h][R, j]  for j in 0..pos */
            float *wr_a_h = w->b[bl].wr_a + h*E*R;
            float *wr_b_h = w->b[bl].wr_b + h*R*T;
            float mid[128]; /* R */
            for (int r = 0; r < R; r++) {
                float s = 0;
                for (int e = 0; e < E; e++) s += rn[e] * wr_a_h[e*R+r];
                mid[r] = s;
            }
            float r_attn[2048];
            for (int j = 0; j <= pos; j++) {
                float s = 0;
                for (int r = 0; r < R; r++) s += mid[r] * wr_b_h[r*T+j];
                r_attn[j] = s * sc;
            }
            softmax_f(r_attn, pos + 1);

            /* RRPRAM output: r_attn @ cached_Vr */
            float r_out[128];
            memset(r_out, 0, D * sizeof(float));
            for (int j = 0; j <= pos; j++) {
                float *vrj = kv_vr + ((size_t)bl * T + j) * E + h*D;
                for (int d = 0; d < D; d++) r_out[d] += r_attn[j] * vrj[d];
            }

            /* Echo per head */
            float *e_h = echo_out + h*D;

            /* Blend */
            for (int d = 0; d < D; d++)
                cat[h*D+d] = gs[h][0]*c_out[d] + gs[h][1]*r_out[d] + gs[h][2]*e_h[d];
        }

        /* Output projection + residual with nanochat lambda */
        float ao[1024];
        mm_t(ao, cat, w->b[bl].cproj, 1, E, E);
        float rl = w->resid_l[bl];
        for (int e = 0; e < E; e++) x[e] = rl * (x[e] + ao[e]);

        /* MLP */
        rmsnorm(rn2, x, E);
        float mg[2048], mu[2048], mo[1024]; /* M */
        mm_t(mg, rn2, w->b[bl].wg, 1, E, M);
        mm_t(mu, rn2, w->b[bl].wu, 1, E, M);
        for (int i = 0; i < M; i++) mg[i] = siluf(mg[i]) * mu[i];
        mm_t(mo, mg, w->b[bl].wd, 1, M, E);
        for (int e = 0; e < E; e++) x[e] += mo[e];
    }

    rmsnorm(rn, x, E);
    if (hidden) memcpy(hidden, rn, E * sizeof(float));
    mm_t(logits, rn, w->head, 1, E, V);
}

int main(int argc, char **argv) {
    srand(time(NULL));
    if (argc < 2) { printf("usage: %s weights.bin [prompt]\n", argv[0]); return 1; }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("cannot open %s\n", argv[1]); return 1; }
    int hdr[8];
    fread(hdr, 4, 8, f);
    V=hdr[0]; E=hdr[1]; H=hdr[2]; D=hdr[3]; B=hdr[4]; M=hdr[5]; T=hdr[6]; R=hdr[7];
    printf("[janus-v4] V=%d E=%d H=%d D=%d B=%d M=%d T=%d R=%d\n", V,E,H,D,B,M,T,R);

    int np = 20+20+1+1+24+V*E;
    for (int i = 0; i < B; i++) np += E*E*4 + H*E*R + H*R*T + E*E*2 + H*3 + E*E + M*E*2 + E*M;
    np += V*E;
    printf("[janus-v4] %d params (%.1fM)\n", np, np/1e6);

    float *data = malloc((size_t)np * sizeof(float));
    fread(data, sizeof(float), np, f);
    fclose(f);

    Weights w;
    assign(&w, data);
    kv_init(T);
    printf("[janus-v4] loaded. KV cache allocated (%dMB)\n\n", (int)((size_t)B*T*E*3*4/1024/1024));

    const char *prompt = argc > 2 ? argv[2] : "Q: What is resonance?\nA:";
    printf("prompt: \"%s\"\n\n", prompt);

    /* encode prompt as bytes */
    int ctx[4096]; int len = 0;
    for (int i = 0; prompt[i] && len < 4096; i++)
        ctx[len++] = (unsigned char)prompt[i];

    /* process prompt tokens */
    float logits[32768];
    float hidden[640];
    for (int i = 0; i < len; i++) {
        forward_token(&w, ctx[i], i, logits, hidden);
        if (i % 10 == 0) { printf("\rprompt: %d/%d", i+1, len); fflush(stdout); }
    }
    printf("\rprompt: %d/%d done\n\n--- generation ---\n", len, len);

    /* generate */
    for (int step = 0; step < 200 && len < T; step++) {
        float temp = 0.7f;
        for (int i = 0; i < V; i++) logits[i] /= temp;
        softmax_f(logits, V);

        float r = (float)rand() / RAND_MAX, cum = 0;
        int next = 0;
        for (int i = 0; i < V; i++) { cum += logits[i]; if (cum >= r) { next = i; break; } }

        if (next < 256 && next > 31) putchar(next);
        else if (next == 10) putchar('\n');
        else printf("[%d]", next);
        fflush(stdout);

        ctx[len] = next;
        forward_token(&w, next, len, logits, hidden);
        len++;
    }
    printf("\n\ndone.\n");

    free(kv_k); free(kv_v); free(kv_vr);
    free(data);
    return 0;
}
