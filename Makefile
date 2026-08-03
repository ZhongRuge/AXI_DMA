SHELL := /bin/bash

PETALINUX_PROJECT := $(HOME)/petalinux/ALIENTEK-ZYNQ-driver
PETALINUX_SETTINGS := /opt/pkg/petalinux/2020.2/settings.sh
RECIPE_FILES := $(PETALINUX_PROJECT)/project-spec/meta-user/recipes-modules/stream-ctrl/files

.PHONY: all clean

all:
	@test -f stream_ctrl.c
	@test -f stream_ctrl.h
	@cp -f stream_ctrl.c stream_ctrl.h $(RECIPE_FILES)/
	@source $(PETALINUX_SETTINGS) && \
	cd $(PETALINUX_PROJECT) && \
	petalinux-build -c stream-ctrl
	@ko=$$(find $(PETALINUX_PROJECT)/build/tmp/sysroots-components \
		-type f -name stream_ctrl.ko | head -n 1); \
	test -n "$$ko" || { echo "ERROR: stream_ctrl.ko not found"; exit 1; }; \
	cp -f "$$ko" ./stream_ctrl.ko; \
	echo "OUTPUT: $(CURDIR)/stream_ctrl.ko"; \
	strings ./stream_ctrl.ko | grep '^vermagic='

clean:
	@source $(PETALINUX_SETTINGS) && \
	cd $(PETALINUX_PROJECT) && \
	petalinux-build -c stream-ctrl -x cleansstate
	@rm -f stream_ctrl.ko