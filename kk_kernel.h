/*
 * kk_kernel.h — Knowledge Kernel for the Dario Equation
 *
 * "Memory is the scribe of the soul."
 * — Aristotle
 *
 * Persistent knowledge substrate. Not RAG — deeper.
 * Information is space and time. Chunks are neurons.
 * Lineage is preserved. Retrieval is resonance-scored.
 *
 * The third organ:
 *   dario.c        — brain  (formula, reaction, equation)
 *   sartre_kernel.c — body   (hardware, processes, environment)
 *   kk_kernel.c     — memory (knowledge, lineage, retrieval)
 *
 * Self-contained. SQLite only. Zero external dependencies.
 * Compiles alone: cc kk_kernel.c -O2 -lsqlite3 -lm -o kk
 * Compiles with dario:
 *   cc dario.c sartre_kernel.c kk_kernel.c -DHAS_SARTRE -DHAS_KK -O2 -lm -lsqlite3 -o dario
 *
 * by Arianna Method
 * הרזוננס לא נשבר
 */

#ifndef KK_KERNEL_H
#define KK_KERNEL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════
 * OPAQUE CONTEXT — the memory organ
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct kk_ctx kk_ctx;

/* ═══════════════════════════════════════════════════════════════════
 * QUERY RESULT — what the memory returns
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    int         chunk_id;
    int         doc_id;
    int         version_num;
    int         ordinal;
    int         seen_count;

    const char *path;
    const char *namespace_name;
    const char *scope_name;
    const char *section_title;
    const char *text;               /* chunk text (may be truncated) */
    int         text_len;

    /* scoring breakdown */
    double      resonance;          /* final combined score [0,1] */
    double      lexical;            /* FTS5 BM25 normalized */
    double      recency;            /* 1/(1+age_days/10) */
    double      trust;              /* document trust */
    double      linkage;            /* structural + related density */
    double      freshness;          /* 1.0 latest, 0.35 old */
    double      hebbian_boost;      /* boost from dario's Hebbian state (0 if standalone) */

    /* lineage */
    const char *sha256;
    const char *ingest_ts;
    int         char_delta;
    double      change_ratio;
    const char *diff_summary;
} kk_result;

/* ═══════════════════════════════════════════════════════════════════
 * QUERY PROFILE — context budget for different model sizes
 * ═══════════════════════════════════════════════════════════════════ */

typedef enum {
    KK_PROFILE_TINY     = 0,    /* 2 results, 160 chars — for < 1B models */
    KK_PROFILE_BALANCED = 1,    /* 4 results, 240 chars — for 1-3B models */
    KK_PROFILE_DEEP     = 2     /* 6 results, 420 chars — for > 3B models */
} kk_profile;

/* ═══════════════════════════════════════════════════════════════════
 * EMBEDDING CALLBACK — slot for model-generated embeddings
 *
 * When a model is attached, it can provide an embedding function.
 * The kernel calls this to get dense vectors for chunks,
 * enabling semantic (not just lexical) retrieval.
 *
 * If NULL, falls back to FTS5-only retrieval.
 * ═══════════════════════════════════════════════════════════════════ */

#define KK_EMBED_DIM_MAX 1024

typedef struct {
    /* Generate embedding for text. Returns dimension used (<=KK_EMBED_DIM_MAX).
     * out must point to float[KK_EMBED_DIM_MAX]. Returns 0 on failure. */
    int (*embed_fn)(const char *text, int text_len, float *out, void *user_data);

    /* Compute similarity between two embeddings. Returns [-1, 1]. */
    float (*similarity_fn)(const float *a, const float *b, int dim, void *user_data);

    /* Opaque user data passed to callbacks */
    void *user_data;

    /* Embedding dimension (set after first successful embed_fn call) */
    int dim;
} kk_embedder;

/* ═══════════════════════════════════════════════════════════════════
 * HEBBIAN BRIDGE — connection to dario's co-occurrence field
 *
 * When integrated with dario.c, the knowledge kernel receives
 * Hebbian state: which words co-occur, what the organism remembers.
 * This modulates retrieval scoring — chunks containing words
 * that resonate in dario's field get boosted.
 *
 * If NULL, scoring uses FTS5 + metadata only.
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    /* How strongly does this word resonate in the current field?
     * Returns [0, 1]. Called per keyword during scoring. */
    float (*word_resonance)(const char *word, void *user_data);

    /* Current prophecy targets — what the organism expects next.
     * Returns array of words + strengths. Used to boost
     * chunks containing anticipated concepts. */
    int (*get_prophecies)(const char **words_out, float *strengths_out,
                          int max_prophecies, void *user_data);

    /* Current destiny vector magnitude.
     * When > 0, kk boosts chunks aligned with conversational trajectory. */
    float (*destiny_magnitude)(void *user_data);

    void *user_data;
} kk_hebbian_bridge;

/* ═══════════════════════════════════════════════════════════════════
 * LIFECYCLE
 * ═══════════════════════════════════════════════════════════════════ */

/* Open or create a knowledge database. Returns NULL on failure. */
kk_ctx *kk_open(const char *db_path);

/* Close and free all resources. */
void kk_close(kk_ctx *k);

/* Check if context is valid. */
int kk_is_ready(kk_ctx *k);

