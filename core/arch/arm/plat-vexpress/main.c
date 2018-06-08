/*
 * Copyright (c) 2016, Linaro Limited
 * Copyright (c) 2014, STMicroelectronics International N.V.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <arm.h>
#include <console.h>
#include <drivers/gic.h>
#include <drivers/pl011.h>
#include <drivers/tzc400.h>
#include <initcall.h>
#include <keep.h>
#include <kernel/generic_boot.h>
#include <kernel/misc.h>
#include <kernel/panic.h>
#include <kernel/pm_stubs.h>
#include <kernel/tee_time.h>
#include <mm/core_memprot.h>
#include <mm/core_mmu.h>
#include <platform_config.h>
#include <sm/psci.h>
#include <stdint.h>
#include <string.h>
#include <tee/entry_fast.h>
#include <tee/entry_std.h>
#include <trace.h>
#include <io.h>

#ifdef CFG_PL050
#include <drivers/pl050.h>
#endif

static void main_fiq(void);

static const struct thread_handlers handlers = {
	.std_smc = tee_entry_std,
	.fast_smc = tee_entry_fast,
	.nintr = main_fiq,
#if defined(CFG_WITH_ARM_TRUSTED_FW)
	.cpu_on = cpu_on_handler,
	.cpu_off = pm_do_nothing,
	.cpu_suspend = pm_do_nothing,
	.cpu_resume = pm_do_nothing,
	.system_off = pm_do_nothing,
	.system_reset = pm_do_nothing,
#else
	.cpu_on = pm_panic,
	.cpu_off = pm_panic,
	.cpu_suspend = pm_panic,
	.cpu_resume = pm_panic,
	.system_off = pm_panic,
	.system_reset = pm_panic,
#endif
};

static struct gic_data gic_data;
static struct pl011_data console_data;

#if defined(PLATFORM_FLAVOR_fvp)
register_phys_mem(MEM_AREA_RAM_SEC, TZCDRAM_BASE, TZCDRAM_SIZE);
#endif
#if defined(PLATFORM_FLAVOR_qemu_virt)
register_phys_mem(MEM_AREA_IO_SEC, SECRAM_BASE, SECRAM_COHERENT_SIZE);
#endif
#if defined(PLATFORM_FLAVOR_qemu_vexpress)
register_phys_mem(MEM_AREA_IO_SEC, SECRAM_BASE, SECRAM_COHERENT_SIZE);
#ifdef CFG_PL050
register_phys_mem(MEM_AREA_IO_SEC, KMI_KB_BASE, KMI_KB_REG_SIZE);
#endif
#endif	//PLATFORM_FLAVOR_qemu_vexpress
register_phys_mem(MEM_AREA_IO_SEC, CONSOLE_UART_BASE, PL011_REG_SIZE);
register_nsec_ddr(DRAM0_BASE, DRAM0_SIZE);
#ifdef DRAM1_BASE
register_nsec_ddr(DRAM1_BASE, DRAM1_SIZE);
#endif

const struct thread_handlers *generic_boot_get_handlers(void)
{
	return &handlers;
}

#ifdef GIC_BASE

register_phys_mem(MEM_AREA_IO_SEC, GICD_BASE, GIC_DIST_REG_SIZE);
register_phys_mem(MEM_AREA_IO_SEC, GICC_BASE, GIC_DIST_REG_SIZE);

void main_init_gic(void)
{
	vaddr_t gicc_base;
	vaddr_t gicd_base;

	gicc_base = (vaddr_t)phys_to_virt(GIC_BASE + GICC_OFFSET,
					  MEM_AREA_IO_SEC);
	gicd_base = (vaddr_t)phys_to_virt(GIC_BASE + GICD_OFFSET,
					  MEM_AREA_IO_SEC);
	if (!gicc_base || !gicd_base)
		panic();

#if defined(CFG_WITH_ARM_TRUSTED_FW)
	/* On ARMv8, GIC configuration is initialized in ARM-TF */
	gic_init_base_addr(&gic_data, gicc_base, gicd_base);
#else
	/* Initialize GIC */
	gic_init(&gic_data, gicc_base, gicd_base);
#endif
	itr_init(&gic_data.chip);

	gic_dump_state(&gic_data);
}

#if !defined(CFG_WITH_ARM_TRUSTED_FW)
void main_secondary_init_gic(void)
{
	gic_cpu_init(&gic_data);
}
#else
void main_secondary_init_gic(void)
{
	gic_dump_state(&gic_data);
}
#endif

#endif

static void main_fiq(void)
{
	gic_it_handle(&gic_data);
}

#ifdef CFG_PL050
static vaddr_t kmi_kb;
static bool skip = true;


