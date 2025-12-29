#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/ctype.h>
#include <linux/byteorder/generic.h>
#include <linux/fs.h>
#include <linux/usb/ch9.h>


#define MAX_RULES 128
#define MAX_SERIALS 128
#define RULES_FILE "/etc/usbguard.rules"
#define RULE_LINE_MAX 256

struct vidpid {
    u16 vid;
    u16 pid;
};

static struct vidpid *rules;
static size_t rule_count;

static char *blocked_serials[MAX_SERIALS];
static size_t blocked_serial_count;

static DEFINE_MUTEX(rules_lock);

static struct kobject *usbguard_kobj;

/* Trim whitespace */
static char *trim(char *s)
{
    char *end;
    while (isspace(*s)) s++;
    if (*s == 0) return s;
    end = s + strlen(s) - 1;
    while (end > s && isspace(*end)) *end-- = '\0';
    return s;
}

/* Parse VID/PID line */
static int parse_vidpid_line(char *line, u16 *vid, u16 *pid)
{
    char *p = trim(line);
    unsigned long v, pnum;
    int rc;

    if (p[0] == '\0' || p[0] == '#')
        return -EINVAL;

    rc = kstrtoul(p, 16, &v);
    if (rc) return rc;

    p = strchr(p, ' ');
    if (!p) return -EINVAL;

    while (*p && isspace(*p)) p++;

    rc = kstrtoul(p, 16, &pnum);
    if (rc) return rc;

    if (v > 0xFFFF || pnum > 0xFFFF)
        return -ERANGE;

    *vid = (u16)v;
    *pid = (u16)pnum;
    return 0;
}

/* -------------------------------------------
 * Modern Linux 6.x-compatible rule loader
 * ------------------------------------------- */
static int load_rules_from_file(void)
{
    struct file *fp;
    char *buf, *line, *next;
    loff_t pos = 0;
    ssize_t read_len;

    fp = filp_open(RULES_FILE, O_RDONLY, 0);
    if (IS_ERR(fp)) {
        pr_warn("usbguard: could not open %s\n", RULES_FILE);
        return PTR_ERR(fp);
    }

    buf = kzalloc(RULE_LINE_MAX, GFP_KERNEL);
    if (!buf) {
        filp_close(fp, NULL);
        return -ENOMEM;
    }

    while ((read_len = kernel_read(fp, buf, RULE_LINE_MAX - 1, &pos)) > 0) {

        buf[read_len] = '\0';
        line = buf;

        while (line && *line) {

            next = strchr(line, '\n');
            if (next) {
                *next = '\0';
                next++;
            }

            {
                u16 vid, pid;
                if (parse_vidpid_line(line, &vid, &pid) == 0) {
                    mutex_lock(&rules_lock);
                    if (rule_count < MAX_RULES) {
                        rules[rule_count].vid = vid;
                        rules[rule_count].pid = pid;
                        rule_count++;
                        pr_info("usbguard: loaded rule %04x:%04x\n", vid, pid);
                    }
                    mutex_unlock(&rules_lock);
                }
            }

            line = next;
        }
    }

    kfree(buf);
    filp_close(fp, NULL);
    return 0;
}

/* Check if VID/PID matches */
static bool match_rules(struct usb_device *udev)
{
    int i;
    u16 vid = le16_to_cpu(udev->descriptor.idVendor);
    u16 pid = le16_to_cpu(udev->descriptor.idProduct);

    mutex_lock(&rules_lock);
    for (i = 0; i < rule_count; i++) {
        if (rules[i].vid == vid && rules[i].pid == pid) {
            mutex_unlock(&rules_lock);
            return true;
        }
    }
    mutex_unlock(&rules_lock);
    return false;
}

/* Check if serial number is blocked */
static bool serial_blocked(const char *s)
{
    int i;
    if (!s || !*s)
        return false;

    mutex_lock(&rules_lock);
    for (i = 0; i < blocked_serial_count; i++) {
        if (blocked_serials[i] && strcmp(blocked_serials[i], s) == 0) {
            mutex_unlock(&rules_lock);
            return true;
        }
    }
    mutex_unlock(&rules_lock);
    return false;
}

/* Stub class check */
static bool check_interface_classes(struct usb_interface *interface)
{
    return true;
}

/* Probe */
static int usbguard_probe(struct usb_interface *interface, const struct usb_device_id *id)
{
    struct usb_device *udev = interface_to_usbdev(interface);
    char serial[128] = {0};
    int ret;

    pr_info("usbguard: device VID=%04x PID=%04x attached\n",
            le16_to_cpu(udev->descriptor.idVendor),
            le16_to_cpu(udev->descriptor.idProduct));

    if (!match_rules(udev)) {
        pr_alert("usbguard: VID/PID not allowed, rejecting device\n");
        return -EACCES;
    }

    if (!check_interface_classes(interface)) {
        pr_alert("usbguard: class not allowed, rejecting device\n");
        return -EACCES;
    }

    if (udev->descriptor.iSerialNumber) {
        ret = usb_string(udev, udev->descriptor.iSerialNumber, serial, sizeof(serial));
        if (ret > 0 && serial_blocked(serial)) {
            pr_alert("usbguard: blocked serial %s, rejecting\n", serial);
            return -EACCES;
        }
    }

    pr_info("usbguard: accepted\n");
    return 0;
}

/* Disconnect */
static void usbguard_disconnect(struct usb_interface *interface)
{
    pr_info("usbguard: device disconnected\n");
}

