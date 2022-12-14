/*
 * hades_rookit_detection
 *
 * The methods and codes are mainly based on Elkeid(tyton) and tracee.
 * Mainly, hades detect syscalls, idt and bad eBPF. Also, hooks like
 * do_init_module, security_kernel_read_file, call_usermodehelper are
 * added for general rookit detection.
 */

#ifndef CORE
#include <linux/module.h>
#include <linux/kobject.h>
#include <linux/kthread.h>
#include <linux/err.h>
#else
#include <vmlinux.h>
#include <missing_definitions.h>
#endif

#include "define.h"
#include "utils_buf.h"
#include "utils.h"
#include "bpf_helpers.h"
#include "bpf_core_read.h"
#include "bpf_tracing.h"

// Firstly, a rootkit would be loaded into kernel space. There are some
// hooks that we should pay attention to. I get these information from
// Elkeid, and datadog.
// @Reference: https://github.com/DataDog/datadog-agent/blob/aa1665562704cf7505f4be9b95894cd6e68b31f8/pkg/security/ebpf/probes/module.go
// 1. kprobe/do_init_module (this is well-knowned and used in Elkeid)
// 2. kprobe/module_put (@ Reference: https://ph4ntonn.github.io/rootkit%E5%88%86%E6%9E%90-linux%E5%86%85%E6%A0%B8%E6%9C%BA%E5%88%B6%E5%AD%A6%E4%B9%A0)
// There are several things that interest me:
// Firstly, it's the 'module_put' thing. Here is the rootkit that use this function.
// https://github.com/nurupo/rootkit/blob/master/rootkit.c
// We can see that the rootkit use 'try_module_get' in protect function.
// Since in linux, we can not unload a kernel module which gets a non zero
// count. It's nothing new though.
// And a interestring PR from Google to merge the bpf/LSM to kernel, they
// use custom kernel modules as well!
// And other part, like hidden the kernel modules, will be introduced in
// another repo :)

// Firstly, do_init_module is the thing that we need. Any mod that loaded should
// be monitored.
SEC("kprobe/do_init_module")
int BPF_KPROBE(kprobe_do_init_module)
{
    event_data_t data = {};
    if (!init_event_data(&data, ctx))
        return 0;
    data.context.type = 1026;

    struct module *mod = (struct module *)PT_REGS_PARM1(ctx);
    char *modname = NULL;
    bpf_probe_read_str(&modname, 64 - sizeof(unsigned long), &mod->name);
    save_str_to_buf(&data, &modname, 0);

    // get exe from task
    void *exe = get_exe_from_task(data.task);
    save_str_to_buf(&data, exe, 1);
    save_pid_tree_to_buf(&data, 12, 2);
    // save file from current task->fs->pwd
    struct fs_struct *file = get_task_fs(data.task);
    if (file == NULL)
        return 0;
    void *file_path = get_path_str(GET_FIELD_ADDR(file->pwd));
    save_str_to_buf(&data, file_path, 1);
    return events_perf_submit(&data);
}

/*
 * @kernel_read_file:
 *	Read a file specified by userspace.
 *	@file contains the file structure pointing to the file being read
 *	by the kernel.
 *	@id kernel read file identifier
 *	@contents if a subsequent @kernel_post_read_file will be called.
 *	Return 0 if permission is granted.
 */
// In datadog, security_kernel_module_from_file is hooked. But it seems not
// work since it's been removed in kernel version 4.6...
// security_kernel_read_file seems stable and is used by tracee
SEC("kprobe/security_kernel_read_file")
int BPF_KPROBE(kprobe_security_kernel_read_file)
{
    event_data_t data = {};
    if (!init_event_data(&data, ctx))
        return 0;
    data.context.type = SECURITY_KERNEL_READ_FILE;
    // get the file
    struct file *file = (struct file *)PT_REGS_PARM1(ctx);
    void *file_path = get_path_str(GET_FIELD_ADDR(file->f_path));
    save_str_to_buf(&data, file_path, 0);

    // get the id
    enum kernel_read_file_id type_id = (enum kernel_read_file_id)PT_REGS_PARM2(ctx);
    save_to_submit_buf(&data, &type_id, sizeof(int), 1);
    return events_perf_submit(&data);
}

