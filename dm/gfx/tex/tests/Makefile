# Texture sub-module unit tests (seconds): make run
CFLAGS = -O2 -g -Wall -Wextra
t_tex: t_tex.c ../nvn_deswizzle.c ../nvn_deswizzle.h ../../nvn/nvn_tex.c ../../nvn/nvn_tex.h
	clang $(CFLAGS) -o $@ t_tex.c ../nvn_deswizzle.c ../../nvn/nvn_tex.c -lm
t_fmt_mtl: t_fmt_mtl.m ../nvn_fmt_mtl.c ../nvn_fmt_mtl.h
	clang $(CFLAGS) -fobjc-arc -o $@ t_fmt_mtl.m ../nvn_fmt_mtl.c -framework Metal -framework Foundation
t_bc2astc: t_bc2astc.c ../nvn_bc2astc.c ../nvn_bc2astc.h ../bc2astc_core.h ../../../third_party/bcdec/bcdec.h
	clang $(CFLAGS) -o $@ t_bc2astc.c ../nvn_bc2astc.c -lm
run: t_tex t_fmt_mtl
	./t_fmt_mtl && ./t_tex
# Needs the astcenc binary (ARM-software/astc-encoder): make ASTCENC=/path/to/astcenc astc
astc: t_bc2astc
	ASTCENC=$(ASTCENC) ./t_bc2astc
.PHONY: run astc
