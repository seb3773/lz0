CC = gcc
CFLAGS = -O3 -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wno-unused-parameter
CFLAGS += -Izx0_compressor -Izx0_decompressor -Ilz0

ZX0_SRC = zx0_compressor/compress.c zx0_compressor/optimize.c \
          zx0_compressor/memory.c zx0_compressor/zx0_compress_wrapper.c \
          zx0_decompressor/zx0_decompress.c

LZ0_SRC = lz0/lz0_cost.c lz0/lz0_find.c lz0/lz0_parse.c \
          lz0/lz0_emit.c lz0/lz0_decompress.c lz0/lz0_compress.c

LZ0DEC_SRC = lz0/lz0dec_ref.c lz0/lz0dec_min.c lz0/lz0dec_x86_64.s

VERSION ?= 0.1.0

.PHONY: all bench clean test smoke bounds diag lite32 state sdp32 scale prof cmpfind jumpsweep pruneab bits final gap saab streamab freeze vectors boundary malformed fuzz lz0 lz0dec cli sanitize valgrind bench_all bench_dec bench_dec_arm64 test_arm64 arm64 windows dist dist_linux dist_windows

all: bench/lz0_bench bench/lz0_smoke bench/lz0_bounds bench/lz0_diag_rep bench/lz0_lite32 bench/lz0_diag_state bench/lz0_sdp32 bench/lz0_scale bench/lz0_prof bench/lz0_cmpfind bench/lz0_jumpsweep bench/lz0_pruneab bench/lz0_bits bench/lz0_final

bench/lz0_bench: bench/bench.c $(ZX0_SRC) $(LZ0_SRC)
	$(CC) $(CFLAGS) -o $@ bench/bench.c $(ZX0_SRC) $(LZ0_SRC)

bench/lz0_bench_all: bench/bench_all.c $(ZX0_SRC) $(LZ0_SRC) $(LZ0DEC_SRC)
	$(CC) $(CFLAGS) -o $@ bench/bench_all.c $(ZX0_SRC) $(LZ0_SRC) $(LZ0DEC_SRC) -llz4

bench_all: bench/lz0_bench_all
	./bench/lz0_bench_all

bench/lz0_bench_dec: bench/bench_dec.c $(LZ0_SRC) $(LZ0DEC_SRC)
	$(CC) $(CFLAGS) -o $@ bench/bench_dec.c $(LZ0_SRC) $(LZ0DEC_SRC)

bench_dec: bench/lz0_bench_dec
	./bench/lz0_bench_dec

bench_dec_arm64: bench/bench_dec.c $(LZ0_SRC) $(ARM64_DEC_SRC)
	$(ARM64_CC) $(ARM64_CFLAGS) -o bench/lz0_bench_dec_arm64 bench/bench_dec.c $(LZ0_SRC) $(ARM64_DEC_SRC)
	$(ARM64_EMU) ./bench/lz0_bench_dec_arm64

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

bench/lz0_lite32: bench/lite32.c $(ZX0_SRC) $(LZ0_SRC)
	$(CC) $(CFLAGS) -o $@ bench/lite32.c $(ZX0_SRC) $(LZ0_SRC)

lite32: bench/lz0_lite32
	./bench/lz0_lite32

bench/lz0_diag_state: bench/diag_state.c $(LZ0_SRC)
	$(CC) $(CFLAGS) -o $@ bench/diag_state.c $(LZ0_SRC)

state: bench/lz0_diag_state
	./bench/lz0_diag_state

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

bin/lz0: lz0/lz0_cli.c $(LZ0_SRC)
	mkdir -p bin
	$(CC) $(CFLAGS) -o $@ lz0/lz0_cli.c $(LZ0_SRC)

bin/lz0dec: lz0/lz0dec_cli.c lz0/lz0dec_min.c
	mkdir -p bin
	$(CC) $(CFLAGS) -o $@ lz0/lz0dec_cli.c lz0/lz0dec_min.c

lz0: bin/lz0 bin/lz0dec

lz0dec: bin/lz0dec

MINGW_CC = x86_64-w64-mingw32-gcc

bin/lz0.exe: lz0/lz0_cli.c $(LZ0_SRC)
	mkdir -p bin
	$(MINGW_CC) $(CFLAGS) -static -s -o $@ lz0/lz0_cli.c $(LZ0_SRC)

bin/lz0dec.exe: lz0/lz0dec_cli.c lz0/lz0dec_min.c
	mkdir -p bin
	$(MINGW_CC) $(CFLAGS) -static -s -o $@ lz0/lz0dec_cli.c lz0/lz0dec_min.c

windows: bin/lz0.exe bin/lz0dec.exe

