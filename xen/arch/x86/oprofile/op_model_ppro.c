/**
 * @file op_model_ppro.h
 * pentium pro / P6 model-specific MSR operations
 *
 * @remark Copyright 2002 OProfile authors
 * @remark Read the file COPYING
 *
 * @author John Levon
 * @author Philippe Elie
 * @author Graydon Hoare
 */

#include <xen/types.h>
#include <asm/msr.h>
#include <asm/io.h>
#include <asm/apic.h>
#include <asm/processor.h>
#include <xen/sched.h>
#include <asm/regs.h>
#include <asm/current.h>
#include <asm/hvm/vmx/vpmu.h>
#include <asm/hvm/vmx/vpmu_core2.h>
 
#include "op_x86_model.h"
#include "op_counter.h"

/*
 * Intel "Architectural Performance Monitoring" CPUID
 * detection/enumeration details:
 */
union cpuid10_eax {
	struct {
		unsigned int version_id:8;
		unsigned int num_counters:8;
		unsigned int bit_width:8;
		unsigned int mask_length:8;
	} split;
	unsigned int full;
};

static int num_counters = 2;
static int counter_width = 32;

#define CTR_OVERFLOWED(n) (!((n) & (1ULL<<(counter_width-1)))) 

#define CTRL_READ(l,h,msrs,c) do {rdmsr((msrs->controls[(c)].addr), (l), (h));} while (0)
#define CTRL_WRITE(l,h,msrs,c) do {wrmsr((msrs->controls[(c)].addr), (l), (h));} while (0)
#define CTRL_SET_ACTIVE(n) (n |= (1<<22))
#define CTRL_SET_INACTIVE(n) (n &= ~(1<<22))
#define CTRL_CLEAR(x) (x &= (1<<21))
#define CTRL_SET_ENABLE(val) (val |= 1<<20)
#define CTRL_SET_USR(val,u) (val |= ((u & 1) << 16))
#define CTRL_SET_KERN(val,k) (val |= ((k & 1) << 17))
#define CTRL_SET_UM(val, m) (val |= (m << 8))
#define CTRL_SET_EVENT(val, e) (val |= e)
#define IS_ACTIVE(val) (val & (1 << 22) )  
#define IS_ENABLE(val) (val & (1 << 20) )
static unsigned long reset_value[OP_MAX_COUNTER];
int ppro_has_global_ctrl = 0;
 
static void ppro_fill_in_addresses(struct op_msrs * const msrs)
{
	int i;

	for (i = 0; i < num_counters; i++)
		msrs->counters[i].addr = MSR_P6_PERFCTR0 + i;
	for (i = 0; i < num_counters; i++)
		msrs->controls[i].addr = MSR_P6_EVNTSEL0 + i;
}


static void ppro_setup_ctrs(struct op_msrs const * const msrs)
{
	unsigned int low, high;
	int i;
	
	if (cpu_has_arch_perfmon) {
		union cpuid10_eax eax;
		eax.full = cpuid_eax(0xa);

		/*
		 * For Core2 (family 6, model 15), don't reset the
		 * counter width:
		 */
		if (!(eax.split.version_id == 0 &&
			current_cpu_data.x86 == 6 &&
				current_cpu_data.x86_model == 15)) {

			if (counter_width < eax.split.bit_width)
				counter_width = eax.split.bit_width;
		}
	}

	/* clear all counters */
	for (i = 0 ; i < num_counters; ++i) {
		CTRL_READ(low, high, msrs, i);
		CTRL_CLEAR(low);
		CTRL_WRITE(low, high, msrs, i);
	}
	
	/* avoid a false detection of ctr overflows in NMI handler */
	for (i = 0; i < num_counters; ++i)
		wrmsrl(msrs->counters[i].addr, -1LL);

	/* enable active counters */
	for (i = 0; i < num_counters; ++i) {
		if (counter_config[i].enabled) {
			reset_value[i] = counter_config[i].count;

			wrmsrl(msrs->counters[i].addr, -reset_value[i]);

			CTRL_READ(low, high, msrs, i);
			CTRL_CLEAR(low);
			CTRL_SET_ENABLE(low);
			CTRL_SET_USR(low, counter_config[i].user);
			CTRL_SET_KERN(low, counter_config[i].kernel);
			CTRL_SET_UM(low, counter_config[i].unit_mask);
			CTRL_SET_EVENT(low, counter_config[i].event);
			CTRL_WRITE(low, high, msrs, i);
		} else {
			reset_value[i] = 0;
		}
	}
}