// Add rootkit detection just like in Elkeid.
// @Notice: this is under full test
SEC("kprobe/call_usermodehelper")
int BPF_KPROBE(kprobe_call_usermodehelper)
{
    event_data_t data = {};
    if (!init_event_data(&data, ctx))
        return 0;
    data.context.type = CALL_USERMODEHELPER;
    void *path = (void *)PT_REGS_PARM1(ctx);
    save_str_to_buf(&data, path, 0);
    unsigned long argv = PT_REGS_PARM2(ctx);
    save_str_arr_to_buf(&data, (const char *const *)argv, 1);
    unsigned long envp = PT_REGS_PARM3(ctx);
    // Think twice about this.
    // I do not use `save_envp_to_buf` here, since there is not that much
    // call_usermodehelper called... And since it's very important, it's
    // good to just get them all.
    save_str_arr_to_buf(&data, (const char *const *)envp, 2);
    int wait = PT_REGS_PARM4(ctx);
    save_to_submit_buf(&data, (void *)&wait, sizeof(int), 3);
    // Think twice
    void *exe = get_exe_from_task(data.task);
    save_str_to_buf(&data, exe, 4);
    return events_perf_submit(&data);
}

// For Hidden Rootkit. It's interestring in Elkeid, let's learn firstly.
// In Elkeid, it's in anti_rootkit file, function `find_hidden_module`
// This reference: (https://www.cnblogs.com/LoyenWang/p/13334196.html)
// helps me understand better.
// Before that, we should have a brief conception of sth.
// IDT(Interrupt Description Table):
//   It's a table shows the relationship
//   of the Interrupt and it's process function. (x86 only)
// sys_call_table:
//   @Reference: https://blog.tofile.dev/2021/07/07/ebpf-hooks.html
//   this article shows the way we do anti rootkit with stack_id. But
//   hook inside kernel(like Reptile), like vfs_read, would be called by
//   other part of the kernel module, so we should determine whether it's
//   right from the stack trace... (interestring)
// core_kernel_text:
//   scan the sys_call_table. If the function did not point to the kernel
//   text section, then it's more likely hooked. Since the text section
//   is stable after the kernel builded.
// kernel sets(ksets):
//    kset contains kobject(s). This reference explains well:
//    https://he1m4n6a.github.io/2020/07/16/%E5%AF%B9%E6%8A%97rootkits/
//    we go through every kobject from the kset and get all kobjs.
//    if the kobject do not exist in the kset
// In Elkeid, the find_hidden_module go through the kobject to judge whether
// it's in the sys_call_table and IDT. And the kernel sets finding is also
// considered.
//
// In a really brief way, it goes like this. In userspace, we do something
// to trigger the system interrupt.
// It's reasonable that we can do sth with the IDT or the sys_call_table
// to hijack the function. Also for the syscall & sys_enter/exit
//
// As for as I concerned, things like scan the idt and sys_call_table are
// "positive" action. eBPF progs are more "passive" in anti rootkit.
// We can collect the syscalls from specific probes or ...
// @Reference: https://github.com/pathtofile/bpf-hookdetect/blob/main/src/hookdetect.c

// At last, here is my reference:
// @Reference: https://www.lse.epita.fr/lse-summer-week-2015/slides/lse-summer-week-2015-14-linux_rootkit.pdf
// @Reference: https://github.com/RouNNdeL/anti-rootkit-lkm/blob/14d9f934f7f9a5bf27849c2b51b096fe585bea35/module/anti_rootkit/main.c
// @Reference: https://github.com/JnuSimba/MiscSecNotes/blob/dacdefb60d7e5350a077b135382412cbba0f084f/Linux%E6%B8%97%E9%80%8F/Rootkit%20%E7%BB%BC%E5%90%88%E6%95%99%E7%A8%8B.md
// @Reference: https://blog.csdn.net/dog250/article/details/105371830
// @Reference: https://blog.csdn.net/dog250/article/details/105394840
// @Reference: https://blog.csdn.net/dog250/article/details/105842029
// @Reference: https://he1m4n6a.github.io/2020/07/16/%E5%AF%B9%E6%8A%97rootkits/
// Pre define for all
#define IDT_CACHE           0
#define SYSCALL_CACHE       1
#define IDT_ENTRIES         256
#define MAX_KSYM_NAME_SIZE  64
#define IOCTL_SCAN_SYSCALLS 65
#define IOCTL_SCAN_IDTS     66

typedef struct ksym_name
{
    char str[MAX_KSYM_NAME_SIZE];
} ksym_name_t;
// https://github.com/m0nad/Diamorphine/blob/master/diamorphine.c
BPF_HASH(ksymbols_map, ksym_name_t, __u64, 64);
BPF_HASH(analyze_cache, int, __u64, 2);

