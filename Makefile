CFLAGS=-std=gnu11 -Wall -I/usr/include/libdrm
LDLIBS=-lm -ldrm -lturbojpeg -lheif -lspng

# Performance flags, all platforms.
CFLAGS += -Os -march=native -DSTBIR_USE_FMA

# Disable HEIF support and remove libheif dependency.
#CFLAGS += -DNO_HEIF_SUPPORT
#LDLIBS := $(filter-out -lheif,$(LDLIBS))

# https://stackoverflow.com/questions/45125516/possible-values-for-uname-m#45125525
# Use sed to gather up arm32 and arm64 synonyms.
ARCH := $(shell uname -m | sed -Ee 's/^(aarch64|armv8).*/aarch64/; s/^arm.*/arm/')

# Detect GCC vs Clang
IS_GCC := $(shell $(CC) -v 2>&1 | sed -Ene '1p;$$p' | fgrep -cm1 gcc)

# See comment at stb_image_resize2.h:92 "SIMD" re. optimization flags.
# -march=native automatically uses SSE, AVX, AVX2, F16C, etc if present on
# the build machine cpu.
ifeq "$(ARCH)" "arm"
	# ARM 32
	CFLAGS += -mfpu=neon-vfpv4
	ifeq "$(IS_GCC)" "1"
		CFLAGS += -mfp16-format=ieee
	endif
else ifeq "$(ARCH)" "aarch64"
	# ARM 64
	# Neon is on by default
else ifeq "$(ARCH)" "x86_64"
	# nothing special needed
else
	# weirdo arch
endif

OBJS=console-jpeg.o stb_impl.o drm_search.o frame_buffer.o util.o \
	read_jpeg.o read_heif.o read_png.o

console-jpeg : $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDLIBS)

%.o : %.c %.h drm_search.h frame_buffer.h util.h
	$(CC) $(CFLAGS) -c $<

stb_impl.o : stb_impl.c stb_image_resize2.h
	$(CC) $(CFLAGS) -Wno-unused-function -c $<

clean :
	rm -f console-jpeg $(OBJS)

rsync :
	rsync -avz "$${USER}@$${SSH_CONNECTION%% *}":console-jpeg/* .
