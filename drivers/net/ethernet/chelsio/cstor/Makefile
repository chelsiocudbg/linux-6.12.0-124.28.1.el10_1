# SPDX-License-Identifier: GPL-2.0-only
ccflags-y := -I $(srctree)/drivers/net/ethernet/chelsio/cxgb4

obj-$(CONFIG_CHELSIO_CSTOR) += cstor.o

cstor-y :=  cstor_main.o cstor_ioctl.o cstor_cm.o cstor_mem.o cstor_cq.o \
	    cstor_qp.o cstor_srq.o cstor_rxq.o cstor_resource.o cstor_ev.o \
	    cstor_debugfs.o cstor_ucontext.o umem.o
