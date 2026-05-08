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
# DARIO_TOPK_FLAG: optional, e.g. -DDARIO_TOPK=0 to disable top-k cutoff
all: dario.c sartre_kernel.c sartre_kernel.h kk_kernel.c kk_kernel.h
	$(CC) $(WARN) dario.c sartre_kernel.c kk_kernel.c \
		-DHAS_SARTRE -DHAS_DARIO -DHAS_KK $(DARIO_TOPK_FLAG) $(CFLAGS) -lsqlite3 -o dario

# topk=0 ablation build (used by tests/test_15)
all-topk0: dario.c sartre_kernel.c sartre_kernel.h kk_kernel.c kk_kernel.h
	$(CC) $(WARN) dario.c sartre_kernel.c kk_kernel.c \
		-DHAS_SARTRE -DHAS_DARIO -DHAS_KK -DDARIO_TOPK=0 $(CFLAGS) -lsqlite3 -o dario_topk0
	@echo "Built dario_topk0 with DARIO_TOPK=0 (full distribution sampling)"

# ── kk_kernel alone (CLI mode) ──
kk: kk_kernel.c kk_kernel.h
	$(CC) $(WARN) kk_kernel.c $(CFLAGS) -lsqlite3 -DKK_STANDALONE -o kk

test: tests/test_dario.c dario.c
	$(CC) $(WARN) tests/test_dario.c $(CFLAGS) -o test_dario
	./test_dario

no-web: dario.c
	$(CC) $(WARN) dario.c $(CFLAGS) -DDARIO_NO_WEB -o dario

# ── Janus v4 inference with system notorch (libnotorch.a) ──
UNAME := $(shell uname)
ifeq ($(UNAME), Darwin)
  NOTORCH_INC = -I/opt/homebrew/include
  NOTORCH_LIB = -L/opt/homebrew/lib -lnotorch
  BLAS_FLAGS  = -DUSE_BLAS -DACCELERATE -DACCELERATE_NEW_LAPACK -framework Accelerate
endif
ifeq ($(UNAME), Linux)
  NOTORCH_INC = -I/usr/local/include
  NOTORCH_LIB = -L/usr/local/lib -lnotorch
  BLAS_FLAGS  = -DUSE_BLAS -lopenblas
endif

infer_v4: infer_v4.c
	$(CC) -O3 -Wall infer_v4.c $(NOTORCH_INC) $(NOTORCH_LIB) $(BLAS_FLAGS) -lm -o infer_v4
	@echo "Built infer_v4 with system notorch (matvec hot path)"

# ── infer_v4 with notorch profiler (RunPod benchmark builds) ──
profile: infer_v4.c
	$(CC) -DDARIO_PROFILE -O2 -Wall infer_v4.c $(NOTORCH_INC) $(NOTORCH_LIB) $(BLAS_FLAGS) -lm -o infer_v4_profile
	@echo "Built infer_v4_profile with -DDARIO_PROFILE (notorch profiler enabled)"

# ── dario + Leo inference ──
dario_leo: dario_leo.c kk_kernel.c kk_kernel.h
	$(CC) $(WARN) dario_leo.c kk_kernel.c $(NOTORCH_INC) $(NOTORCH_LIB) $(BLAS_FLAGS) $(CFLAGS) -lsqlite3 -o dario_leo
	@echo "Built dario_leo with system notorch BLAS"

# ── Download all required weights from ataeff/dario HF repo ──
WEIGHTS_DIR ?= weights
HF_REPO = ataeff/dario

weights:
	@mkdir -p $(WEIGHTS_DIR)
	hf download $(HF_REPO) \
		janus_v4_base_22k.bin \
		janus_v4_sft_leo.bin \
		janus_v4_sft_arianna.bin \
		janus_v4_sft_yent.bin \
		resonance_200m_lora_yent.bin \
		leo_janus_d12_f16.bin \
		tokenizer.pkl \
		tokenizer_yent.bin \
		--local-dir $(WEIGHTS_DIR)
	@echo "Downloaded weights to $(WEIGHTS_DIR)/"

# ── Go binaries (drop-in replacements for Python scripts) ──
go-bins:
	cd cmd && go build -o ../bin/dario-infer ./dario-infer
	cd cmd && go build -o ../bin/dario-dialogue ./dario-dialogue
	cd cmd && go build -o ../bin/dario-forum ./dario-forum
	@echo "Built Go binaries: bin/{dario-infer,dario-dialogue,dario-forum}"

# ── AML binaries (canonical port — replaces Python entirely) ──
# Compiles aml/dario_*.aml through amlc. amlc auto-links libnotorch +
# libaml + Accelerate; we add -lsqlite3 for the kk_kernel.c bindings.
# Each binary is self-contained — uses libnotorch's BPE tokenizer and
# spawns ./infer_v4 as a sub-process for the actual forward pass.
aml-bins: aml/dario_infer aml/dario_dialogue aml/dario_forum

aml/dario_infer: aml/dario_infer.aml
	cd aml && amlc dario_infer.aml -o dario_infer

aml/dario_dialogue: aml/dario_dialogue.aml kk_kernel.c kk_kernel.h
	cd aml && amlc dario_dialogue.aml -o dario_dialogue

aml/dario_forum: aml/dario_forum.aml kk_kernel.c kk_kernel.h
	cd aml && amlc dario_forum.aml -o dario_forum

clean:
	rm -f dario sartre_kernel kk test_dario dario_memory.db infer_v4 infer_v4_profile dario_leo
	rm -f aml/dario_infer aml/dario_infer.c
	rm -f aml/dario_dialogue aml/dario_dialogue.c
	rm -f aml/dario_forum aml/dario_forum.c
	rm -rf bin/

.PHONY: all all-topk0 full dario sartre kk test no-web clean infer_v4 profile dario_leo weights go-bins aml-bins
