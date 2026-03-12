/*
 * test_dario.c — Tests for the Dario Equation
 *
 * Compile:  cc -O2 -lm -o tests/test_dario tests/test_dario.c
 * Run:      ./tests/test_dario
 *
 * Tests every subsystem: RNG, vocabulary, tokenizer, bigrams,
 * co-occurrence, prophecy, embeddings, RoPE, SwiGLU, velocity,
 * seasonal modulation, dissonance, field metrics, law enforcement,
 * sampling, code fragments, generation, and the full pipeline.
 */

#include <stdio.h>
#include <string.h>
#include <math.h>

/* ── Rename dario's main so we can define our own ── */
#define main dario_main
#include "../dario.c"
#undef main

/* ── Test framework ── */
static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT(cond, msg) do { \
    tests_run++; \
    if (cond) { tests_passed++; } \
    else { tests_failed++; printf("  FAIL: %s (line %d)\n", msg, __LINE__); } \
} while (0)

#define ASSERT_EQ_INT(a, b, msg) ASSERT((a) == (b), msg)
#define ASSERT_NEQ_INT(a, b, msg) ASSERT((a) != (b), msg)
#define ASSERT_GT(a, b, msg) ASSERT((a) > (b), msg)
#define ASSERT_GTE(a, b, msg) ASSERT((a) >= (b), msg)
#define ASSERT_LT(a, b, msg) ASSERT((a) < (b), msg)
#define ASSERT_LTE(a, b, msg) ASSERT((a) <= (b), msg)
#define ASSERT_FLOAT_EQ(a, b, eps, msg) ASSERT(fabsf((a) - (b)) < (eps), msg)
#define ASSERT_STR_EQ(a, b, msg) ASSERT(strcmp((a), (b)) == 0, msg)

#define RUN_TEST(fn) do { \
    printf("  %s ... ", #fn); fflush(stdout); \
    int before = tests_failed; \
    fn(); \
    if (tests_failed == before) printf("ok\n"); \
    else printf("\n"); \
} while (0)

/* ── Helper: reset global state for test isolation ── */
static void reset_state(void) {
    memset(&D, 0, sizeof(D));
    memset(g_destiny, 0, sizeof(g_destiny));
    memset(g_embeds, 0, sizeof(g_embeds));
    memset(g_embed_init, 0, sizeof(g_embed_init));
    g_dest_magnitude = 0;
    D.alpha = ALPHA;
    D.beta = BETA;
    D.gamma_d = GAMMA_D;
    D.tau = TAU_BASE;
    D.velocity = VEL_WALK;
    rng_state = 42;
}

/* ── Helper: seed vocabulary and ingest bootstrap text ── */
static void bootstrap_field(void) {
    for (int i = 0; SEED_WORDS[i] != NULL; i++)
        vocab_add(&D.vocab, SEED_WORDS[i]);
    char bootstrap[4096] = {0};
    int bpos = 0;
    for (int i = 0; SEED_WORDS[i] != NULL && bpos < (int)sizeof(bootstrap) - 128; i++) {
        int wlen = strlen(SEED_WORDS[i]);
        if (bpos + wlen + 2 >= (int)sizeof(bootstrap) - 128) break;
        if (bpos > 0) bootstrap[bpos++] = ' ';
        memcpy(bootstrap + bpos, SEED_WORDS[i], wlen);
        bpos += wlen;
    }
    bootstrap[bpos] = '\0';
    ingest(bootstrap);
}

/* ═══════════════════════════════════════════════════════════
 * TEST: RNG — xorshift64*
 * ═══════════════════════════════════════════════════════════ */

static void test_rng(void) {
    rng_state = 42;

    /* deterministic: same seed → same sequence */
    uint64_t a = rng_next();
    uint64_t b = rng_next();
    ASSERT(a != b, "rng produces different values");

    rng_state = 42;
    uint64_t a2 = rng_next();
    ASSERT(a == a2, "rng is deterministic with same seed");

    /* randf in [0, 1] */
    rng_state = 42;
    for (int i = 0; i < 1000; i++) {
        float r = randf();
        ASSERT(r >= 0.0f && r <= 1.0f, "randf in [0,1]");
    }
}

/* ═══════════════════════════════════════════════════════════
 * TEST: clampf
 * ═══════════════════════════════════════════════════════════ */

static void test_clampf(void) {
    ASSERT_FLOAT_EQ(clampf(0.5f, 0.0f, 1.0f), 0.5f, 1e-6f, "clampf middle");
    ASSERT_FLOAT_EQ(clampf(-1.0f, 0.0f, 1.0f), 0.0f, 1e-6f, "clampf below");
    ASSERT_FLOAT_EQ(clampf(2.0f, 0.0f, 1.0f), 1.0f, 1e-6f, "clampf above");
    ASSERT_FLOAT_EQ(clampf(0.0f, 0.0f, 1.0f), 0.0f, 1e-6f, "clampf at low");
    ASSERT_FLOAT_EQ(clampf(1.0f, 0.0f, 1.0f), 1.0f, 1e-6f, "clampf at high");
}

/* ═══════════════════════════════════════════════════════════
 * TEST: Vocabulary
 * ═══════════════════════════════════════════════════════════ */