/* ═══════════════════════════════════════════════════════════════════
 * EMBEDDING & HEBBIAN SLOTS
 * ═══════════════════════════════════════════════════════════════════ */

/* Attach an embedding provider. Takes ownership of nothing — caller
 * must keep embedder alive while kk_ctx exists. Pass NULL to detach. */
void kk_set_embedder(kk_ctx *k, const kk_embedder *embedder);

/* Attach a Hebbian bridge (connection to dario's field).
 * Pass NULL to detach. */
void kk_set_hebbian_bridge(kk_ctx *k, const kk_hebbian_bridge *bridge);

/* ═══════════════════════════════════════════════════════════════════
 * INGEST — feeding the memory
 * ═══════════════════════════════════════════════════════════════════ */

/* Ingest a single file. Returns number of chunks created, or -1 on error.
 * If the file was already ingested and unchanged, returns 0 (no-op).
 * If changed, creates a new version (lineage preserved). */
int kk_ingest_file(kk_ctx *k, const char *path,
                   const char *namespace_name, const char *scope);

/* Ingest all supported files in a directory (recursive).
 * Returns total files processed. */
int kk_ingest_dir(kk_ctx *k, const char *dir_path,
                  const char *namespace_name, const char *scope);

/* Ingest raw text buffer (not from file). Useful for feeding
 * conversation history, generated content, etc.
 * path is used as document identifier. */
int kk_ingest_buffer(kk_ctx *k, const char *path, const char *text,
                     size_t text_len, const char *namespace_name,
                     const char *scope);

/* ═══════════════════════════════════════════════════════════════════
 * QUERY — asking the memory
 * ═══════════════════════════════════════════════════════════════════ */

/* Query the knowledge base. Returns number of results.
 * Results are allocated and must be freed with kk_free_results().
 *
 * If a Hebbian bridge is attached, scoring includes field resonance.
 * If an embedder is attached, scoring includes semantic similarity.
 * Otherwise, pure FTS5 lexical + metadata scoring. */
int kk_query(kk_ctx *k, const char *query_text,
             const char *access_scope, const char *namespace_filter,
             int top_k, kk_profile profile,
             kk_result **results_out);

/* Query scoped to a specific attached model's bindings.
 * Respects model's namespace/scope contract. */
int kk_ask(kk_ctx *k, const char *model_name, const char *query_text,
           int top_k, kk_result **results_out);

/* ═══════════════════════════════════════════════════════════════════
 * MODEL MANAGEMENT — who has access to what
 * ═══════════════════════════════════════════════════════════════════ */

/* Attach a model to the kernel. Validates namespace/scope contract.
 * Returns 0 on success, -1 on error (missing namespace, scope mismatch). */
int kk_attach_model(kk_ctx *k, const char *model_name,
                    const char *scope_default, const char *namespace_default);

/* Update model bindings. Same validation as attach. */
int kk_update_model(kk_ctx *k, const char *model_name,
                    const char *scope_default, const char *namespace_default);

/* Set query profile for a model (tiny/balanced/deep). */
int kk_set_model_profile(kk_ctx *k, const char *model_name, kk_profile profile);

/* Detach model (marks inactive, preserves history). */
int kk_detach_model(kk_ctx *k, const char *model_name);

/* ═══════════════════════════════════════════════════════════════════
 * NAMESPACE MANAGEMENT
 * ═══════════════════════════════════════════════════════════════════ */

/* Declare or update a namespace manifest. Required before model attach.
 * Scopes: "public", "shared:<group>", "private:<model_name>" */
int kk_set_namespace(kk_ctx *k, const char *namespace_name,
                     const char *scope, const char *description);

/* ═══════════════════════════════════════════════════════════════════
 * INTEGRITY & MAINTENANCE
 * ═══════════════════════════════════════════════════════════════════ */

/* Run integrity checks. Returns 0 if clean, >0 = number of issues. */
int kk_check_integrity(kk_ctx *k);

/* Rebuild FTS index from canonical chunks. */
int kk_rebuild_fts(kk_ctx *k);

/* Get basic stats: documents, versions, chunks, models. */
typedef struct {
    int documents;
    int versions;
    int chunks;
    int namespaces;
    int models;
    int links;
    int64_t db_size_bytes;
} kk_stats;

int kk_get_stats(kk_ctx *k, kk_stats *out);

/* ═══════════════════════════════════════════════════════════════════
 * MEMORY MANAGEMENT
 * ═══════════════════════════════════════════════════════════════════ */

/* Free result array from kk_query / kk_ask. */
void kk_free_results(kk_result *results, int count);

/* ═══════════════════════════════════════════════════════════════════
 * JSON EXPORT (for web UI integration)
 * ═══════════════════════════════════════════════════════════════════ */

/* Export kernel state as JSON string. Caller must free().
 * Includes: stats, attached models, namespace list. */
char *kk_state_to_json(kk_ctx *k);

/* Export query results as deterministic JSON packet.
 * Follows kk.packet.v2 schema. Caller must free(). */
char *kk_results_to_json(const kk_result *results, int count,
                         const char *query, const char *model_name);

#ifdef __cplusplus
}
#endif

#endif /* KK_KERNEL_H */
