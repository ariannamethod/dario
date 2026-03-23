/*
 * sartre_kernel.c — SARTRE: Meta-Linux Kernel for the Dario Equation
 *
 * "L'enfer, c'est les autres processus."
 *
 * Self-contained meta-Linux kernel. Compiles alone, compiles with dario.c,
 * compiles with or without the sartre/ extensions directory.
 *
 * Alone:      cc sartre_kernel.c -O2 -lm -o sartre_kernel
 * With dario: cc dario.c sartre_kernel.c -DHAS_SARTRE -DHAS_DARIO -O2 -lm -o dario
 *
 * Features:
 *   - SystemState aggregation (16 modules, inner world metrics)
 *   - Hardware detection + tongue tier routing (0.5B/1.5B/3B)
 *   - OverlayFS concept (immutable base + writable delta)
 *   - Namespace isolation (8 slots)
 *   - Package management (apk-inspired, 32 slots)
 *   - Event ringbuffer (8 events)
 *   - JSON state export (for web UI)
 *
 * Zero dependencies. Zero weights. Pure existence.
 *
 * by Arianna Method
 * הרזוננס לא נשבר
 */

#include "sartre_kernel.h"
#include <time.h>

#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

/* ═══════════════════════════════════════════════════════════════════
 * GLOBAL STATE — l'être-pour-soi
 * ═══════════════════════════════════════════════════════════════════ */

static SartreSystemState sys = {0};
static int sartre_ready = 0;

/* ═══════════════════════════════════════════════════════════════════
 * LIFECYCLE — birth, existence, nothingness
 * ═══════════════════════════════════════════════════════════════════ */

int sartre_init(const char *config_path) {
    (void)config_path;

    memset(&sys, 0, sizeof(SartreSystemState));
    sys.tongue_override = -1;
    sys.boot_time_ms = (int64_t)time(NULL) * 1000;

    sartre_detect_tongue_tier();
    sartre_ready = 1;

    /* register self as first module */
    sartre_update_module("sartre_kernel", SARTRE_MODULE_ACTIVE, 0.01f);

    fprintf(stderr, "[sartre] kernel initialized. RAM: %lld MB, tongue: %s\n",
            (long long)sys.total_ram_mb, sartre_tongue_tier_name(sys.tongue_tier));
    return 0;
}

void sartre_shutdown(void) {
    if (!sartre_ready) return;
    sartre_notify_event("kernel_shutdown");
    sartre_ready = 0;
    fprintf(stderr, "[sartre] kernel shutdown\n");
}

int sartre_is_ready(void) {
    return sartre_ready;
}

/* ═══════════════════════════════════════════════════════════════════
 * EVENT RINGBUFFER — what happened
 * ═══════════════════════════════════════════════════════════════════ */

void sartre_notify_event(const char *event) {
    if (!sartre_ready || !event) return;

    if (sys.event_count < SARTRE_MAX_EVENTS) {
        strncpy(sys.last_events[sys.event_count], event, 255);
        sys.last_events[sys.event_count][255] = '\0';
        sys.event_count++;
    } else {
        for (int i = 0; i < SARTRE_MAX_EVENTS - 1; i++) {
            strncpy(sys.last_events[i], sys.last_events[i + 1], 255);
            sys.last_events[i][255] = '\0';
        }
        strncpy(sys.last_events[SARTRE_MAX_EVENTS - 1], event, 255);
        sys.last_events[SARTRE_MAX_EVENTS - 1][255] = '\0';
    }

    sys.step_count++;
}

/* ═══════════════════════════════════════════════════════════════════
 * METRIC UPDATES — the inner world
 * ═══════════════════════════════════════════════════════════════════ */

void sartre_update_inner_state(float trauma, float arousal, float valence,
                               float coherence, float prophecy_debt) {
    if (!sartre_ready) return;
    sys.trauma_level  = trauma;
    sys.arousal       = arousal;
    sys.valence       = valence;
    sys.coherence     = coherence;
    sys.prophecy_debt = prophecy_debt;
}

