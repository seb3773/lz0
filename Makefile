CC ?= gcc
VERSION ?= 0.6.0-dev
UNAME_S := $(shell uname -s 2>/dev/null || echo Linux)
UNAME_M := $(shell uname -m 2>/dev/null || echo x86_64)
DARWIN_CFLAGS := $(if $(filter Darwin,$(UNAME_S)),-D_DARWIN_C_SOURCE,)
STRIP_FLAG := $(if $(filter Darwin,$(UNAME_S)),,-s)
CFLAGS = -O3 -std=c11 -D_POSIX_C_SOURCE=200809L $(DARWIN_CFLAGS) -Wall -Wextra -Wno-unused-parameter
CFLAGS += -Izx0_compressor -Izx0_decompressor -Ilz0 -DLZ0_VERSION=\"$(VERSION)\"

ZX0_SRC = zx0_compressor/compress.c zx0_compressor/optimize.c \
          zx0_compressor/memory.c zx0_compressor/zx0_compress_wrapper.c \
          zx0_decompressor/zx0_decompress.c

# Encoder SIMD kernels: every architecture file is guarded by #if, so all of
# them can be listed for every target (foreign ones assemble to nothing; Win64
# and other targets fall back to the portable C tier in lz0_simd.c).
LZ0_SIMD_SRC = lz0/lz0_simd.c lz0/lz0_simd_x86_64.S lz0/lz0_simd_arm64.S lz0/lz0_simd_riscv64.S

LZ0_SRC = lz0/lz0_cost.c lz0/lz0_find.c lz0/lz0_parse.c \
          lz0/lz0_emit.c lz0/lz0_decompress.c lz0/lz0_compress.c lz0/lz0_ultra.c \
          $(LZ0_SIMD_SRC)

ifneq ($(filter arm64 aarch64,$(UNAME_M)),)
HOST_ASM_DEC = lz0/lz0dec_arm64.S lz0/lz0dec_fast_arm64.S
else ifeq ($(UNAME_M),riscv64)
HOST_ASM_DEC = lz0/lz0dec_riscv64.S lz0/lz0dec_fast_riscv64.S
else
HOST_ASM_DEC = lz0/lz0dec_x86_64.S lz0/lz0dec_fast_x86_64.S
endif

LZ0DEC_SRC = lz0/lz0dec_ref.c lz0/lz0dec_min.c $(HOST_ASM_DEC)

.PHONY: all bench clean test smoke bounds limits diag sdp32 scale prof cmpfind jumpsweep pruneab bits final gap saab streamab freeze vectors boundary malformed fuzz guard simd sa_probe enc_bench enc_tiers find_bench mem_peak lz0 lz0dec cli sanitize valgrind bench_all bench_ratio bench_dec bench_dec_arm64 bench_dec_riscv64 bench_blobs test_arm64 arm64 test_riscv64 riscv64 windows test_windows dist dist_linux dist_arm64 dist_riscv64 dist_windows dist_macos

all: bin/lz0 bin/lz0dec bench/lz0_bench bench/lz0_smoke bench/lz0_bounds bench/lz0_diag_rep bench/lz0_sdp32 bench/lz0_scale bench/lz0_prof bench/lz0_cmpfind bench/lz0_jumpsweep bench/lz0_pruneab bench/lz0_bits bench/lz0_final

bench/lz0_bench_ratio: bench/bench_ratio.c $(ZX0_SRC) $(LZ0_SRC) $(LZ0DEC_SRC)
	$(CC) $(CFLAGS) -o $@ bench/bench_ratio.c $(ZX0_SRC) $(LZ0_SRC) $(LZ0DEC_SRC) -lm

bench_ratio: bench/lz0_bench_ratio
	./bench/lz0_bench_ratio

bench/lz0_bench: bench/bench.c $(ZX0_SRC) $(LZ0_SRC)
	$(CC) $(CFLAGS) -o $@ bench/bench.c $(ZX0_SRC) $(LZ0_SRC)

bench/lz0_bench_blobs: bench/bench_blobs.c $(LZ0_SRC) $(LZ0DEC_SRC) bench/external/upkr/c_unpacker/unpack.c
	$(CC) $(CFLAGS) -o $@ bench/bench_blobs.c $(LZ0_SRC) $(LZ0DEC_SRC) bench/external/upkr/c_unpacker/unpack.c -llz4 -lm