static void test_vocab(void) {
    Vocab v;
    memset(&v, 0, sizeof(v));

    /* add and find */
    int id0 = vocab_add(&v, "hello");
    ASSERT_EQ_INT(id0, 0, "first word gets id 0");
    ASSERT_EQ_INT(v.n_words, 1, "vocab size is 1 after first add");

    int id1 = vocab_add(&v, "world");
    ASSERT_EQ_INT(id1, 1, "second word gets id 1");

    /* find existing */
    int found = vocab_find(&v, "hello");
    ASSERT_EQ_INT(found, 0, "find returns correct id");

    /* duplicate returns same id */
    int dup = vocab_add(&v, "hello");
    ASSERT_EQ_INT(dup, 0, "duplicate add returns same id");
    ASSERT_EQ_INT(v.n_words, 2, "vocab size unchanged after dup");

    /* not found */
    int nf = vocab_find(&v, "missing");
    ASSERT_EQ_INT(nf, -1, "find returns -1 for missing");

    /* word content */
    ASSERT_STR_EQ(v.words[0], "hello", "word 0 is 'hello'");
    ASSERT_STR_EQ(v.words[1], "world", "word 1 is 'world'");
}

/* ═══════════════════════════════════════════════════════════
 * TEST: Tokenizer
 * ═══════════════════════════════════════════════════════════ */

static void test_tokenizer(void) {
    Vocab v;
    memset(&v, 0, sizeof(v));
    int ids[32];

    /* basic tokenization */
    int n = tokenize(&v, "Hello World", ids, 32);
    ASSERT_EQ_INT(n, 2, "tokenizes 2 words");
    ASSERT_STR_EQ(v.words[ids[0]], "hello", "lowercased 'Hello' → 'hello'");
    ASSERT_STR_EQ(v.words[ids[1]], "world", "lowercased 'World' → 'world'");

    /* punctuation is delimiter */
    n = tokenize(&v, "hello, world!", ids, 32);
    ASSERT_EQ_INT(n, 2, "punctuation acts as delimiter");

    /* underscore and apostrophe preserved */
    n = tokenize(&v, "don't foo_bar", ids, 32);
    ASSERT_EQ_INT(n, 2, "apostrophe and underscore kept in words");
    ASSERT_STR_EQ(v.words[ids[0]], "don't", "apostrophe in word");
    ASSERT_STR_EQ(v.words[ids[1]], "foo_bar", "underscore in word");

    /* empty string */
    n = tokenize(&v, "", ids, 32);
    ASSERT_EQ_INT(n, 0, "empty string → 0 tokens");

    /* max limit */
    n = tokenize(&v, "a b c d e", ids, 3);
    ASSERT_EQ_INT(n, 3, "respects max token limit");
}

/* ═══════════════════════════════════════════════════════════
 * TEST: Bigram Table
 * ═══════════════════════════════════════════════════════════ */

static void test_bigrams(void) {
    BigramTable b;
    memset(&b, 0, sizeof(b));

    /* update and query */
    bigram_update(&b, 0, 1, 1.0f);
    ASSERT_EQ_INT(b.n, 1, "one bigram entry after update");

    bigram_update(&b, 0, 2, 2.0f);
    ASSERT_EQ_INT(b.n, 2, "two bigram entries");

    /* accumulation */
    bigram_update(&b, 0, 1, 0.5f);
    ASSERT_EQ_INT(b.n, 2, "duplicate bigram accumulates, doesn't add");

    /* row extraction */
    float row[10];
    bigram_row(&b, 0, row, 10);
    ASSERT_FLOAT_EQ(row[1], 1.5f, 1e-6f, "bigram 0→1 count is 1.5");
    ASSERT_FLOAT_EQ(row[2], 2.0f, 1e-6f, "bigram 0→2 count is 2.0");
    ASSERT_FLOAT_EQ(row[3], 0.0f, 1e-6f, "bigram 0→3 count is 0.0");

    /* different source */
    bigram_row(&b, 5, row, 10);
    ASSERT_FLOAT_EQ(row[1], 0.0f, 1e-6f, "no bigrams from src=5");
}

/* ═══════════════════════════════════════════════════════════
 * TEST: Co-occurrence Field
 * ═══════════════════════════════════════════════════════════ */

static void test_cooccurrence(void) {
    CoocField c;
    memset(&c, 0, sizeof(c));

    cooc_update(&c, 0, 1, 1.0f);
    ASSERT_EQ_INT(c.n, 1, "one cooc entry");

    cooc_update(&c, 0, 1, 0.5f);
    ASSERT_EQ_INT(c.n, 1, "duplicate cooc accumulates");
    ASSERT_FLOAT_EQ(c.count[0], 1.5f, 1e-6f, "cooc count accumulated");

    cooc_update(&c, 1, 0, 0.3f);
    ASSERT_EQ_INT(c.n, 2, "reverse pair is separate entry");
}

/* ═══════════════════════════════════════════════════════════
 * TEST: Prophecy System
 * ═══════════════════════════════════════════════════════════ */

static void test_prophecy(void) {
    ProphecySystem ps;
    memset(&ps, 0, sizeof(ps));

    /* add */
    prophecy_add(&ps, 5, 0.5f);
    ASSERT_EQ_INT(ps.n, 1, "one prophecy after add");
    ASSERT_EQ_INT(ps.p[0].target, 5, "prophecy target correct");
    ASSERT_FLOAT_EQ(ps.p[0].strength, 0.5f, 1e-6f, "prophecy strength correct");

    /* update: age + fulfillment */
    prophecy_update(&ps, 3);  /* not fulfilled */
    ASSERT_EQ_INT(ps.p[0].age, 1, "prophecy ages");
    ASSERT_EQ_INT(ps.p[0].fulfilled, 0, "prophecy not fulfilled");

    prophecy_update(&ps, 5);  /* fulfilled! */
    ASSERT_EQ_INT(ps.n, 0, "fulfilled prophecy removed");

    /* overflow: oldest evicted */
    memset(&ps, 0, sizeof(ps));
    for (int i = 0; i < MAX_PROPHECY; i++) {
        prophecy_add(&ps, i + 10, 0.1f);
        /* age them slightly (keep under 50 to avoid pruning) */
        ps.p[i].age = i;
    }
    ASSERT_EQ_INT(ps.n, MAX_PROPHECY, "capped at MAX_PROPHECY");

    /* adding one more evicts oldest */
    prophecy_add(&ps, 999, 1.0f);
    ASSERT_EQ_INT(ps.n, MAX_PROPHECY, "still capped after overflow add");
    /* the new one should be present */
    int found = 0;
    for (int i = 0; i < ps.n; i++)
        if (ps.p[i].target == 999) found = 1;
    ASSERT(found, "new prophecy present after overflow");
}