void sartre_update_schumann(float coherence, float phase) {
    if (!sartre_ready) return;
    sys.schumann_coherence = coherence;
    sys.schumann_phase     = phase;
}

void sartre_update_calendar(float tension, int is_shabbat) {
    if (!sartre_ready) return;
    sys.calendar_tension = tension;
    sys.is_shabbat       = is_shabbat;
}

void sartre_update_module(const char *name, SartreModuleStatus status, float load) {
    if (!sartre_ready || !name) return;

    int idx = -1;
    for (int i = 0; i < sys.module_count; i++) {
        if (strncmp(sys.modules[i].name, name, 63) == 0) {
            idx = i;
            break;
        }
    }

    if (idx == -1 && sys.module_count < SARTRE_MAX_MODULES) {
        idx = sys.module_count++;
        strncpy(sys.modules[idx].name, name, 63);
        sys.modules[idx].name[63] = '\0';
    }

    if (idx >= 0) {
        sys.modules[idx].status         = status;
        sys.modules[idx].load           = load;
        sys.modules[idx].last_active_ms = (int64_t)time(NULL) * 1000;
    }
}

SartreSystemState *sartre_get_state(void) {
    return &sys;
}

/* ═══════════════════════════════════════════════════════════════════
 * OVERLAY — R∪W filesystem
 *
 * base  = immutable: the formula, seed vocab, laws of nature
 * delta = writable:  learned co-occurrences, prophecies, bigrams
 *
 * overlay_ratio measures how far the organism has drifted from its
 * original state. 0.0 = pure origin, approaching 1.0 = mostly learned.
 * ═══════════════════════════════════════════════════════════════════ */

void sartre_overlay_init(int64_t base_size) {
    sys.overlay.base_size      = base_size;
    sys.overlay.delta_size     = 0;
    sys.overlay.overlay_writes = 0;
    sys.overlay.overlay_ratio  = 0.0f;
}

void sartre_overlay_write(int64_t bytes) {
    sys.overlay.delta_size += bytes;
    sys.overlay.overlay_writes++;

    int64_t total = sys.overlay.base_size + sys.overlay.delta_size;
    if (total > 0) {
        sys.overlay.overlay_ratio = (float)sys.overlay.delta_size / (float)total;
    }
}

float sartre_overlay_ratio(void) {
    return sys.overlay.overlay_ratio;
}

/* ═══════════════════════════════════════════════════════════════════
 * NAMESPACES — Leibniz monads, process isolation
 * ═══════════════════════════════════════════════════════════════════ */

int sartre_ns_create(const char *name, float cpu_share, float mem_limit_mb) {
    if (sys.ns_count >= SARTRE_MAX_NS) return -1;

    int id = sys.ns_count++;
    strncpy(sys.namespaces[id].name, name, 63);
    sys.namespaces[id].name[63] = '\0';
    sys.namespaces[id].pid          = id + 1;
    sys.namespaces[id].cpu_share    = cpu_share;
    sys.namespaces[id].mem_limit_mb = mem_limit_mb;
    sys.namespaces[id].active       = 1;

    char ev[128];
    snprintf(ev, sizeof(ev), "ns_create:%s", name);
    sartre_notify_event(ev);

    return id;
}

void sartre_ns_destroy(int ns_id) {
    if (ns_id < 0 || ns_id >= sys.ns_count) return;
    sys.namespaces[ns_id].active = 0;

    char ev[128];
    snprintf(ev, sizeof(ev), "ns_destroy:%s", sys.namespaces[ns_id].name);
    sartre_notify_event(ev);
}

SartreNamespace *sartre_ns_get(int ns_id) {
    if (ns_id < 0 || ns_id >= sys.ns_count) return NULL;
    return &sys.namespaces[ns_id];
}