bench_blobs: bench/lz0_bench_blobs
	./bench/lz0_bench_blobs

bench/lz0_bench_all: bench/bench_all.c $(ZX0_SRC) $(LZ0_SRC) $(LZ0DEC_SRC) bench/external/upkr/c_unpacker/unpack.c
	$(CC) $(CFLAGS) -o $@ bench/bench_all.c $(ZX0_SRC) $(LZ0_SRC) $(LZ0DEC_SRC) bench/external/upkr/c_unpacker/unpack.c -llz4 -lm

bench_all: bench/lz0_bench_all
	./bench/lz0_bench_all

bench/lz0_bench_dec: bench/bench_dec.c $(LZ0_SRC) $(LZ0DEC_SRC)
	$(CC) $(CFLAGS) -o $@ bench/bench_dec.c $(LZ0_SRC) $(LZ0DEC_SRC)

bench_dec: bench/lz0_bench_dec
	./bench/lz0_bench_dec

bench_dec_arm64: bench/bench_dec.c $(LZ0_SRC) $(ARM64_DEC_SRC)
	$(ARM64_CC) $(ARM64_CFLAGS) -o bench/lz0_bench_dec_arm64 bench/bench_dec.c $(LZ0_SRC) $(ARM64_DEC_SRC)
	$(ARM64_EMU) ./bench/lz0_bench_dec_arm64

bench_dec_riscv64: bench/bench_dec.c $(LZ0_SRC) $(RISCV64_DEC_SRC)
	$(RISCV64_CC) $(RISCV64_CFLAGS) -o bench/lz0_bench_dec_riscv64 bench/bench_dec.c $(LZ0_SRC) $(RISCV64_DEC_SRC)
	$(RISCV64_EMU) ./bench/lz0_bench_dec_riscv64

bench/lz0_smoke: bench/smoke.c $(LZ0_SRC)
	$(CC) $(CFLAGS) -o $@ bench/smoke.c $(LZ0_SRC)

bench/lz0_bounds: bench/offset_bounds.c $(LZ0_SRC)
	$(CC) $(CFLAGS) -o $@ bench/offset_bounds.c $(LZ0_SRC)

bench/lz0_diag_rep: bench/diag_rep.c $(LZ0_SRC)
	$(CC) $(CFLAGS) -o $@ bench/diag_rep.c $(LZ0_SRC)

smoke: bench/lz0_smoke
	./bench/lz0_smoke

bounds: bench/lz0_bounds
	./bench/lz0_bounds

diag: bench/lz0_diag_rep
	./bench/lz0_diag_rep

bench/lz0_sdp32: bench/sdp32.c $(ZX0_SRC) $(LZ0_SRC)
	$(CC) $(CFLAGS) -o $@ bench/sdp32.c $(ZX0_SRC) $(LZ0_SRC)

sdp32: bench/lz0_sdp32
	./bench/lz0_sdp32

bench/lz0_scale: bench/scale.c $(ZX0_SRC) $(LZ0_SRC)
	$(CC) $(CFLAGS) -o $@ bench/scale.c $(ZX0_SRC) $(LZ0_SRC)

scale: bench/lz0_scale
	./bench/lz0_scale

bench/lz0_prof: bench/prof.c $(LZ0_SRC)
	$(CC) $(CFLAGS) -o $@ bench/prof.c $(LZ0_SRC)

prof: bench/lz0_prof
	./bench/lz0_prof 256K

bench/lz0_cmpfind: bench/cmp_find.c $(LZ0_SRC)
	$(CC) $(CFLAGS) -o $@ bench/cmp_find.c $(LZ0_SRC)

cmpfind: bench/lz0_cmpfind
	./bench/lz0_cmpfind

bench/lz0_jumpsweep: bench/jump_sweep.c $(LZ0_SRC)
	$(CC) $(CFLAGS) -o $@ bench/jump_sweep.c $(LZ0_SRC)

jumpsweep: bench/lz0_jumpsweep
	./bench/lz0_jumpsweep

bench/lz0_pruneab: bench/prune_ab.c $(LZ0_SRC)
	$(CC) $(CFLAGS) -o $@ bench/prune_ab.c $(LZ0_SRC)

pruneab: bench/lz0_pruneab
	./bench/lz0_pruneab

