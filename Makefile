obj-m := stream_ctrl.o

KDIR := $(HOME)/workspace/kernel-driver/linux-xlnx-xlnx_rebase_v5.4_2020.2

.PHONY: all clean

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=arm modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=arm clean