/* ═══════════════════════════════════════════════════════════════════
 * PACKAGES — apk-inspired package management
 *
 * Not a full package manager. A registry of what's available and
 * what's installed. The kernel knows its own composition.
 * ═══════════════════════════════════════════════════════════════════ */

int sartre_pkg_register(const char *name, const char *version, int size_kb) {
    if (sys.pkg_count >= SARTRE_MAX_PACKAGES) return -1;

    int id = sys.pkg_count++;
    strncpy(sys.packages[id].name, name, 63);
    sys.packages[id].name[63] = '\0';
    strncpy(sys.packages[id].version, version, 31);
    sys.packages[id].version[31] = '\0';
    sys.packages[id].installed = 0;
    sys.packages[id].size_kb   = size_kb;
    return id;
}

int sartre_pkg_find(const char *name) {
    for (int i = 0; i < sys.pkg_count; i++) {
        if (strncmp(sys.packages[i].name, name, 63) == 0) return i;
    }
    return -1;
}

int sartre_pkg_install(const char *name) {
    int id = sartre_pkg_find(name);
    if (id < 0) return -1;
    if (sys.packages[id].installed) return 0; /* already installed */

    sys.packages[id].installed = 1;

    char ev[128];
    snprintf(ev, sizeof(ev), "pkg_install:%s@%s", name, sys.packages[id].version);
    sartre_notify_event(ev);

    /* track in overlay */
    sartre_overlay_write((int64_t)sys.packages[id].size_kb * 1024);

    return 0;
}

int sartre_pkg_remove(const char *name) {
    int id = sartre_pkg_find(name);
    if (id < 0) return -1;
    if (!sys.packages[id].installed) return 0;

    sys.packages[id].installed = 0;

    char ev[128];
    snprintf(ev, sizeof(ev), "pkg_remove:%s", name);
    sartre_notify_event(ev);

    return 0;
}

void sartre_pkg_list(void) {
    printf("\n=== SARTRE PACKAGES ===\n");
    for (int i = 0; i < sys.pkg_count; i++) {
        printf("  %s %s [%s] %dKB\n",
               sys.packages[i].installed ? "*" : " ",
               sys.packages[i].name,
               sys.packages[i].version,
               sys.packages[i].size_kb);
    }
    int installed_count = 0;
    for (int i = 0; i < sys.pkg_count; i++)
        if (sys.packages[i].installed) installed_count++;
    printf("  %d packages, %d installed\n\n", sys.pkg_count, installed_count);
}

/* ═══════════════════════════════════════════════════════════════════
 * TONGUE ROUTING — hardware-based model selection
 * ═══════════════════════════════════════════════════════════════════ */

static int64_t detect_total_ram_mb(void) {
#ifdef __APPLE__
    int64_t memsize;
    size_t len = sizeof(memsize);
    if (sysctlbyname("hw.memsize", &memsize, &len, NULL, 0) == 0)
        return memsize / (1024 * 1024);
    return 4096;
#else
    FILE *f = fopen("/proc/meminfo", "r");
    if (f) {
        int64_t total_kb = 0;
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "MemTotal:", 9) == 0) {
                sscanf(line + 9, " %lld", &total_kb);
                break;
            }
        }
        fclose(f);
        if (total_kb > 0) return total_kb / 1024;
    }
    return 4096;
#endif
}

SartreTongueTier sartre_detect_tongue_tier(void) {
    int64_t ram_mb = detect_total_ram_mb();
    sys.total_ram_mb = ram_mb;

    if (sys.tongue_override >= 0) {
        sys.tongue_tier = (SartreTongueTier)sys.tongue_override;
        return sys.tongue_tier;
    }

    if (ram_mb >= 8000)      sys.tongue_tier = SARTRE_TONGUE_3B;
    else if (ram_mb >= 4000) sys.tongue_tier = SARTRE_TONGUE_15B;
    else                     sys.tongue_tier = SARTRE_TONGUE_05B;

    return sys.tongue_tier;
}

