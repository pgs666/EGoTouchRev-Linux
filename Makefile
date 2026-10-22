# SPDX-License-Identifier: GPL-2.0

DRIVER_DIR := touchscreen-hx83121a-dkms
TEST_DIR := tests
CHECKPATCH := /lib/modules/$(shell uname -r)/build/scripts/checkpatch.pl
DRIVER_STYLE_SOURCES := $(DRIVER_DIR)/himax-spi-core.c \
	$(DRIVER_DIR)/himax-spi-hw.c $(DRIVER_DIR)/himax-spi-sysfs.c \
	$(DRIVER_DIR)/himax-spi.h $(DRIVER_DIR)/hx-algo.c \
	$(DRIVER_DIR)/hx-algo-preprocess.c $(DRIVER_DIR)/hx-algo-detect.c \
	$(DRIVER_DIR)/hx-algo-resolve.c $(DRIVER_DIR)/hx-algo-track.c \
	$(DRIVER_DIR)/hx-algo.h \
	$(DRIVER_DIR)/hx-algo-internal.h $(DRIVER_DIR)/hx-frame-status.h \
	$(DRIVER_DIR)/hx-otp.c $(DRIVER_DIR)/hx-otp.h \
	$(DRIVER_DIR)/himax-v4l2.c $(DRIVER_DIR)/himax-v4l2.h \
	$(DRIVER_DIR)/hx-v4l2-uapi.h

.PHONY: all clean driver test userspace check checkpatch

all: test driver userspace

userspace:
	cmake -S userspace -B userspace/build -DCMAKE_BUILD_TYPE=Release
	cmake --build userspace/build -j4
	ctest --test-dir userspace/build --output-on-failure

driver:
	$(MAKE) -C $(DRIVER_DIR) all

test:
	$(MAKE) -C $(TEST_DIR) test

check: checkpatch
	$(MAKE) -C $(TEST_DIR) clean test
	$(MAKE) -C $(DRIVER_DIR) clean all
	$(MAKE) userspace
	git diff HEAD --check

checkpatch:
	@if test -x $(CHECKPATCH); then \
		for source in $(DRIVER_STYLE_SOURCES); do \
			$(CHECKPATCH) --quiet --no-tree --file $$source || exit; \
		done; \
	else \
		echo "checkpatch unavailable; skipping style check"; \
	fi

clean:
	$(MAKE) -C $(TEST_DIR) clean
	$(MAKE) -C $(DRIVER_DIR) clean
