SHELL := /bin/bash

KERN_DIR :=/home/zy/workspace/kernel-driver/linux-xlnx-xlnx_rebase_v5.4_2020.2
PETALINUX_ENV := /opt/petalinux/2020.2/environment-setup-cortexa9t2hf-neon-xilinx-linux-gnueabi

ARCH := arm
MODULE ?= stream_ctrl

obj-m := $(MODULE).o

all:
	source $(PETALINUX_ENV) && \
	$(MAKE) -C $(KERN_DIR) ARCH=$(ARCH) CROSS_COMPILE=$${TARGET_PREFIX:-arm-xilinx-linux-gnueabi-} M=$(CURDIR) modules

clean:
	source $(PETALINUX_ENV) && \
	$(MAKE) -C $(KERN_DIR) ARCH=$(ARCH) CROSS_COMPILE=$${TARGET_PREFIX:-arm-xilinx-linux-gnueabi-} M=$(CURDIR) clean