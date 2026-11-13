# SPDX-License-Identifier: GPL-2.0
ccflags-y := -I$(srctree)/drivers/dma-buf/heaps
vendor_src := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
ccflags-y += -I$(vendor_src)

obj-$(CONFIG_DMABUF_HEAPS_HPA) += hpa_heap.o hpa_trace.o hpa_debug.o
obj-$(CONFIG_DMABUF_HEAPS_HPA) += trace/samsung_trace.o
obj-$(CONFIG_DMABUF_HEAPS_HPA) += utils/generic-folio-pool.o
obj-$(CONFIG_DMABUF_HEAPS_HPA) += secure/ppmp-mapping.o

subdir-ccflags-y += -I$(vendor_src)
