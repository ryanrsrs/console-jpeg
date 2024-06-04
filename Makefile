CFLAGS=-std=gnu11 -Wall -I/usr/include/libdrm -march=native -Os -DSTBIR_USE_FMA
LDFLAGS=-lturbojpeg -ldrm -lm

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

console-jpeg : console-jpeg.o stb_impl.o
	$(CC) -o console-jpeg console-jpeg.o stb_impl.o $(LDFLAGS)

console-jpeg.o : console-jpeg.c stb_image_resize2.h
	$(CC) $(CFLAGS) -c console-jpeg.c

stb_impl.o : stb_impl.c stb_image_resize2.h
	$(CC) $(CFLAGS) -Wno-unused-function -c stb_impl.c

clean :
	rm -f console-jpeg console-jpeg.o stb_impl.o
