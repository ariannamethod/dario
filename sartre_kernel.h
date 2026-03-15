/*
 * sartre_kernel.h — SARTRE: Meta-Linux Kernel for the Dario Equation
 *
 * "L'existence précède l'essence."
 * — Jean-Paul Sartre
 *
 * A minimal operating system nucleus. No weights. No inference.
 * Pure state aggregation + hardware detection + module lifecycle.
 *
 * Self-contained. Zero dependencies beyond libc.
 * Compiles alone: cc sartre_kernel.c -O2 -lm -o sartre_kernel
 * Compiles with dario: cc dario.c sartre_kernel.c -DHAS_SARTRE -DHAS_DARIO -O2 -lm -o dario
 *
 * by Arianna Method
 */

#ifndef SARTRE_KERNEL_H
#define SARTRE_KERNEL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════
 * MODULE STATUS — existence states
 * ═══════════════════════════════════════════════════════════════════ */

typedef enum {
    SARTRE_MODULE_UNKNOWN   = 0,
    SARTRE_MODULE_IDLE      = 1,
    SARTRE_MODULE_ACTIVE    = 2,
    SARTRE_MODULE_ERROR     = 3,
    SARTRE_MODULE_LOADING   = 4,
    SARTRE_MODULE_UNLOADING = 5
} SartreModuleStatus;

/* ═══════════════════════════════════════════════════════════════════
 * TONGUE TIER — hardware-aware model routing
 * ═══════════════════════════════════════════════════════════════════ */

typedef enum {
    SARTRE_TONGUE_05B = 0,   /* 0.5B (~537MB runtime) */
    SARTRE_TONGUE_15B = 1,   /* 1.5B (~1.4GB runtime) */
    SARTRE_TONGUE_3B  = 2    /* 3B (~2.8GB runtime)   */
} SartreTongueTier;

/* ═══════════════════════════════════════════════════════════════════
 * MODULE INFO — each module is an existential fact
 * ═══════════════════════════════════════════════════════════════════ */

#define SARTRE_MAX_MODULES  16
#define SARTRE_MAX_EVENTS    8
#define SARTRE_MAX_PACKAGES 32

typedef struct {
    char               name[64];
    SartreModuleStatus status;
    float              load;            /* 0-1: resource usage */
    int64_t            last_active_ms;  /* timestamp */
    char               last_event[128];
} SartreModuleInfo;

/* ═══════════════════════════════════════════════════════════════════
 * PACKAGE — minimal apk-inspired package tracking
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    char name[64];
    char version[32];
    int  installed;    /* 0=available, 1=installed */
    int  size_kb;      /* approximate size */
} SartrePackage;

/* ═══════════════════════════════════════════════════════════════════
 * OVERLAY — R∪W filesystem concept
 *
 * base  = immutable (the formula, the seed words, the laws)
 * delta = writable  (conversation, learned co-occurrences, prophecies)
 *
 * overlay_writes tracks how much the writable layer has grown.
 * overlay_ratio = delta / (base + delta) — how far from origin.
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    int64_t base_size;       /* immutable layer (bytes) */
    int64_t delta_size;      /* writable layer (bytes)  */
    int     overlay_writes;  /* total write operations   */
    float   overlay_ratio;   /* delta / (base + delta)   */
} SartreOverlay;

/* ═══════════════════════════════════════════════════════════════════
 * NAMESPACE — process isolation (conceptual)
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    int   pid;
    char  name[64];
    float cpu_share;    /* 0-1: allocated CPU fraction */
    float mem_limit_mb; /* memory limit */
    int   active;
} SartreNamespace;

#define SARTRE_MAX_NS 8

/* ═══════════════════════════════════════════════════════════════════
 * SYSTEM STATE — the central nervous system
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    /* modules */
    SartreModuleInfo modules[SARTRE_MAX_MODULES];
    int              module_count;

    /* resources */
    float memory_pressure;   /* 0-1 */
    float cpu_load;          /* 0-1 */

    /* hardware detection */
    int64_t        total_ram_mb;
    SartreTongueTier tongue_tier;
    int            tongue_override; /* -1 = auto */

    /* inner world (mirrors Dario chambers when linked) */
    float trauma_level;
    float arousal;
    float valence;
    float coherence;
    float prophecy_debt;
    float entropy;

    /* schumann */
    float schumann_coherence;
    float schumann_phase;

    /* calendar */
    float calendar_tension;
    int   is_shabbat;

    /* flags */
    int spiral_detected;
    int wormhole_active;
    int strange_loop;

    /* event ringbuffer */
    char last_events[SARTRE_MAX_EVENTS][256];
    int  event_count;

    /* overlay filesystem */
    SartreOverlay overlay;

    /* namespaces */
    SartreNamespace namespaces[SARTRE_MAX_NS];
    int             ns_count;

    /* packages */
    SartrePackage packages[SARTRE_MAX_PACKAGES];
    int           pkg_count;

    /* uptime */
    int64_t boot_time_ms;
    int64_t step_count;
} SartreSystemState;

/* ═══════════════════════════════════════════════════════════════════
 * LIFECYCLE
 * ═══════════════════════════════════════════════════════════════════ */

int  sartre_init(const char *config_path);
void sartre_shutdown(void);
int  sartre_is_ready(void);

/* ═══════════════════════════════════════════════════════════════════
 * METRIC UPDATES
 * ═══════════════════════════════════════════════════════════════════ */

void sartre_notify_event(const char *event);
void sartre_update_inner_state(float trauma, float arousal, float valence,
                               float coherence, float prophecy_debt);
void sartre_update_schumann(float coherence, float phase);
void sartre_update_calendar(float tension, int is_shabbat);
void sartre_update_module(const char *name, SartreModuleStatus status, float load);
SartreSystemState *sartre_get_state(void);

/* ═══════════════════════════════════════════════════════════════════
 * OVERLAY
 * ═══════════════════════════════════════════════════════════════════ */

void  sartre_overlay_init(int64_t base_size);
void  sartre_overlay_write(int64_t bytes);
float sartre_overlay_ratio(void);

/* ═══════════════════════════════════════════════════════════════════
 * NAMESPACES
 * ═══════════════════════════════════════════════════════════════════ */

int  sartre_ns_create(const char *name, float cpu_share, float mem_limit_mb);
void sartre_ns_destroy(int ns_id);
SartreNamespace *sartre_ns_get(int ns_id);

/* ═══════════════════════════════════════════════════════════════════
 * PACKAGES (minimal apk-style)
 * ═══════════════════════════════════════════════════════════════════ */

int  sartre_pkg_register(const char *name, const char *version, int size_kb);
int  sartre_pkg_install(const char *name);
int  sartre_pkg_remove(const char *name);
int  sartre_pkg_find(const char *name);
void sartre_pkg_list(void);

/* ═══════════════════════════════════════════════════════════════════
 * TONGUE ROUTING
 * ═══════════════════════════════════════════════════════════════════ */

SartreTongueTier sartre_detect_tongue_tier(void);
void             sartre_set_tongue_override(SartreTongueTier tier);
void             sartre_clear_tongue_override(void);
SartreTongueTier sartre_get_tongue_tier(void);
const char      *sartre_tongue_tier_name(SartreTongueTier tier);
int64_t          sartre_get_total_ram_mb(void);

/* ═══════════════════════════════════════════════════════════════════
 * DEBUG / MONITORING
 * ═══════════════════════════════════════════════════════════════════ */

void sartre_print_state(void);
int  sartre_state_to_json(char *buf, int max);

#ifdef __cplusplus
}
#endif

#endif /* SARTRE_KERNEL_H */
