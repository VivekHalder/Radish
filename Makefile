# redis-mc build
#   make        optimized build      -> build/redis-mc
#   make asan   ASan + UBSan build   -> build-asan/redis-mc
#   make test   differential tests vs redis-server (6379 = reference, 6380 = ours)
#   make clean

CC      ?= cc
SRCS    := $(wildcard src/*.c)

# Strict C11 hides POSIX/Linux APIs (epoll, SO_REUSEPORT, ...); _GNU_SOURCE exposes them.
CFLAGS_COMMON := -std=c11 -Wall -Wextra -Werror -D_GNU_SOURCE -pthread -MMD -MP
LDFLAGS_COMMON := -pthread

CFLAGS_REL  := $(CFLAGS_COMMON) -O2 -g
CFLAGS_ASAN := $(CFLAGS_COMMON) -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all
LDFLAGS_ASAN := $(LDFLAGS_COMMON) -fsanitize=address,undefined

OBJS_REL  := $(SRCS:src/%.c=build/%.o)
OBJS_ASAN := $(SRCS:src/%.c=build-asan/%.o)

.PHONY: all asan test clean

all: build/redis-mc

asan: build-asan/redis-mc

build/redis-mc: $(OBJS_REL)
	$(CC) $(LDFLAGS_COMMON) -o $@ $^

build-asan/redis-mc: $(OBJS_ASAN)
	$(CC) $(LDFLAGS_ASAN) -o $@ $^

build/%.o: src/%.c | build
	$(CC) $(CFLAGS_REL) -c -o $@ $<

build-asan/%.o: src/%.c | build-asan
	$(CC) $(CFLAGS_ASAN) -c -o $@ $<

build build-asan:
	mkdir -p $@

# Expects redis-server on 6379 and redis-mc on 6380 to be running.
test: all
	python3 tests/diff_test.py

clean:
	rm -rf build build-asan

-include $(OBJS_REL:.o=.d) $(OBJS_ASAN:.o=.d)
