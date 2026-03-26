/*
 * dario_debug.c — Single-step debug: full pipeline trace
 *
 * One forward pass → hidden state → KK resonance → logit injection → top tokens.
 * Shows exactly what happens at each stage.
 *
 * cc dario_debug.c kk_kernel.c -O2 -lm -lsqlite3 -o dario_debug
 * ./dario_debug janus_bpe_leo_d12.bin leo_expanded.txt "Q: What is RRPRAM?"
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <ctype.h>
#include "kk_kernel.h"
#include "leo_bpe_merges.h"

/* ── BPE (same as dario_leo.c) ── */
static int bpe_encode(const char *text, int text_len, int *out, int max_tokens) {
    int n = 0;
    for (int i = 0; i < text_len && n < max_tokens; i++)
        out[n++] = (unsigned char)text[i];
    for (int m = 0; m < BPE_MERGES && n > 1; m++) {
        int a = bpe_merges[m][0], b = bpe_merges[m][1];
        int new_tok = 256 + m;
        int j = 0;
        for (int i = 0; i < n; i++) {
            if (i < n-1 && out[i] == a && out[i+1] == b) { out[j++] = new_tok; i++; }
            else out[j++] = out[i];
        }
        n = j;
    }
    return n;
}

static char bpe_dec[BPE_VOCAB][16];
static int bpe_dlen[BPE_VOCAB];
static void bpe_init(void) {
    for (int i = 0; i < 256; i++) { bpe_dec[i][0] = (char)i; bpe_dlen[i] = 1; }
    for (int m = 0; m < BPE_MERGES; m++) {
        int tok = 256+m, a = bpe_merges[m][0], b = bpe_merges[m][1];
        int la = bpe_dlen[a], lb = bpe_dlen[b];
        if (la+lb < 16) { memcpy(bpe_dec[tok], bpe_dec[a], la); memcpy(bpe_dec[tok]+la, bpe_dec[b], lb); bpe_dlen[tok] = la+lb; }
    }
}
static void bpe_print_token(int tok) {
    if (tok >= 0 && tok < BPE_VOCAB) fwrite(bpe_dec[tok], 1, bpe_dlen[tok], stdout);
    else printf("[?%d]", tok);
}

/* ── Janus model (minimal, same architecture) ── */
static int V, xE, xH, xD, BLK, xM, MT;
#define E xE
#define H xH
#define D xD
#define M xM

static void mm(float *C, const float *A, const float *B, int m, int k, int n) {
    for (int i = 0; i < m; i++) for (int j = 0; j < n; j++) {
        float s = 0; for (int p = 0; p < k; p++) s += A[i*k+p] * B[p*n+j]; C[i*n+j] = s;
    }
}
static void mm_t(float *C, const float *A, const float *B, int m, int k, int n) {
    for (int i = 0; i < m; i++) for (int j = 0; j < n; j++) {
        float s = 0; for (int p = 0; p < k; p++) s += A[i*k+p] * B[j*k+p]; C[i*n+j] = s;
    }
}
static void rmsnorm(float *out, const float *x, const float *w, int T, int dim) {
    for (int t = 0; t < T; t++) {
        float ss = 0; for (int i = 0; i < dim; i++) ss += x[t*dim+i]*x[t*dim+i];
        float inv = 1.0f/sqrtf(ss/dim+1e-5f);
        for (int i = 0; i < dim; i++) out[t*dim+i] = w[i]*x[t*dim+i]*inv;
    }
}
static void softmax_f(float *x, int n) {
    float mx = x[0]; for (int i = 1; i < n; i++) if (x[i]>mx) mx=x[i];
    float s = 0; for (int i = 0; i < n; i++) { x[i]=expf(x[i]-mx); s+=x[i]; }
    for (int i = 0; i < n; i++) x[i]/=s;
}
static float siluf(float x) { return x>-20?x/(1+expf(-x)):0; }

#define MAX_BLK 24
typedef struct {
    float *tok_emb, *pos_emb;
    struct { float *rms1,*wq,*wk,*wv,*wr,*wvr,*wj,*gate,*wo,*rms2,*wg,*wu,*wd; } b[MAX_BLK];
    float *rms_f, *head;
} W;