dist_linux: bin/lz0 bin/lz0dec
	mkdir -p build/dist/lz0-v$(VERSION)-linux-x86_64 dist
	cp bin/lz0 bin/lz0dec README.md LICENSE build/dist/lz0-v$(VERSION)-linux-x86_64/
	cp -r docs build/dist/lz0-v$(VERSION)-linux-x86_64/
	tar -czvf dist/lz0-v$(VERSION)-linux-x86_64.tar.gz -C build/dist lz0-v$(VERSION)-linux-x86_64
	rm -rf build/dist/lz0-v$(VERSION)-linux-x86_64

dist_windows: windows
	mkdir -p build/dist/lz0-v$(VERSION)-windows-x86_64 dist
	cp bin/lz0.exe bin/lz0dec.exe README.md LICENSE build/dist/lz0-v$(VERSION)-windows-x86_64/
	cp -r docs build/dist/lz0-v$(VERSION)-windows-x86_64/
	cd build/dist && zip -r ../../dist/lz0-v$(VERSION)-windows-x86_64.zip lz0-v$(VERSION)-windows-x86_64
	rm -rf build/dist/lz0-v$(VERSION)-windows-x86_64

dist: dist_linux dist_windows

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

tests/test_fuzz: tests/test_fuzz.c $(LZ0_SRC) $(LZ0DEC_SRC)
	$(CC) $(CFLAGS) -o $@ tests/test_fuzz.c $(LZ0_SRC) $(LZ0DEC_SRC)

fuzz: tests/test_fuzz
	./tests/test_fuzz

SANITIZE_CFLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer -g -O2 -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wno-unused-parameter -Izx0_compressor -Izx0_decompressor -Ilz0

sanitize:
	$(CC) $(SANITIZE_CFLAGS) -o tests/test_vectors tests/test_vectors.c $(LZ0_SRC) $(LZ0DEC_SRC)
	./tests/test_vectors
	$(CC) $(SANITIZE_CFLAGS) -o tests/test_boundary tests/test_boundary.c $(LZ0_SRC) $(LZ0DEC_SRC)
	./tests/test_boundary
	$(CC) $(SANITIZE_CFLAGS) -o tests/test_malformed tests/test_malformed.c $(LZ0_SRC) $(LZ0DEC_SRC)
	./tests/test_malformed
	$(CC) $(SANITIZE_CFLAGS) -o tests/test_fuzz tests/test_fuzz.c $(LZ0_SRC) $(LZ0DEC_SRC)
	./tests/test_fuzz
	@echo "All tests passed cleanly under ASan + UBSan."

valgrind: bin/lz0
	valgrind --leak-check=full --error-exitcode=1 ./bin/lz0 c README.md /tmp/lz0_vg.lz0
	valgrind --leak-check=full --error-exitcode=1 ./bin/lz0 d /tmp/lz0_vg.lz0 /tmp/lz0_vg.out
	cmp README.md /tmp/lz0_vg.out
	rm -f /tmp/lz0_vg.lz0 /tmp/lz0_vg.out
	@echo "Valgrind: zero leaks detected."

test: smoke bounds bench/lz0_bench vectors boundary malformed
	./bench/lz0_bench --quick

ARM64_CC = aarch64-linux-gnu-gcc
ARM64_EMU = qemu-aarch64-static
ARM64_CFLAGS = -O3 -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Izx0_compressor -Izx0_decompressor -Ilz0 -static
ARM64_DEC_SRC = lz0/lz0dec_ref.c lz0/lz0dec_min.c lz0/lz0dec_arm64.s

test_arm64: lz0/lz0dec_arm64.s
	@echo "=== Cross-compiling and running ARM64 / AArch64 tests under QEMU ==="
	$(ARM64_CC) $(ARM64_CFLAGS) -o tests/test_vectors_arm64 tests/test_vectors.c $(LZ0_SRC) $(ARM64_DEC_SRC)
	$(ARM64_EMU) ./tests/test_vectors_arm64
	$(ARM64_CC) $(ARM64_CFLAGS) -o tests/test_boundary_arm64 tests/test_boundary.c $(LZ0_SRC) $(ARM64_DEC_SRC)
	$(ARM64_EMU) ./tests/test_boundary_arm64
	$(ARM64_CC) $(ARM64_CFLAGS) -o tests/test_malformed_arm64 tests/test_malformed.c $(LZ0_SRC) $(ARM64_DEC_SRC)
	$(ARM64_EMU) ./tests/test_malformed_arm64
	$(ARM64_CC) $(ARM64_CFLAGS) -o tests/test_fuzz_arm64 tests/test_fuzz.c $(LZ0_SRC) $(ARM64_DEC_SRC)
	$(ARM64_EMU) ./tests/test_fuzz_arm64
	@echo "All ARM64 (AArch64) tests passed cleanly under QEMU!"

arm64: test_arm64

clean:
	rm -f bench/lz0_* bench/dump_rep bench/debug_a3 bench/count_bench bench/scatter_bench
	rm -f bin/lz0* tests/test_vectors tests/test_boundary tests/test_malformed tests/test_fuzz tests/*_arm64
	rm -rf dist build
