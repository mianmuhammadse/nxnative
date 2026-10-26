# Texture sub-module unit tests (seconds): make run
CFLAGS = -O2 -g -Wall -Wextra
# SAN: extra compiler flags, O: the output folder (with a trailing slash) of a sanitizer or coverage build (make asan and make coverage below set both); both empty otherwise
SAN ?=
O ?=
CLANG = clang $(SAN)
$(O)t_tex: t_tex.c ../nvn_deswizzle.c ../nvn_deswizzle.h ../../nvn/nvn_tex.c ../../nvn/nvn_tex.h
	$(CLANG) $(CFLAGS) -o $@ t_tex.c ../nvn_deswizzle.c ../../nvn/nvn_tex.c -lm
$(O)t_fmt_mtl: t_fmt_mtl.m ../nvn_fmt_mtl.c ../nvn_fmt_mtl.h
	$(CLANG) $(CFLAGS) -fobjc-arc -o $@ t_fmt_mtl.m ../nvn_fmt_mtl.c -framework Metal -framework Foundation
bench_bc2astc_dread: bench_bc2astc_dread.c ../nvn_bc2astc.c ../nvn_bc2astc.h ../bc2astc_core.h ../bc2astc_body.inc ../nvn_deswizzle.c ../../nvn/nvn_tex.c
	clang $(CFLAGS) -Wno-unused -o $@ bench_bc2astc_dread.c ../nvn_bc2astc.c ../nvn_deswizzle.c ../../nvn/nvn_tex.c -lz -lm
# Real-data benchmark (Dread-specific, game data stays outside the repository): make bench ROMFS=<extracted RomFS> ASTCENC=<astcenc> OUT=<report folder>
bench: bench_bc2astc_dread
	mkdir -p $(OUT) && ./bench_bc2astc_dread $(ROMFS) $(ASTCENC) $(OUT)
run: t_tex t_fmt_mtl
	./t_fmt_mtl && ./t_tex
.PHONY: run
# ASan and UBSan (../../../sanitizers/sanitizers.mk): the CPU test t_tex, built into asan/ (t_fmt_mtl creates Metal textures and stays local)
include ../../../sanitizers/sanitizers.mk
.PHONY: asan
asan:
	@$(call san_fresh,asan,$(ASAN_FLAGS))
	$(MAKE) O=asan/ SAN="$(ASAN_FLAGS)" asan/t_tex
	$(ASAN_ENV) ./asan/t_tex
# Line coverage (../../../sanitizers/coverage.mk): t_tex built into cov/ with COV_FLAGS and run; writes cov/coverage.lcov
include ../../../sanitizers/coverage.mk
.PHONY: coverage
coverage:
	@$(call san_fresh,cov,$(COV_FLAGS))
	rm -rf cov/prof
	$(MAKE) O=cov/ SAN="$(COV_FLAGS)" cov/t_tex
	-LLVM_PROFILE_FILE="$(call cov_profile,cov)" ./cov/t_tex
	@$(call cov_export,cov)
t_bc2astc: t_bc2astc.c ../nvn_bc2astc.c ../nvn_bc2astc.h ../bc2astc_core.h ../../../third_party/bcdec/bcdec.h
	clang $(CFLAGS) -o $@ t_bc2astc.c ../nvn_bc2astc.c -lm
# Needs the astcenc binary (ARM-software/astc-encoder): make ASTCENC=/path/to/astcenc astc
astc: t_bc2astc
	ASTCENC=$(ASTCENC) ./t_bc2astc
.PHONY: run astc bench