// get symbol_addr from user_space in /proc/kallsyms
static __always_inline void *get_symbol_addr(char *symbol_name)
{
    char new_ksym_name[MAX_KSYM_NAME_SIZE] = {};
    bpf_probe_read_str(new_ksym_name, MAX_KSYM_NAME_SIZE, symbol_name);
    void **sym = bpf_map_lookup_elem(&ksymbols_map, (void *)&new_ksym_name);

    if (sym == NULL)
        return 0;

    return *sym;
}
// It's very happy to see https://github.com/aquasecurity/tracee/commit/44c3fb1e6ff2faa42be7285690f7a97990abcb08
// Do a trigger to scan. It's brilliant and I'll go through this and
// do the same thing in Hades. And it's done by @itamarmaouda101
// 2022-04-21: start to review the invoke_print_syscall_table_event function

// The way Elkeid does can not be simply done in eBPF program since the limit of 4096
// under kernel version 5.2. In Elkeid, it always go through the syscalls or idt_entries
// to find any hidden kernel module in this.
// Back to Elkeid anti_rootkit, which is based on https://github.com/nbulischeck/tyton
// detecting syscall_hooks/interrupt_hooks(IDT)/modules/fops(/proc). And I think a BAD
// eBPF program should also be considered.

// Below here is the Elkeid way of anti_rootkit by scanning the syscall_table
// idt_table and ... to get the mod of the function to figure out if it's a
// hidden module. But this a little bit tricky in eBPF program since the
// __module_address to the the mod of the address.

// Below here, Tracee... scan limited syscal_table_addr ...
static __always_inline void sys_call_table_scan(event_data_t *data)
{
    char syscall_table[15] = "sys_call_table";
    unsigned long *syscall_table_addr = (unsigned long *)get_symbol_addr(syscall_table);

    __u64 idx = SYSCALL_CACHE;
    __u64 *syscall_num_p;
    __u64 syscall_num;
    unsigned long syscall_addr = 0;

    syscall_num_p = bpf_map_lookup_elem(&analyze_cache, (void *)&idx);
    if (syscall_num_p == NULL)
        return;
    syscall_num = (__u64)*syscall_num_p;
    syscall_addr = READ_KERN(syscall_table_addr[syscall_num]);
    if (syscall_addr == 0)
        return;

    save_to_submit_buf(data, &syscall_addr, sizeof(unsigned long), 0);
    save_to_submit_buf(data, &syscall_num, sizeof(__u64), 1);

    int field = ANTI_ROOTKIT_SYSCALL;
    save_to_submit_buf(data, &field, sizeof(int), 2);
    events_perf_submit(data);
}

// idt_table_scan
/*
 * still wrong with the address we take...
 */
static __always_inline void idt_table_scan(event_data_t *data)
{
    char idt_table[10] = "idt_table";
    unsigned long *idt_table_addr = (unsigned long *)get_symbol_addr(idt_table);

    __u64 idx = IDT_CACHE;
    __u64 *idt_num_p;
    __u64 idt_num;
    unsigned long idt_addr = 0;

    idt_num_p = bpf_map_lookup_elem(&analyze_cache, (void *)&idx);
    if (idt_num_p == NULL)
        return;
    idt_num = (__u64)*idt_num_p;
    /* get the address of the interrupt */
    idt_addr = READ_KERN(idt_table_addr[idt_num]);
    if (idt_addr == 0)
        return;

    struct gate_struct* gate = (struct gate_struct *)idt_addr;
    if (gate == NULL)
        return;

    __u16 offset_low;
    bpf_probe_read(&offset_low, sizeof(offset_low), &gate->offset_low);
    __u16 offset_middle;
    bpf_probe_read(&offset_middle, sizeof(offset_middle), &gate->offset_middle);
    __u32 offset_high;
    bpf_probe_read(&offset_high, sizeof(offset_high), &gate->offset_high);

    /* calc the offset */
    idt_addr = (unsigned long)offset_low | ((unsigned long)offset_middle << 16) | ((unsigned long)offset_high << 32);

    save_to_submit_buf(data, &idt_addr, sizeof(unsigned long), 0);
    save_to_submit_buf(data, &idt_num, sizeof(__u64), 1);

    int field = ANTI_ROOTKIT_IDT;
    save_to_submit_buf(data, &field, sizeof(int), 2);
    events_perf_submit(data);
}

SEC("kprobe/security_file_ioctl")
int BPF_KPROBE(kprobe_security_file_ioctl)
{
    event_data_t data = {};
    if (!init_event_data(&data, ctx))
        return 0;
    data.context.type = ANTI_ROOTKIT;
    unsigned int cmd = PT_REGS_PARM2(ctx);
    // Skip if not the pid we need
    if (get_config(CONFIG_HADES_PID) != data.context.tid)
        return 0;

    if (cmd == IOCTL_SCAN_SYSCALLS)
    {
        sys_call_table_scan(&data);
    } else if (cmd == IOCTL_SCAN_IDTS) {
        idt_table_scan(&data);
    }
    return 0;
}