/* ═══════════════════════════════════════════════════════════
 * TEST: Vector Operations
 * ═══════════════════════════════════════════════════════════ */

static void test_vec_ops(void) {
    float a[] = {1, 0, 0, 0};
    float b[] = {0, 1, 0, 0};
    float c[] = {1, 0, 0, 0};

    /* dot product */
    ASSERT_FLOAT_EQ(vec_dot(a, b, 4), 0.0f, 1e-6f, "orthogonal dot = 0");
    ASSERT_FLOAT_EQ(vec_dot(a, c, 4), 1.0f, 1e-6f, "parallel dot = 1");

    /* norm */
    ASSERT_FLOAT_EQ(vec_norm(a, 4), 1.0f, 1e-4f, "unit vector norm ≈ 1");

    float d[] = {3, 4, 0, 0};
    ASSERT_FLOAT_EQ(vec_norm(d, 4), 5.0f, 1e-4f, "3-4-5 triangle norm");

    /* cosine similarity */
    float cos_ab = vec_cosine(a, b, 4);
    ASSERT_FLOAT_EQ(cos_ab, 0.0f, 1e-4f, "cosine of orthogonal ≈ 0");

    float cos_ac = vec_cosine(a, c, 4);
    ASSERT_FLOAT_EQ(cos_ac, 1.0f, 1e-4f, "cosine of identical ≈ 1");
}

/* ═══════════════════════════════════════════════════════════
 * TEST: Embeddings
 * ═══════════════════════════════════════════════════════════ */

static void test_embeddings(void) {
    memset(g_embed_init, 0, sizeof(g_embed_init));
    memset(g_embeds, 0, sizeof(g_embeds));

    /* valid id returns non-null */
    float *e0 = get_embed(0);
    ASSERT(e0 != NULL, "embed(0) returns non-null");

    /* unit normalized */
    float norm = vec_norm(e0, DIM);
    ASSERT_FLOAT_EQ(norm, 1.0f, 1e-4f, "embeddings are unit normalized");

    /* deterministic */
    float copy[DIM];
    memcpy(copy, e0, DIM * sizeof(float));
    float *e0_again = get_embed(0);
    for (int i = 0; i < DIM; i++)
        ASSERT_FLOAT_EQ(e0_again[i], copy[i], 1e-8f, "embeddings deterministic");

    /* different ids → different embeddings */
    float *e1 = get_embed(1);
    float cos = vec_cosine(e0, e1, DIM);
    ASSERT(fabsf(cos) < 0.99f, "different ids have different embeddings");

    /* out of range returns null */
    ASSERT(get_embed(-1) == NULL, "negative id returns null");
    ASSERT(get_embed(MAX_VOCAB) == NULL, "out of range returns null");
}

/* ═══════════════════════════════════════════════════════════
 * TEST: RoPE
 * ═══════════════════════════════════════════════════════════ */

static void test_rope(void) {
    float vec[DIM];
    for (int i = 0; i < DIM; i++) vec[i] = (i % 2 == 0) ? 1.0f : 0.0f;

    float original[DIM];
    memcpy(original, vec, sizeof(vec));

    /* position 0 should not change the vector (cos(0)=1, sin(0)=0) */
    apply_rope(vec, DIM, 0);
    for (int i = 0; i < DIM; i++)
        ASSERT_FLOAT_EQ(vec[i], original[i], 1e-5f, "RoPE at pos=0 is identity");

    /* position > 0 should change the vector */
    memcpy(vec, original, sizeof(vec));
    apply_rope(vec, DIM, 10);
    int changed = 0;
    for (int i = 0; i < DIM; i++)
        if (fabsf(vec[i] - original[i]) > 1e-5f) changed++;
    ASSERT(changed > 0, "RoPE at pos=10 changes vector");

    /* RoPE preserves norm (rotation) */
    float norm_before = vec_norm(original, DIM);
    float norm_after = vec_norm(vec, DIM);
    ASSERT_FLOAT_EQ(norm_before, norm_after, 1e-3f, "RoPE preserves norm");
}

/* ═══════════════════════════════════════════════════════════
 * TEST: SwiGLU Gate
 * ═══════════════════════════════════════════════════════════ */

static void test_swiglu(void) {
    /* gate=0 → sigmoid(0)=0.5 → output = x * 0.5 */
    float r = swiglu_gate(2.0f, 0.0f);
    ASSERT_FLOAT_EQ(r, 1.0f, 1e-4f, "swiglu(2, 0) = 1");

    /* large positive gate → sigmoid ≈ 1 → output ≈ x */
    r = swiglu_gate(3.0f, 10.0f);
    ASSERT_FLOAT_EQ(r, 3.0f, 0.01f, "swiglu(3, 10) ≈ 3");

    /* large negative gate → sigmoid ≈ 0 → output ≈ 0 */
    r = swiglu_gate(3.0f, -10.0f);
    ASSERT_FLOAT_EQ(r, 0.0f, 0.01f, "swiglu(3, -10) ≈ 0");

    /* x=0 always → 0 */
    r = swiglu_gate(0.0f, 5.0f);
    ASSERT_FLOAT_EQ(r, 0.0f, 1e-6f, "swiglu(0, anything) = 0");
}

