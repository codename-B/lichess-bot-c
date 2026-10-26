# Bundled tablebase libraries retain their upstream licenses.
# Apply CFLAGS/LDFLAGS without the project's warning flags.
TB_CPPFLAGS = -Ivendor/fathom
GTB_CPPFLAGS = -D_POSIX_C_SOURCE=200809L -DZ_PREFIX -Dtb_init=gtb_init \
 -Ivendor/gaviota -Ivendor/gaviota/sysport -Ivendor/gaviota/compression \
 -Ivendor/gaviota/compression/huffman -Ivendor/gaviota/compression/liblzf \
 -Ivendor/gaviota/compression/zlib -Ivendor/gaviota/compression/lzma
GTB_NAMES = gtb-probe.c gtb-dec.c gtb-att.c sysport/sysport.c \
 compression/wrap.c compression/huffman/hzip.c \
 compression/lzma/LzmaEnc.c compression/lzma/LzmaDec.c \
 compression/lzma/Alloc.c compression/lzma/LzFind.c \
 compression/lzma/Lzma86Enc.c compression/lzma/Lzma86Dec.c compression/lzma/Bra86.c \
 compression/zlib/zcompress.c compression/zlib/uncompr.c compression/zlib/inflate.c \
 compression/zlib/deflate.c compression/zlib/adler32.c compression/zlib/crc32.c \
 compression/zlib/infback.c compression/zlib/inffast.c compression/zlib/inftrees.c \
 compression/zlib/trees.c compression/zlib/zutil.c \
 compression/liblzf/lzf_c.c compression/liblzf/lzf_d.c
TB_BUILD_DIR ?= build/vendor
TB_GTB_OBJECTS = $(addprefix $(TB_BUILD_DIR)/gaviota/,$(GTB_NAMES:.c=.o))
TABLEBASE_OBJECTS = $(TB_BUILD_DIR)/fathom/tbprobe.o $(TB_GTB_OBJECTS)
TABLEBASE_LIBS = -lpthread -lm

.PHONY: tablebase-vendors
tablebase-vendors: $(TABLEBASE_OBJECTS)

$(TB_BUILD_DIR)/fathom/tbprobe.o: vendor/fathom/tbprobe.c vendor/fathom/tbprobe.h vendor/fathom/tbchess.c vendor/fathom/tbconfig.h vendor/fathom/stdendian.h vendor/tablebases.mk
	mkdir -p $(@D)
	$(CC) $(CFLAGS) -std=c11 -fPIC -D_POSIX_C_SOURCE=200809L $(TB_CPPFLAGS) -c $< -o $@

$(TB_BUILD_DIR)/gaviota/%.o: vendor/gaviota/%.c $(wildcard vendor/gaviota/*.h) $(wildcard vendor/gaviota/compression/*/*.h) vendor/gaviota/compression/wrap.h vendor/gaviota/sysport/sysport.h vendor/tablebases.mk
	mkdir -p $(@D)
	# The bundled old zconf.h only prefixes part of zlib's public API. Hidden
	# vendor definitions prevent the rest from interposing libcurl's host zlib.
	$(CC) $(CFLAGS) -std=c11 -fPIC -fvisibility=hidden $(GTB_CPPFLAGS) -c $< -o $@