/* Match all devices */
/* Match (a) any vendor-specific interface, and (b) mass-storage interfaces */
/* Match ALL USB devices */
static const struct usb_device_id usbguard_table[] = {
    { USB_DEVICE(0xFFFF, 0xFFFF) },   // matches all devices
    { }  // terminator
};
MODULE_DEVICE_TABLE(usb, usbguard_table);

/* USB driver struct */
static struct usb_driver usbguard_driver = {
    .name = "usbguard_demo",
    .probe = usbguard_probe,
    .disconnect = usbguard_disconnect,
    .id_table = usbguard_table,
};

/* Sysfs: show rules */
static ssize_t rules_show(struct kobject *k, struct kobj_attribute *attr, char *buf)
{
    ssize_t len = 0;
    int i;

    mutex_lock(&rules_lock);
    for (i = 0; i < rule_count; i++) {
        len += scnprintf(buf + len, PAGE_SIZE - len,
                         "%04x %04x\n",
                         rules[i].vid, rules[i].pid);
    }
    mutex_unlock(&rules_lock);

    return len;
}

/* Sysfs: store rules */
static ssize_t rules_store(struct kobject *k, struct kobj_attribute *attr,
                           const char *buf, size_t count)
{
    char *tmp = kstrdup(buf, GFP_KERNEL), *line;

    if (!tmp)
        return -ENOMEM;

    line = tmp;
    while (line) {
        char *next = strchr(line, '\n');
        if (next) *next++ = '\0';

        {
            u16 vid, pid;
            if (parse_vidpid_line(trim(line), &vid, &pid) == 0) {
                mutex_lock(&rules_lock);
                if (rule_count < MAX_RULES) {
                    rules[rule_count].vid = vid;
                    rules[rule_count].pid = pid;
                    rule_count++;
                    pr_info("usbguard: sysfs added rule %04x:%04x\n", vid, pid);
                }
                mutex_unlock(&rules_lock);
            }
        }

        line = next;
    }

    kfree(tmp);
    return count;
}

static struct kobj_attribute rules_attr =
    __ATTR(rules, 0664, rules_show, rules_store);

/* Serial block list show/store */
static ssize_t blocked_show(struct kobject *k, struct kobj_attribute *attr, char *buf)
{
    ssize_t len = 0;
    int i;

    mutex_lock(&rules_lock);
    for (i = 0; i < blocked_serial_count; i++) {
        if (blocked_serials[i])
            len += scnprintf(buf + len, PAGE_SIZE - len, "%s\n",
                             blocked_serials[i]);
    }
    mutex_unlock(&rules_lock);

    return len;
}

static ssize_t blocked_store(struct kobject *k, struct kobj_attribute *attr,
                             const char *buf, size_t count)
{
    char *tmp = kstrdup(buf, GFP_KERNEL), *line;

    if (!tmp)
        return -ENOMEM;

    line = tmp;
    while (line) {
        char *next = strchr(line, '\n');
        if (next) *next++ = '\0';

        {
            char *s = trim(line);
            if (*s) {
                mutex_lock(&rules_lock);
                if (blocked_serial_count < MAX_SERIALS)
                    blocked_serials[blocked_serial_count++] =
                        kstrdup(s, GFP_KERNEL);
                mutex_unlock(&rules_lock);
            }
        }

        line = next;
    }

    kfree(tmp);
    return count;
}

static struct kobj_attribute blocked_attr =
    __ATTR(blocked_serials, 0664, blocked_show, blocked_store);

/* Init */
static int __init usbguard_init(void)
{
    int rc;

    rules = kzalloc(sizeof(struct vidpid) * MAX_RULES, GFP_KERNEL);
    if (!rules)
        return -ENOMEM;

    rule_count = 0;
    blocked_serial_count = 0;

    /* Load rules from file (modern version) */
    load_rules_from_file();

    usbguard_kobj = kobject_create_and_add("usbguard", kernel_kobj);
    if (!usbguard_kobj) {
        kfree(rules);
        return -ENOMEM;
    }

    rc = sysfs_create_file(usbguard_kobj, &rules_attr.attr);
    if (rc) goto fail;

    rc = sysfs_create_file(usbguard_kobj, &blocked_attr.attr);
    if (rc) {
        sysfs_remove_file(usbguard_kobj, &rules_attr.attr);
        goto fail;
    }

    rc = usb_register(&usbguard_driver);
    if (rc) {
        pr_alert("usbguard: usb_register failed %d\n", rc);
        goto fail2;
    }

    pr_info("usbguard: module loaded\n");
    return 0;

fail2:
    sysfs_remove_file(usbguard_kobj, &rules_attr.attr);
    sysfs_remove_file(usbguard_kobj, &blocked_attr.attr);

fail:
    kobject_put(usbguard_kobj);
    kfree(rules);
    return rc;
}

/* Exit */
static void __exit usbguard_exit(void)
{
    int i;

    usb_deregister(&usbguard_driver);

    sysfs_remove_file(usbguard_kobj, &rules_attr.attr);
    sysfs_remove_file(usbguard_kobj, &blocked_attr.attr);
    kobject_put(usbguard_kobj);

    mutex_lock(&rules_lock);
    for (i = 0; i < blocked_serial_count; i++)
        kfree(blocked_serials[i]);
    mutex_unlock(&rules_lock);

    kfree(rules);

    pr_info("usbguard: module unloaded\n");
}

module_init(usbguard_init);
module_exit(usbguard_exit);

MODULE_AUTHOR("vijeeth63");
MODULE_DESCRIPTION("USB Access Control Framework - Demo Implementation");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.1");