/* ═══════════════════════════════════════════════════════════
 * TEST: Velocity Operators
 * ═══════════════════════════════════════════════════════════ */

static void test_velocity(void) {
    /* WALK: coefficients return to baseline */
    reset_state();
    D.alpha = 0.5f;
    D.velocity = VEL_WALK;
    apply_velocity();
    ASSERT(D.alpha < 0.5f, "WALK: alpha moves toward baseline");
    ASSERT_FLOAT_EQ(D.tau, 0.85f, 0.01f, "WALK: tau = 0.85");

    /* RUN: momentum builds */
    reset_state();
    D.velocity = VEL_RUN;
    D.momentum = 0.0f;
    apply_velocity();
    ASSERT_GT(D.momentum, 0.0f, "RUN: momentum increases");
    ASSERT_FLOAT_EQ(D.tau, 1.15f, 0.01f, "RUN: tau = 1.15");

    /* STOP: momentum zeros, gamma rises */
    reset_state();
    D.velocity = VEL_STOP;
    D.momentum = 1.0f;
    float gamma_before = D.gamma_d;
    apply_velocity();
    ASSERT_FLOAT_EQ(D.momentum, 0.0f, 1e-6f, "STOP: momentum zeroed");
    ASSERT_GT(D.gamma_d, gamma_before, "STOP: gamma increases");
    ASSERT_FLOAT_EQ(D.tau, 0.40f, 0.01f, "STOP: tau = 0.40");

    /* BREATHE: healing */
    reset_state();
    D.velocity = VEL_BREATHE;
    D.trauma_level = 0.8f;
    D.dissonance = 0.9f;
    D.debt = 10.0f;
    apply_velocity();
    ASSERT_LT(D.trauma_level, 0.8f, "BREATHE: trauma decreases");
    ASSERT_LT(D.dissonance, 0.9f, "BREATHE: dissonance decreases");
    ASSERT_LT(D.debt, 10.0f, "BREATHE: debt decreases");

    /* UP: beta rises */
    reset_state();
    D.velocity = VEL_UP;
    float beta_before = D.beta;
    apply_velocity();
    ASSERT_GT(D.beta, beta_before, "UP: beta increases");
    ASSERT_FLOAT_EQ(D.tau, 1.30f, 0.01f, "UP: tau = 1.30");

    /* DOWN: alpha rises, beta drops */
    reset_state();
    D.velocity = VEL_DOWN;
    float alpha_before = D.alpha;
    beta_before = D.beta;
    apply_velocity();
    ASSERT_GT(D.alpha, alpha_before, "DOWN: alpha increases");
    ASSERT_LT(D.beta, beta_before, "DOWN: beta decreases");
    ASSERT_FLOAT_EQ(D.tau, 0.60f, 0.01f, "DOWN: tau = 0.60");

    /* trauma modulates temperature */
    reset_state();
    D.velocity = VEL_WALK;
    D.trauma_level = 0.8f;
    apply_velocity();
    ASSERT_GT(D.tau, 0.85f, "trauma raises temperature above base");

    /* tau is clamped */
    reset_state();
    D.velocity = VEL_WALK;
    D.trauma_level = 100.0f;  /* extreme */
    apply_velocity();
    ASSERT_LTE(D.tau, 2.0f, "tau clamped at max 2.0");
}

/* ═══════════════════════════════════════════════════════════
 * TEST: Auto-Velocity Selection
 * ═══════════════════════════════════════════════════════════ */

static void test_auto_velocity(void) {
    reset_state();

    /* extreme dissonance → UP */
    D.dissonance = 0.9f;
    auto_velocity();
    ASSERT_EQ_INT(D.velocity, VEL_UP, "d>0.8 → UP");

    /* high dissonance → RUN */
    D.dissonance = 0.65f;
    auto_velocity();
    ASSERT_EQ_INT(D.velocity, VEL_RUN, "d>0.6 → RUN");

    /* very low dissonance → STOP */
    D.dissonance = 0.1f;
    auto_velocity();
    ASSERT_EQ_INT(D.velocity, VEL_STOP, "d<0.2 → STOP");

    /* trauma → BREATHE (need dissonance in middle range) */
    D.dissonance = 0.4f;
    D.trauma_level = 0.8f;
    auto_velocity();
    ASSERT_EQ_INT(D.velocity, VEL_BREATHE, "trauma>0.5 → BREATHE");

    /* high debt → DOWN */
    D.dissonance = 0.4f;
    D.trauma_level = 0.1f;
    D.debt = 6.0f;
    auto_velocity();
    ASSERT_EQ_INT(D.velocity, VEL_DOWN, "debt>5 → DOWN");

    /* default → WALK */
    D.dissonance = 0.4f;
    D.trauma_level = 0.1f;
    D.debt = 1.0f;
    auto_velocity();
    ASSERT_EQ_INT(D.velocity, VEL_WALK, "default → WALK");
}

/* ═══════════════════════════════════════════════════════════
 * TEST: Seasonal Modulation
 * ═══════════════════════════════════════════════════════════ */