static int pcnt(void) {
    int s=V*E+MT*E;
    for(int i=0;i<BLK;i++) s+=E+E*E+E*E+E*E+H*E*MT+E*E+E*E+H*3+E*E+E+M*E+M*E+E*M;
    return s+E+V*E;
}
static void assign(W *w, float *p) {
    w->tok_emb=p; p+=V*E; w->pos_emb=p; p+=MT*E;
    for(int i=0;i<BLK;i++) {
        w->b[i].rms1=p; p+=E; w->b[i].wr=p; p+=H*E*MT; w->b[i].gate=p; p+=H*3;
        w->b[i].wq=p; p+=E*E; w->b[i].wk=p; p+=E*E; w->b[i].wv=p; p+=E*E;
        w->b[i].wvr=p; p+=E*E; w->b[i].wj=p; p+=E*E; w->b[i].wo=p; p+=E*E;
        w->b[i].rms2=p; p+=E; w->b[i].wg=p; p+=M*E; w->b[i].wu=p; p+=M*E; w->b[i].wd=p; p+=E*M;
    }
    w->rms_f=p; p+=E; w->head=p;
}

/* Forward — returns logits + hidden state */
static void fwd(W *w, int *tok, int T, float *logits, float *hidden) {
    float *x=calloc(T*E,4), *rn=calloc(T*E,4);
    float sc=1.0f/sqrtf((float)D);
    for(int t=0;t<T;t++) for(int e=0;e<E;e++) x[t*E+e]=w->tok_emb[tok[t]*E+e]+w->pos_emb[t*E+e];
    float *cat=calloc(T*E,4),*ao=calloc(T*E,4),*r1=calloc(T*E,4);
    float *mg=calloc(T*M,4),*mu=calloc(T*M,4),*mo=calloc(T*E,4);
    for(int bl=0;bl<BLK;bl++) {
        rmsnorm(rn,x,w->b[bl].rms1,T,E);
        float *qa=calloc(T*E,4),*ka=calloc(T*E,4),*va=calloc(T*E,4),*vra=calloc(T*E,4);
        mm_t(qa,rn,w->b[bl].wq,T,E,E); mm_t(ka,rn,w->b[bl].wk,T,E,E);
        mm_t(va,rn,w->b[bl].wv,T,E,E); mm_t(vra,rn,w->b[bl].wvr,T,E,E);
        float *echo=calloc(T*E,4); mm_t(echo,rn,w->b[bl].wj,T,E,E);
        float *eback=calloc(T*E,4); mm(eback,echo,w->b[bl].wj,T,E,E);
        float *jsc=calloc(T,4);
        for(int t=0;t<T;t++){float s=0;for(int e=0;e<E;e++)s+=rn[t*E+e]*eback[t*E+e];jsc[t]=s/sqrtf((float)E);}
        float *jat=calloc(T*T,4);
        for(int i=0;i<T;i++){for(int j=0;j<T;j++)jat[i*T+j]=(j>i)?-1e9f:jsc[i]*jsc[j];softmax_f(jat+i*T,T);}
        float gs[16][3];
        for(int h=0;h<H;h++){gs[h][0]=w->b[bl].gate[h*3];gs[h][1]=w->b[bl].gate[h*3+1];gs[h][2]=w->b[bl].gate[h*3+2];softmax_f(gs[h],3);}
        memset(cat,0,T*E*4);
        float *at=calloc(T*T,4),*ho=calloc(T*D,4);
        for(int h=0;h<H;h++){
            float *q=calloc(T*D,4),*k=calloc(T*D,4),*v=calloc(T*D,4);
            for(int t=0;t<T;t++)for(int d=0;d<D;d++){q[t*D+d]=qa[t*E+h*D+d];k[t*D+d]=ka[t*E+h*D+d];v[t*D+d]=va[t*E+h*D+d];}
            for(int i=0;i<T;i++){for(int j=0;j<T;j++){if(j>i){at[i*T+j]=-1e9f;continue;}float s=0;for(int d=0;d<D;d++)s+=q[i*D+d]*k[j*D+d];at[i*T+j]=s*sc;}softmax_f(at+i*T,T);}
            mm(ho,at,v,T,T,D);
            float *wr_h=w->b[bl].wr+h*E*MT;float *rsc=calloc(MT,4);
            for(int j=0;j<T;j++){float s=0;for(int e=0;e<E;e++)s+=rn[j*E+e]*wr_h[e*MT+j];rsc[j]=s*sc;}
            float *ra=calloc(T*T,4);for(int i=0;i<T;i++){for(int j=0;j<T;j++)ra[i*T+j]=(j>i)?-1e9f:rsc[j];softmax_f(ra+i*T,T);}
            float *rv=calloc(T*D,4);for(int t=0;t<T;t++)for(int d=0;d<D;d++)rv[t*D+d]=vra[t*E+h*D+d];
            float *ro=calloc(T*D,4);mm(ro,ra,rv,T,T,D);
            float *jv=calloc(T*D,4);for(int t=0;t<T;t++)for(int d=0;d<D;d++)jv[t*D+d]=echo[t*E+h*D+d];
            float *jo=calloc(T*D,4);mm(jo,jat,jv,T,T,D);
            for(int t=0;t<T;t++)for(int d=0;d<D;d++)cat[t*E+h*D+d]=gs[h][0]*ho[t*D+d]+gs[h][1]*ro[t*D+d]+gs[h][2]*jo[t*D+d];
            free(q);free(k);free(v);free(ra);free(rv);free(ro);free(jv);free(jo);free(rsc);
        }
        mm_t(ao,cat,w->b[bl].wo,T,E,E);for(int i=0;i<T*E;i++)r1[i]=x[i]+ao[i];
        rmsnorm(rn,r1,w->b[bl].rms2,T,E);
        mm_t(mg,rn,w->b[bl].wg,T,E,M);mm_t(mu,rn,w->b[bl].wu,T,E,M);
        for(int i=0;i<T*M;i++)mg[i]=siluf(mg[i])*mu[i];
        mm_t(mo,mg,w->b[bl].wd,T,M,E);for(int i=0;i<T*E;i++)x[i]=r1[i]+mo[i];
        free(qa);free(ka);free(va);free(vra);free(echo);free(eback);free(jsc);free(jat);free(at);free(ho);
    }
    rmsnorm(rn,x,w->rms_f,T,E); mm_t(logits,rn,w->head,T,E,V);
    if(hidden) memcpy(hidden,rn+(T-1)*E,E*sizeof(float));
    free(x);free(rn);free(cat);free(ao);free(r1);free(mg);free(mu);free(mo);
}

