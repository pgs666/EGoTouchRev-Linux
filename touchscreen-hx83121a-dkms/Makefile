# SPDX-License-Identifier: GPL-2.0
MODNAME = himax-spi
MODVER = 0.1

# Active-pen support is intentionally isolated from the finger path. There is
# no pen parser in this tree yet, so production builds do not compile stylus
# code.
HIMAX_STYLUS ?= 0
HIMAX_V4L2 ?= 1

# Sparse mailbox logging and bounded pre-SPI startup-failure injection only.
# No extra hardware reads or per-frame work. Both default to off at runtime.
HIMAX_AFE_DIAGNOSTICS ?= 0
ifeq ($(HIMAX_AFE_DIAGNOSTICS),1)
ccflags-y += -DCONFIG_TOUCHSCREEN_HIMAX_HX83121A_AFE_DIAGNOSTICS=1
else ifneq ($(HIMAX_AFE_DIAGNOSTICS),0)
$(error HIMAX_AFE_DIAGNOSTICS must be 0 or 1)
endif

# The multi-slot SafeBaseline, held-in-hand reacquire, SHB extension and
# automatic BLRecal paths are still experimental.  They remain enabled by
# default for the current development branch, but can be disabled as one
# coherent behaviour set without removing the basic BLIIR/BLSM pipeline.
HIMAX_EXPERIMENTAL_SAFE_BASELINE ?= 1

obj-m += $(MODNAME).o
$(MODNAME)-y := hx-otp.o hx-afe.o hx-raw-init.o
ifeq ($(HIMAX_V4L2),1)
ccflags-y += -DCONFIG_TOUCHSCREEN_HIMAX_HX83121A_V4L2=1
$(MODNAME)-y += himax-v4l2.o himax-spi-hw.o hx-ahb.o
else ifeq ($(HIMAX_V4L2),0)
$(MODNAME)-y += himax-spi-core.o himax-spi-hw.o himax-spi-sysfs.o \
	hx-algo.o hx-algo-preprocess.o hx-algo-detect.o hx-algo-resolve.o \
	hx-algo-track.o hx-baseline-state.o hx-baseline-property.o \
	hx-baseline-observe.o hx-baseline-update.o hx-blreset.o \
	hx-blreset-abnormal.o hx-blreset-state.o hx-blrecal.o hx-blsm-shb.o \
	hx-safe-baseline.o hx-safe-baseline-signal.o hx-safe-baseline-stats.o \
	hx-safe-baseline-judge.o hx-safe-baseline-queue.o

$(MODNAME)-y += hx-panel-profile.o

# Full-rate diagnostic capture is deliberately excluded from normal builds.
# Build an instrumented module with: make HIMAX_DIAGNOSTICS=1
HIMAX_DIAGNOSTICS ?= 0
ifeq ($(HIMAX_DIAGNOSTICS),1)
ccflags-y += -DCONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS=1
endif

ifeq ($(HIMAX_EXPERIMENTAL_SAFE_BASELINE),1)
ccflags-y += -DCONFIG_TOUCHSCREEN_HIMAX_HX83121A_EXPERIMENTAL_SAFE_BASELINE=1
else ifneq ($(HIMAX_EXPERIMENTAL_SAFE_BASELINE),0)
$(error HIMAX_EXPERIMENTAL_SAFE_BASELINE must be 0 or 1)
endif

ifeq ($(HIMAX_STYLUS),1)
ccflags-y += -DCONFIG_TOUCHSCREEN_HIMAX_HX83121A_STYLUS=1
$(MODNAME)-y += hx-stylus.o
DKMS_SOURCES += hx-stylus.c
else
ccflags-y += -DHIMAX_STYLUS_DISABLED=1
endif
else
$(error HIMAX_V4L2 must be 0 or 1)
endif

KVER ?= $(if $(KERNELRELEASE),$(KERNELRELEASE),$(shell uname -r))
KDIR ?= /lib/modules/$(KVER)/build
MDIR = /usr/src/$(MODNAME)-$(MODVER)
MODPROBE_CONF = /etc/modprobe.d/$(MODNAME)-dkms.conf
DKMS_SOURCES = Makefile dkms.conf himax-spi-core.c himax-spi-hw.c \
	       himax-spi-sysfs.c \
	       himax-spi.h hx-algo.c \
	       hx-algo-preprocess.c hx-algo-detect.c hx-algo-resolve.c \
	       hx-baseline-state.c hx-baseline-property.c hx-baseline-observe.c \
	       hx-baseline-update.c hx-blreset.c hx-blreset-abnormal.c \
	       hx-blreset-state.c hx-blrecal.c hx-blsm-shb.c hx-safe-baseline.c \
	       hx-safe-baseline-signal.c hx-safe-baseline-stats.c \
	       hx-safe-baseline-judge.c hx-safe-baseline-queue.c hx-algo-track.c \
	       hx-algo.h hx-algo-internal.h hx-frame-status.h hx-stylus.h \
	       hx-panel-profile.c hx-panel-profile.h himax-v4l2.c \
	       himax-v4l2.h hx-v4l2-uapi.h hx-raw-policy.h hx-otp.c hx-otp.h \
	       hx-afe.c hx-afe.h hx-raw-init.c hx-raw-init.h hx-ahb.c hx-ahb.h

.PHONY: all clean test deploy-local dkmsinstall dkmsremove print-dkms-sources

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

test:
	$(MAKE) -C ../tests test

deploy-local:
	HIMAX_V4L2=$(HIMAX_V4L2) ../scripts/deploy-local.sh

print-dkms-sources:
	@printf '%s\n' $(DKMS_SOURCES)

dkmsinstall:
	install -d $(MDIR)
	find $(MDIR) -mindepth 1 -delete
	install -m644 $(DKMS_SOURCES) $(MDIR)/
	install -Dm644 $(MODNAME)-dkms.conf $(MODPROBE_CONF)
	dkms install --force -m $(MODNAME) -v $(MODVER)

dkmsremove:
	dkms remove -m $(MODNAME) -v $(MODVER) --all
	rm -f $(MODPROBE_CONF)
	rm -rf /usr/src/$(MODNAME)-$(MODVER)
