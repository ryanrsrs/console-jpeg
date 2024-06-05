CFLAGS=-std=gnu11 -Wall -I/usr/include/libdrm -march=native -Os -DSTBIR_USE_FMA
LDLIBS=-lm -ldrm -lturbojpeg -lheif -lspng

# https://stackoverflow.com/questions/45125516/possible-values-for-uname-m#45125525
# Use sed to gather up arm32 and arm64 synonyms.
ARCH := $(shell uname -m | sed -Ee 's/^(aarch64|armv8).*/aarch64/; s/^arm.*/arm/')

# Detect GCC vs Clang
IS_GCC := $(shell $(CC) -v 2>&1 | sed -Ene '1p;$$p' | fgrep -cm1 gcc)

# See comment at stb_image_resize2.h:92 "SIMD" re. optimization flags.
# -march=native automatically uses SSE, AVX, AVX2, F16C, etc if present.
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

OBJS=console-jpeg.o stb_impl.o frame_buffer.o util.o read_jpeg.o read_heif.o read_png.o

console-jpeg : $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDLIBS)

%.o : %.c %.h frame_buffer.h util.h
	$(CC) $(CFLAGS) -c $<

stb_impl.o : stb_impl.c stb_image_resize2.h
	$(CC) $(CFLAGS) -Wno-unused-function -c $<

clean :
	rm -f console-jpeg $(OBJS)
