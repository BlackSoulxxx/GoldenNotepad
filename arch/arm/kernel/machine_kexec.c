/*
 * machine_kexec.c - handle transition of Linux booting another kernel
 */

#include <linux/mm.h>
#include <linux/kexec.h>
#include <linux/delay.h>
#include <linux/reboot.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include <asm/mmu_context.h>
#include <asm/cacheflush.h>
#include <asm/fncpy.h>
#include <asm/mach-types.h>
#include <asm/system_misc.h>

extern void relocate_new_kernel(void);
extern const unsigned int relocate_new_kernel_size;

extern unsigned long kexec_start_address;
extern unsigned long kexec_indirection_page;
extern unsigned long kexec_mach_type;
extern unsigned long kexec_boot_atags;

static atomic_t waiting_for_crash_ipi;

/*
 * Provide a dummy crash_notes definition while crash dump arrives to arm.
 * This prevents breakage of crash_notes attribute in kernel/ksysfs.c.
 */

int machine_kexec_prepare(struct kimage *image)
{
	return 0;
}

void machine_kexec_cleanup(struct kimage *image)
{
}

void machine_crash_nonpanic_core(void *unused)
{
	struct pt_regs regs;

	crash_setup_regs(&regs, NULL);
	printk(KERN_DEBUG "CPU %u will stop doing anything useful since another CPU has crashed\n",
	       smp_processor_id());
	crash_save_cpu(&regs, smp_processor_id());
	flush_cache_all();

	atomic_dec(&waiting_for_crash_ipi);
	while (1)
		cpu_relax();
}

static void machine_kexec_mask_interrupts(void)
{
	unsigned int i;
	struct irq_desc *desc;

	for_each_irq_desc(i, desc) {
		struct irq_chip *chip;

		chip = irq_desc_get_chip(desc);
		if (!chip)
			continue;

		if (chip->irq_eoi && irqd_irq_inprogress(&desc->irq_data))
			chip->irq_eoi(&desc->irq_data);

		if (chip->irq_mask)
			chip->irq_mask(&desc->irq_data);

		if (chip->irq_disable && !irqd_irq_disabled(&desc->irq_data))
			chip->irq_disable(&desc->irq_data);
	}
}

void machine_crash_shutdown(struct pt_regs *regs)
{
	unsigned long msecs;

	local_irq_disable();

	atomic_set(&waiting_for_crash_ipi, num_online_cpus() - 1);
	smp_call_function(machine_crash_nonpanic_core, NULL, false);
	msecs = 1000; /* Wait at most a second for the other cpus to stop */
	while ((atomic_read(&waiting_for_crash_ipi) > 0) && msecs) {
		mdelay(1);
		msecs--;
	}
	if (atomic_read(&waiting_for_crash_ipi) > 0)
		printk(KERN_WARNING "Non-crashing CPUs did not react to IPI\n");

	crash_save_cpu(regs, smp_processor_id());
	machine_kexec_mask_interrupts();

	printk(KERN_INFO "Loading crashdump kernel...\n");
}

/*
 * Function pointer to optional machine-specific reinitialization
 */
void (*kexec_reinit)(void);

void machine_kexec(struct kimage *image)
{
	unsigned long page_list;
	unsigned long reboot_code_buffer_phys;
	unsigned long reboot_entry = (unsigned long)relocate_new_kernel;
	unsigned long reboot_entry_phys;
	void *reboot_code_buffer;


	page_list = image->head & PAGE_MASK;

	/* we need both effective and real address here */
	reboot_code_buffer_phys =
	    page_to_pfn(image->control_code_page) << PAGE_SHIFT;
	reboot_code_buffer = page_address(image->control_code_page);

	/* Prepare parameters for reboot_code_buffer*/
	kexec_start_address = image->start;
	kexec_indirection_page = page_list;
	kexec_mach_type = machine_arch_type;
	kexec_boot_atags = image->start - KEXEC_ARM_ZIMAGE_OFFSET + KEXEC_ARM_ATAGS_OFFSET;

	/* copy our kernel relocation code to the control code page */
	reboot_entry = fncpy(reboot_code_buffer,
			     reboot_entry,
			     relocate_new_kernel_size);
	reboot_entry_phys = (unsigned long)reboot_entry +
		(reboot_code_buffer_phys - (unsigned long)reboot_code_buffer);

	printk(KERN_INFO "Bye!\n");

	if (kexec_reinit)
		kexec_reinit();

	soft_restart(reboot_entry_phys);
}

void arch_crash_save_vmcoreinfo(void)
{
#ifdef CONFIG_ARM_LPAE
	VMCOREINFO_CONFIG(ARM_LPAE);
#endif
}