static int ppro_check_ctrs(unsigned int const cpu,
                           struct op_msrs const * const msrs,
                           struct cpu_user_regs * const regs)
{
	u64 val;
	int i;
	int ovf = 0;
	unsigned long eip = regs->eip;
	int mode = xenoprofile_get_mode(current, regs);
	struct arch_msr_pair *msrs_content = vcpu_vpmu(current)->context;

	for (i = 0 ; i < num_counters; ++i) {
		if (!reset_value[i])
			continue;
		rdmsrl(msrs->counters[i].addr, val);
		if (CTR_OVERFLOWED(val)) {
			xenoprof_log_event(current, regs, eip, mode, i);
			wrmsrl(msrs->counters[i].addr, -reset_value[i]);
			if ( is_passive(current->domain) && (mode != 2) && 
				(vcpu_vpmu(current)->flags & PASSIVE_DOMAIN_ALLOCATED) ) 
			{
				if ( IS_ACTIVE(msrs_content[i].control) )
				{
					msrs_content[i].counter = val;
					if ( IS_ENABLE(msrs_content[i].control) )
						ovf = 2;
				}
			}
			if ( !ovf )
				ovf = 1;
		}
	}

	/* Only P6 based Pentium M need to re-unmask the apic vector but it
	 * doesn't hurt other P6 variant */
	apic_write(APIC_LVTPC, apic_read(APIC_LVTPC) & ~APIC_LVT_MASKED);

	return ovf;
}

 
static void ppro_start(struct op_msrs const * const msrs)
{
	unsigned int low,high;
	int i;

	for (i = 0; i < num_counters; ++i) {
		if (reset_value[i]) {
			CTRL_READ(low, high, msrs, i);
			CTRL_SET_ACTIVE(low);
			CTRL_WRITE(low, high, msrs, i);
		}
	}
    /* Global Control MSR is enabled by default when system power on.
     * However, this may not hold true when xenoprof starts to run.
     */
    if ( ppro_has_global_ctrl )
        wrmsrl(MSR_CORE_PERF_GLOBAL_CTRL, (1<<num_counters) - 1);
}


static void ppro_stop(struct op_msrs const * const msrs)
{
	unsigned int low,high;
	int i;

	for (i = 0; i < num_counters; ++i) {
		if (!reset_value[i])
			continue;
		CTRL_READ(low, high, msrs, i);
		CTRL_SET_INACTIVE(low);
		CTRL_WRITE(low, high, msrs, i);
	}
    if ( ppro_has_global_ctrl )
        wrmsrl(MSR_CORE_PERF_GLOBAL_CTRL, 0);
}

static int ppro_is_arch_pmu_msr(u64 msr_index, int *type, int *index)
{
	if ( (msr_index >= MSR_IA32_PERFCTR0) &&
            (msr_index < (MSR_IA32_PERFCTR0 + num_counters)) )
	{
        	*type = MSR_TYPE_ARCH_COUNTER;
		*index = msr_index - MSR_IA32_PERFCTR0;
		return 1;
        }
        if ( (msr_index >= MSR_P6_EVNTSEL0) &&
            (msr_index < (MSR_P6_EVNTSEL0 + num_counters)) )
        {
		*type = MSR_TYPE_ARCH_CTRL;
		*index = msr_index - MSR_P6_EVNTSEL0;
		return 1;
        }

        return 0;
}

