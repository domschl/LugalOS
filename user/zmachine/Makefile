# Makefile — host development build for the LugalOS Z-machine
#
# The interpreter core (zmem.c, zvm.c, ...) is freestanding C; only
# host_main.c touches libc. Sanitizers are on by default to match the
# repo's host-harness conventions (tests/runner.py).
#
#   make -f Makefile            build zork-host
#   make -f Makefile run        build and run with z-data/zork1.z3
#                               (fetched automatically via fetch-games.sh)
#   make -f Makefile clean

CC      ?= cc
CFLAGS  ?= -std=c17 -Wall -Wextra -Werror -O2 -g -fsanitize=address,undefined
LDFLAGS ?= -fsanitize=address,undefined

CORE_SRCS = zmem.c zvm.c ztext.c zparse.c zsave.c
HOST_SRCS = host_main.c
HDRS      = zmem.h zvm.h ztext.h zparse.h

TARGET = zork-host

all: $(TARGET)

$(TARGET): $(CORE_SRCS) $(HOST_SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(CORE_SRCS) $(HOST_SRCS) $(LDFLAGS)

# Play the MIT-built Zork I (games/zork1/zork1-mit.z3, committed).
run-mit: $(TARGET)
	./$(TARGET) games/zork1/zork1-mit.z3

# Play with the retail r119 oracle binary (fetched into z-data/, test-only).
run: $(TARGET) z-data/zork1.z3
	./$(TARGET) z-data/zork1.z3

z-data/zork1.z3:
	./fetch-games.sh zork1-oracle

clean:
	rm -f $(TARGET)

.PHONY: all run clean
