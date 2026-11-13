#
# Copyright (c) 2026 Samsung Electronics Co., Ltd.
#		http://www.samsung.com
#
# Licensed under GPLv2
#

vendor_src := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
subdir-ccflags-y := -I$(vendor_src)
scaler-objs := scaler-core.o scaler-regs.o scaler-debug.o scaler-ext.o
scaler-objs += scaler-version.o scaler-format.o
obj-$(CONFIG_VIDEO_EXYNOS_SCALER)	+= scaler.o

