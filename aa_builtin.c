// aa_builtin.c —— 编进内核的判定篡改（不再是模块）
// 放到 drivers/misc/aa_builtin.c，并在 drivers/misc/Makefile 里加 obj-y += aa_builtin.o
//
// 行为：
//   1. 开机起一个内核线程，每 2 秒扫一次进程表
//   2. 发现目标进程（comm 匹配）就在它的 mm 里找 libUE4.so 的基址
//   3. 在 base+pc0 / base+pc1 挂硬件执行断点
//   4. 命中时改 pt_regs（pc / 条件标志），浮点用 cpacr_el1 开关（和原版模块同一手法）
//   5. 进程退出自动注销
//
// 参数来源（按优先级）：/data/local/tmp/aa.cfg > 下面的默认值
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/pid.h>
#include <linux/rcupdate.h>
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>
#include <linux/atomic.h>
#include <linux/slab.h>
#include <asm/neon.h>

#define AA_CFG   "/data/local/tmp/aa.cfg"

static char     aa_pkg[64]  = "com.tencent.tmgp.dfm";
static int      aa_on       = 1;
static int      aa_strength = 40;
static unsigned long aa_pc0 = 0x0B81F4F4UL;
static unsigned long aa_pc1 = 0x0B81F47CUL;
static int aa_pass_near = 95;      /* 近距通过率 % */
static int aa_pass_mid  = 88;
static int aa_pass_far  = 78;
static unsigned int aa_near_exp = 63;   /* |s0| 指数 < 63 → 近距 */
static unsigned int aa_mid_exp  = 66;   /* < 66 → 中距，否则远距 */
static u32 aa_seed = 0x9E3779B9;

static struct task_struct *aa_task;
static struct perf_event __percpu *aa_bp0, *aa_bp1;
static unsigned long aa_base;
static int aa_pid;
static int aa_live;
static atomic64_t aa_hits0 = ATOMIC64_INIT(0), aa_hits1 = ATOMIC64_INIT(0);

static const u32 aa_cmp[11] = {0x00000000,0x3f4ed917,0x3f1db22d,0x3ed91687,0x3e6d9168,
                               0x3d23d70a,0x3d088889,0x3cea0ea1,0x3ccccccd,0x3cb60b61,0x3ca3d70a};

/* ---------- 浮点：原版模块用 cpacr_el1 直接开关，这里照抄 ---------- */
static inline u64 aa_fp_on(void)
{
    u64 v;
    asm volatile("mrs %0, cpacr_el1" : "=r"(v));
    asm volatile("msr cpacr_el1, %0" :: "r"(v | 0x300000UL));
    asm volatile("isb");
    return v;
}
static inline void aa_fp_off(u64 v)
{
    asm volatile("msr cpacr_el1, %0" :: "r"(v));
    asm volatile("isb");
}

static inline u32 aa_rand(void)
{
    u32 x = aa_seed;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    aa_seed = x;
    return x;
}

static void aa_handler(struct perf_event *bp, struct perf_sample_data *data,
                       struct pt_regs *regs)
{
    unsigned long pc, a0, a1;
    if (!aa_live || !regs || !user_mode(regs)) return;
    if (current->tgid != aa_pid) return;

    pc = regs->pc;
    a0 = aa_base + aa_pc0;
    a1 = aa_base + aa_pc1;

    if (pc == a0) {
        u64 s = aa_fp_on();
        /* fcmge v1.2s, v0.2s, #0.0 —— 用 .inst 绕过 -mgeneral-regs-only 的汇编检查 */
        asm volatile(".inst 0x2ea0c801" ::: "memory");
        aa_fp_off(s);
        regs->pc = pc + 4;
        atomic64_inc(&aa_hits0);
    } else if (pc == a1) {
        int i = aa_strength / 10;
        int pass;
        u32 bits, s0bits = 0;
        unsigned int exp;
        u64 nzcv, s;

        if (i < 0) i = 0;
        if (i > 10) i = 10;

        /* 先读游戏真实的 s0（不动它），用来判断"远近" */
        s = aa_fp_on();
        {
            register u32 out asm("w2");
            asm volatile(".inst 0x1e260002" : "=r"(out) :: "memory");  /* fmov w2, s0 */
            s0bits = out;
        }
        aa_fp_off(s);

        exp = (s0bits >> 23) & 0xFF;
        if (exp < aa_near_exp)      { pass = aa_pass_near; i = (i + 1 > 10) ? 10 : i + 1; }
        else if (exp < aa_mid_exp)  { pass = aa_pass_mid;  }
        else                        { pass = aa_pass_far;  i = (i - 1 < 0) ? 0 : i - 1; }

        /* 概率通过：没中就完全不干预，让游戏按原样执行 */
        if ((int)(aa_rand() % 100) >= pass) { atomic64_inc(&aa_hits1); return; }

        bits = aa_cmp[i];
        s = aa_fp_on();
        {
            register u32 bin asm("w1") = bits;
            register u64 nz  asm("x1");
            /* fmov s1,w1 ; fcmp s0,s1 ; mrs x1,nzcv */
            asm volatile(".inst 0x1e270021\n\t"
                         ".inst 0x1e212000\n\t"
                         ".inst 0xd53b4201"
                         : "=r"(nz) : "r"(bin) : "cc", "memory");
            nzcv = nz;
        }
        aa_fp_off(s);
        {
            unsigned long ps = regs->pstate;
            ps &= ~(0xFUL << 28);
            ps |= ((nzcv & 0xFUL) << 28);
            regs->pstate = ps;
        }
        regs->pc = pc + 4;
        atomic64_inc(&aa_hits1);
    }
}

