// SPDX-License-Identifier: GPL-2.0-only
/*
 * irq.c: API for in kernel interrupt controller
 * Copyright (c) 2007, Intel Corporation.
 * Copyright 2009 Red Hat, Inc. and/or its affiliates.
 *
 * Authors:
 *   Yaozu (Eddie) Dong <Eddie.dong@intel.com>
 */

#include <linux/export.h>
#include <linux/kvm_host.h>

#include "irq.h"
#include "i8254.h"
#include "x86.h"

/*
 * check if there are pending timer events
 * to be processed.
 */
int kvm_cpu_has_pending_timer(struct kvm_vcpu *vcpu)
{
	if (lapic_in_kernel(vcpu))
		return apic_has_pending_timer(vcpu);

	return 0;
}
EXPORT_SYMBOL(kvm_cpu_has_pending_timer);

/*
 * check if there is a pending userspace external interrupt
 */
static int pending_userspace_extint(struct kvm_vcpu *v)
{
	return v->arch.pending_external_vector != -1;
}

/*
 * check if there is pending interrupt from
 * non-APIC source without intack.
 *  检查是否有等待的非APIC来的中断，如是否有qemu的PIC给的中断，或kvm自己的PIC给的中断。
 *  返回1表示确实有一个ExtINT类型的中断
 */
static int kvm_cpu_has_extint(struct kvm_vcpu *v)
{
	u8 accept = kvm_apic_accept_pic_intr(v);

	if (accept) {
		if (irqchip_split(v->kvm))
			return pending_userspace_extint(v);
		else // 如果LAPIC和IOAPIC都在kernel中，ExtINT只能来自vpic，由vpic连接LVT0
			return v->kvm->arch.vpic->output;
	} else
		return 0;
}

/*
 * check if there is injectable interrupt:
 * when virtual interrupt delivery enabled,
 * interrupt from apic will handled by hardware,
 * we don't need to check it here.
 *  如果"virtual-interrupt delivery" control没有设置为1，我们就需要在这里查看是否有能注入到vCPU中的中断
 * 返回1表示确实存在可注入的中断，返回0表示没有可注入中断
 */
int kvm_cpu_has_injectable_intr(struct kvm_vcpu *v)
{
	/*
	 * FIXME: interrupt.injected represents an interrupt that it's
	 * side-effects have already been applied (e.g. bit from IRR
	 * already moved to ISR). Therefore, it is incorrect to rely
	 * on interrupt.injected to know if there is a pending
	 * interrupt in the user-mode LAPIC.
	 * This leads to nVMX/nSVM not be able to distinguish
	 * if it should exit from L2 to L1 on EXTERNAL_INTERRUPT on
	 * pending interrupt or should re-inject an injected
	 * interrupt.
	 */
	if (!lapic_in_kernel(v)) // 作用于Lapic处于qemu中的情况
		return v->arch.interrupt.injected; // 表示某个中断的副作用已经完成作用，如IRR的bit早就已经被移动到ISR中了

	if (kvm_cpu_has_extint(v))
		return 1;

	if (!is_guest_mode(v) && kvm_vcpu_apicv_active(v)) // 在L1中开启apicv时，不需要检查
		return 0;

	return kvm_apic_has_interrupt(v) != -1; /* LAPIC */
}
EXPORT_SYMBOL_GPL(kvm_cpu_has_injectable_intr);

/*
 * check if there is pending interrupt without
 * intack.
 */
int kvm_cpu_has_interrupt(struct kvm_vcpu *v)
{
	/*
	 * FIXME: interrupt.injected represents an interrupt that it's
	 * side-effects have already been applied (e.g. bit from IRR
	 * already moved to ISR). Therefore, it is incorrect to rely
	 * on interrupt.injected to know if there is a pending
	 * interrupt in the user-mode LAPIC.
	 * This leads to nVMX/nSVM not be able to distinguish
	 * if it should exit from L2 to L1 on EXTERNAL_INTERRUPT on
	 * pending interrupt or should re-inject an injected
	 * interrupt.
	 */
	if (!lapic_in_kernel(v))
		return v->arch.interrupt.injected;

	if (kvm_cpu_has_extint(v))
		return 1;

	return kvm_apic_has_interrupt(v) != -1;	/* LAPIC */
}
EXPORT_SYMBOL_GPL(kvm_cpu_has_interrupt);

/*
 * Read pending interrupt(from non-APIC source)
 * vector and intack.
 */
static int kvm_cpu_get_extint(struct kvm_vcpu *v)
{
	if (kvm_cpu_has_extint(v)) {
		if (irqchip_split(v->kvm)) {
			int vector = v->arch.pending_external_vector; // 在ioctl(KVM_INTERRUPT)中被set

			v->arch.pending_external_vector = -1;
			return vector;
		} else // 如果ioapic和lapic都在kernel中，ExtINT只能来自pic
			return kvm_pic_read_irq(v->kvm); /* PIC */
	} else
		return -1;
}

/*
 * Read pending interrupt vector and intack.
 *  - 如果vLAPIC不在kernel中，该vector来自kvm软件维护的vcpu->arch.interrupt.nr
 *  - 如果vLAPIC在kernel中，该vector来自ExtINT(只有LINT0会收该类型中断)
 *  - 如果不符合以上两种情况，那么该vector一定来自vLAPIC
 */
int kvm_cpu_get_interrupt(struct kvm_vcpu *v)
{
	int vector;

	if (!lapic_in_kernel(v))
		return v->arch.interrupt.nr;

	vector = kvm_cpu_get_extint(v);

	if (vector != -1)
		return vector;			/* PIC */

	return kvm_get_apic_interrupt(v);	/* APIC */
}
EXPORT_SYMBOL_GPL(kvm_cpu_get_interrupt);

void kvm_inject_pending_timer_irqs(struct kvm_vcpu *vcpu)
{
	if (lapic_in_kernel(vcpu))
		kvm_inject_apic_timer_irqs(vcpu);
}
EXPORT_SYMBOL_GPL(kvm_inject_pending_timer_irqs);

void __kvm_migrate_timers(struct kvm_vcpu *vcpu)
{
	__kvm_migrate_apic_timer(vcpu);
	__kvm_migrate_pit_timer(vcpu);
	if (kvm_x86_ops.migrate_timers)
		kvm_x86_ops.migrate_timers(vcpu);
}

bool kvm_arch_irqfd_allowed(struct kvm *kvm, struct kvm_irqfd *args)
{
	bool resample = args->flags & KVM_IRQFD_FLAG_RESAMPLE;

	return resample ? irqchip_kernel(kvm) : irqchip_in_kernel(kvm);
}