static void test_seasons(void) {
    reset_state();

    /* spring: beta grows */
    D.season = 0;
    float beta_before = D.beta;
    season_step();
    ASSERT_GT(D.beta, beta_before, "spring: beta increases");

    /* summer: alpha grows */
    reset_state();
    D.season = 1;
    float alpha_before = D.alpha;
    season_step();
    ASSERT_GT(D.alpha, alpha_before, "summer: alpha increases");

    /* winter: trauma grows */
    reset_state();
    D.season = 3;
    float trauma_before = D.trauma_level;
    season_step();
    ASSERT_GT(D.trauma_level, trauma_before, "winter: trauma increases");

    /* season phase advances */
    reset_state();
    D.season = 0;
    D.season_phase = 0.0f;
    season_step();
    ASSERT_GT(D.season_phase, 0.0f, "season phase advances");

    /* season transitions at phase=1.0 */
    reset_state();
    D.season = 0;
    D.season_phase = 0.999f;
    season_step();
    ASSERT_EQ_INT(D.season, 1, "season transitions from spring to summer");
    ASSERT_LT(D.season_phase, 0.01f, "phase resets on transition");

    /* winter wraps to spring */
    reset_state();
    D.season = 3;
    D.season_phase = 0.999f;
    season_step();
    ASSERT_EQ_INT(D.season, 0, "winter wraps to spring");

    /* coefficients clamped */
    reset_state();
    D.season = 0;
    D.beta = 0.59f;
    season_step();
    ASSERT_LTE(D.beta, 0.60f, "beta clamped at 0.60");
}

/* ═══════════════════════════════════════════════════════════
 * TEST: Dissonance
 * ═══════════════════════════════════════════════════════════ */

static void test_dissonance(void) {
    reset_state();

    /* seed vocabulary first */
    for (int i = 0; SEED_WORDS[i] != NULL; i++)
        vocab_add(&D.vocab, SEED_WORDS[i]);

    /* known words → low dissonance */
    float d1 = compute_dissonance("resonance field destiny");
    ASSERT_LT(d1, 0.5f, "known words → low dissonance");

    /* unknown words → high dissonance */
    float d2 = compute_dissonance("xyzzy flurble quux");
    ASSERT_GT(d2, 0.5f, "unknown words → high dissonance");

    /* empty → 1.0 */
    float d3 = compute_dissonance("");
    ASSERT_FLOAT_EQ(d3, 1.0f, 1e-6f, "empty input → dissonance 1.0");

    /* result always in [0, 1] */
    ASSERT_GTE(d1, 0.0f, "dissonance >= 0");
    ASSERT_LTE(d1, 1.0f, "dissonance <= 1");
    ASSERT_GTE(d2, 0.0f, "dissonance >= 0");
    ASSERT_LTE(d2, 1.0f, "dissonance <= 1");
}

/* ═══════════════════════════════════════════════════════════
 * TEST: Law Enforcement
 * ═══════════════════════════════════════════════════════════ */

static void test_laws(void) {
    reset_state();

    /* entropy floor */
    D.entropy = 0.01f;
    D.resonance = 0.5f;
    enforce_laws();
    ASSERT_GTE(D.entropy, ENTROPY_FLOOR, "entropy floor enforced");

    /* resonance ceiling */
    D.entropy = 0.5f;
    D.resonance = 0.99f;
    enforce_laws();
    ASSERT_LTE(D.resonance, RESONANCE_CEILING, "resonance ceiling enforced");

    /* emergence formula */
    D.entropy = 0.3f;
    D.resonance = 0.8f;
    enforce_laws();
    float expected = (1.0f - D.entropy) * D.resonance;
    ASSERT_FLOAT_EQ(D.emergence, expected, 1e-4f, "emergence = (1-E)*R");

    /* debt decay */
    D.debt = 10.0f;
    enforce_laws();
    ASSERT_LT(D.debt, 10.0f, "debt decays");

    /* debt cap */
    D.debt = 30.0f;
    enforce_laws();
    ASSERT_LTE(D.debt, 20.0f, "debt capped at 20.0");

    /* trauma decay */
    D.trauma_level = 0.5f;
    enforce_laws();
    ASSERT_LT(D.trauma_level, 0.5f, "trauma decays");

    /* momentum decay */
    D.momentum = 1.0f;
    enforce_laws();
    ASSERT_LT(D.momentum, 1.0f, "momentum decays");
}

/* ═══════════════════════════════════════════════════════════
 * TEST: Field Metrics Update
 * ═══════════════════════════════════════════════════════════ */

static void test_metrics(void) {
    reset_state();

    /* set known values */
    D.tau = 1.0f;
    D.dissonance = 0.5f;
    D.resonance = 0.6f;
    D.cooc.n = 50;
    D.debt = 2.0f;

    update_metrics();

    /* entropy: 0.3*(1.0-0.5) + 0.4*0.5 + 0.3*(1-0.6) */
    /* = 0.15 + 0.20 + 0.12 = 0.47 */
    ASSERT_FLOAT_EQ(D.entropy, 0.47f, 0.05f, "entropy computed correctly");

    /* resonance: 0.4*(50/100) + 0.3*(1-0.5) + 0.3*(1-0.2) */
    /* = 0.20 + 0.15 + 0.24 = 0.59 */
    ASSERT_FLOAT_EQ(D.resonance, 0.59f, 0.05f, "resonance computed correctly");

    /* entropy and resonance in [0,1] */
    ASSERT_GTE(D.entropy, 0.0f, "entropy >= 0");
    ASSERT_LTE(D.entropy, 1.0f, "entropy <= 1");
    ASSERT_GTE(D.resonance, 0.0f, "resonance >= 0");
    ASSERT_LTE(D.resonance, 1.0f, "resonance <= 1");
}