void sartre_set_tongue_override(SartreTongueTier tier) {
    sys.tongue_override = (int)tier;
    sys.tongue_tier     = tier;
}

void sartre_clear_tongue_override(void) {
    sys.tongue_override = -1;
    sartre_detect_tongue_tier();
}

SartreTongueTier sartre_get_tongue_tier(void) {
    return sys.tongue_tier;
}

const char *sartre_tongue_tier_name(SartreTongueTier tier) {
    switch (tier) {
        case SARTRE_TONGUE_05B: return "0.5B";
        case SARTRE_TONGUE_15B: return "1.5B";
        case SARTRE_TONGUE_3B:  return "3B";
        default: return "unknown";
    }
}

int64_t sartre_get_total_ram_mb(void) {
    return sys.total_ram_mb;
}

/* ═══════════════════════════════════════════════════════════════════
 * MODEL ROUTING — agnostic, DoE-style auto-detection
 *
 * Any model file. Any size. Any architecture.
 * Sartre profiles it from the file itself:
 *   .bin  → param_count = file_size / 4 (float32)
 *   .gguf → read GGUF metadata header for actual param count
 *   .pt   → param_count = file_size / 4 (approximation)
 *
 * Runtime memory estimated. Fit-in-RAM checked against 80% threshold.
 * Best model = largest that fits.
 * ═══════════════════════════════════════════════════════════════════ */

static int64_t file_size_bytes(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    int64_t size = ftell(f);
    fclose(f);
    return size;
}

static int detect_gguf_params(const char *path, SartreModelProfile *out) {
    /* GGUF magic: 0x46554747 ("GGUF") at offset 0 */
    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    uint32_t magic = 0;
    if (fread(&magic, 4, 1, f) != 1) { fclose(f); return 0; }

    if (magic != 0x46554747) { fclose(f); return 0; } /* not GGUF */

    /* GGUF v3: version(4) + tensor_count(8) + metadata_kv_count(8) */
    uint32_t version = 0;
    fread(&version, 4, 1, f);

    uint64_t tensor_count = 0, kv_count = 0;
    if (version >= 3) {
        fread(&tensor_count, 8, 1, f);
        fread(&kv_count, 8, 1, f);
    } else {
        uint32_t tc32 = 0, kv32 = 0;
        fread(&tc32, 4, 1, f);
        fread(&kv32, 4, 1, f);
        tensor_count = tc32;
        kv_count = kv32;
    }

    (void)kv_count; /* we'd need to parse KV pairs for exact dims */

    fclose(f);

    /* Estimate: typical GGUF packs ~2 bytes per param (Q4/Q5 average) */
    if (out->file_size_bytes > 0 && tensor_count > 0) {
        out->param_count = out->file_size_bytes / 2; /* rough for quantized */
        out->runtime_mb = (float)(out->file_size_bytes + 512 * 1024 * 1024) / (1024.0f * 1024.0f);
    }
    return 1;
}

static void profile_model_file(SartreModelProfile *p) {
    p->file_size_bytes = file_size_bytes(p->path);
    if (p->file_size_bytes <= 0) return;

    const char *ext = strrchr(p->path, '.');
    if (!ext) ext = "";

    if (strcmp(ext, ".gguf") == 0 || strcmp(ext, ".GGUF") == 0) {
        detect_gguf_params(p->path, p);
    } else if (strcmp(ext, ".bin") == 0) {
        /* Raw float32 weights: params = file_size / 4 */
        p->param_count = p->file_size_bytes / 4;
        p->runtime_mb = (float)p->file_size_bytes / (1024.0f * 1024.0f) * 1.2f;
    } else if (strcmp(ext, ".pt") == 0 || strcmp(ext, ".pth") == 0) {
        /* PyTorch checkpoint: rough estimate, mixed types */
        p->param_count = p->file_size_bytes / 3; /* avg ~3 bytes per param */
        p->runtime_mb = (float)p->file_size_bytes / (1024.0f * 1024.0f) * 1.5f;
    } else {
        /* Unknown format: assume float32 */
        p->param_count = p->file_size_bytes / 4;
        p->runtime_mb = (float)p->file_size_bytes / (1024.0f * 1024.0f) * 1.2f;
    }

    /* Check if it fits */
    p->fits_in_ram = (p->runtime_mb < sys.total_ram_mb * 0.8f) ? 1 : 0;
}

