#include <linux/module.h>

#define BUILDIN_MODNAME "oplus_bsp_mm_osvelte"
#define BUILDIN_DBG_MODNAME "oplus_bsp_mm_osvelte_dbg"

int buildin_modules_add(const char *name);

static int __init buildin_init(void)
{
	int ret;

#ifdef CONFIG_OPLUS_FEATURE_MM_OSVELTE
	ret = buildin_modules_add(BUILDIN_MODNAME);
	if (ret)
		return ret;
#endif

#ifdef CONFIG_OPLUS_FEATURE_MM_OSVELTE_DBG
	ret = buildin_modules_add(BUILDIN_DBG_MODNAME);
	if (ret)
		return ret;
#endif

	return 0;
}

module_init(buildin_init);
