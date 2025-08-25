#include <linux/module.h>

#define BUILDIN_MODNAME "oplus_bsp_hybridswap_zram"

int buildin_modules_add(const char *name);

static int __init buildin_init(void)
{
	return buildin_modules_add(BUILDIN_MODNAME);
}

module_init(buildin_init);
