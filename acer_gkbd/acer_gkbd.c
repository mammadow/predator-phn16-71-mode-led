// SPDX-License-Identifier: GPL-2.0
/*
 * acer_gkbd - tiny debugfs bridge to the Acer gaming WMI lighting methods
 * (GUID 7A4DDFE7-5B5D-40B4-8595-4408E0CC7F56, ACPI WMBH) on the PHN16-71.
 *
 *   echo "<method> <hex byte> ..." > /sys/kernel/debug/acer_gkbd/call
 *   cat /sys/kernel/debug/acer_gkbd/call      -> result of the last call
 *
 * Only the keyboard/LED lighting methods are allowed: 6, 7, 20, 21.
 */
#include <linux/module.h>
#include <linux/acpi.h>
#include <linux/wmi.h>
#include <linux/debugfs.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>

#define GAMING_GUID "7A4DDFE7-5B5D-40B4-8595-4408E0CC7F56"

static struct dentry *dir;
static DEFINE_MUTEX(lock);
static char result[512] = "no call yet\n";

static bool allowed(u32 m) { return m == 6 || m == 7 || m == 20 || m == 21; }

static ssize_t call_write(struct file *f, const char __user *ubuf, size_t len, loff_t *off)
{
	char kbuf[256], *p, *tok;
	u8 in[32];
	int n = 0, ret, pos = 0;
	u32 method;
	struct acpi_buffer ib, ob = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *o;
	acpi_status st;

	if (len >= sizeof(kbuf))
		return -EINVAL;
	if (copy_from_user(kbuf, ubuf, len))
		return -EFAULT;
	kbuf[len] = 0;
	p = strim(kbuf);

	tok = strsep(&p, " ");
	if (!tok || kstrtou32(tok, 0, &method) || !allowed(method))
		return -EPERM;
	while ((tok = strsep(&p, " ")) && n < (int)sizeof(in)) {
		u8 b;
		if (!*tok)
			continue;
		ret = kstrtou8(tok, 16, &b);
		if (ret)
			return ret;
		in[n++] = b;
	}
	if (n == 0)
		return -EINVAL;

	ib.length = n;
	ib.pointer = in;

	mutex_lock(&lock);
	st = wmi_evaluate_method(GAMING_GUID, 0, method, &ib, &ob);
	pos = scnprintf(result, sizeof(result), "method %u in[%d] status 0x%x", method, n, st);
	o = ob.pointer;
	if (ACPI_SUCCESS(st) && o) {
		if (o->type == ACPI_TYPE_BUFFER) {
			int i;
			pos += scnprintf(result + pos, sizeof(result) - pos, " out[%u]:", o->buffer.length);
			for (i = 0; i < o->buffer.length && i < 64; i++)
				pos += scnprintf(result + pos, sizeof(result) - pos, " %02x", o->buffer.pointer[i]);
		} else if (o->type == ACPI_TYPE_INTEGER) {
			pos += scnprintf(result + pos, sizeof(result) - pos, " out int: 0x%llx", o->integer.value);
		} else {
			pos += scnprintf(result + pos, sizeof(result) - pos, " out type %d", o->type);
		}
	}
	scnprintf(result + pos, sizeof(result) - pos, "\n");
	kfree(ob.pointer);
	mutex_unlock(&lock);
	pr_info("acer_gkbd: %s", result);
	return ACPI_SUCCESS(st) ? len : -EIO;
}

static ssize_t call_read(struct file *f, char __user *ubuf, size_t len, loff_t *off)
{
	ssize_t r;
	mutex_lock(&lock);
	r = simple_read_from_buffer(ubuf, len, off, result, strlen(result));
	mutex_unlock(&lock);
	return r;
}

static const struct file_operations call_fops = {
	.owner = THIS_MODULE,
	.read = call_read,
	.write = call_write,
};

static int __init gkbd_init(void)
{
	if (!wmi_has_guid(GAMING_GUID))
		return -ENODEV;
	dir = debugfs_create_dir("acer_gkbd", NULL);
	debugfs_create_file("call", 0600, dir, NULL, &call_fops);
	return 0;
}

static void __exit gkbd_exit(void)
{
	debugfs_remove_recursive(dir);
}

module_init(gkbd_init);
module_exit(gkbd_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("debugfs bridge to Acer gaming WMI lighting methods");
