CC ?= clang
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
HID_LDFLAGS ?= -framework IOKit -framework CoreFoundation
IMAGE_LDFLAGS ?= -framework IOKit -framework CoreFoundation -framework CoreGraphics -framework ImageIO

all: cidoo-clock cidoo-image

cidoo-clock: cidoo-clock.c
	$(CC) $(CFLAGS) -o $@ $< $(HID_LDFLAGS)

cidoo-image: cidoo-image.c
	$(CC) $(CFLAGS) -o $@ $< $(IMAGE_LDFLAGS)

clean:
	rm -f cidoo-clock cidoo-image

.PHONY: all clean
