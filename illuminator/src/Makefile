# SG-IR850-8M illuminator controller — production build (C).
# Builds on the Jetson natively (aarch64 gcc). No external dependencies.

CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -Wpedantic
PREFIX  ?= /usr/local

LIB     := libsgir850.a
BIN     := sgctl

all: $(BIN)

$(LIB): sg_ir850.o
	$(AR) rcs $@ $^

sg_ir850.o: sg_ir850.c sg_ir850.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(BIN): sgctl.c $(LIB)
	$(CC) $(CFLAGS) -o $@ sgctl.c $(LIB)

install: $(BIN)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)

clean:
	rm -f $(BIN) $(LIB) sg_ir850.o

.PHONY: all install clean
