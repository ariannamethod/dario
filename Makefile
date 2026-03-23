CC ?= cc
CFLAGS = -O2 -lm
WARN = -Wall -Wextra -Wno-unused-parameter

# ── dario alone (works without sartre or kk) ──
dario: dario.c
	$(CC) $(WARN) dario.c $(CFLAGS) -o dario

# ── sartre_kernel alone (works without dario) ──
sartre: sartre_kernel.c sartre_kernel.h
	$(CC) $(WARN) sartre_kernel.c $(CFLAGS) -o sartre_kernel

# ── formula + operating system ──
full: dario.c sartre_kernel.c sartre_kernel.h
	$(CC) $(WARN) dario.c sartre_kernel.c -DHAS_SARTRE -DHAS_DARIO $(CFLAGS) -o dario

# ── formula + operating system + knowledge kernel ──
all: dario.c sartre_kernel.c sartre_kernel.h kk_kernel.c kk_kernel.h
	$(CC) $(WARN) dario.c sartre_kernel.c kk_kernel.c \
		-DHAS_SARTRE -DHAS_DARIO -DHAS_KK $(CFLAGS) -lsqlite3 -o dario

# ── kk_kernel alone (CLI mode) ──
kk: kk_kernel.c kk_kernel.h
	$(CC) $(WARN) kk_kernel.c $(CFLAGS) -lsqlite3 -DKK_STANDALONE -o kk

test: tests/test_dario.c dario.c
	$(CC) $(WARN) tests/test_dario.c $(CFLAGS) -o test_dario
	./test_dario

no-web: dario.c
	$(CC) $(WARN) dario.c $(CFLAGS) -DDARIO_NO_WEB -o dario

clean:
	rm -f dario sartre_kernel kk test_dario dario_memory.db

.PHONY: all full dario sartre kk test no-web clean