bench/lz0_bits: bench/bits.c $(LZ0_SRC)
	$(CC) $(CFLAGS) -o $@ bench/bits.c $(LZ0_SRC)

bits: bench/lz0_bits
	./bench/lz0_bits

bench/lz0_final: bench/final.c $(ZX0_SRC) $(LZ0_SRC)
	$(CC) $(CFLAGS) -o $@ bench/final.c $(ZX0_SRC) $(LZ0_SRC)

final: bench/lz0_final
	./bench/lz0_final

bench/lz0_gap: bench/gap.c $(ZX0_SRC) $(LZ0_SRC)
	$(CC) $(CFLAGS) -o $@ bench/gap.c $(ZX0_SRC) $(LZ0_SRC)

gap: bench/lz0_gap
	./bench/lz0_gap

bench/lz0_saab: bench/sa_ab.c $(LZ0_SRC)
	$(CC) $(CFLAGS) -o $@ bench/sa_ab.c $(LZ0_SRC)

saab: bench/lz0_saab
	./bench/lz0_saab

bench/lz0_streamab: bench/stream_ab.c $(LZ0_SRC)
	$(CC) $(CFLAGS) -o $@ bench/stream_ab.c $(LZ0_SRC)

streamab: bench/lz0_streamab
	./bench/lz0_streamab

bench/lz0_freeze: bench/freeze.c $(LZ0_SRC)
	$(CC) $(CFLAGS) -o $@ bench/freeze.c $(LZ0_SRC)

freeze: bench/lz0_freeze
	./bench/lz0_freeze

bin/lz0: lz0/lz0_cli.c $(LZ0_SRC) $(LZ0DEC_SRC)
	mkdir -p bin
	$(CC) $(CFLAGS) -o $@ lz0/lz0_cli.c $(LZ0_SRC) $(LZ0DEC_SRC)

bin/lz0dec: lz0/lz0dec_cli.c lz0/lz0dec_min.c $(HOST_ASM_DEC)
	mkdir -p bin
	$(CC) $(CFLAGS) $(STRIP_FLAG) -o $@ lz0/lz0dec_cli.c lz0/lz0dec_min.c $(HOST_ASM_DEC)

lz0: bin/lz0 bin/lz0dec

lz0dec: bin/lz0dec

bin/lz0_arm64: lz0/lz0_cli.c $(LZ0_SRC) $(ARM64_DEC_SRC)
	mkdir -p bin
	$(ARM64_CC) $(ARM64_CFLAGS) -s -o $@ lz0/lz0_cli.c $(LZ0_SRC) $(ARM64_DEC_SRC)

bin/lz0dec_arm64: lz0/lz0dec_cli.c lz0/lz0dec_min.c lz0/lz0dec_arm64.S lz0/lz0dec_fast_arm64.S
	mkdir -p bin
	$(ARM64_CC) $(ARM64_CFLAGS) -s -o $@ lz0/lz0dec_cli.c lz0/lz0dec_min.c lz0/lz0dec_arm64.S lz0/lz0dec_fast_arm64.S

bin/lz0_riscv64: lz0/lz0_cli.c $(LZ0_SRC) $(RISCV64_DEC_SRC)
	mkdir -p bin
	$(RISCV64_CC) $(RISCV64_CFLAGS) -s -o $@ lz0/lz0_cli.c $(LZ0_SRC) $(RISCV64_DEC_SRC)

bin/lz0dec_riscv64: lz0/lz0dec_cli.c lz0/lz0dec_min.c lz0/lz0dec_riscv64.S lz0/lz0dec_fast_riscv64.S
	mkdir -p bin
	$(RISCV64_CC) $(RISCV64_CFLAGS) -s -o $@ lz0/lz0dec_cli.c lz0/lz0dec_min.c lz0/lz0dec_riscv64.S lz0/lz0dec_fast_riscv64.S

MINGW_CC ?= x86_64-w64-mingw32-gcc

bin/lz0.exe: lz0/lz0_cli.c $(LZ0_SRC) lz0/lz0dec_min.c lz0/lz0dec_x86_64.S lz0/lz0dec_fast_x86_64.S
	mkdir -p bin
	$(MINGW_CC) $(CFLAGS) -static -s -o $@ lz0/lz0_cli.c $(LZ0_SRC) lz0/lz0dec_min.c lz0/lz0dec_x86_64.S lz0/lz0dec_fast_x86_64.S