int sartre_model_register(const char *name, const char *path) {
    if (!sartre_ready || !name || !path) return -1;
    if (sys.model_count >= SARTRE_MAX_MODELS) return -1;

    /* Check if already registered */
    for (int i = 0; i < sys.model_count; i++) {
        if (strncmp(sys.models[i].name, name, 63) == 0) {
            /* Update path, re-profile */
            strncpy(sys.models[i].path, path, 255);
            sys.models[i].path[255] = '\0';
            profile_model_file(&sys.models[i]);

            char ev[128];
            snprintf(ev, sizeof(ev), "model_update:%s %lldM params",
                     name, (long long)(sys.models[i].param_count / 1000000));
            sartre_notify_event(ev);
            return i;
        }
    }

    int idx = sys.model_count++;
    memset(&sys.models[idx], 0, sizeof(SartreModelProfile));
    strncpy(sys.models[idx].name, name, 63);
    sys.models[idx].name[63] = '\0';
    strncpy(sys.models[idx].path, path, 255);
    sys.models[idx].path[255] = '\0';

    profile_model_file(&sys.models[idx]);

    char ev[128];
    snprintf(ev, sizeof(ev), "model_register:%s %lldM params %.0fMB fits=%d",
             name, (long long)(sys.models[idx].param_count / 1000000),
             sys.models[idx].runtime_mb, sys.models[idx].fits_in_ram);
    sartre_notify_event(ev);

    /* Also update legacy tongue tier based on best model */
    const SartreModelProfile *best = sartre_model_best();
    if (best && sys.tongue_override < 0) {
        if (best->param_count >= 2000000000LL)      sys.tongue_tier = SARTRE_TONGUE_3B;
        else if (best->param_count >= 1000000000LL)  sys.tongue_tier = SARTRE_TONGUE_15B;
        else                                          sys.tongue_tier = SARTRE_TONGUE_05B;
    }

    return idx;
}

const SartreModelProfile *sartre_model_get(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < sys.model_count; i++) {
        if (strncmp(sys.models[i].name, name, 63) == 0)
            return &sys.models[i];
    }
    return NULL;
}

const SartreModelProfile *sartre_model_best(void) {
    const SartreModelProfile *best = NULL;
    int64_t best_params = -1;

    for (int i = 0; i < sys.model_count; i++) {
        if (sys.models[i].fits_in_ram && sys.models[i].param_count > best_params) {
            best = &sys.models[i];
            best_params = sys.models[i].param_count;
        }
    }

    /* Fallback: if nothing fits, pick smallest */
    if (!best && sys.model_count > 0) {
        int64_t smallest = INT64_MAX;
        for (int i = 0; i < sys.model_count; i++) {
            if (sys.models[i].param_count < smallest) {
                smallest = sys.models[i].param_count;
                best = &sys.models[i];
            }
        }
    }

    return best;
}

void sartre_model_set_loaded(const char *name, int loaded) {
    for (int i = 0; i < sys.model_count; i++) {
        if (strncmp(sys.models[i].name, name, 63) == 0) {
            sys.models[i].loaded = loaded;
            sartre_update_module(name, loaded ? SARTRE_MODULE_ACTIVE : SARTRE_MODULE_IDLE,
                                 loaded ? sys.models[i].runtime_mb / (float)sys.total_ram_mb : 0.0f);
            char ev[128];
            snprintf(ev, sizeof(ev), "model_%s:%s", loaded ? "load" : "unload", name);
            sartre_notify_event(ev);
            return;
        }
    }
}

