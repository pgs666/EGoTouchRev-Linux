CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Werror

.PHONY: all clean test

all: hx-algo-test

hx-algo-test: hx-algo-test.c ../touchscreen-hx83121a-dkms/hx-algo.c \
		../touchscreen-hx83121a-dkms/hx-algo.h host-compat.h
	$(CC) $(CFLAGS) -DHX_ALGO_HOST_TEST \
		-I../touchscreen-hx83121a-dkms \
		../touchscreen-hx83121a-dkms/hx-algo.c hx-algo-test.c -o $@

test: hx-algo-test
	./hx-algo-test

clean:
	rm -f hx-algo-test
