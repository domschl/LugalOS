# Host harnesses: kernel modules built for the development machine and run
# under AddressSanitizer + UndefinedBehaviorSanitizer, and under valgrind
# (phase 40 review). See README.md.
#
#   make            build both variants
#   make check      run every harness under ASan/UBSan, then under valgrind
#   make asan       ASan/UBSan only (fast)
#   make valgrind   valgrind only
#
# ITER sets the corrupt-input iterations per harness, SEED the PRNG seed.

ROOT    := ../..
OUT     := $(ROOT)/build/host
CC      ?= gcc
ITER    ?= 2000
SEED    ?= 0x40c0ffee

INC     := -I. -I$(ROOT)/drivers -I$(ROOT)/kernel/include -I$(ROOT)/fs/include -I$(ROOT)/drivers/include \
           -I$(ROOT)/net/include -I$(ROOT)/net -I$(ROOT)/user/chibicc/include -I$(ROOT)/arch/riscv/include -I$(ROOT)
WARN    := -Wall -Wextra -Wno-unused-parameter -Wno-format
BASE    := -std=gnu11 -g -fno-omit-frame-pointer $(INC) $(WARN) -DLUGALOS_HOST_TEST=1
ASAN    := -O1 -fsanitize=address,undefined -fno-sanitize-recover=all
PLAIN   := -O1

HARNESSES := fat32_host chibicc_host p9_host net_host kobj_host uheap_host uprintf_host nvs_host

fat32_host_SRC   := fat32_host.c $(ROOT)/fs/fat32.c
chibicc_host_SRC := chibicc_host.c $(wildcard $(ROOT)/user/chibicc/*.c)
p9_host_SRC      := p9_host.c $(ROOT)/fs/9p.c $(ROOT)/kernel/sha256.c
nvs_host_SRC     := nvs_host.c $(ROOT)/drivers/radio/nvs_ram.c
uprintf_host_SRC := uprintf_host.c $(ROOT)/drivers/radio/uprintf.c
uheap_host_SRC   := uheap_host.c $(ROOT)/kernel/uheap.c
kobj_host_SRC    := kobj_host.c $(ROOT)/kernel/kobj.c
net_host_SRC     := net_host.c $(ROOT)/net/arp.c $(ROOT)/net/ipv4.c $(ROOT)/net/icmp.c \
                    $(ROOT)/net/udp.c $(OUT)/gen/tcp.c

# net/tcp.c orders its buffer writes with RISC-V fences; the host gets the
# equivalent C11 fence, in a copy, so the source itself stays as it is.
$(OUT)/gen/tcp.c: $(ROOT)/net/tcp.c
	@mkdir -p $(dir $@)
	sed 's/__asm__ __volatile__("fence [rw], [rw]" ::: "memory")/__atomic_thread_fence(__ATOMIC_SEQ_CST)/' $< > $@

all: $(addprefix $(OUT)/asan/,$(HARNESSES)) $(addprefix $(OUT)/plain/,$(HARNESSES))

$(OUT)/asan/net_host $(OUT)/plain/net_host: $(OUT)/gen/tcp.c

$(OUT)/asan/%: shim.c shim.h %.c FORCE
	@mkdir -p $(dir $@)
	$(CC) $(BASE) $(ASAN) -o $@ shim.c $($*_SRC)

$(OUT)/plain/%: shim.c shim.h %.c FORCE
	@mkdir -p $(dir $@)
	$(CC) $(BASE) $(PLAIN) -o $@ shim.c $($*_SRC)

asan: $(addprefix $(OUT)/asan/,$(HARNESSES))
	@for h in $(HARNESSES); do \
	    echo "== $$h (ASan/UBSan)"; \
	    ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 $(OUT)/asan/$$h $(ITER) $(SEED) || exit 1; \
	done

# Fewer iterations: valgrind is ~30x slower, and what it adds over ASan is
# uninitialised-memory use, which the first few hundred inputs already reach.
valgrind: $(addprefix $(OUT)/plain/,$(HARNESSES))
	@for h in $(HARNESSES); do \
	    echo "== $$h (valgrind)"; \
	    valgrind -q --error-exitcode=9 --leak-check=full --errors-for-leak-kinds=definite \
	        $(OUT)/plain/$$h $$(( $(ITER) / 10 )) $(SEED) || exit 1; \
	done

check: asan valgrind

FORCE:
.PHONY: all asan valgrind check FORCE
