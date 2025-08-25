#
# Makefile for vendor kernel modules.
#

KBUILD_CFLAGS					+= -I$(srctree)/vendor/include

obj-$(CONFIG_VENDOR_KERNEL_MODULES)		+= buildin_modules.o

obj-$(CONFIG_OPLUS_FEATURE_PERFORMANCE_MM)	+= mm/
