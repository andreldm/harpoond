CC=gcc
CFLAGS=-O2 -Wall -Wextra $(shell pkg-config --cflags libusb-1.0)
LDFLAGS=$(shell pkg-config --libs libusb-1.0)
PREFIX=/usr

harpoond: harpoond.c
	$(CC) $(CFLAGS) harpoond.c -o harpoond $(LDFLAGS)

install:
	install -c harpoond $(PREFIX)/bin
	install -c harpoond.service $(PREFIX)/lib/systemd/user
	install -c -m 644 99-harpoond.rules $(PREFIX)/lib/udev/rules.d
	udevadm control --reload-rules
	@echo
	@echo '### Please unplug and plug back the mouse ###'

clean:
	rm -f harpoond

.PHONY: clean install
