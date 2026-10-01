/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef _RTW88_COMPAT_MODULE_H
#define _RTW88_COMPAT_MODULE_H

/* Module stubs — not needed in kext context */
#define MODULE_AUTHOR(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_LICENSE(x)
#define MODULE_FIRMWARE(x)
#define MODULE_VERSION(x)
#define MODULE_DEVICE_TABLE(type, name)
#define MODULE_PARM_DESC(name, desc)
#define EXPORT_SYMBOL(sym)
#define EXPORT_SYMBOL_GPL(sym)

#define module_param(name, type, perm)
#define module_param_named(name, var, type, perm)
#define module_param_string(name, str, len, perm)
#define module_param_array(name, type, nump, perm)

#define THIS_MODULE ((struct module *)0)

struct module;
struct device_driver { const char *name; };
struct bus_type { const char *name; };

#define module_driver(__driver, __register, __unregister, ...) \
    static int __init __driver##_init(void) \
    { return __register(&(__driver), ##__VA_ARGS__); } \
    static void __exit __driver##_exit(void) \
    { __unregister(&(__driver), ##__VA_ARGS__); }

#define module_pci_driver(__pci_driver) \
    module_driver(__pci_driver, pci_register_driver, pci_unregister_driver)

#define module_usb_driver(__usb_driver) \
    module_driver(__usb_driver, usb_register, usb_deregister)

#define __init
#define __exit
#define __devinit
#define __devexit
#define __devexit_p(f) (f)

#endif /* _RTW88_COMPAT_MODULE_H */