bin/lz0dec.exe: lz0/lz0dec_cli.c lz0/lz0dec_min.c lz0/lz0dec_x86_64.S lz0/lz0dec_fast_x86_64.S
	mkdir -p bin
	$(MINGW_CC) $(CFLAGS) -static -s -o $@ lz0/lz0dec_cli.c lz0/lz0dec_min.c lz0/lz0dec_x86_64.S lz0/lz0dec_fast_x86_64.S

windows: bin/lz0.exe bin/lz0dec.exe

dist_linux: bin/lz0 bin/lz0dec
	mkdir -p build/dist/lz0-v$(VERSION)-linux-x86_64 dist
	cp bin/lz0 bin/lz0dec README.md LICENSE build/dist/lz0-v$(VERSION)-linux-x86_64/
	cp -r docs build/dist/lz0-v$(VERSION)-linux-x86_64/
	tar -czvf dist/lz0-v$(VERSION)-linux-x86_64.tar.gz -C build/dist lz0-v$(VERSION)-linux-x86_64
	rm -rf build/dist/lz0-v$(VERSION)-linux-x86_64

dist_arm64: bin/lz0_arm64 bin/lz0dec_arm64
	mkdir -p build/dist/lz0-v$(VERSION)-linux-arm64 dist
	cp bin/lz0_arm64 build/dist/lz0-v$(VERSION)-linux-arm64/lz0
	cp bin/lz0dec_arm64 build/dist/lz0-v$(VERSION)-linux-arm64/lz0dec
	cp README.md LICENSE build/dist/lz0-v$(VERSION)-linux-arm64/
	cp -r docs build/dist/lz0-v$(VERSION)-linux-arm64/
	tar -czvf dist/lz0-v$(VERSION)-linux-arm64.tar.gz -C build/dist lz0-v$(VERSION)-linux-arm64
	rm -rf build/dist/lz0-v$(VERSION)-linux-arm64

dist_riscv64: bin/lz0_riscv64 bin/lz0dec_riscv64
	mkdir -p build/dist/lz0-v$(VERSION)-linux-riscv64 dist
	cp bin/lz0_riscv64 build/dist/lz0-v$(VERSION)-linux-riscv64/lz0
	cp bin/lz0dec_riscv64 build/dist/lz0-v$(VERSION)-linux-riscv64/lz0dec
	cp README.md LICENSE build/dist/lz0-v$(VERSION)-linux-riscv64/
	cp -r docs build/dist/lz0-v$(VERSION)-linux-riscv64/
	tar -czvf dist/lz0-v$(VERSION)-linux-riscv64.tar.gz -C build/dist lz0-v$(VERSION)-linux-riscv64
	rm -rf build/dist/lz0-v$(VERSION)-linux-riscv64

dist_windows: windows
	mkdir -p build/dist/lz0-v$(VERSION)-windows-x86_64 dist
	cp bin/lz0.exe bin/lz0dec.exe README.md LICENSE build/dist/lz0-v$(VERSION)-windows-x86_64/
	cp -r docs build/dist/lz0-v$(VERSION)-windows-x86_64/
	cd build/dist && zip -r ../../dist/lz0-v$(VERSION)-windows-x86_64.zip lz0-v$(VERSION)-windows-x86_64
	rm -rf build/dist/lz0-v$(VERSION)-windows-x86_64

dist_macos: bin/lz0 bin/lz0dec
	mkdir -p build/dist/lz0-v$(VERSION)-macos-$(UNAME_M) dist
	cp bin/lz0 bin/lz0dec README.md LICENSE build/dist/lz0-v$(VERSION)-macos-$(UNAME_M)/
	cp -r docs build/dist/lz0-v$(VERSION)-macos-$(UNAME_M)/
	tar -czvf dist/lz0-v$(VERSION)-macos-$(UNAME_M).tar.gz -C build/dist lz0-v$(VERSION)-macos-$(UNAME_M)
	rm -rf build/dist/lz0-v$(VERSION)-macos-$(UNAME_M)

dist: dist_linux dist_arm64 dist_riscv64 dist_windows

