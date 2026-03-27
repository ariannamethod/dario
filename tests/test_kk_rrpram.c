/*
 * test_kk_rrpram.c — End-to-end test for KK RRPRAM resonance
 *
 * Ingests a text file, builds metaweights, queries with synthetic embedding,
 * traces the full pipeline: ingest → metaweights → resonance → retrieval.
 *
 * cc test_kk_rrpram.c kk_kernel.c -DKK_STANDALONE -O2 -lsqlite3 -lm -o test_rrpram
 * ./test_rrpram /path/to/document.txt
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "kk_kernel.h"

static void print_meta(const kk_chunk_meta *m) {
    printf("  tokens: %d\n", m->n_tokens);
    printf("  bigrams: %d, hebbian pairs: %d\n", m->bigram_n, m->hebb_n);
    printf("  affinity[0..7]: ");
    for (int i = 0; i < 8 && i < KK_META_AFFINITY_DIM; i++)
        printf("%.3f ", m->affinity[i]);
    printf("\n");
    if (m->bigram_n > 0) {
        printf("  top bigram: %d→%d (p=%.3f)\n",
               m->bigram_src[0], m->bigram_dst[0], m->bigram_prob[0]);
    }
    if (m->hebb_n > 0) {
        printf("  top hebbian: (%d,%d) str=%.3f\n",
               m->hebb_a[0], m->hebb_b[0], m->hebb_strength[0]);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <document.txt> [query]\n", argv[0]);
        return 1;
    }
    const char *doc_path = argv[1];
    const char *query = argc > 2 ? argv[2] : "resonance and meaning";

    /* 1. Open KK */
    const char *db_path = "/tmp/kk_rrpram_test.db";
    remove(db_path);
    kk_ctx *k = kk_open(db_path);
    if (!k) { fprintf(stderr, "ERROR: cannot open KK\n"); return 1; }

    /* 2. Create namespace */
    kk_set_namespace(k, "test", "public", "test namespace");

    /* 3. Ingest document */
    printf("=== INGEST ===\n");
    printf("document: %s\n", doc_path);
    int chunks = kk_ingest_file(k, doc_path, "test", "public");
    printf("chunks created: %d\n", chunks);

    if (chunks <= 0) {
        fprintf(stderr, "ERROR: no chunks created\n");
        kk_close(k);
        return 1;
    }

    /* 4. Verify metaweights were built */
    printf("\n=== METAWEIGHTS (first 3 chunks) ===\n");
    kk_stats stats;
    kk_get_stats(k, &stats);
    printf("total chunks in DB: %d\n\n", stats.chunks);

    for (int cid = 1; cid <= 3 && cid <= stats.chunks; cid++) {
        kk_chunk_meta meta;
        if (kk_build_chunk_meta(k, cid) == 0) {
            printf("chunk %d: metaweights OK\n", cid);
            /* reload to verify storage roundtrip */
            memset(&meta, 0, sizeof(meta));
            /* we'll use kk_chunk_resonance directly */
        }
    }

    /* 5. Standard query (no RRPRAM) */
    printf("\n=== STANDARD QUERY: \"%s\" ===\n", query);
    kk_result *results = NULL;
    int count = kk_query(k, query, "public", "test", 5, KK_PROFILE_BALANCED, &results);
    printf("results: %d\n", count);
    for (int i = 0; i < count; i++) {
        printf("  [%d] resonance=%.4f lexical=%.4f rrpram=%.4f\n",
               i, results[i].resonance, results[i].lexical, results[i].rrpram_resonance);
        printf("      text: %.80s...\n", results[i].text ? results[i].text : "(null)");
    }
    if (results) kk_free_results(results, count);

    /* 6. Build synthetic embedding from query text
     * (In production, this comes from model's hidden state) */
    printf("\n=== RESONANT QUERY (with RRPRAM) ===\n");
    float embedding[KK_META_AFFINITY_DIM];
    memset(embedding, 0, sizeof(embedding));
    for (int i = 0; query[i]; i++) {
        int pos = i % KK_META_AFFINITY_DIM;
        embedding[pos] += 1.0f + 0.5f * (float)((unsigned char)query[i] % 64) / 64.0f;
    }
    /* normalize */
    float emax = 0;
    for (int i = 0; i < KK_META_AFFINITY_DIM; i++)
        if (embedding[i] > emax) emax = embedding[i];
    if (emax > 0) for (int i = 0; i < KK_META_AFFINITY_DIM; i++) embedding[i] /= emax;

    printf("embedding[0..7]: ");
    for (int i = 0; i < 8; i++) printf("%.3f ", embedding[i]);
    printf("\n\n");

    results = NULL;
    count = kk_query_resonant(k, query, embedding, KK_META_AFFINITY_DIM,
                              "public", "test", 5, KK_PROFILE_BALANCED, &results);
    printf("results: %d\n", count);
    for (int i = 0; i < count; i++) {
        printf("  [%d] resonance=%.4f lexical=%.4f rrpram=%.4f\n",
               i, results[i].resonance, results[i].lexical, results[i].rrpram_resonance);
        printf("      text: %.80s...\n", results[i].text ? results[i].text : "(null)");
    }
    if (results) kk_free_results(results, count);

    printf("\n=== PIPELINE TRACE COMPLETE ===\n");
    printf("ingest → metaweights → query → RRPRAM resonance → re-ranked results\n");

    kk_close(k);
    return 0;
}