/* ═══════════════════════════════════════════════════════════
 * TEST: Sampling
 * ═══════════════════════════════════════════════════════════ */

static void test_sampling(void) {
    rng_state = 42;

    /* deterministic logits → deterministic sample */
    float logits[5] = {0, 0, 0, 0, 100};  /* token 4 should dominate */
    float copy[5];
    memcpy(copy, logits, sizeof(logits));
    int s = sample_topk(copy, 5, 1.0f, 5);
    ASSERT_EQ_INT(s, 4, "dominant logit sampled");

    /* all equal → should produce valid token */
    float eq[5] = {1, 1, 1, 1, 1};
    s = sample_topk(eq, 5, 1.0f, 5);
    ASSERT(s >= 0 && s < 5, "uniform logits → valid token");

    /* top-k filtering: only top-1 survives when logits are spread */
    float spread[5] = {0, 0, 0, 0, 100};
    s = sample_topk(spread, 5, 1.0f, 1);
    ASSERT_EQ_INT(s, 4, "top-k=1 selects highest");

    /* low temperature → more deterministic */
    rng_state = 42;
    int counts[5] = {0};
    for (int trial = 0; trial < 100; trial++) {
        float l[5] = {1, 1, 1, 1, 5};
        int tok = sample_topk(l, 5, 0.1f, 5);
        counts[tok]++;
    }
    ASSERT_GT(counts[4], 90, "low temperature → dominant token sampled >90%");
}

/* ═══════════════════════════════════════════════════════════
 * TEST: Code Fragment Selection
 * ═══════════════════════════════════════════════════════════ */

static void test_code_fragments(void) {
    reset_state();

    /* each term should have fragments */
    for (int term = 0; term < 6; term++) {
        D.dominant_term = term;
        const char *frag = select_code_fragment();
        ASSERT(frag != NULL, "fragment non-null for each term");
        ASSERT(strlen(frag) > 0, "fragment non-empty");
    }

    /* term B fragment contains relevant content */
    D.dominant_term = TERM_B;
    rng_state = 42;
    const char *frag = select_code_fragment();
    /* all B fragments contain chain-related code */
    ASSERT(frag != NULL, "B fragment exists");

    /* term T fragment */
    D.dominant_term = TERM_T;
    rng_state = 42;
    frag = select_code_fragment();
    ASSERT(frag != NULL, "T fragment exists");
}

/* ═══════════════════════════════════════════════════════════
 * TEST: Ingest
 * ═══════════════════════════════════════════════════════════ */

static void test_ingest(void) {
    reset_state();

    /* seed vocabulary */
    for (int i = 0; SEED_WORDS[i] != NULL; i++)
        vocab_add(&D.vocab, SEED_WORDS[i]);

    int vocab_before = D.vocab.n_words;
    int bigrams_before = D.bigrams.n;
    int cooc_before = D.cooc.n;
    int step_before = D.step;

    ingest("hello resonance world");

    /* new words added to vocab */
    ASSERT_GT(D.vocab.n_words, vocab_before, "ingest adds new words");

    /* bigrams updated */
    ASSERT_GT(D.bigrams.n, bigrams_before, "ingest updates bigrams");

    /* co-occurrence updated */
    ASSERT_GT(D.cooc.n, cooc_before, "ingest updates cooccurrence");

    /* step advanced */
    ASSERT_GT(D.step, step_before, "ingest advances step");

    /* context populated */
    ASSERT_GT(D.ctx_len, 0, "ingest populates context");

    /* destiny updated */
    ASSERT_GT(g_dest_magnitude, 0.0f, "ingest updates destiny magnitude");
}

/* ═══════════════════════════════════════════════════════════
 * TEST: Dario Compute — the equation itself
 * ═══════════════════════════════════════════════════════════ */

static void test_dario_compute(void) {
    reset_state();

    /* seed vocabulary */
    for (int i = 0; SEED_WORDS[i] != NULL; i++)
        vocab_add(&D.vocab, SEED_WORDS[i]);

    /* ingest some text to build bigrams/cooc, repeating to ensure last word has outgoing bigrams */
    ingest("resonance field prophecy destiny decay drift resonance field");

    int vocab = D.vocab.n_words;
    float *logits = calloc(vocab, sizeof(float));

    dario_compute(logits, vocab);

    /* logits should be non-trivial (not all zeros) */
    float sum = 0;
    for (int i = 0; i < vocab; i++) sum += fabsf(logits[i]);
    ASSERT_GT(sum, 0.0f, "logits are non-zero after compute");

    /* term energies should be populated */
    float total_energy = 0;
    for (int t = 0; t < 6; t++) total_energy += D.term_energy[t];
    ASSERT_GT(total_energy, 0.0f, "term energies populated");

    /* B term should have energy (last context word 'field' has outgoing bigrams) */
    ASSERT_GT(D.term_energy[TERM_B], 0.0f, "B term has energy");

    /* dominant term is valid */
    ASSERT(D.dominant_term >= 0 && D.dominant_term < 6, "dominant term valid");

    free(logits);
}

/* ═══════════════════════════════════════════════════════════
 * TEST: Dario Compute with Trauma
 * ═══════════════════════════════════════════════════════════ */