void sartre_model_list(void) {
    printf("\n=== SARTRE MODELS ===\n");
    for (int i = 0; i < sys.model_count; i++) {
        SartreModelProfile *m = &sys.models[i];
        printf("  %s %s [%lldM params, %.0fMB, %s%s]\n",
               m->loaded ? "*" : " ",
               m->name,
               (long long)(m->param_count / 1000000),
               m->runtime_mb,
               m->fits_in_ram ? "fits" : "NO FIT",
               m->can_embed ? ", embed" : "");
        printf("    %s\n", m->path);
    }
    const SartreModelProfile *best = sartre_model_best();
    printf("  best: %s\n\n", best ? best->name : "(none)");
}

/* ═══════════════════════════════════════════════════════════════════
 * DEBUG / MONITORING — sartre observes itself
 * ═══════════════════════════════════════════════════════════════════ */

void sartre_print_state(void) {
    if (!sartre_ready) {
        printf("[sartre] not initialized\n");
        return;
    }

    int64_t uptime_s = (int64_t)time(NULL) - sys.boot_time_ms / 1000;

    printf("\n=== SARTRE KERNEL STATE ===\n\n");

    printf("Uptime: %llds | Steps: %lld\n\n", (long long)uptime_s, (long long)sys.step_count);

    printf("Inner World:\n");
    printf("  trauma: %.2f  arousal: %.2f  valence: %.2f\n",
           sys.trauma_level, sys.arousal, sys.valence);
    printf("  coherence: %.2f  prophecy_debt: %.2f  entropy: %.2f\n\n",
           sys.coherence, sys.prophecy_debt, sys.entropy);

    printf("Overlay: base=%lldB delta=%lldB writes=%d ratio=%.3f\n\n",
           (long long)sys.overlay.base_size, (long long)sys.overlay.delta_size,
           sys.overlay.overlay_writes, sys.overlay.overlay_ratio);

    printf("Resources: mem_pressure=%.2f cpu=%.2f\n", sys.memory_pressure, sys.cpu_load);
    printf("Tongue: %s (RAM: %lld MB, %s)\n\n",
           sartre_tongue_tier_name(sys.tongue_tier),
           (long long)sys.total_ram_mb,
           sys.tongue_override >= 0 ? "override" : "auto");

    printf("Modules (%d):\n", sys.module_count);
    for (int i = 0; i < sys.module_count; i++) {
        printf("  [%d] %-20s status=%d load=%.2f\n",
               i, sys.modules[i].name, sys.modules[i].status, sys.modules[i].load);
    }

    printf("\nNamespaces (%d):\n", sys.ns_count);
    for (int i = 0; i < sys.ns_count; i++) {
        printf("  [%d] %-16s cpu=%.1f%% mem=%.0fMB %s\n",
               i, sys.namespaces[i].name,
               sys.namespaces[i].cpu_share * 100,
               sys.namespaces[i].mem_limit_mb,
               sys.namespaces[i].active ? "ACTIVE" : "dead");
    }

    printf("\nRecent Events (%d):\n", sys.event_count);
    for (int i = 0; i < sys.event_count; i++) {
        printf("  [%d] %s\n", i, sys.last_events[i]);
    }

    printf("\nFlags: spiral=%d wormhole=%d strange_loop=%d\n\n",
           sys.spiral_detected, sys.wormhole_active, sys.strange_loop);
}

/* ═══════════════════════════════════════════════════════════════════
 * JSON EXPORT — for web UI integration
 * ═══════════════════════════════════════════════════════════════════ */

