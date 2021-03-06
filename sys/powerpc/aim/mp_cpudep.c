/*-
 * Copyright (c) 2008 Marcel Moolenaar
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD: src/sys/powerpc/aim/mp_cpudep.c,v 1.6.2.1.2.1 2009/10/25 01:10:29 kensmith Exp $");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/bus.h>
#include <sys/pcpu.h>
#include <sys/proc.h>
#include <sys/smp.h>

#include <machine/bus.h>
#include <machine/cpu.h>
#include <machine/hid.h>
#include <machine/intr_machdep.h>
#include <machine/pcb.h>
#include <machine/psl.h>
#include <machine/smp.h>
#include <machine/spr.h>
#include <machine/trap_aim.h>

#include <dev/ofw/openfirm.h>
#include <machine/ofw_machdep.h>

extern void *rstcode;
extern register_t l2cr_config;
extern register_t l3cr_config;

void *ap_pcpu;

static register_t
l2_enable(void)
{
	register_t ccr;

	ccr = mfspr(SPR_L2CR);
	if (ccr & L2CR_L2E)
		return (ccr);

	/* Configure L2 cache. */
	ccr = l2cr_config & ~L2CR_L2E;
	mtspr(SPR_L2CR, ccr | L2CR_L2I);
	do {
		ccr = mfspr(SPR_L2CR);
	} while (ccr & L2CR_L2I);
	powerpc_sync();
	mtspr(SPR_L2CR, l2cr_config);
	powerpc_sync();

	return (l2cr_config);
}

static register_t
l3_enable(void)
{
	register_t ccr;

	ccr = mfspr(SPR_L3CR);
	if (ccr & L3CR_L3E)
		return (ccr);

	/* Configure L3 cache. */
	ccr = l3cr_config & ~(L3CR_L3E | L3CR_L3I | L3CR_L3PE | L3CR_L3CLKEN);
	mtspr(SPR_L3CR, ccr);
	ccr |= 0x4000000;       /* Magic, but documented. */
	mtspr(SPR_L3CR, ccr);
	ccr |= L3CR_L3CLKEN;
	mtspr(SPR_L3CR, ccr);
	mtspr(SPR_L3CR, ccr | L3CR_L3I);
	while (mfspr(SPR_L3CR) & L3CR_L3I)
		;
	mtspr(SPR_L3CR, ccr & ~L3CR_L3CLKEN);
	powerpc_sync();
	DELAY(100);
	mtspr(SPR_L3CR, ccr);
	powerpc_sync();
	DELAY(100);
	ccr |= L3CR_L3E;
	mtspr(SPR_L3CR, ccr);
	powerpc_sync();

	return(ccr);
}

static register_t
l1d_enable(void)
{
	register_t hid;

	hid = mfspr(SPR_HID0);
	if (hid & HID0_DCE)
		return (hid);

	/* Enable L1 D-cache */
	hid |= HID0_DCE;
	powerpc_sync();
	mtspr(SPR_HID0, hid | HID0_DCFI);
	powerpc_sync();

	return (hid);
}

static register_t
l1i_enable(void)
{
	register_t hid;

	hid = mfspr(SPR_HID0);
	if (hid & HID0_ICE)
		return (hid);

	/* Enable L1 I-cache */
	hid |= HID0_ICE;
	isync();
	mtspr(SPR_HID0, hid | HID0_ICFI);
	isync();

	return (hid);
}

uint32_t
cpudep_ap_bootstrap(void)
{
	uint32_t hid, msr, reg, sp;

	// reg = mfspr(SPR_MSSCR0);
	// mtspr(SPR_MSSCR0, reg | 0x3);

	__asm __volatile("mtsprg 0, %0" :: "r"(ap_pcpu));
	powerpc_sync();

	__asm __volatile("mtspr 1023,%0" :: "r"(PCPU_GET(cpuid)));
	__asm __volatile("mfspr %0,1023" : "=r"(pcpup->pc_pir));

	msr = PSL_FP | PSL_IR | PSL_DR | PSL_ME | PSL_RI;
	powerpc_sync();
	isync();
	mtmsr(msr);
	isync();

	if (l3cr_config != 0)
		reg = l3_enable();
	if (l2cr_config != 0)
		reg = l2_enable();
	reg = l1d_enable();
	reg = l1i_enable();

	hid = mfspr(SPR_HID0);
	hid &= ~(HID0_DOZE | HID0_SLEEP);
	hid |= HID0_NAP | HID0_DPM;
	mtspr(SPR_HID0, hid);
	isync();

	pcpup->pc_curthread = pcpup->pc_idlethread;
	pcpup->pc_curpcb = pcpup->pc_curthread->td_pcb;
	sp = pcpup->pc_curpcb->pcb_sp;

	return (sp);
}

