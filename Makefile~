# override e.g. `make install PREFIX=/usr`
PREFIX ?= /usr/local

CFLAGS=-Wall `pkg-config --cflags jack` -O3
LIBS=`pkg-config --libs jack` -lpthread -lm
#compat w/ NetBSD and GNU Make
LDADD=${LIBS}
LDLIBS=${LIBS}

all: jack-peak

jack-peak: jack-peak.c

jack-peak.1: jack-peak
	help2man -N -o jack-peak.1 -n "live peak-signal meter for JACK" ./jack-peak

install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 jack-peak $(DESTDIR)$(PREFIX)/bin/
	install -d $(DESTDIR)$(PREFIX)/share/man/man1
	install -m 644 jack-peak.1 $(DESTDIR)$(PREFIX)/share/man/man1/

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/jack-peak
	rm -f $(DESTDIR)$(PREFIX)/share/man/man1/jack-peak.1
	-rmdir $(DESTDIR)$(PREFIX)/bin
	-rmdir $(DESTDIR)$(PREFIX)/share/man/man1

clean:
	/bin/rm -f jack-peak

maintainerclean: clean
	/bin/rm -f jack-peak.1

.PHONY: all install uninstall clean
