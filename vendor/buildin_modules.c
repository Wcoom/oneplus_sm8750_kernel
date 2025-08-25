#define pr_fmt(fmt) "build_modules: %s: " fmt, __func__

#include <linux/string.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/mutex.h>

struct module_entry {
	char name[PAGE_SIZE];
	struct list_head list;
};

static LIST_HEAD(module_list);
static DEFINE_MUTEX(module_list_lock);

int buildin_modules_add(const char *name)
{
	struct module_entry *entry;
	struct list_head *pos;

	mutex_lock(&module_list_lock);

	list_for_each(pos, &module_list) {
		entry = list_entry(pos, struct module_entry, list);
		if (strcmp(entry->name, name) == 0) {
			mutex_unlock(&module_list_lock);
			pr_err("Module \"%s\" has been added\n", name);
			return -EEXIST;
		}
	}

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry) {
		mutex_unlock(&module_list_lock);
		pr_err("Failed to alloc memory");
		return -ENOMEM;
	}

	strncpy(entry->name, name, sizeof(entry->name) - 1);
	entry->name[sizeof(entry->name) - 1] = '\0';
	INIT_LIST_HEAD(&entry->list);
	list_add(&entry->list, &module_list);

	pr_info("Added buildin module \"%s\"\n", name);
	mutex_unlock(&module_list_lock);

	return 0;
}

bool is_modules_buildin(const char *name)
{
	struct module_entry *entry;
	struct list_head *pos;
	bool found = false;

	mutex_lock(&module_list_lock);

	list_for_each(pos, &module_list) {
		entry = list_entry(pos, struct module_entry, list);
		if (strcmp(entry->name, name) == 0) {
			found = true;
			break;
		}
	}

	mutex_unlock(&module_list_lock);
	return found;
}
