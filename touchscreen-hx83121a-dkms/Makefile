# SPDX-License-Identifier: GPL-2.0
MODNAME = himax-spi
MODVER = 0.1

obj-m += $(MODNAME).o
$(MODNAME)-y := himax-spi-core.o hx-algo.o

KVER = $(shell uname -r)
KDIR = /lib/modules/$(KVER)/build
MDIR = /usr/src/$(MODNAME)-$(MODVER)
MODPROBE_CONF = /etc/modprobe.d/$(MODNAME)-dkms.conf
DKMS_SOURCES = Makefile dkms.conf himax-spi-core.c hx-algo.c hx-algo.h \
	       hx-frame-status.h

.PHONY: all clean test deploy-local dkmsinstall dkmsremove

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

test:
	$(MAKE) -C ../tests test

deploy-local:
	../scripts/deploy-local.sh

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
