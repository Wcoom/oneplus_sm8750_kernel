obj-$(CONFIG_REKERNEL_X) += rekernel_x.o

rekernel_x-y := rkx.o rkx_genl.o rkx_netuid.o rkx_free_async.o \
                rkx_frozen.o rkx_signal.o rkx_netfilter.o \
                rkx_binder_kp.o rkx_binder_alloc.o rkx_binder.o

ifdef RKX_VERSION
ccflags-y += -DRKX_VERSION=\"$(RKX_VERSION)\"
endif
