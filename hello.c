#include <linux/module.h>
#include <linux/kernel.h>
static int __init h_init(void){ pr_info("HELLO: r510928 module loaded ok\n"); return 0; }
static void __exit h_exit(void){ pr_info("HELLO: unloaded\n"); }
module_init(h_init); module_exit(h_exit);
MODULE_LICENSE("GPL");
