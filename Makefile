CC ?= cc
CFLAGS = -O2 -lm
WARN = -Wall -Wextra -Wno-unused-parameter

# ── dario alone (works without sartre) ──
dario: dario.c
	$(CC) $(WARN) dario.c $(CFLAGS) -o dario

# ── sartre_kernel alone (works without dario) ──
sartre: sartre_kernel.c sartre_kernel.h
	$(CC) $(WARN) sartre_kernel.c $(CFLAGS) -o sartre_kernel

# ── together: formula + operating system ──
all: dario.c sartre_kernel.c sartre_kernel.h
	$(CC) $(WARN) dario.c sartre_kernel.c -DHAS_SARTRE -DHAS_DARIO $(CFLAGS) -o dario

test: tests/test_dario.c dario.c
	$(CC) $(WARN) tests/test_dario.c $(CFLAGS) -o test_dario
	./test_dario

no-web: dario.c
	$(CC) $(WARN) dario.c $(CFLAGS) -DDARIO_NO_WEB -o dario

clean:
	rm -f dario sartre_kernel test_dario

.PHONY: all dario sartre test no-web clean
