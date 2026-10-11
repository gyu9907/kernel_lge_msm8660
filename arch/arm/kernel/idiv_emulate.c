/*
 * ARMv7 userspace SDIV/UDIV emulation.
 * Based on Vladimir Murzin's idiv_emulate.c (smdk4412 cc2317978a887).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/atomic.h>
#include <linux/perf_event.h>
#include <asm/opcodes.h>
#include <asm/traps.h>

#define IDIV_ARM_MASK   0x0fd0f0f0
#define IDIV_ARM_VALUE  0x0710f010
#define IDIV_THUMB_MASK 0xffd0f0f0
#define IDIV_THUMB_VALUE 0xfb90f0f0
#define IDIV_UNSIGNED   (1U << 21)

static atomic_t sdiv_count = ATOMIC_INIT(0);
static atomic_t udiv_count = ATOMIC_INIT(0);

/* Advance the split CPSR ITSTATE without changing the condition flags. */
static unsigned long idiv_advance_it(unsigned long cpsr)
{
	unsigned int it = ((cpsr >> 8) & 0xfc) | ((cpsr >> 25) & 3);

	if ((it & 7) == 0)
		it = 0;
	else
		it = (it & 0xe0) | ((it << 1) & 0x1f);
	return (cpsr & ~PSR_IT_MASK) | ((it & 0xfc) << 8) |
		((it & 3) << 25);
}

static u32 idiv_result(u32 dividend, u32 divisor, bool is_unsigned)
{
	/* ARMv7-A division by zero returns zero; overflow wraps to INT_MIN. */
	if (!divisor)
		return 0;
	if (is_unsigned)
		return dividend / divisor;
	if (dividend == 0x80000000U && divisor == 0xffffffffU)
		return dividend;
	return (s32)dividend / (s32)divisor;
}

static int idiv_handler(struct pt_regs *regs, unsigned int instr)
{
	unsigned int rd, rn, rm, condition;
	bool thumb = thumb_mode(regs);
	bool is_unsigned = instr & IDIV_UNSIGNED;
	unsigned long cpsr = regs->ARM_cpsr;
	u32 result;

	if (!user_mode(regs))
		return -EFAULT;
	if (thumb) {
		unsigned int it;

		if ((instr & IDIV_THUMB_MASK) != IDIV_THUMB_VALUE)
			return -EFAULT;
		rd = (instr >> 8) & 15;
		rn = (instr >> 16) & 15;
		rm = instr & 15;
		/* SP and PC operands are UNPREDICTABLE in ARMv7 Thumb. */
		if (rd == 13 || rn == 13 || rm == 13 ||
		    rd == 15 || rn == 15 || rm == 15)
			return -EFAULT;
		it = ((cpsr >> 8) & 0xfc) | ((cpsr >> 25) & 3);
		condition = it ? it >> 4 : 14;
	} else {
		if ((instr & IDIV_ARM_MASK) != IDIV_ARM_VALUE)
			return -EFAULT;
		rd = (instr >> 16) & 15;
		rn = instr & 15;
		rm = (instr >> 8) & 15;
		condition = instr >> 28;
		if (rd == 15 || rn == 15 || rm == 15 || condition == 15)
			return -EFAULT;
	}

	if (arm_check_condition(condition << 28, cpsr) ==
	    ARM_OPCODE_CONDTEST_PASS) {
		result = idiv_result(regs->uregs[rn], regs->uregs[rm], is_unsigned);
		regs->uregs[rd] = result;
		atomic_inc(is_unsigned ? &udiv_count : &sdiv_count);
		perf_sw_event(PERF_COUNT_SW_EMULATION_FAULTS, 1, regs, regs->ARM_pc);
	}
	regs->ARM_pc += 4;
	if (thumb)
		regs->ARM_cpsr = idiv_advance_it(cpsr);
	return 0;
}

static struct undef_hook idiv_arm_hook = {
	.instr_mask = IDIV_ARM_MASK,
	.instr_val = IDIV_ARM_VALUE,
	.cpsr_mask = MODE_MASK | PSR_T_BIT,
	.cpsr_val = USR_MODE,
	.fn = idiv_handler,
};

static struct undef_hook idiv_thumb_hook = {
	.instr_mask = IDIV_THUMB_MASK,
	.instr_val = IDIV_THUMB_VALUE,
	.cpsr_mask = MODE_MASK | PSR_T_BIT,
	.cpsr_val = USR_MODE | PSR_T_BIT,
	.fn = idiv_handler,
};

#ifdef CONFIG_PROC_FS
static int idiv_show(struct seq_file *m, void *v)
{
	seq_printf(m, "Emulated SDIV:\t%u\n", (unsigned int)atomic_read(&sdiv_count));
	seq_printf(m, "Emulated UDIV:\t%u\n", (unsigned int)atomic_read(&udiv_count));
	return 0;
}

static int idiv_open(struct inode *inode, struct file *file)
{
	return single_open(file, idiv_show, NULL);
}

static const struct file_operations idiv_fops = {
	.open = idiv_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};
#endif

static int __init idiv_emulation_init(void)
{
	register_undef_hook(&idiv_arm_hook);
	register_undef_hook(&idiv_thumb_hook);
#ifdef CONFIG_PROC_FS
	if (!proc_create("cpu/idiv_emulation", S_IRUGO, NULL, &idiv_fops))
		pr_warn("Cannot create SDIV/UDIV emulation statistics\n");
#endif
	pr_info("Registered ARM/Thumb SDIV/UDIV emulation\n");
	return 0;
}
late_initcall(idiv_emulation_init);