static enum itr_return kb_itr_handler(struct itr_handler *h __unused){
	volatile uint8_t tsc;
	char code;

	uint8_t ir = read8(kmi_kb + KMIIR_OFFSET);

	if (ir & KMIIR_RXINTR) {
		tsc = read8(kmi_kb + KMIDATA_OFFSET);

		if (tsc < 0x80 && !skip)	{
			code = kbd_get_code(tsc);
			DMSG("Got key: '%c'\n", code);
			skip = true;
		} else if (tsc == 0xf0)
			skip = false;
	}

	return ITRR_HANDLED;
}

static struct itr_handler kb_itr = {
	.it = IT_KMI_KEYBOARD,
	.flags = ITRF_TRIGGER_LEVEL,
	.handler = kb_itr_handler,
};
KEEP_PAGER(kb_itr);

static __maybe_unused TEE_Result kb_init_itr(void)
{
	DMSG("Enable interrupt %d for kmi keyboard\n", IT_KMI_KEYBOARD);
	itr_add(&kb_itr);					//set irq to group0
	itr_enable(IT_KMI_KEYBOARD);		//enable irq line

	return TEE_SUCCESS;
}

//driver_init(kb_init_itr);

void kb_init(void)
{
	DMSG("Initializing KMI Keyboard ...");
	kmi_kb = (vaddr_t)phys_to_virt(KMI_KB_BASE, MEM_AREA_IO_SEC);
	if (!kmi_kb) {
		DMSG("kmi-kb not mapped");
		panic();
	}

	kbd_enable(kmi_kb);
}

#endif


void console_init(void)
{
	pl011_init(&console_data, CONSOLE_UART_BASE, CONSOLE_UART_CLK_IN_HZ,
		   CONSOLE_BAUDRATE);
	register_serial_console(&console_data.chip);
}

#ifdef IT_CONSOLE_UART
static enum itr_return console_itr_cb(struct itr_handler *h __unused)
{
	struct serial_chip *cons = &console_data.chip;

	while (cons->ops->have_rx_data(cons)) {
		int ch __maybe_unused = cons->ops->getchar(cons);

		DMSG("cpu %zu: got 0x%x, '%c'", get_core_pos(), ch, ch);
	}
	return ITRR_HANDLED;
}

static struct itr_handler console_itr = {
	.it = IT_CONSOLE_UART,
	.flags = ITRF_TRIGGER_LEVEL,
	.handler = console_itr_cb,
};
KEEP_PAGER(console_itr);

static TEE_Result init_console_itr(void)
{
	DMSG("Enable interrupt %d for uart console\n", IT_CONSOLE_UART);
	itr_add(&console_itr);
	itr_enable(IT_CONSOLE_UART);
	return TEE_SUCCESS;
}
driver_init(init_console_itr);
#endif

#ifdef CFG_TZC400
register_phys_mem(MEM_AREA_IO_SEC, TZC400_BASE, TZC400_REG_SIZE);

static TEE_Result init_tzc400(void)
{
	void *va;

	DMSG("Initializing TZC400");

	va = phys_to_virt(TZC400_BASE, MEM_AREA_IO_SEC);
	if (!va) {
		EMSG("TZC400 not mapped");
		panic();
	}

	tzc_init((vaddr_t)va);
	tzc_dump_state();

	return TEE_SUCCESS;
}

service_init(init_tzc400);
#endif /*CFG_TZC400*/

#if defined(PLATFORM_FLAVOR_qemu_virt)
int psci_cpu_on(uint32_t core_id, uint32_t entry, uint32_t context_id __unused)
{
	size_t pos = get_core_pos_mpidr(core_id);
	uint32_t *sec_entry_addrs = phys_to_virt(SECRAM_BASE, MEM_AREA_IO_SEC);
	static bool core_is_released[CFG_TEE_CORE_NB_CORE];

	if (!sec_entry_addrs)
		panic();

	if (!pos || pos >= CFG_TEE_CORE_NB_CORE)
		return PSCI_RET_INVALID_PARAMETERS;

	DMSG("core pos: %zu: ns_entry %#" PRIx32, pos, entry);

	if (core_is_released[pos]) {
		EMSG("core %zu already released", pos);
		return PSCI_RET_DENIED;
	}
	core_is_released[pos] = true;

	/* set NS entry addresses of core */
	ns_entry_addrs[pos] = entry;
	dsb_ishst();

	sec_entry_addrs[pos] = CFG_TEE_LOAD_ADDR;
	dsb_ishst();
	sev();

	return PSCI_RET_SUCCESS;
}
#endif /*PLATFORM_FLAVOR_qemu_virt*/