/* ── MAIN: one step, full trace ── */
int main(int argc, char **argv) {
    if (argc < 3) { printf("usage: %s weights.bin document.txt [prompt]\n",argv[0]); return 1; }
    srand(time(NULL));

    /* load model */
    FILE *f=fopen(argv[1],"rb"); if(!f){printf("no weights\n");return 1;}
    int hdr[7]; fread(hdr,4,7,f);
    V=hdr[0];xE=hdr[1];xH=hdr[2];xD=hdr[3];BLK=hdr[4];xM=hdr[5];MT=hdr[6];
    int np=pcnt(); float *data=malloc(np*4); fread(data,4,np,f); fclose(f);
    W w; assign(&w,data); bpe_init();
    printf("=== MODEL: V=%d E=%d H=%d D=%d B=%d M=%d T=%d params=%dK ===\n\n",V,E,H,D,BLK,M,MT,np/1000);

    /* KK setup */
    remove("/tmp/dario_debug_kk.db");
    kk_ctx *kk = kk_open("/tmp/dario_debug_kk.db");
    kk_set_namespace(kk, "leo", "public", "Leo knowledge");
    kk_ingest_file(kk, argv[2], "leo", "public");
    kk_stats st; kk_get_stats(kk, &st);
    printf("=== KK: %d chunks from %s ===\n\n", st.chunks, argv[2]);

    /* encode prompt */
    const char *prompt = argc > 3 ? argv[3] : "Q: What is RRPRAM?\nA:";
    int ctx[4096];
    int len = bpe_encode(prompt, strlen(prompt), ctx, 4096);
    printf("=== PROMPT: \"%s\" → %d tokens ===\n", prompt, len);
    printf("tokens: "); for(int i=0;i<len;i++) printf("%d ", ctx[i]); printf("\n\n");

    /* STEP 1: forward pass */
    printf("=== STEP 1: FORWARD PASS (12 blocks × triple attention) ===\n");
    float *logits = calloc(len * V, 4);
    float hidden[1024];
    fwd(&w, ctx, len, logits, hidden);
    float *last = logits + (len-1)*V;

    /* show top 10 tokens BEFORE injection */
    printf("\nTop 10 tokens BEFORE KK injection:\n");
    int top_idx[10]; float top_val[10];
    for(int i=0;i<10;i++){top_idx[i]=0;top_val[i]=-1e9;}
    for(int i=0;i<V;i++){
        for(int j=0;j<10;j++) if(last[i]>top_val[j]){
            for(int k=9;k>j;k--){top_idx[k]=top_idx[k-1];top_val[k]=top_val[k-1];}
            top_idx[j]=i;top_val[j]=last[i];break;
        }
    }
    for(int i=0;i<10;i++){
        printf("  [%4d] logit=%7.3f  \"", top_idx[i], top_val[i]);
        bpe_print_token(top_idx[i]);
        printf("\"\n");
    }

    /* show hidden state summary */
    printf("\nHidden state (last position, dim=%d):\n  ", E);
    float hmin=1e9,hmax=-1e9,hmean=0;
    for(int i=0;i<E;i++){if(hidden[i]<hmin)hmin=hidden[i];if(hidden[i]>hmax)hmax=hidden[i];hmean+=hidden[i];}
    hmean/=E;
    printf("min=%.4f max=%.4f mean=%.4f\n", hmin, hmax, hmean);

    /* STEP 2: KK resonance */
    printf("\n=== STEP 2: KK RESONANCE ===\n");
    float kk_emb[KK_META_AFFINITY_DIM];
    int stride = E/KK_META_AFFINITY_DIM; if(stride<1)stride=1;
    for(int i=0;i<KK_META_AFFINITY_DIM;i++){float s=0;for(int j=0;j<stride&&i*stride+j<E;j++)s+=hidden[i*stride+j];kk_emb[i]=s/stride;}
    float emax=0;for(int i=0;i<KK_META_AFFINITY_DIM;i++)if(fabsf(kk_emb[i])>emax)emax=fabsf(kk_emb[i]);
    if(emax>0)for(int i=0;i<KK_META_AFFINITY_DIM;i++)kk_emb[i]/=emax;

    /* decode prompt as query text */
    char qtxt[256]={0}; int ql=0;
    for(int i=0;i<len&&ql<250;i++){
        if(ctx[i]<256&&ctx[i]>31){qtxt[ql++]=(char)ctx[i];}
        else{char b[16];int bl=bpe_dlen[ctx[i]];memcpy(b,bpe_dec[ctx[i]],bl);for(int j=0;j<bl&&ql<250;j++)qtxt[ql++]=b[j];}
    }
    printf("query text: \"%s\"\n", qtxt);
    printf("embedding[0..7]: ");for(int i=0;i<8;i++)printf("%.3f ",kk_emb[i]);printf("\n\n");

    kk_result *res=NULL;
    int nres=kk_query_resonant(kk,qtxt,kk_emb,KK_META_AFFINITY_DIM,"public","leo",5,KK_PROFILE_BALANCED,&res);
    printf("KK returned %d results:\n", nres);
    for(int i=0;i<nres;i++){
        printf("  [%d] resonance=%.4f lexical=%.4f rrpram=%.4f\n", i, res[i].resonance, res[i].lexical, res[i].rrpram_resonance);
        if(res[i].text){char s[80];strncpy(s,res[i].text,79);s[79]=0;printf("      \"%s...\"\n",s);}
    }

    /* STEP 3: inject */
    printf("\n=== STEP 3: LOGIT INJECTION ===\n");
    int injected = 0;
    for(int ri=0;ri<nres;ri++){
        if(!res[ri].text)continue;
        float boost=(float)res[ri].resonance * 2.0f;
        int ctoks[512];
        int tl=strlen(res[ri].text); if(tl>200)tl=200;
        int cn=bpe_encode(res[ri].text,tl,ctoks,512);
        for(int ci=0;ci<cn;ci++) if(ctoks[ci]>=0&&ctoks[ci]<V){last[ctoks[ci]]+=boost;injected++;}
    }
    printf("injected boost into %d token slots from %d chunks\n", injected, nres);
    if(res)kk_free_results(res,nres);

    /* show top 10 AFTER injection */
    printf("\nTop 10 tokens AFTER KK injection:\n");
    for(int i=0;i<10;i++){top_idx[i]=0;top_val[i]=-1e9;}
    for(int i=0;i<V;i++){
        for(int j=0;j<10;j++) if(last[i]>top_val[j]){
            for(int k=9;k>j;k--){top_idx[k]=top_idx[k-1];top_val[k]=top_val[k-1];}
            top_idx[j]=i;top_val[j]=last[i];break;
        }
    }
    for(int i=0;i<10;i++){
        printf("  [%4d] logit=%7.3f  \"", top_idx[i], top_val[i]);
        bpe_print_token(top_idx[i]);
        printf("\"\n");
    }

    /* ── STEP 4: CONTEXT INJECTION — knowledge resonates, not forces ──
     *
     * Instead of boosting logits (breaks fluency), inject resonating
     * chunk INTO the context. Leo sees it as part of the conversation
     * and naturally incorporates the knowledge.
     *
     * Flow: Leo generates → KK finds resonating chunk → chunk injected
     * into context → Leo continues, now aware of knowledge.
     */
    printf("\n=== STEP 4: CONTEXT-INJECTION GENERATION (30 tokens) ===\n");

    /* Query KK for best resonating chunk */
    kk_result *res2 = NULL;
    int nres2 = kk_query(kk, qtxt, "public", "leo", 1, KK_PROFILE_TINY, &res2);

    int gen_ctx[4096];
    int gen_len = 0;

    /* Build context: original prompt + injected chunk + "A:" */
    /* Format: Q: What is RRPRAM?\n[knowledge: ...chunk text...]\nA: */
    const char *pre = "Q: What is RRPRAM?\n";
    gen_len = bpe_encode(pre, strlen(pre), gen_ctx, 4096);

    if (nres2 > 0 && res2[0].text) {
        /* inject answer portion of chunk, trimmed to fit context window.
         * skip "Q: ..." part, take "A: ..." if present, else take tail. */
        const char *chunk = res2[0].text;
        const char *answer = strstr(chunk, "A: ");
        if (!answer) answer = strstr(chunk, "A:");
        if (!answer) answer = chunk;
        /* trim to fit: leave room for Q + A: markers (~20 tokens) */
        int max_inject = MT - 25; /* tokens budget for injected knowledge */
        char inject_buf[256];
        int alen = strlen(answer);
        if (alen > 150) alen = 150; /* hard cap */
        int ilen = snprintf(inject_buf, sizeof(inject_buf), "%.*s\n", alen, answer);
        int inj_toks = bpe_encode(inject_buf, ilen, gen_ctx + gen_len, max_inject);
        printf("Injected knowledge (res=%.3f): \"%.*s\"\n", res2[0].resonance, 80, answer);
        printf("  → %d tokens added to context\n", inj_toks);
        gen_len += inj_toks;
    }
    if (res2) kk_free_results(res2, nres2);

    const char *post = "A:";
    gen_len += bpe_encode(post, strlen(post), gen_ctx + gen_len, 4096 - gen_len);

    printf("Total context: %d tokens (max %d)\n\n", gen_len, MT);
    printf("Leo sees: ");
    for (int i = 0; i < gen_len; i++) bpe_print_token(gen_ctx[i]);
    printf("\n\n--- Leo speaks ---\n");

    /* Generate */
    for (int step = 0; step < 30; step++) {
        int T = gen_len < MT ? gen_len : MT;
        int *tok = gen_ctx + (gen_len > MT ? gen_len - MT : 0);
        float *lg2 = calloc(T * V, 4);
        fwd(&w, tok, T, lg2, NULL);
        float *l2 = lg2 + (T-1) * V;

        float temp = 0.7f;
        for (int i = 0; i < V; i++) l2[i] /= temp;
        softmax_f(l2, V);
        float rv = (float)rand() / RAND_MAX, cum2 = 0;
        int nxt = 0;
        for (int i = 0; i < V; i++) { cum2 += l2[i]; if (cum2 >= rv) { nxt = i; break; } }

        bpe_print_token(nxt);
        fflush(stdout);
        if (gen_len < 4096) gen_ctx[gen_len++] = nxt;
        free(lg2);
    }

    printf("\n\n=== DONE ===\n");
    free(logits); free(data);
    kk_close(kk);
    return 0;
}