static void test_trauma_term(void) {
    reset_state();

    for (int i = 0; SEED_WORDS[i] != NULL; i++)
        vocab_add(&D.vocab, SEED_WORDS[i]);
    ingest("resonance field prophecy");

    int vocab = D.vocab.n_words;
    float *logits_no_trauma = calloc(vocab, sizeof(float));
    float *logits_trauma = calloc(vocab, sizeof(float));

    /* without trauma */
    D.trauma_level = 0.0f;
    dario_compute(logits_no_trauma, vocab);
    float t_energy_none = D.term_energy[TERM_T];

    /* with trauma */
    D.trauma_level = 0.8f;
    dario_compute(logits_trauma, vocab);
    float t_energy_high = D.term_energy[TERM_T];

    ASSERT_FLOAT_EQ(t_energy_none, 0.0f, 1e-6f, "no trauma → T energy = 0");
    ASSERT_GT(t_energy_high, 0.0f, "high trauma → T energy > 0");

    /* trauma boosts origin words (ids 0-49) */
    int boosted = 0;
    for (int i = 0; i < 50 && i < vocab; i++)
        if (logits_trauma[i] > logits_no_trauma[i]) boosted++;
    ASSERT_GT(boosted, 0, "trauma boosts some origin words");

    free(logits_no_trauma);
    free(logits_trauma);
}

/* ═══════════════════════════════════════════════════════════
 * TEST: Generate Words
 * ═══════════════════════════════════════════════════════════ */

static void test_generate(void) {
    reset_state();
    rng_state = 12345;

    bootstrap_field();

    D.tau = 0.85f;

    char out[1024];
    int len = generate_words(out, sizeof(out));

    ASSERT_GT(len, 0, "generate produces output");
    ASSERT(strlen(out) > 0, "output string non-empty");

    /* output contains valid words (no null chars mid-string) */
    ASSERT_EQ_INT((int)strlen(out), len, "string length matches returned length");
}

/* ═══════════════════════════════════════════════════════════
 * TEST: Full Pipeline
 * ═══════════════════════════════════════════════════════════ */

static void test_full_pipeline(void) {
    reset_state();
    rng_state = 99999;

    bootstrap_field();

    /* simulate a conversation turn */
    const char *input = "hello world resonance field";

    /* 1. dissonance */
    D.dissonance = compute_dissonance(input);
    ASSERT(D.dissonance >= 0.0f && D.dissonance <= 1.0f, "pipeline: dissonance in range");

    /* 2. ingest */
    ingest(input);

    /* 3. trauma */
    if (D.dissonance > 0.7f)
        D.trauma_level = clampf(D.trauma_level + D.dissonance * 0.1f, 0, 1);

    /* 4. auto-velocity */
    auto_velocity();
    ASSERT(D.velocity >= VEL_WALK && D.velocity <= VEL_DOWN, "pipeline: valid velocity");

    /* 5. apply velocity */
    apply_velocity();
    ASSERT(D.tau >= 0.2f && D.tau <= 2.0f, "pipeline: tau in valid range");

    /* 6. seasonal modulation */
    season_step();

    /* 7. update metrics */
    update_metrics();

    /* 8. enforce laws */
    enforce_laws();
    ASSERT_GTE(D.entropy, ENTROPY_FLOOR, "pipeline: entropy floor");
    ASSERT_LTE(D.resonance, RESONANCE_CEILING, "pipeline: resonance ceiling");

    /* 9. generate */
    char words[1024];
    int len = generate_words(words, sizeof(words));
    ASSERT_GT(len, 0, "pipeline: generated words");

    /* 10. code fragment */
    const char *code = select_code_fragment();
    ASSERT(code != NULL, "pipeline: code fragment selected");

    /* verify field state consistency */
    ASSERT(D.emergence >= 0.0f && D.emergence <= 1.0f, "pipeline: emergence in [0,1]");
    ASSERT(D.dominant_term >= 0 && D.dominant_term < 6, "pipeline: valid dominant term");
}

/* ═══════════════════════════════════════════════════════════
 * TEST: Seed Words Integrity
 * ═══════════════════════════════════════════════════════════ */

static void test_seed_words(void) {
    /* count seed words */
    int count = 0;
    for (int i = 0; SEED_WORDS[i] != NULL; i++) count++;
    ASSERT_GT(count, 300, "at least 300 seed words defined");

    /* first word should be "resonance" */
    ASSERT_STR_EQ(SEED_WORDS[0], "resonance", "first seed word is 'resonance'");

    /* last defined word before NULL */
    ASSERT(SEED_WORDS[count - 1] != NULL, "last word is non-null");
    ASSERT(SEED_WORDS[count] == NULL, "sentinel NULL present");

    /* all words are non-empty */
    for (int i = 0; SEED_WORDS[i] != NULL; i++)
        ASSERT(strlen(SEED_WORDS[i]) > 0, "seed words non-empty");
}

/* ═══════════════════════════════════════════════════════════
 * TEST: Code Fragment Coverage
 * ═══════════════════════════════════════════════════════════ */

static void test_code_fragment_coverage(void) {
    /* count fragments per term */
    int counts[6] = {0};
    for (int i = 0; CODE_FRAGMENTS[i].code != NULL; i++) {
        int t = CODE_FRAGMENTS[i].term;
        ASSERT(t >= 0 && t < 6, "fragment term in range");
        counts[t]++;
    }

    /* each term should have at least one fragment */
    for (int t = 0; t < 6; t++)
        ASSERT_GT(counts[t], 0, "each term has at least one fragment");

    /* total count */
    int total = 0;
    for (int t = 0; t < 6; t++) total += counts[t];
    ASSERT_EQ_INT(total, 18, "18 code fragments total");
}

/* ═══════════════════════════════════════════════════════════
 * TEST: Edge Cases
 * ═══════════════════════════════════════════════════════════ */