static int ppro_allocate_msr(struct vcpu *v)
{
	struct vpmu_struct *vpmu = vcpu_vpmu(v);
	struct arch_msr_pair *msr_content;

	msr_content = xmalloc_bytes( sizeof(struct arch_msr_pair) * num_counters );
	if ( !msr_content )
		goto out;
	memset(msr_content, 0, sizeof(struct arch_msr_pair) * num_counters);
	vpmu->context = (void *)msr_content;
	vpmu->flags = 0;
	vpmu->flags |= PASSIVE_DOMAIN_ALLOCATED;
	return 1;
out:
        gdprintk(XENLOG_WARNING, "Insufficient memory for oprofile, oprofile is "
                 "unavailable on domain %d vcpu %d.\n",
                 v->vcpu_id, v->domain->domain_id);
        return 0;	
}

static void ppro_free_msr(struct vcpu *v)
{
	struct vpmu_struct *vpmu = vcpu_vpmu(v);

	if ( !(vpmu->flags & PASSIVE_DOMAIN_ALLOCATED) )
		return;
	xfree(vpmu->context);
	vpmu->flags &= ~PASSIVE_DOMAIN_ALLOCATED;
}

static void ppro_load_msr(struct vcpu *v, int type, int index, u64 *msr_content)
{
	struct arch_msr_pair *msrs = vcpu_vpmu(v)->context;
	switch ( type )
	{
	case MSR_TYPE_ARCH_COUNTER:
		*msr_content = msrs[index].counter;
		break;
	case MSR_TYPE_ARCH_CTRL:
		*msr_content = msrs[index].control;
		break;
	}	
}

static void ppro_save_msr(struct vcpu *v, int type, int index, u64 msr_content)
{
	struct arch_msr_pair *msrs = vcpu_vpmu(v)->context;
	
	switch ( type )
	{
	case MSR_TYPE_ARCH_COUNTER:
		msrs[index].counter = msr_content;
		break;
	case MSR_TYPE_ARCH_CTRL:
		msrs[index].control = msr_content;
		break;
	}	
}

/*
 * Architectural performance monitoring.
 *
 * Newer Intel CPUs (Core1+) have support for architectural
 * events described in CPUID 0xA. See the IA32 SDM Vol3b.18 for details.
 * The advantage of this is that it can be done without knowing about
 * the specific CPU.
 */
void arch_perfmon_setup_counters(void)
{
	union cpuid10_eax eax;

	eax.full = cpuid_eax(0xa);

	/* Workaround for BIOS bugs in 6/15. Taken from perfmon2 */
	if (eax.split.version_id == 0 && current_cpu_data.x86 == 6 &&
	    current_cpu_data.x86_model == 15) {
		eax.split.version_id = 2;
		eax.split.num_counters = 2;
		eax.split.bit_width = 40;
	}

	num_counters = min_t(u8, eax.split.num_counters, OP_MAX_COUNTER);

	op_arch_perfmon_spec.num_counters = num_counters;
	op_arch_perfmon_spec.num_controls = num_counters;
	op_ppro_spec.num_counters = num_counters;
	op_ppro_spec.num_controls = num_counters;
}

struct op_x86_model_spec __read_mostly op_ppro_spec = {
	.num_counters = 2,
	.num_controls = 2,
	.fill_in_addresses = &ppro_fill_in_addresses,
	.setup_ctrs = &ppro_setup_ctrs,
	.check_ctrs = &ppro_check_ctrs,
	.start = &ppro_start,
	.stop = &ppro_stop,
	.is_arch_pmu_msr = &ppro_is_arch_pmu_msr,
	.allocated_msr = &ppro_allocate_msr,
	.free_msr = &ppro_free_msr,
	.load_msr = &ppro_load_msr,
	.save_msr = &ppro_save_msr
};

struct op_x86_model_spec __read_mostly op_arch_perfmon_spec = {
	/* num_counters/num_controls filled in at runtime */
	.fill_in_addresses = &ppro_fill_in_addresses,
	.setup_ctrs = &ppro_setup_ctrs,
	.check_ctrs = &ppro_check_ctrs,
	.start = &ppro_start,
	.stop = &ppro_stop,
	.is_arch_pmu_msr = &ppro_is_arch_pmu_msr,
	.allocated_msr = &ppro_allocate_msr,
	.free_msr = &ppro_free_msr,
	.load_msr = &ppro_load_msr,
	.save_msr = &ppro_save_msr
};