cli: bin/lz0
	./bin/lz0 c tests/vectors/literals.bin /tmp/lz0_cli.lz0
	./bin/lz0 d /tmp/lz0_cli.lz0 /tmp/lz0_cli.out
	cmp tests/vectors/literals.bin /tmp/lz0_cli.out

tests/test_vectors: tests/test_vectors.c $(LZ0_SRC) $(LZ0DEC_SRC)
	$(CC) $(CFLAGS) -o $@ tests/test_vectors.c $(LZ0_SRC) $(LZ0DEC_SRC)

vectors: tests/test_vectors
	./tests/test_vectors

tests/test_boundary: tests/test_boundary.c $(LZ0_SRC) $(LZ0DEC_SRC)
	$(CC) $(CFLAGS) -o $@ tests/test_boundary.c $(LZ0_SRC) $(LZ0DEC_SRC)

boundary: tests/test_boundary
	./tests/test_boundary

tests/test_malformed: tests/test_malformed.c $(LZ0_SRC) $(LZ0DEC_SRC)
	$(CC) $(CFLAGS) -o $@ tests/test_malformed.c $(LZ0_SRC) $(LZ0DEC_SRC)

malformed: tests/test_malformed
	./tests/test_malformed

tests/test_guard: tests/test_guard.c $(LZ0_SRC) $(LZ0DEC_SRC)
	$(CC) $(CFLAGS) -o $@ tests/test_guard.c $(LZ0_SRC) $(LZ0DEC_SRC)

# Guard-page memory-safety + differential test (all decoders vs lz0dec_ref)
guard: tests/test_guard
	./tests/test_guard

tests/test_fuzz: tests/test_fuzz.c $(LZ0_SRC) $(LZ0DEC_SRC)
	$(CC) $(CFLAGS) -o $@ tests/test_fuzz.c $(LZ0_SRC) $(LZ0DEC_SRC)

fuzz: tests/test_fuzz
	./tests/test_fuzz

tests/test_simd: tests/test_simd.c $(LZ0_SIMD_SRC) lz0/lz0_simd.h
	$(CC) $(CFLAGS) -o $@ tests/test_simd.c $(LZ0_SIMD_SRC)

# Differential test: every SIMD tier of every encoder kernel against the C
# reference, inputs placed against a PROT_NONE guard page.
simd: tests/test_simd
	./tests/test_simd

tests/test_sa_probe: tests/test_sa_probe.c lz0/lz0_cost.c lz0/lz0_parse.c lz0/lz0_emit.c lz0/lz0_decompress.c lz0/lz0_compress.c lz0/lz0_ultra.c $(LZ0_SIMD_SRC)
	$(CC) $(CFLAGS) -o $@ tests/test_sa_probe.c lz0/lz0_cost.c lz0/lz0_parse.c lz0/lz0_emit.c lz0/lz0_decompress.c lz0/lz0_compress.c lz0/lz0_ultra.c $(LZ0_SIMD_SRC) -lm

sa_probe: tests/test_sa_probe
	./tests/test_sa_probe

tests/test_threadsafe: tests/test_threadsafe.c $(LZ0_SRC) $(LZ0DEC_SRC)
	$(CC) $(CFLAGS) -pthread -o $@ tests/test_threadsafe.c $(LZ0_SRC) $(LZ0DEC_SRC)

threadsafe: tests/test_threadsafe
	./tests/test_threadsafe

tests/test_window: tests/test_window.c $(LZ0_SRC) $(LZ0DEC_SRC)
	$(CC) $(CFLAGS) -o $@ tests/test_window.c $(LZ0_SRC) $(LZ0DEC_SRC)

window: tests/test_window
	./tests/test_window

tests/test_limits: tests/test_limits.c $(LZ0_SRC) $(LZ0DEC_SRC)
	$(CC) $(CFLAGS) -o $@ tests/test_limits.c $(LZ0_SRC) $(LZ0DEC_SRC)

limits: tests/test_limits
	./tests/test_limits

bench/lz0_enc_bench: bench/enc_bench.c $(LZ0_SRC) $(LZ0DEC_SRC)
	$(CC) $(CFLAGS) -o $@ bench/enc_bench.c $(LZ0_SRC) $(LZ0DEC_SRC) -lm