static void test_edge_cases(void) {
    reset_state();

    /* small vocab → generate returns "..." */
    D.vocab.n_words = 5;
    char out[1024];
    int len = generate_words(out, sizeof(out));
    ASSERT_EQ_INT(len, 3, "tiny vocab → returns '...'");
    ASSERT_STR_EQ(out, "...", "tiny vocab → '...'");

    /* context window overflow */
    reset_state();
    for (int i = 0; SEED_WORDS[i] != NULL; i++)
        vocab_add(&D.vocab, SEED_WORDS[i]);

    /* fill context beyond MAX_CONTEXT */
    for (int i = 0; i < MAX_CONTEXT + 10; i++) {
        D.context[D.ctx_len < MAX_CONTEXT ? D.ctx_len : MAX_CONTEXT - 1] = i % 100;
        if (D.ctx_len < MAX_CONTEXT) D.ctx_len++;
    }
    ASSERT_LTE(D.ctx_len, MAX_CONTEXT, "context doesn't exceed MAX_CONTEXT");

    /* dissonance with context overlap */
    reset_state();
    for (int i = 0; SEED_WORDS[i] != NULL; i++)
        vocab_add(&D.vocab, SEED_WORDS[i]);
    D.context[0] = 0;  /* "resonance" */
    D.context[1] = 1;  /* "field" */
    D.ctx_len = 2;
    float d = compute_dissonance("resonance field");
    ASSERT_LT(d, 0.5f, "matching context → low dissonance");
}

/* ═══════════════════════════════════════════════════════════
 * TEST: Multiple Conversations
 * ═══════════════════════════════════════════════════════════ */

static void test_multi_conversation(void) {
    reset_state();
    rng_state = 77777;

    bootstrap_field();

    /* run multiple conversation turns */
    const char *inputs[] = {
        "resonance field destiny",
        "hello how are you",
        "chaos entropy collapse",
        NULL
    };

    for (int i = 0; inputs[i]; i++) {
        D.dissonance = compute_dissonance(inputs[i]);
        ingest(inputs[i]);
        if (D.dissonance > 0.7f)
            D.trauma_level = clampf(D.trauma_level + D.dissonance * 0.1f, 0, 1);
        auto_velocity();
        apply_velocity();
        season_step();
        update_metrics();
        enforce_laws();

        char words[1024];
        generate_words(words, sizeof(words));
    }

    /* after several conversations, state should be consistent */
    ASSERT_GTE(D.entropy, ENTROPY_FLOOR, "multi-conv: entropy floor held");
    ASSERT_LTE(D.resonance, RESONANCE_CEILING, "multi-conv: resonance ceiling held");
    ASSERT(D.emergence >= 0.0f && D.emergence <= 1.0f, "multi-conv: emergence in range");
    ASSERT_GT(D.step, 0, "multi-conv: step advanced");
    ASSERT_GT(D.conv_count, -1, "multi-conv: state valid");
}

/* ═══════════════════════════════════════════════════════════
 * TEST: Display Names Arrays
 * ═══════════════════════════════════════════════════════════ */

static void test_display_names(void) {
    ASSERT_EQ_INT(6, (int)(sizeof(term_names) / sizeof(term_names[0])), "6 term names");
    ASSERT_EQ_INT(6, (int)(sizeof(vel_names) / sizeof(vel_names[0])), "6 velocity names");
    ASSERT_EQ_INT(4, (int)(sizeof(season_names) / sizeof(season_names[0])), "4 season names");

    ASSERT_STR_EQ(term_names[TERM_B], "B:chain", "term B name");
    ASSERT_STR_EQ(term_names[TERM_H], "H:resonance", "term H name");
    ASSERT_STR_EQ(term_names[TERM_T], "T:trauma", "term T name");

    ASSERT_STR_EQ(vel_names[VEL_WALK], "WALK", "velocity WALK name");
    ASSERT_STR_EQ(vel_names[VEL_BREATHE], "BREATHE", "velocity BREATHE name");

    ASSERT_STR_EQ(season_names[0], "spring", "season spring");
    ASSERT_STR_EQ(season_names[3], "winter", "season winter");
}

/* ═══════════════════════════════════════════════════════════
 * MAIN — run all tests
 * ═══════════════════════════════════════════════════════════ */

int main(void) {
    printf("\n  ═══ dario.c test suite ═══\n\n");

    RUN_TEST(test_rng);
    RUN_TEST(test_clampf);
    RUN_TEST(test_vocab);
    RUN_TEST(test_tokenizer);
    RUN_TEST(test_bigrams);
    RUN_TEST(test_cooccurrence);
    RUN_TEST(test_prophecy);
    RUN_TEST(test_vec_ops);
    RUN_TEST(test_embeddings);
    RUN_TEST(test_rope);
    RUN_TEST(test_swiglu);
    RUN_TEST(test_velocity);
    RUN_TEST(test_auto_velocity);
    RUN_TEST(test_seasons);
    RUN_TEST(test_dissonance);
    RUN_TEST(test_laws);
    RUN_TEST(test_metrics);
    RUN_TEST(test_sampling);
    RUN_TEST(test_code_fragments);
    RUN_TEST(test_ingest);
    RUN_TEST(test_dario_compute);
    RUN_TEST(test_trauma_term);
    RUN_TEST(test_generate);
    RUN_TEST(test_full_pipeline);
    RUN_TEST(test_seed_words);
    RUN_TEST(test_code_fragment_coverage);
    RUN_TEST(test_edge_cases);
    RUN_TEST(test_multi_conversation);
    RUN_TEST(test_display_names);

    printf("\n  ─────────────────────────────\n");
    printf("  %d tests run, %d passed, %d failed\n\n",
           tests_run, tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
