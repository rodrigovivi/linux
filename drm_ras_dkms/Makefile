# SPDX-License-Identifier: GPL-2.0
#
# Out-of-tree build wrapper for the standalone drm-ras backport modules.
#
# By default it builds against the kernel modules tree of the *running* kernel,
# which is what you want on the real 5.15 target (and for DKMS):
#
#     make                 # builds drm_ras.ko, xe_ras.ko, blah.ko
#     make clean
#
# To build against a specific prepared kernel tree instead, override KDIR:
#
#     make KDIR=/path/to/linux            # a modules_prepare'd source/O= tree
#     make KDIR=/lib/modules/5.15.0/build # another installed kernel-devel
#
# Requires the matching kernel-devel/headers to be installed (the directory
# $(KDIR) must exist and be configured, i.e. contain a .config + generated
# headers). If `make` errors with "No rule to make target 'modules'" or a
# missing KDIR, install the kernel-devel package for your target kernel.

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: all clean