static int aa_install(struct perf_event __percpu **out, unsigned long addr)
{
    struct perf_event_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.type = PERF_TYPE_BREAKPOINT;
    attr.size = sizeof(attr);
    attr.bp_type = HW_BREAKPOINT_X;
    attr.bp_addr = addr;
    attr.bp_len = HW_BREAKPOINT_LEN_4;
    attr.sample_period = 1;
    attr.disabled = 0;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    *out = register_wide_hw_breakpoint(&attr, aa_handler, NULL);
    if (IS_ERR(*out)) { int r = PTR_ERR(*out); *out = NULL; return r; }
    return 0;
}

/* ---------- 在目标 mm 里找 libUE4.so 的映射基址 ---------- */
static unsigned long aa_find_base(struct mm_struct *mm)
{
    struct vm_area_struct *v;
    char *buf = kmalloc(PATH_MAX, GFP_KERNEL);
    unsigned long ret = 0;
    if (!buf) return 0;
    VMA_ITERATOR(vmi, mm, 0);
    for_each_vma(vmi, v) {
        if (!v->vm_file) continue;
        if (v->vm_pgoff != 0) continue;
        {
            char *p = d_path(&v->vm_file->f_path, buf, PATH_MAX);
            if (!IS_ERR(p) && strstr(p, "libUE4.so")) { ret = v->vm_start; break; }
        }
    }
    kfree(buf);
    return ret;
}

static void aa_release(void)
{
    aa_live = 0;
    if (aa_bp0) { unregister_wide_hw_breakpoint(aa_bp0); aa_bp0 = NULL; }
    if (aa_bp1) { unregister_wide_hw_breakpoint(aa_bp1); aa_bp1 = NULL; }
    if (aa_task) { put_task_struct(aa_task); aa_task = NULL; }
    aa_pid = 0;
}

static struct task_struct *aa_find_task(void)
{
    struct task_struct *p;
    for_each_process(p) {
        if (!strncmp(p->comm, aa_pkg, sizeof(p->comm))) {
            if (p->mm) return p;
        }
    }
    return NULL;
}

static void aa_read_cfg(void)
{
    struct file *f;
    char *buf;
    loff_t pos = 0;
    f = filp_open(AA_CFG, O_RDONLY, 0);
    if (IS_ERR(f)) return;
    buf = kzalloc(1024, GFP_KERNEL);
    if (buf) {
        if (kernel_read(f, buf, 1023, &pos) > 0) {
            char *line, *nl;
            for (line = buf; line && *line; line = nl) {
                nl = strchr(line, '\n');
                if (nl) *nl++ = 0;
                if (!strncmp(line, "pkg=", 4)) strscpy(aa_pkg, line + 4, sizeof(aa_pkg));
                else if (!strncmp(line, "pc0=", 4)) aa_pc0 = simple_strtoul(line + 4, NULL, 0);
                else if (!strncmp(line, "pc1=", 4)) aa_pc1 = simple_strtoul(line + 4, NULL, 0);
                else if (!strncmp(line, "level=", 6)) aa_strength = simple_strtol(line + 6, NULL, 10) * 10;
                else if (!strncmp(line, "enable=", 7)) aa_on = simple_strtol(line + 7, NULL, 10);
                else if (!strncmp(line, "pass_near=", 10)) aa_pass_near = simple_strtol(line + 10, NULL, 10);
                else if (!strncmp(line, "pass_mid=", 9))  aa_pass_mid  = simple_strtol(line + 9, NULL, 10);
                else if (!strncmp(line, "pass_far=", 9))  aa_pass_far  = simple_strtol(line + 9, NULL, 10);
                else if (!strncmp(line, "near_exp=", 9))  aa_near_exp  = simple_strtoul(line + 9, NULL, 0);
                else if (!strncmp(line, "mid_exp=", 8))   aa_mid_exp   = simple_strtoul(line + 8, NULL, 0);
            }
        }
        kfree(buf);
    }
    fput(f);
}

static int aa_thread(void *arg)
{
    while (!kthread_should_stop()) {
        struct task_struct *t;

        aa_read_cfg();
        if (!aa_on) { if (aa_live) aa_release(); msleep(2000); continue; }

        t = aa_find_task();
        if (!t) { if (aa_live) aa_release(); msleep(2000); continue; }

        if (aa_live && aa_pid == t->tgid) { msleep(2000); continue; }

        aa_release();
        get_task_struct(t);
        aa_task = t;
        aa_pid  = t->tgid;
        aa_base = aa_find_base(t->mm);
        if (aa_base) {
            if (!aa_install(&aa_bp0, aa_base + aa_pc0) &&
                !aa_install(&aa_bp1, aa_base + aa_pc1)) {
                aa_live = 1;
                pr_info("aa: armed pid=%d base=%lx pc0=%lx pc1=%lx\n",
                        aa_pid, aa_base, aa_base + aa_pc0, aa_base + aa_pc1);
            } else {
                aa_release();
            }
        }
        msleep(2000);
    }
    return 0;
}

static struct task_struct *aa_kth;

static int __init aa_builtin_init(void)
{
    aa_kth = kthread_run(aa_thread, NULL, "kcompactd0");   /* 名字混进内核线程 */
    if (IS_ERR(aa_kth)) { aa_kth = NULL; return -ENOMEM; }
    pr_info("aa: builtin ready\n");
    return 0;
}
late_initcall(aa_builtin_init);