int sartre_state_to_json(char *buf, int max) {
    if (!sartre_ready) return snprintf(buf, max, "{\"ready\":false}");

    int64_t uptime_s = (int64_t)time(NULL) - sys.boot_time_ms / 1000;

    /* count installed packages */
    int installed = 0;
    for (int i = 0; i < sys.pkg_count; i++)
        if (sys.packages[i].installed) installed++;

    /* count active namespaces */
    int active_ns = 0;
    for (int i = 0; i < sys.ns_count; i++)
        if (sys.namespaces[i].active) active_ns++;

    return snprintf(buf, max,
        "{"
        "\"ready\":true,"
        "\"uptime\":%lld,"
        "\"steps\":%lld,"
        "\"ram_mb\":%lld,"
        "\"tongue\":\"%s\","
        "\"modules\":%d,"
        "\"trauma\":%.3f,"
        "\"arousal\":%.3f,"
        "\"valence\":%.3f,"
        "\"coherence\":%.3f,"
        "\"prophecy_debt\":%.3f,"
        "\"entropy\":%.3f,"
        "\"overlay_ratio\":%.4f,"
        "\"overlay_writes\":%d,"
        "\"overlay_base\":%lld,"
        "\"overlay_delta\":%lld,"
        "\"namespaces\":%d,"
        "\"ns_active\":%d,"
        "\"packages\":%d,"
        "\"pkg_installed\":%d,"
        "\"events\":%d,"
        "\"mem_pressure\":%.3f,"
        "\"cpu_load\":%.3f,"
        "\"spiral\":%d,"
        "\"wormhole\":%d,"
        "\"strange_loop\":%d"
        "}",
        (long long)uptime_s,
        (long long)sys.step_count,
        (long long)sys.total_ram_mb,
        sartre_tongue_tier_name(sys.tongue_tier),
        sys.module_count,
        sys.trauma_level, sys.arousal, sys.valence,
        sys.coherence, sys.prophecy_debt, sys.entropy,
        sys.overlay.overlay_ratio, sys.overlay.overlay_writes,
        (long long)sys.overlay.base_size, (long long)sys.overlay.delta_size,
        sys.ns_count, active_ns,
        sys.pkg_count, installed,
        sys.event_count,
        sys.memory_pressure, sys.cpu_load,
        sys.spiral_detected, sys.wormhole_active, sys.strange_loop
    );
}

/* ═══════════════════════════════════════════════════════════════════
 * STANDALONE MAIN — when sartre_kernel.c compiles alone
 *
 * cc sartre_kernel.c -O2 -lm -o sartre_kernel && ./sartre_kernel
 * ═══════════════════════════════════════════════════════════════════ */

#ifndef HAS_DARIO

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    printf("\n  sartre_kernel — Meta-Linux for the Dario Equation\n");
    printf("  \"L'existence precede l'essence.\"\n\n");

    sartre_init(NULL);

    /* register core packages */
    sartre_pkg_register("dario_equation", "1.0.0", 83);
    sartre_pkg_register("hebbian_field",  "1.0.0", 12);
    sartre_pkg_register("prophecy",       "1.0.0", 8);
    sartre_pkg_register("trauma_engine",  "1.0.0", 4);
    sartre_pkg_register("velocity_ops",   "1.0.0", 6);
    sartre_pkg_register("chambers",       "1.0.0", 10);
    sartre_pkg_register("overlay_fs",     "1.0.0", 2);

    /* install core */
    sartre_pkg_install("dario_equation");
    sartre_pkg_install("hebbian_field");
    sartre_pkg_install("prophecy");

    /* init overlay */
    sartre_overlay_init(83 * 1024); /* dario.c ~ 83KB */

    /* create a namespace for the equation */
    sartre_ns_create("dario", 0.8f, 64.0f);
    sartre_ns_create("observer", 0.1f, 8.0f);

    /* simulate some inner state */
    sartre_update_inner_state(0.15f, 0.6f, 0.7f, 0.85f, 1.2f);
    sartre_notify_event("boot_complete");

    /* print state */
    sartre_print_state();

    /* JSON export demo */
    char json[2048];
    sartre_state_to_json(json, sizeof(json));
    printf("JSON:\n%s\n\n", json);

    /* package list */
    sartre_pkg_list();

    sartre_shutdown();
    return 0;
}

#endif /* !HAS_DARIO */
