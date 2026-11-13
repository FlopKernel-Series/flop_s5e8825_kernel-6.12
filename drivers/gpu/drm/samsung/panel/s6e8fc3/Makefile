# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the Linux kernel panel device drivers.
#


vendor_src := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))

subdir-ccflags-y += -I$(vendor_src)/../oled_common


subdir-ccflags-y += -I$(vendor_src)/../oled_common

mcd-panel-s6e8fc3_a26x-y += s6e8fc3.o s6e8fc3_a26x.o
obj-$(CONFIG_USDM_PANEL_S6E8FC3) += mcd-panel-s6e8fc3_a26x.o

