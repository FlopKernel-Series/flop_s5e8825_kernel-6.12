# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the Linux kernel panel device drivers.
#

# $(srctree)/$(src) does not resolve from an O= build, so derive the include
# directories from this file instead.
vendor_src := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))

subdir-ccflags-$(CONFIG_SEC_KUNIT) += \
	-I$(vendor_src)/.. \
	-Wframe-larger-than=8192 \
	-Wno-unused-variable \
	-Wunused-function \
	-Wno-missing-braces \
	-Wno-implicit-fallthrough \
	-Wformat

subdir-ccflags-y += -I$(vendor_src)/../oled_common

mcd-panel-s6e3fc3_a25x-y += s6e3fc3.o s6e3fc3_a25x.o
obj-$(CONFIG_USDM_PANEL_S6E3FC3) += mcd-panel-s6e3fc3_a25x.o
mcd-panel-s6e3fc3_m34x-y += s6e3fc3.o s6e3fc3_m34x.o
obj-$(CONFIG_USDM_PANEL_S6E3FC3) += mcd-panel-s6e3fc3_m34x.o