ENC_BENCH_FILES ?= bench/corpus/*
ENC_BENCH_LEVELS ?= 1,3,6,8,9
ENC_BENCH_REPS ?= 3

# Encoder speed + bit-exactness: per file/level size, FNV-1a hash of the
# compressed stream, best-of-N time; every stream is decoded by three decoders.
enc_bench: bench/lz0_enc_bench
	./bench/lz0_enc_bench $(ENC_BENCH_REPS) $(ENC_BENCH_LEVELS) $(ENC_BENCH_FILES)

# Same streams with each SIMD tier forced: hashes must be identical.
enc_tiers: bench/lz0_enc_bench
	@for t in 2 1 0; do echo "== LZ0_SIMD=$$t"; LZ0_SIMD=$$t ./bench/lz0_enc_bench 1 1,6,9 bench/corpus/cp.html bench/corpus/fields.c bench/corpus/sum | grep -v TOTAL; done

bench/lz0_find_bench: bench/find_bench.c $(LZ0_SRC)
	$(CC) $(CFLAGS) -o $@ bench/find_bench.c $(LZ0_SRC)

FIND_BENCH_LEVELS ?= 1,6,8,9

# Match-finder alone (insert + find_matches at every position): best-of-N
# time and an FNV-1a hash of every candidate list. Two builds return the
# same candidates iff all hashes match.
find_bench: bench/lz0_find_bench
	./bench/lz0_find_bench $(ENC_BENCH_REPS) $(FIND_BENCH_LEVELS) $(ENC_BENCH_FILES)

bench/lz0_mem_peak: bench/mem_peak.c $(LZ0_SRC)
	$(CC) $(CFLAGS) -o $@ bench/mem_peak.c $(LZ0_SRC)

MEM_PEAK_LEVELS ?= 1,3,6,8,9

# Encoder peak heap (exact, allocator interposed) + stream hash per file/level.
mem_peak: bench/lz0_mem_peak
	./bench/lz0_mem_peak $(MEM_PEAK_LEVELS) $(ENC_BENCH_FILES)

SANITIZE_CFLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer -g -O2 -std=c11 -D_POSIX_C_SOURCE=200809L $(DARWIN_CFLAGS) -Wall -Wextra -Wno-unused-parameter -Izx0_compressor -Izx0_decompressor -Ilz0

sanitize:
	$(CC) $(SANITIZE_CFLAGS) -o tests/test_vectors tests/test_vectors.c $(LZ0_SRC) $(LZ0DEC_SRC)
	./tests/test_vectors
	$(CC) $(SANITIZE_CFLAGS) -o tests/test_boundary tests/test_boundary.c $(LZ0_SRC) $(LZ0DEC_SRC)
	./tests/test_boundary
	$(CC) $(SANITIZE_CFLAGS) -o tests/test_malformed tests/test_malformed.c $(LZ0_SRC) $(LZ0DEC_SRC)
	./tests/test_malformed
	$(CC) $(SANITIZE_CFLAGS) -o tests/test_fuzz tests/test_fuzz.c $(LZ0_SRC) $(LZ0DEC_SRC)
	./tests/test_fuzz
	$(CC) $(SANITIZE_CFLAGS) -o tests/test_simd tests/test_simd.c $(LZ0_SIMD_SRC)
	./tests/test_simd 50000
	$(CC) $(SANITIZE_CFLAGS) -o tests/test_limits tests/test_limits.c $(LZ0_SRC) $(LZ0DEC_SRC)
	./tests/test_limits
	@echo "All tests passed cleanly under ASan + UBSan."

valgrind: bin/lz0
	valgrind --leak-check=full --error-exitcode=1 ./bin/lz0 c README.md /tmp/lz0_vg.lz0
	valgrind --leak-check=full --error-exitcode=1 ./bin/lz0 d /tmp/lz0_vg.lz0 /tmp/lz0_vg.out
	cmp README.md /tmp/lz0_vg.out
	rm -f /tmp/lz0_vg.lz0 /tmp/lz0_vg.out
	@echo "Valgrind: zero leaks detected."

test: smoke bounds limits bench/lz0_bench vectors boundary malformed guard simd threadsafe window
	./bench/lz0_bench --quick

ARM64_CC ?= aarch64-linux-gnu-gcc
ARM64_EMU = qemu-aarch64-static
ARM64_CFLAGS = -O3 -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Izx0_compressor -Izx0_decompressor -Ilz0 -DLZ0_VERSION=\"$(VERSION)\" -static
ARM64_DEC_SRC = lz0/lz0dec_ref.c lz0/lz0dec_min.c lz0/lz0dec_arm64.S lz0/lz0dec_fast_arm64.S

test_arm64: lz0/lz0dec_arm64.S lz0/lz0dec_fast_arm64.S
	@echo "=== Cross-compiling and running ARM64 / AArch64 tests under QEMU ==="
	$(ARM64_CC) $(ARM64_CFLAGS) -o tests/test_vectors_arm64 tests/test_vectors.c $(LZ0_SRC) $(ARM64_DEC_SRC)
	$(ARM64_EMU) ./tests/test_vectors_arm64
	$(ARM64_CC) $(ARM64_CFLAGS) -o tests/test_boundary_arm64 tests/test_boundary.c $(LZ0_SRC) $(ARM64_DEC_SRC)
	$(ARM64_EMU) ./tests/test_boundary_arm64
	$(ARM64_CC) $(ARM64_CFLAGS) -o tests/test_malformed_arm64 tests/test_malformed.c $(LZ0_SRC) $(ARM64_DEC_SRC)
	$(ARM64_EMU) ./tests/test_malformed_arm64
	$(ARM64_CC) $(ARM64_CFLAGS) -o tests/test_fuzz_arm64 tests/test_fuzz.c $(LZ0_SRC) $(ARM64_DEC_SRC)
	$(ARM64_EMU) ./tests/test_fuzz_arm64
	$(ARM64_CC) $(ARM64_CFLAGS) -o tests/test_guard_arm64 tests/test_guard.c $(LZ0_SRC) $(ARM64_DEC_SRC)
	$(ARM64_EMU) ./tests/test_guard_arm64
	$(ARM64_CC) $(ARM64_CFLAGS) -o tests/test_simd_arm64 tests/test_simd.c $(LZ0_SIMD_SRC)
	$(ARM64_EMU) ./tests/test_simd_arm64 50000
	$(ARM64_CC) $(ARM64_CFLAGS) -o tests/test_limits_arm64 tests/test_limits.c $(LZ0_SRC) $(ARM64_DEC_SRC)
	$(ARM64_EMU) ./tests/test_limits_arm64
	@echo "All ARM64 (AArch64) tests passed cleanly under QEMU!"

arm64: test_arm64

RISCV64_CC ?= riscv64-linux-gnu-gcc
RISCV64_EMU ?= qemu-riscv64-static
# Same emulator with the V extension enabled (exercises the RVV encoder tier).
RISCV64_EMU_V ?= $(RISCV64_EMU) -cpu rv64,v=true,vlen=128
RISCV64_CFLAGS = -O3 -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Izx0_compressor -Izx0_decompressor -Ilz0 -DLZ0_VERSION=\"$(VERSION)\" -static
RISCV64_DEC_SRC = lz0/lz0dec_ref.c lz0/lz0dec_min.c lz0/lz0dec_riscv64.S lz0/lz0dec_fast_riscv64.S

test_riscv64: lz0/lz0dec_riscv64.S lz0/lz0dec_fast_riscv64.S
	@echo "=== Cross-compiling and running RISC-V 64 tests under QEMU ==="
	$(RISCV64_CC) $(RISCV64_CFLAGS) -o tests/test_vectors_riscv64 tests/test_vectors.c $(LZ0_SRC) $(RISCV64_DEC_SRC)
	$(RISCV64_EMU) ./tests/test_vectors_riscv64
	$(RISCV64_CC) $(RISCV64_CFLAGS) -o tests/test_boundary_riscv64 tests/test_boundary.c $(LZ0_SRC) $(RISCV64_DEC_SRC)
	$(RISCV64_EMU) ./tests/test_boundary_riscv64
	$(RISCV64_CC) $(RISCV64_CFLAGS) -o tests/test_malformed_riscv64 tests/test_malformed.c $(LZ0_SRC) $(RISCV64_DEC_SRC)
	$(RISCV64_EMU) ./tests/test_malformed_riscv64
	$(RISCV64_CC) $(RISCV64_CFLAGS) -o tests/test_fuzz_riscv64 tests/test_fuzz.c $(LZ0_SRC) $(RISCV64_DEC_SRC)
	$(RISCV64_EMU) ./tests/test_fuzz_riscv64
	$(RISCV64_CC) $(RISCV64_CFLAGS) -o tests/test_guard_riscv64 tests/test_guard.c $(LZ0_SRC) $(RISCV64_DEC_SRC)
	$(RISCV64_EMU) ./tests/test_guard_riscv64
	$(RISCV64_CC) $(RISCV64_CFLAGS) -o tests/test_simd_riscv64 tests/test_simd.c $(LZ0_SIMD_SRC)
	$(RISCV64_EMU) ./tests/test_simd_riscv64 20000
	$(RISCV64_EMU_V) ./tests/test_simd_riscv64 20000
	$(RISCV64_EMU_V) ./tests/test_vectors_riscv64
	$(RISCV64_EMU_V) ./tests/test_fuzz_riscv64
	$(RISCV64_CC) $(RISCV64_CFLAGS) -o tests/test_limits_riscv64 tests/test_limits.c $(LZ0_SRC) $(RISCV64_DEC_SRC)
	$(RISCV64_EMU) ./tests/test_limits_riscv64
	@echo "All RISC-V 64 tests passed cleanly under QEMU!"

riscv64: test_riscv64

RUN_WIN ?= WINEDEBUG=-all wine
WIN_DEC_SRC = lz0/lz0dec_ref.c lz0/lz0dec_min.c lz0/lz0dec_x86_64.S lz0/lz0dec_fast_x86_64.S

test_windows: windows
	@echo "=== Cross-compiling and running Win64 tests ==="
	$(MINGW_CC) $(CFLAGS) -static -o tests/test_vectors.exe tests/test_vectors.c $(LZ0_SRC) $(WIN_DEC_SRC)
	$(RUN_WIN) ./tests/test_vectors.exe
	$(MINGW_CC) $(CFLAGS) -static -o tests/test_boundary.exe tests/test_boundary.c $(LZ0_SRC) $(WIN_DEC_SRC)
	$(RUN_WIN) ./tests/test_boundary.exe
	$(MINGW_CC) $(CFLAGS) -static -o tests/test_malformed.exe tests/test_malformed.c $(LZ0_SRC) $(WIN_DEC_SRC)
	$(RUN_WIN) ./tests/test_malformed.exe
	$(MINGW_CC) $(CFLAGS) -static -o tests/test_fuzz.exe tests/test_fuzz.c $(LZ0_SRC) $(WIN_DEC_SRC)
	$(RUN_WIN) ./tests/test_fuzz.exe
	$(MINGW_CC) $(CFLAGS) -static -o tests/test_simd.exe tests/test_simd.c $(LZ0_SIMD_SRC)
	$(RUN_WIN) ./tests/test_simd.exe 50000
	$(MINGW_CC) $(CFLAGS) -static -o tests/test_limits.exe tests/test_limits.c $(LZ0_SRC) $(WIN_DEC_SRC)
	$(RUN_WIN) ./tests/test_limits.exe
	$(MINGW_CC) $(CFLAGS) -static -o tests/test_window.exe tests/test_window.c $(LZ0_SRC) $(WIN_DEC_SRC)
	$(RUN_WIN) ./tests/test_window.exe
	$(MINGW_CC) $(CFLAGS) -pthread -static -o tests/test_threadsafe.exe tests/test_threadsafe.c $(LZ0_SRC) $(WIN_DEC_SRC)
	$(RUN_WIN) ./tests/test_threadsafe.exe
	$(RUN_WIN) ./bin/lz0.exe c tests/vectors/literals.bin build_win_cli.lz0
	$(RUN_WIN) ./bin/lz0dec.exe -e fast build_win_cli.lz0 build_win_cli.out
	cmp tests/vectors/literals.bin build_win_cli.out
	rm -f build_win_cli.lz0 build_win_cli.out
	@echo "All Win64 tests passed cleanly!"

clean:
	rm -f bench/lz0_* bench/dump_rep bench/debug_a3 bench/count_bench bench/scatter_bench
	rm -f bin/lz0* tests/test_vectors tests/test_boundary tests/test_malformed tests/test_fuzz tests/test_guard tests/test_simd tests/test_sa_probe tests/test_threadsafe tests/test_window tests/test_limits tests/*_arm64 tests/*_riscv64 tests/*.exe
	rm -rf dist build
