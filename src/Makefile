# SPDX-License-Identifier: GPL-2.0-or-later
FG_VERSION ?= 0.1.0
CFLAGS ?= -O2 -g
override CFLAGS += -std=c11 -Wall -Wextra
override CPPFLAGS += -D_GNU_SOURCE -DFG_VERSION=\"$(FG_VERSION)\"
LDLIBS = -lubus -lubox -luci -lmnl

OBJS = main.o ctrl.o proto.o config.o uciconf.o linux.o netifd.o log.o

all: fritz-guestd

fritz-guestd: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(OBJS): $(wildcard *.h)

install: fritz-guestd
	install -D -m 0755 fritz-guestd $(DESTDIR)/usr/sbin/fritz-guestd

clean:
	rm -f $(OBJS) fritz-guestd

.PHONY: all install clean
