CFLAGS=-std=gnu11 -Wall -Os -I/usr/include/libdrm
LDFLAGS=-lturbojpeg -ldrm

console-jpeg : console-jpeg.c
	$(CC) $(CFLAGS) -o console-jpeg console-jpeg.c $(LDFLAGS)

clean :
	rm -f console-jpeg
