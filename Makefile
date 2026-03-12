CC ?= cc
CFLAGS = -O2 -lm
WARN = -Wall -Wextra -Wno-unused-parameter

all: dario

dario: dario.c
	$(CC) $(WARN) dario.c $(CFLAGS) -o dario

test: tests/test_dario.c dario.c
	$(CC) $(WARN) tests/test_dario.c $(CFLAGS) -o test_dario
	./test_dario

no-web: dario.c
	$(CC) $(WARN) dario.c $(CFLAGS) -DDARIO_NO_WEB -o dario

clean:
	rm -f dario test_dario

.PHONY: all test no-web clean
