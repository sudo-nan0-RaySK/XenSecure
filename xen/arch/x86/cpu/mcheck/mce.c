/*
 * mce.c - x86 Machine Check Exception Reporting
 * (c) 2002 Alan Cox <alan@redhat.com>, Dave Jones <davej@codemonkey.org.uk>
 */

#include <xen/init.h>
#include <xen/types.h>
#include <xen/kernel.h>
#include <xen/config.h>
#include <xen/smp.h>
#include <xen/errno.h>
#include <xen/console.h>
#include <xen/sched.h>
#include <xen/sched-if.h>
#include <xen/cpumask.h>
#include <xen/event.h>
#include <xen/guest_access.h>
#include <xen/hypercall.h> /* for do_mca */

#include <asm/processor.h>
#include <asm/system.h>
#include <asm/msr.h>

#include "mce.h"

int mce_disabled;
invbool_param("mce", mce_disabled);
static int mce_force_broadcast;
boolean_param("mce_fb", mce_force_broadcast);
int is_mc_panic;
unsigned int nr_mce_banks;

int mce_broadcast = 0;
static uint64_t g_mcg_cap;

/* Real value in physical CTL MSR */
static uint64_t h_mcg_ctl = 0UL;
static uint64_t *h_mci_ctrl;
int firstbank;

static void intpose_init(void);
static void mcinfo_clear(struct mc_info *);

#define	SEG_PL(segsel)			((segsel) & 0x3)
#define _MC_MSRINJ_F_REQ_HWCR_WREN	(1 << 16)

#if 0
static int x86_mcerr(const char *msg, int err)
{
    gdprintk(XENLOG_WARNING, "x86_mcerr: %s, returning %d\n",
             msg != NULL ? msg : "", err);
    return err;
}
#else
#define x86_mcerr(msg, err) (err)
#endif

cpu_banks_t mca_allbanks;

int mce_verbosity;
static void __init mce_set_verbosity(char *str)
{
    if (strcmp("verbose", str) == 0)
        mce_verbosity = MCE_VERBOSE;
    else
        printk(KERN_DEBUG "Machine Check verbosity level %s not recognised"
                 "use mce_verbosity=verbose", str);
}
custom_param("mce_verbosity", mce_set_verbosity);

/* Handle unconfigured int18 (should never happen) */
static void unexpected_machine_check(struct cpu_user_regs *regs, long error_code)
{
	printk(XENLOG_ERR "CPU#%d: Unexpected int18 (Machine Check).\n",
		smp_processor_id());
}


static x86_mce_vector_t _machine_check_vector = unexpected_machine_check;

void x86_mce_vector_register(x86_mce_vector_t hdlr)
{
	_machine_check_vector = hdlr;
	wmb();
}

/* Call the installed machine check handler for this CPU setup. */

void machine_check_vector(struct cpu_user_regs *regs, long error_code)
{
	_machine_check_vector(regs, error_code);
}

/* Init machine check callback handler
 * It is used to collect additional information provided by newer
 * CPU families/models without the need to duplicate the whole handler.
 * This avoids having many handlers doing almost nearly the same and each
 * with its own tweaks ands bugs. */
static x86_mce_callback_t mc_callback_bank_extended = NULL;

void x86_mce_callback_register(x86_mce_callback_t cbfunc)
{
	mc_callback_bank_extended = cbfunc;
}

/* Machine check recoverable judgement callback handler 
 * It is used to judge whether an UC error is recoverable by software
 */
static mce_recoverable_t mc_recoverable_scan = NULL;

void mce_recoverable_register(mce_recoverable_t cbfunc)
{
    mc_recoverable_scan = cbfunc;
}

/* Judging whether to Clear Machine Check error bank callback handler
 * According to Intel latest MCA OS Recovery Writer's Guide, 
 * whether the error MCA bank needs to be cleared is decided by the mca_source
 * and MCi_status bit value. 
 */
static mce_need_clearbank_t mc_need_clearbank_scan = NULL;

void mce_need_clearbank_register(mce_need_clearbank_t cbfunc)
{
    mc_need_clearbank_scan = cbfunc;
}

/* Utility function to perform MCA bank telemetry readout and to push that
 * telemetry towards an interested dom0 for logging and diagnosis.
 * The caller - #MC handler or MCA poll function - must arrange that we
 * do not migrate cpus. */

/* XXFM Could add overflow counting? */

/* Add out_param clear_bank for Machine Check Handler Caller.
 * For Intel latest CPU, whether to clear the error bank status needs to
 * be judged by the callback function defined above.
 */
mctelem_cookie_t mcheck_mca_logout(enum mca_source who, cpu_banks_t bankmask,
    struct mca_summary *sp, cpu_banks_t* clear_bank)
{
	struct vcpu *v = current;
	struct domain *d;
	uint64_t gstatus, status, addr, misc;
	struct mcinfo_global mcg;	/* on stack */
	struct mcinfo_common *mic;
	struct mcinfo_global *mig;	/* on stack */
	mctelem_cookie_t mctc = NULL;
	uint32_t uc = 0, pcc = 0, recover, need_clear = 1 ;
	struct mc_info *mci = NULL;
	mctelem_class_t which = MC_URGENT;	/* XXXgcc */
	unsigned int cpu_nr;
	int errcnt = 0;
	int i;
	enum mca_extinfo cbret = MCA_EXTINFO_IGNORED;

	cpu_nr = smp_processor_id();
	BUG_ON(cpu_nr != v->processor);

	mca_rdmsrl(MSR_IA32_MCG_STATUS, gstatus);

	memset(&mcg, 0, sizeof (mcg));
	mcg.common.type = MC_TYPE_GLOBAL;
	mcg.common.size = sizeof (mcg);
	if (v != NULL && ((d = v->domain) != NULL)) {
		mcg.mc_domid = d->domain_id;
		mcg.mc_vcpuid = v->vcpu_id;
	} else {
		mcg.mc_domid = -1;
		mcg.mc_vcpuid = -1;
	}
	mcg.mc_gstatus = gstatus;	/* MCG_STATUS */

	switch (who) {
	case MCA_MCE_HANDLER:
	case MCA_MCE_SCAN:
		mcg.mc_flags = MC_FLAG_MCE;
		which = MC_URGENT;
		break;

	case MCA_POLLER:
	case MCA_RESET:
		mcg.mc_flags = MC_FLAG_POLLED;
		which = MC_NONURGENT;
		break;

	case MCA_CMCI_HANDLER:
		mcg.mc_flags = MC_FLAG_CMCI;
		which = MC_NONURGENT;
		break;

	default:
		BUG();
	}

	/* Retrieve detector information */
	x86_mc_get_cpu_info(cpu_nr, &mcg.mc_socketid,
	    &mcg.mc_coreid, &mcg.mc_core_threadid,
	    &mcg.mc_apicid, NULL, NULL, NULL);

	/* If no mc_recovery_scan callback handler registered,
	 * this error is not recoverable
	 */
	recover = (mc_recoverable_scan)? 1: 0;

	for (i = 0; i < 32 && i < nr_mce_banks; i++) {
		struct mcinfo_bank mcb;		/* on stack */

		/* Skip bank if corresponding bit in bankmask is clear */
		if (!test_bit(i, bankmask))
			continue;

		mca_rdmsrl(MSR_IA32_MC0_STATUS + i * 4, status);
		if (!(status & MCi_STATUS_VAL))
			continue;	/* this bank has no valid telemetry */

		/* For Intel Latest CPU CMCI/MCE Handler caller, we need to
		 * decide whether to clear bank by MCi_STATUS bit value such as
		 * OVER/UC/EN/PCC/S/AR
		 */
		if ( mc_need_clearbank_scan )
			need_clear = mc_need_clearbank_scan(who, status);

		/* If this is the first bank with valid MCA DATA, then
		 * try to reserve an entry from the urgent/nonurgent queue
		 * depending on whethere we are called from an exception or
		 * a poller;  this can fail (for example dom0 may not
		 * yet have consumed past telemetry). */
		if (errcnt == 0) {
			if ((mctc = mctelem_reserve(which)) != NULL) {
				mci = mctelem_dataptr(mctc);
				mcinfo_clear(mci);
			}
		}

		memset(&mcb, 0, sizeof (mcb));
		mcb.common.type = MC_TYPE_BANK;
		mcb.common.size = sizeof (mcb);
		mcb.mc_bank = i;
		mcb.mc_status = status;

		/* form a mask of which banks have logged uncorrected errors */
		if ((status & MCi_STATUS_UC) != 0)
			uc |= (1 << i);

		/* likewise for those with processor context corrupt */
		if ((status & MCi_STATUS_PCC) != 0)
			pcc |= (1 << i);

		if (recover && uc)
		 /* uc = 1, recover = 1, we need not panic.
		  */
			recover = mc_recoverable_scan(status);

		addr = misc = 0;

		if (status & MCi_STATUS_ADDRV) {
			mca_rdmsrl(MSR_IA32_MC0_ADDR + 4 * i, addr);
			if (mfn_valid(paddr_to_pfn(addr))) {
				d = maddr_get_owner(addr);
				if (d != NULL && (who == MCA_POLLER ||
				    who == MCA_CMCI_HANDLER))
					mcb.mc_domid = d->domain_id;
			}
		}

		if (status & MCi_STATUS_MISCV)
			mca_rdmsrl(MSR_IA32_MC0_MISC + 4 * i, misc);

		mcb.mc_addr = addr;
		mcb.mc_misc = misc;

		if (who == MCA_CMCI_HANDLER) {
			mca_rdmsrl(MSR_IA32_MC0_CTL2 + i, mcb.mc_ctrl2);
			rdtscll(mcb.mc_tsc);
		}

		/* Increment the error count;  if this is the first bank
		 * with a valid error then add the global info to the mcinfo. */
		if (errcnt++ == 0 && mci != NULL)
			x86_mcinfo_add(mci, &mcg);

		/* Add the bank data */
		if (mci != NULL)
			x86_mcinfo_add(mci, &mcb);

		if (mc_callback_bank_extended && cbret != MCA_EXTINFO_GLOBAL) {
			cbret = mc_callback_bank_extended(mci, i, status);
		}

		/* By default, need_clear = 1 */
		if (who != MCA_MCE_SCAN && need_clear)
			/* Clear status */
			mca_wrmsrl(MSR_IA32_MC0_STATUS + 4 * i, 0x0ULL);
		else if ( who == MCA_MCE_SCAN && need_clear)
			set_bit(i, clear_bank);

		wmb();
	}

	if (mci != NULL && errcnt > 0) {
		x86_mcinfo_lookup(mic, mci, MC_TYPE_GLOBAL);
		mig = container_of(mic, struct mcinfo_global, common);
		if (mic == NULL)
			;
		else if (pcc)
			mig->mc_flags |= MC_FLAG_UNCORRECTABLE;
		else if (uc)
			mig->mc_flags |= MC_FLAG_RECOVERABLE;
		else
			mig->mc_flags |= MC_FLAG_CORRECTABLE;
	}


	if (sp) {
		sp->errcnt = errcnt;
		sp->ripv = (gstatus & MCG_STATUS_RIPV) != 0;
		sp->eipv = (gstatus & MCG_STATUS_EIPV) != 0;
		sp->uc = uc;
		sp->pcc = pcc;
		sp->recoverable = recover;
	}

	return mci != NULL ? mctc : NULL;	/* may be NULL */
}

#define DOM_NORMAL	0
#define DOM0_TRAP	1
#define DOMU_TRAP	2
#define DOMU_KILLED	4

/* Shared #MC handler. */
void mcheck_cmn_handler(struct cpu_user_regs *regs, long error_code,
    cpu_banks_t bankmask)
{
	int xen_state_lost, dom0_state_lost, domU_state_lost;
	struct vcpu *v = current;
	struct domain *curdom = v->domain;
	domid_t domid = curdom->domain_id;
	int ctx_xen, ctx_dom0, ctx_domU;
	uint32_t dom_state = DOM_NORMAL;
	mctelem_cookie_t mctc = NULL;
	struct mca_summary bs;
	struct mc_info *mci = NULL;
	int irqlocked = 0;
	uint64_t gstatus;
	int ripv;

	/* This handler runs as interrupt gate. So IPIs from the
	 * polling service routine are defered until we're finished.
	 */

	/* Disable interrupts for the _vcpu_. It may not re-scheduled to
	 * another physical CPU. */
	vcpu_schedule_lock_irq(v);
	irqlocked = 1;

	/* Read global status;  if it does not indicate machine check
	 * in progress then bail as long as we have a valid ip to return to. */
	mca_rdmsrl(MSR_IA32_MCG_STATUS, gstatus);
	ripv = ((gstatus & MCG_STATUS_RIPV) != 0);
	if (!(gstatus & MCG_STATUS_MCIP) && ripv) {
		add_taint(TAINT_MACHINE_CHECK); /* questionable */
		vcpu_schedule_unlock_irq(v);
		irqlocked = 0;
		goto cmn_handler_done;
	}

	/* Go and grab error telemetry.  We must choose whether to commit
	 * for logging or dismiss the cookie that is returned, and must not
	 * reference the cookie after that action.
	 */
	mctc = mcheck_mca_logout(MCA_MCE_HANDLER, bankmask, &bs, NULL);
	if (mctc != NULL)
		mci = (struct mc_info *)mctelem_dataptr(mctc);

	/* Clear MCIP or another #MC will enter shutdown state */
	gstatus &= ~MCG_STATUS_MCIP;
	mca_wrmsrl(MSR_IA32_MCG_STATUS, gstatus);
	wmb();

	/* If no valid errors and our stack is intact, we're done */
	if (ripv && bs.errcnt == 0) {
		vcpu_schedule_unlock_irq(v);
		irqlocked = 0;
		goto cmn_handler_done;
	}

	if (bs.uc || bs.pcc)
		add_taint(TAINT_MACHINE_CHECK);

	/* Machine check exceptions will usually be for UC and/or PCC errors,
	 * but it is possible to configure machine check for some classes
	 * of corrected error.
	 *
	 * UC errors could compromise any domain or the hypervisor
	 * itself - for example a cache writeback of modified data that
	 * turned out to be bad could be for data belonging to anyone, not
	 * just the current domain.  In the absence of known data poisoning
	 * to prevent consumption of such bad data in the system we regard
	 * all UC errors as terminal.  It may be possible to attempt some
	 * heuristics based on the address affected, which guests have
	 * mappings to that mfn etc.
	 *
	 * PCC errors apply to the current context.
	 *
	 * If MCG_STATUS indicates !RIPV then even a #MC that is not UC
	 * and not PCC is terminal - the return instruction pointer
	 * pushed onto the stack is bogus.  If the interrupt context is
	 * the hypervisor or dom0 the game is over, otherwise we can
	 * limit the impact to a single domU but only if we trampoline
	 * somewhere safely - we can't return and unwind the stack.
	 * Since there is no trampoline in place we will treat !RIPV
	 * as terminal for any context.
	 */
	ctx_xen = SEG_PL(regs->cs) == 0;
	ctx_dom0 = !ctx_xen && (domid == 0);
	ctx_domU = !ctx_xen && !ctx_dom0;

	xen_state_lost = bs.uc != 0 || (ctx_xen && (bs.pcc || !ripv)) ||
	    !ripv;
	dom0_state_lost = bs.uc != 0 || (ctx_dom0 && (bs.pcc || !ripv));
	domU_state_lost = bs.uc != 0 || (ctx_domU && (bs.pcc || !ripv));

	if (xen_state_lost) {
		/* Now we are going to panic anyway. Allow interrupts, so that
		 * printk on serial console can work. */
		vcpu_schedule_unlock_irq(v);
		irqlocked = 0;

		printk("Terminal machine check exception occurred in "
		    "hypervisor context.\n");

		/* If MCG_STATUS_EIPV indicates, the IP on the stack is related
		 * to the error then it makes sense to print a stack trace.
		 * That can be useful for more detailed error analysis and/or
		 * error case studies to figure out, if we can clear
		 * xen_impacted and kill a DomU instead
		 * (i.e. if a guest only control structure is affected, but then
		 * we must ensure the bad pages are not re-used again).
		 */
		if (bs.eipv & MCG_STATUS_EIPV) {
			printk("MCE: Instruction Pointer is related to the "
			    "error, therefore print the execution state.\n");
			show_execution_state(regs);
		}

		/* Commit the telemetry so that panic flow can find it. */
		if (mctc != NULL) {
			x86_mcinfo_dump(mci);
			mctelem_commit(mctc);
		}
		mc_panic("Hypervisor state lost due to machine check "
		    "exception.\n");
		/*NOTREACHED*/
	}

	/*
	 * Xen hypervisor state is intact.  If dom0 state is lost then
	 * give it a chance to decide what to do if it has registered
	 * a handler for this event, otherwise panic.
	 *
	 * XXFM Could add some Solaris dom0 contract kill here?
	 */
	if (dom0_state_lost) {
		if (dom0 && dom0->max_vcpus && dom0->vcpu[0] &&
		    guest_has_trap_callback(dom0, 0, TRAP_machine_check)) {
			dom_state = DOM0_TRAP;
			send_guest_trap(dom0, 0, TRAP_machine_check);
			/* XXFM case of return with !ripv ??? */
		} else {
			/* Commit telemetry for panic flow. */
			if (mctc != NULL) {
				x86_mcinfo_dump(mci);
				mctelem_commit(mctc);
			}
			mc_panic("Dom0 state lost due to machine check "
			    "exception\n");
			/*NOTREACHED*/
		}
	}

	/*
	 * If a domU has lost state then send it a trap if it has registered
	 * a handler, otherwise crash the domain.
	 * XXFM Revisit this functionality.
	 */
	if (domU_state_lost) {
		if (guest_has_trap_callback(v->domain, v->vcpu_id,
		    TRAP_machine_check)) {
			dom_state = DOMU_TRAP;
			send_guest_trap(curdom, v->vcpu_id,
			    TRAP_machine_check);
		} else {
			dom_state = DOMU_KILLED;
			/* Enable interrupts. This basically results in
			 * calling sti on the *physical* cpu. But after
			 * domain_crash() the vcpu pointer is invalid.
			 * Therefore, we must unlock the irqs before killing
			 * it. */
			vcpu_schedule_unlock_irq(v);
			irqlocked = 0;

			/* DomU is impacted. Kill it and continue. */
			domain_crash(curdom);
		}
	}

	switch (dom_state) {
	case DOM0_TRAP:
	case DOMU_TRAP:
		/* Enable interrupts. */
		vcpu_schedule_unlock_irq(v);
		irqlocked = 0;

		/* guest softirqs and event callbacks are scheduled
		 * immediately after this handler exits. */
		break;
	case DOMU_KILLED:
		/* Nothing to do here. */
		break;

	case DOM_NORMAL:
		vcpu_schedule_unlock_irq(v);
		irqlocked = 0;
		break;
	}

cmn_handler_done:
	BUG_ON(irqlocked);
	BUG_ON(!ripv);

	if (bs.errcnt) {
		/* Not panicing, so forward telemetry to dom0 now if it
		 * is interested. */
		if (dom0_vmce_enabled()) {
			if (mctc != NULL)
				mctelem_commit(mctc);
			send_guest_global_virq(dom0, VIRQ_MCA);
		} else {
			x86_mcinfo_dump(mci);
			if (mctc != NULL)
				mctelem_dismiss(mctc);
		}
	} else if (mctc != NULL) {
		mctelem_dismiss(mctc);
	}
}

void mcheck_mca_clearbanks(cpu_banks_t bankmask)
{
	int i;
	uint64_t status;

	for (i = 0; i < 32 && i < nr_mce_banks; i++) {
		if (!test_bit(i, bankmask))
			continue;
		mca_rdmsrl(MSR_IA32_MC0_STATUS + i * 4, status);
		if (!(status & MCi_STATUS_VAL))
			continue;
		mca_wrmsrl(MSR_IA32_MC0_STATUS + 4 * i, 0x0ULL);
	}
}

static enum mcheck_type amd_mcheck_init(struct cpuinfo_x86 *ci)
{
	enum mcheck_type rc = mcheck_none;

	switch (ci->x86) {
	case 6:
		rc = amd_k7_mcheck_init(ci);
		break;

	default:
		/* Assume that machine check support is available.
		 * The minimum provided support is at least the K8. */
	case 0xf:
		rc = amd_k8_mcheck_init(ci);
		break;

	case 0x10 ... 0x17:
		rc = amd_f10_mcheck_init(ci);
		break;
	}

	return rc;
}

/*check the existence of Machine Check*/
int mce_available(struct cpuinfo_x86 *c)
{
	return cpu_has(c, X86_FEATURE_MCE) && cpu_has(c, X86_FEATURE_MCA);
}

static int mce_is_broadcast(struct cpuinfo_x86 *c)
{
    if (mce_force_broadcast)
        return 1;

    /* According to Intel SDM Dec, 2009, 15.10.4.1, For processors with
     * DisplayFamily_DisplayModel encoding of 06H_EH and above,
     * a MCA signal is broadcast to all logical processors in the system
     */
    if (c->x86_vendor == X86_VENDOR_INTEL && c->x86 == 6 &&
        c->x86_model >= 0xe)
            return 1;
    return 0;
}

/*
 * Check if bank 0 is usable for MCE. It isn't for AMD K7,
 * and Intel P6 family before model 0x1a.
 */
int mce_firstbank(struct cpuinfo_x86 *c)
{
	if (c->x86 == 6) {
		if (c->x86_vendor == X86_VENDOR_AMD)
			return 1;

		if (c->x86_vendor == X86_VENDOR_INTEL && c->x86_model < 0x1a)
			return 1;
	}

	return 0;
}

/* This has to be run for each processor */
void mcheck_init(struct cpuinfo_x86 *c)
{
	int i, broadcast;
	enum mcheck_type inited = mcheck_none;
	static enum mcheck_type g_type = mcheck_unset;
    static int broadcast_check;

	if (mce_disabled == 1) {
		dprintk(XENLOG_INFO, "MCE support disabled by bootparam\n");
		return;
	}

    broadcast = mce_is_broadcast(c);
    if (broadcast_check && (broadcast != mce_broadcast) )
            dprintk(XENLOG_INFO,
                "CPUs have mixed broadcast support"
                "may cause undetermined result!!!\n");

    broadcast_check = 1;
    if (broadcast)
        mce_broadcast = broadcast;

	for (i = 0; i < MAX_NR_BANKS; i++)
		set_bit(i,mca_allbanks);

	/* Enforce at least MCE support in CPUID information.  Individual
	 * families may also need to enforce a check for MCA support. */
	if (!cpu_has(c, X86_FEATURE_MCE)) {
		printk(XENLOG_INFO "CPU%i: No machine check support available\n",
			smp_processor_id());
		return;
	}

	intpose_init();
	mctelem_init(sizeof (struct mc_info));

	switch (c->x86_vendor) {
	case X86_VENDOR_AMD:
		inited = amd_mcheck_init(c);
		break;

	case X86_VENDOR_INTEL:
		switch (c->x86) {
		case 6:
		case 15:
			inited = intel_mcheck_init(c);
			break;
		}
		break;

	default:
		break;
	}

    if ( !h_mci_ctrl )
    {
        h_mci_ctrl = xmalloc_array(uint64_t, nr_mce_banks);
        if (!h_mci_ctrl)
        {
            dprintk(XENLOG_INFO, "Failed to alloc h_mci_ctrl\n");
            return;
        }
        /* Don't care banks before firstbank */
        memset(h_mci_ctrl, 0xff, sizeof(h_mci_ctrl));
        for (i = firstbank; i < nr_mce_banks; i++)
            rdmsrl(MSR_IA32_MC0_CTL + 4*i, h_mci_ctrl[i]);
    }
    if (g_mcg_cap & MCG_CTL_P)
        rdmsrl(MSR_IA32_MCG_CTL, h_mcg_ctl);
    set_poll_bankmask(c);

	if (inited != g_type) {
		char prefix[20];
		static const char *const type_str[] = {
			[mcheck_amd_famXX] = "AMD",
			[mcheck_amd_k7] = "AMD K7",
			[mcheck_amd_k8] = "AMD K8",
			[mcheck_intel] = "Intel"
		};

		snprintf(prefix, ARRAY_SIZE(prefix),
			 g_type != mcheck_unset ? XENLOG_WARNING "CPU%i: "
						: XENLOG_INFO,
			 smp_processor_id());
		BUG_ON(inited >= ARRAY_SIZE(type_str));
		switch (inited) {
		default:
			printk("%s%s machine check reporting enabled\n",
			       prefix, type_str[inited]);
			break;
		case mcheck_amd_famXX:
			printk("%s%s Fam%xh machine check reporting enabled\n",
			       prefix, type_str[inited], c->x86);
			break;
		case mcheck_none:
			printk("%sNo machine check initialization\n", prefix);
			break;
		}

		g_type = inited;
	}
}

u64 mce_cap_init(void)
{
    u32 l, h;
    u64 value;

    rdmsr(MSR_IA32_MCG_CAP, l, h);
    value = ((u64)h << 32) | l;
    /* For Guest vMCE usage */
    g_mcg_cap = value & ~MCG_CMCI_P;

    if (l & MCG_CTL_P) /* Control register present ? */
        wrmsr(MSR_IA32_MCG_CTL, 0xffffffff, 0xffffffff);

    nr_mce_banks = l & MCG_CAP_COUNT;
    if ( nr_mce_banks > MAX_NR_BANKS )
    {
        printk(KERN_WARNING "MCE: exceed max mce banks\n");
        g_mcg_cap = (g_mcg_cap & ~MCG_CAP_COUNT) | MAX_NR_BANKS;
    }

    return value;
}

/* Guest vMCE# MSRs virtualization ops (rdmsr/wrmsr) */
void mce_init_msr(struct domain *d)
{
    d->arch.vmca_msrs.mcg_status = 0x0;
    d->arch.vmca_msrs.mcg_cap = g_mcg_cap;
    d->arch.vmca_msrs.mcg_ctl = ~(uint64_t)0x0;
    d->arch.vmca_msrs.nr_injection = 0;
    memset(d->arch.vmca_msrs.mci_ctl, ~0,
           sizeof(d->arch.vmca_msrs.mci_ctl));
    INIT_LIST_HEAD(&d->arch.vmca_msrs.impact_header);
    spin_lock_init(&d->arch.vmca_msrs.lock);
}

int mce_rdmsr(uint32_t msr, uint64_t *val)
{
    struct domain *d = current->domain;
    int ret = 1;
    unsigned int bank;
    struct bank_entry *entry = NULL;

    *val = 0;
    spin_lock(&d->arch.vmca_msrs.lock);

    switch ( msr )
    {
    case MSR_IA32_MCG_STATUS:
        *val = d->arch.vmca_msrs.mcg_status;
        if (*val)
            mce_printk(MCE_VERBOSE,
                "MCE: rdmsr MCG_STATUS 0x%"PRIx64"\n", *val);
        break;
    case MSR_IA32_MCG_CAP:
        *val = d->arch.vmca_msrs.mcg_cap;
        mce_printk(MCE_VERBOSE, "MCE: rdmsr MCG_CAP 0x%"PRIx64"\n",
            *val);
        break;
    case MSR_IA32_MCG_CTL:
        /* Always 0 if no CTL support */
        *val = d->arch.vmca_msrs.mcg_ctl & h_mcg_ctl;
        mce_printk(MCE_VERBOSE, "MCE: rdmsr MCG_CTL 0x%"PRIx64"\n",
            *val);
        break;
    case MSR_IA32_MC0_CTL ... MSR_IA32_MC0_CTL + 4 * MAX_NR_BANKS - 1:
        bank = (msr - MSR_IA32_MC0_CTL) / 4;
        if ( bank >= (d->arch.vmca_msrs.mcg_cap & MCG_CAP_COUNT) )
        {
            mce_printk(MCE_QUIET, "MCE: MSR %x is not MCA MSR\n", msr);
            ret = 0;
            break;
        }
        switch (msr & (MSR_IA32_MC0_CTL | 3))
        {
        case MSR_IA32_MC0_CTL:
            *val = d->arch.vmca_msrs.mci_ctl[bank] &
                    (h_mci_ctrl ? h_mci_ctrl[bank] : ~0UL);
            mce_printk(MCE_VERBOSE, "MCE: rdmsr MC%u_CTL 0x%"PRIx64"\n",
                     bank, *val);
            break;
        case MSR_IA32_MC0_STATUS:
            /* Only error bank is read. Non-error banks simply return. */
            if ( !list_empty(&d->arch.vmca_msrs.impact_header) )
            {
                entry = list_entry(d->arch.vmca_msrs.impact_header.next,
                                   struct bank_entry, list);
                if (entry->bank == bank) {
                    *val = entry->mci_status;
                    mce_printk(MCE_VERBOSE,
                             "MCE: rd MC%u_STATUS in vMCE# context "
                             "value 0x%"PRIx64"\n", bank, *val);
                }
                else
                    entry = NULL;
            }
            break;
        case MSR_IA32_MC0_ADDR:
            if ( !list_empty(&d->arch.vmca_msrs.impact_header) )
            {
                entry = list_entry(d->arch.vmca_msrs.impact_header.next,
                                   struct bank_entry, list);
                if ( entry->bank == bank )
                {
                    *val = entry->mci_addr;
                    mce_printk(MCE_VERBOSE,
                             "MCE: rdmsr MC%u_ADDR in vMCE# context "
                             "0x%"PRIx64"\n", bank, *val);
                }
            }
            break;
        case MSR_IA32_MC0_MISC:
            if ( !list_empty(&d->arch.vmca_msrs.impact_header) )
            {
                entry = list_entry(d->arch.vmca_msrs.impact_header.next,
                                   struct bank_entry, list);
                if ( entry->bank == bank )
                {
                    *val = entry->mci_misc;
                    mce_printk(MCE_VERBOSE,
                             "MCE: rd MC%u_MISC in vMCE# context "
                             "0x%"PRIx64"\n", bank, *val);
                }
            }
            break;
        }
        break;
    default:
        switch ( boot_cpu_data.x86_vendor )
        {
        case X86_VENDOR_INTEL:
            ret = intel_mce_rdmsr(msr, val);
            break;
        default:
            ret = 0;
            break;
        }
        break;
    }

    spin_unlock(&d->arch.vmca_msrs.lock);
    return ret;
}

int mce_wrmsr(u32 msr, u64 val)
{
    struct domain *d = current->domain;
    struct bank_entry *entry = NULL;
    unsigned int bank;
    int ret = 1;

    if ( !g_mcg_cap )
        return 0;

    spin_lock(&d->arch.vmca_msrs.lock);

    switch ( msr )
    {
    case MSR_IA32_MCG_CTL:
        d->arch.vmca_msrs.mcg_ctl = val;
        break;
    case MSR_IA32_MCG_STATUS:
        d->arch.vmca_msrs.mcg_status = val;
        mce_printk(MCE_VERBOSE, "MCE: wrmsr MCG_STATUS %"PRIx64"\n", val);
        /* For HVM guest, this is the point for deleting vMCE injection node */
        if ( d->is_hvm && (d->arch.vmca_msrs.nr_injection > 0) )
        {
            d->arch.vmca_msrs.nr_injection--; /* Should be 0 */
            if ( !list_empty(&d->arch.vmca_msrs.impact_header) )
            {
                entry = list_entry(d->arch.vmca_msrs.impact_header.next,
                    struct bank_entry, list);
                if ( entry->mci_status & MCi_STATUS_VAL )
                    mce_printk(MCE_QUIET, "MCE: MCi_STATUS MSR should have "
                                "been cleared before write MCG_STATUS MSR\n");

                mce_printk(MCE_QUIET, "MCE: Delete HVM last injection "
                                "Node, nr_injection %u\n",
                                d->arch.vmca_msrs.nr_injection);
                list_del(&entry->list);
                xfree(entry);
            }
            else
                mce_printk(MCE_QUIET, "MCE: Not found HVM guest"
                    " last injection Node, something Wrong!\n");
        }
        break;
    case MSR_IA32_MCG_CAP:
        mce_printk(MCE_QUIET, "MCE: MCG_CAP is read-only\n");
        ret = -1;
        break;
    case MSR_IA32_MC0_CTL ... MSR_IA32_MC0_CTL + 4 * MAX_NR_BANKS - 1:
        bank = (msr - MSR_IA32_MC0_CTL) / 4;
        if ( bank >= (d->arch.vmca_msrs.mcg_cap & MCG_CAP_COUNT) )
        {
            mce_printk(MCE_QUIET, "MCE: MSR %x is not MCA MSR\n", msr);
            ret = 0;
            break;
        }
        switch ( msr & (MSR_IA32_MC0_CTL | 3) )
        {
        case MSR_IA32_MC0_CTL:
            d->arch.vmca_msrs.mci_ctl[bank] = val;
            break;
        case MSR_IA32_MC0_STATUS:
            /* Give the first entry of the list, it corresponds to current
             * vMCE# injection. When vMCE# is finished processing by the
             * the guest, this node will be deleted.
             * Only error bank is written. Non-error banks simply return.
             */
            if ( !list_empty(&d->arch.vmca_msrs.impact_header) )
            {
                entry = list_entry(d->arch.vmca_msrs.impact_header.next,
                                   struct bank_entry, list);
                if ( entry->bank == bank )
                    entry->mci_status = val;
                mce_printk(MCE_VERBOSE,
                         "MCE: wr MC%u_STATUS %"PRIx64" in vMCE#\n",
                         bank, val);
            }
            else
                mce_printk(MCE_VERBOSE,
                         "MCE: wr MC%u_STATUS %"PRIx64"\n", bank, val);
            break;
        case MSR_IA32_MC0_ADDR:
            mce_printk(MCE_QUIET, "MCE: MC%u_ADDR is read-only\n", bank);
            ret = -1;
            break;
        case MSR_IA32_MC0_MISC:
            mce_printk(MCE_QUIET, "MCE: MC%u_MISC is read-only\n", bank);
            ret = -1;
            break;
        }
        break;
    default:
        switch ( boot_cpu_data.x86_vendor )
        {
        case X86_VENDOR_INTEL:
            ret = intel_mce_wrmsr(msr, val);
            break;
        default:
            ret = 0;
            break;
        }
        break;
    }

    spin_unlock(&d->arch.vmca_msrs.lock);
    return ret;
}

static void mcinfo_clear(struct mc_info *mi)
{
	memset(mi, 0, sizeof(struct mc_info));
	x86_mcinfo_nentries(mi) = 0;
}

int x86_mcinfo_add(struct mc_info *mi, void *mcinfo)
{
	int i;
	unsigned long end1, end2;
	struct mcinfo_common *mic, *mic_base, *mic_index;

	mic = (struct mcinfo_common *)mcinfo;
	mic_index = mic_base = x86_mcinfo_first(mi);

	/* go to first free entry */
	for (i = 0; i < x86_mcinfo_nentries(mi); i++) {
		mic_index = x86_mcinfo_next(mic_index);
	}

	/* check if there is enough size */
	end1 = (unsigned long)((uint8_t *)mic_base + sizeof(struct mc_info));
	end2 = (unsigned long)((uint8_t *)mic_index + mic->size);

	if (end1 < end2)
		return x86_mcerr("mcinfo_add: no more sparc", -ENOSPC);

	/* there's enough space. add entry. */
	memcpy(mic_index, mic, mic->size);
	x86_mcinfo_nentries(mi)++;

	return 0;
}

/* Dump machine check information in a format,
 * mcelog can parse. This is used only when
 * Dom0 does not take the notification. */
void x86_mcinfo_dump(struct mc_info *mi)
{
	struct mcinfo_common *mic = NULL;
	struct mcinfo_global *mc_global;
	struct mcinfo_bank *mc_bank;

	/* first print the global info */
	x86_mcinfo_lookup(mic, mi, MC_TYPE_GLOBAL);
	if (mic == NULL)
		return;
	mc_global = (struct mcinfo_global *)mic;
	if (mc_global->mc_flags & MC_FLAG_MCE) {
		printk(XENLOG_WARNING
			"CPU%d: Machine Check Exception: %16"PRIx64"\n",
			mc_global->mc_coreid, mc_global->mc_gstatus);
	} else {
		printk(XENLOG_WARNING "MCE: The hardware reports a non "
			"fatal, correctable incident occurred on "
			"CPU %d.\n",
			mc_global->mc_coreid);
	}

	/* then the bank information */
	x86_mcinfo_lookup(mic, mi, MC_TYPE_BANK); /* finds the first entry */
	do {
		if (mic == NULL)
			return;
		if (mic->type != MC_TYPE_BANK)
			goto next;

		mc_bank = (struct mcinfo_bank *)mic;

		printk(XENLOG_WARNING "Bank %d: %16"PRIx64,
			mc_bank->mc_bank,
			mc_bank->mc_status);
		if (mc_bank->mc_status & MCi_STATUS_MISCV)
			printk("[%16"PRIx64"]", mc_bank->mc_misc);
		if (mc_bank->mc_status & MCi_STATUS_ADDRV)
			printk(" at %16"PRIx64, mc_bank->mc_addr);

		printk("\n");
next:
		mic = x86_mcinfo_next(mic); /* next entry */
		if ((mic == NULL) || (mic->size == 0))
			break;
	} while (1);
}

static void do_mc_get_cpu_info(void *v)
{
	int cpu = smp_processor_id();
	int cindex, cpn;
	struct cpuinfo_x86 *c;
	xen_mc_logical_cpu_t *log_cpus, *xcp;
	uint32_t junk, ebx;

	log_cpus = v;
	c = &cpu_data[cpu];
	cindex = 0;
	cpn = cpu - 1;

	/*
	 * Deal with sparse masks, condensed into a contig array.
	 */
	while (cpn >= 0) {
		if (cpu_isset(cpn, cpu_online_map))
			cindex++;
		cpn--;
	}

	xcp = &log_cpus[cindex];
	c = &cpu_data[cpu];
	xcp->mc_cpunr = cpu;
	x86_mc_get_cpu_info(cpu, &xcp->mc_chipid,
	    &xcp->mc_coreid, &xcp->mc_threadid,
	    &xcp->mc_apicid, &xcp->mc_ncores,
	    &xcp->mc_ncores_active, &xcp->mc_nthreads);
	xcp->mc_cpuid_level = c->cpuid_level;
	xcp->mc_family = c->x86;
	xcp->mc_vendor = c->x86_vendor;
	xcp->mc_model = c->x86_model;
	xcp->mc_step = c->x86_mask;
	xcp->mc_cache_size = c->x86_cache_size;
	xcp->mc_cache_alignment = c->x86_cache_alignment;
	memcpy(xcp->mc_vendorid, c->x86_vendor_id, sizeof xcp->mc_vendorid);
	memcpy(xcp->mc_brandid, c->x86_model_id, sizeof xcp->mc_brandid);
	memcpy(xcp->mc_cpu_caps, c->x86_capability, sizeof xcp->mc_cpu_caps);

	/*
	 * This part needs to run on the CPU itself.
	 */
	xcp->mc_nmsrvals = __MC_NMSRS;
	xcp->mc_msrvalues[0].reg = MSR_IA32_MCG_CAP;
	rdmsrl(MSR_IA32_MCG_CAP, xcp->mc_msrvalues[0].value);

	if (c->cpuid_level >= 1) {
		cpuid(1, &junk, &ebx, &junk, &junk);
		xcp->mc_clusterid = (ebx >> 24) & 0xff;
	} else
		xcp->mc_clusterid = hard_smp_processor_id();
}


void x86_mc_get_cpu_info(unsigned cpu, uint32_t *chipid, uint16_t *coreid,
			 uint16_t *threadid, uint32_t *apicid,
			 unsigned *ncores, unsigned *ncores_active,
			 unsigned *nthreads)
{
	struct cpuinfo_x86 *c;

	*apicid = cpu_physical_id(cpu);
	c = &cpu_data[cpu];
	if (c->apicid == BAD_APICID) {
		*chipid = cpu;
		*coreid = 0;
		*threadid = 0;
		if (ncores != NULL)
			*ncores = 1;
		if (ncores_active != NULL)
			*ncores_active = 1;
		if (nthreads != NULL)
			*nthreads = 1;
	} else {
		*chipid = phys_proc_id[cpu];
		if (c->x86_max_cores > 1)
			*coreid = cpu_core_id[cpu];
		else
			*coreid = 0;
		*threadid = c->apicid & ((1 << (c->x86_num_siblings - 1)) - 1);
		if (ncores != NULL)
			*ncores = c->x86_max_cores;
		if (ncores_active != NULL)
			*ncores_active = c->booted_cores;
		if (nthreads != NULL)
			*nthreads = c->x86_num_siblings;
	}
}

#define	INTPOSE_NENT	50

static struct intpose_ent {
	unsigned  int cpu_nr;
	uint64_t msr;
	uint64_t val;
} intpose_arr[INTPOSE_NENT];

static void intpose_init(void)
{
	static int done;
	int i;

	if (done++ > 0)
		return;

	for (i = 0; i < INTPOSE_NENT; i++) {
		intpose_arr[i].cpu_nr = -1;
	}

}

struct intpose_ent *intpose_lookup(unsigned int cpu_nr, uint64_t msr,
    uint64_t *valp)
{
	int i;

	for (i = 0; i < INTPOSE_NENT; i++) {
		if (intpose_arr[i].cpu_nr == cpu_nr &&
		    intpose_arr[i].msr == msr) {
			if (valp != NULL)
				*valp = intpose_arr[i].val;
			return &intpose_arr[i];
		}
	}

	return NULL;
}

static void intpose_add(unsigned int cpu_nr, uint64_t msr, uint64_t val)
{
	struct intpose_ent *ent;
	int i;

	if ((ent = intpose_lookup(cpu_nr, msr, NULL)) != NULL) {
		ent->val = val;
		return;
	}

	for (i = 0, ent = &intpose_arr[0]; i < INTPOSE_NENT; i++, ent++) {
		if (ent->cpu_nr == -1) {
			ent->cpu_nr = cpu_nr;
			ent->msr = msr;
			ent->val = val;
			return;
		}
	}

	printk("intpose_add: interpose array full - request dropped\n");
}

void intpose_inval(unsigned int cpu_nr, uint64_t msr)
{
	struct intpose_ent *ent;

	if ((ent = intpose_lookup(cpu_nr, msr, NULL)) != NULL) {
		ent->cpu_nr = -1;
	}
}

#define	IS_MCA_BANKREG(r) \
    ((r) >= MSR_IA32_MC0_CTL && \
    (r) <= MSR_IA32_MC0_MISC + (nr_mce_banks - 1) * 4 && \
    ((r) - MSR_IA32_MC0_CTL) % 4 != 0)	/* excludes MCi_CTL */

int mca_ctl_conflict(struct mcinfo_bank *bank, struct domain *d)
{
    int bank_nr;

    if ( !bank || !d || !h_mci_ctrl )
        return 1;

    /* Will MCE happen in host if If host mcg_ctl is 0? */
    if ( ~d->arch.vmca_msrs.mcg_ctl & h_mcg_ctl )
        return 1;

    bank_nr = bank->mc_bank;
    if (~d->arch.vmca_msrs.mci_ctl[bank_nr] & h_mci_ctrl[bank_nr] )
        return 1;
    return 0;
}

static int x86_mc_msrinject_verify(struct xen_mc_msrinject *mci)
{
	struct cpuinfo_x86 *c;
	int i, errs = 0;

	c = &cpu_data[smp_processor_id()];

	for (i = 0; i < mci->mcinj_count; i++) {
		uint64_t reg = mci->mcinj_msr[i].reg;
		const char *reason = NULL;

		if (IS_MCA_BANKREG(reg)) {
			if (c->x86_vendor == X86_VENDOR_AMD) {
				/* On AMD we can set MCi_STATUS_WREN in the
				 * HWCR MSR to allow non-zero writes to banks
				 * MSRs not to #GP.  The injector in dom0
				 * should set that bit, but we detect when it
				 * is necessary and set it as a courtesy to
				 * avoid #GP in the hypervisor. */
				mci->mcinj_flags |=
				    _MC_MSRINJ_F_REQ_HWCR_WREN;
				continue;
			} else {
				/* No alternative but to interpose, so require
				 * that the injector specified as such. */
				if (!(mci->mcinj_flags &
				    MC_MSRINJ_F_INTERPOSE)) {
					reason = "must specify interposition";
				}
			}
		} else {
			switch (reg) {
			/* MSRs acceptable on all x86 cpus */
			case MSR_IA32_MCG_STATUS:
				break;

			/* MSRs that the HV will take care of */
			case MSR_K8_HWCR:
				if (c->x86_vendor == X86_VENDOR_AMD)
					reason = "HV will operate HWCR";
				else
					reason ="only supported on AMD";
				break;

			default:
				reason = "not a recognized MCA MSR";
				break;
			}
		}

		if (reason != NULL) {
			printk("HV MSR INJECT ERROR: MSR 0x%llx %s\n",
			    (unsigned long long)mci->mcinj_msr[i].reg, reason);
			errs++;
		}
	}

	return !errs;
}

static uint64_t x86_mc_hwcr_wren(void)
{
	uint64_t old;

	rdmsrl(MSR_K8_HWCR, old);

	if (!(old & K8_HWCR_MCi_STATUS_WREN)) {
		uint64_t new = old | K8_HWCR_MCi_STATUS_WREN;
		wrmsrl(MSR_K8_HWCR, new);
	}

	return old;
}

static void x86_mc_hwcr_wren_restore(uint64_t hwcr)
{
	if (!(hwcr & K8_HWCR_MCi_STATUS_WREN))
		wrmsrl(MSR_K8_HWCR, hwcr);
}

static void x86_mc_msrinject(void *data)
{
	struct xen_mc_msrinject *mci = data;
	struct mcinfo_msr *msr;
	struct cpuinfo_x86 *c;
	uint64_t hwcr = 0;
	int intpose;
	int i;

	c = &cpu_data[smp_processor_id()];

	if (mci->mcinj_flags & _MC_MSRINJ_F_REQ_HWCR_WREN)
		hwcr = x86_mc_hwcr_wren();

	intpose = (mci->mcinj_flags & MC_MSRINJ_F_INTERPOSE) != 0;

	for (i = 0, msr = &mci->mcinj_msr[0];
	    i < mci->mcinj_count; i++, msr++) {
		printk("HV MSR INJECT (%s) target %u actual %u MSR 0x%llx "
		    "<-- 0x%llx\n",
		    intpose ?  "interpose" : "hardware",
		    mci->mcinj_cpunr, smp_processor_id(),
		    (unsigned long long)msr->reg,
		    (unsigned long long)msr->value);

		if (intpose)
			intpose_add(mci->mcinj_cpunr, msr->reg, msr->value);
		else
			wrmsrl(msr->reg, msr->value);
	}

	if (mci->mcinj_flags & _MC_MSRINJ_F_REQ_HWCR_WREN)
		x86_mc_hwcr_wren_restore(hwcr);
}

/*ARGSUSED*/
static void x86_mc_mceinject(void *data)
{
	printk("Simulating #MC on cpu %d\n", smp_processor_id());
	__asm__ __volatile__("int $0x12");
}

#if BITS_PER_LONG == 64

#define	ID2COOKIE(id)	((mctelem_cookie_t)(id))
#define	COOKIE2ID(c) ((uint64_t)(c))

#elif BITS_PER_LONG == 32

#define	ID2COOKIE(id)	((mctelem_cookie_t)(uint32_t)((id) & 0xffffffffU))
#define	COOKIE2ID(c)	((uint64_t)(uint32_t)(c))

#elif defined(BITS_PER_LONG)
#error BITS_PER_LONG has unexpected value
#else
#error BITS_PER_LONG definition absent
#endif

#ifdef CONFIG_COMPAT
# include <compat/arch-x86/xen-mca.h>

# define xen_mcinfo_msr              mcinfo_msr
CHECK_mcinfo_msr;
# undef xen_mcinfo_msr
# undef CHECK_mcinfo_msr
# define CHECK_mcinfo_msr            struct mcinfo_msr

# define xen_mcinfo_common           mcinfo_common
CHECK_mcinfo_common;
# undef xen_mcinfo_common
# undef CHECK_mcinfo_common
# define CHECK_mcinfo_common         struct mcinfo_common

CHECK_FIELD_(struct, mc_fetch, flags);
CHECK_FIELD_(struct, mc_fetch, fetch_id);
# define CHECK_compat_mc_fetch       struct mc_fetch

CHECK_FIELD_(struct, mc_physcpuinfo, ncpus);
# define CHECK_compat_mc_physcpuinfo struct mc_physcpuinfo

CHECK_mc;
# undef CHECK_compat_mc_fetch
# undef CHECK_compat_mc_physcpuinfo

# define xen_mc_info                 mc_info
CHECK_mc_info;
# undef xen_mc_info

# define xen_mcinfo_global           mcinfo_global
CHECK_mcinfo_global;
# undef xen_mcinfo_global

# define xen_mcinfo_bank             mcinfo_bank
CHECK_mcinfo_bank;
# undef xen_mcinfo_bank

# define xen_mcinfo_extended         mcinfo_extended
CHECK_mcinfo_extended;
# undef xen_mcinfo_extended

# define xen_mcinfo_recovery         mcinfo_recovery
# define xen_cpu_offline_action      cpu_offline_action
# define xen_page_offline_action     page_offline_action
CHECK_mcinfo_recovery;
# undef xen_cpu_offline_action
# undef xen_page_offline_action
# undef xen_mcinfo_recovery
#else
# define compat_mc_fetch xen_mc_fetch
# define compat_mc_physcpuinfo xen_mc_physcpuinfo
# define compat_handle_is_null guest_handle_is_null
# define copy_to_compat copy_to_guest
#endif

/* Machine Check Architecture Hypercall */
long do_mca(XEN_GUEST_HANDLE(xen_mc_t) u_xen_mc)
{
	long ret = 0;
	struct xen_mc curop, *op = &curop;
	struct vcpu *v = current;
	union {
		struct xen_mc_fetch *nat;
		struct compat_mc_fetch *cmp;
	} mc_fetch;
	union {
		struct xen_mc_physcpuinfo *nat;
		struct compat_mc_physcpuinfo *cmp;
	} mc_physcpuinfo;
	uint32_t flags, cmdflags;
	int nlcpu;
	xen_mc_logical_cpu_t *log_cpus = NULL;
	mctelem_cookie_t mctc;
	mctelem_class_t which;
	unsigned int target;
	struct xen_mc_msrinject *mc_msrinject;
	struct xen_mc_mceinject *mc_mceinject;

	if (!IS_PRIV(v->domain) )
		return x86_mcerr(NULL, -EPERM);

	if ( copy_from_guest(op, u_xen_mc, 1) )
		return x86_mcerr("do_mca: failed copyin of xen_mc_t", -EFAULT);

	if ( op->interface_version != XEN_MCA_INTERFACE_VERSION )
		return x86_mcerr("do_mca: interface version mismatch", -EACCES);

	switch (op->cmd) {
	case XEN_MC_fetch:
		mc_fetch.nat = &op->u.mc_fetch;
		cmdflags = mc_fetch.nat->flags;

		switch (cmdflags & (XEN_MC_NONURGENT | XEN_MC_URGENT)) {
		case XEN_MC_NONURGENT:
			which = MC_NONURGENT;
			break;

		case XEN_MC_URGENT:
			which = MC_URGENT;
			break;

		default:
			return x86_mcerr("do_mca fetch: bad cmdflags", -EINVAL);
		}

		flags = XEN_MC_OK;

		if (cmdflags & XEN_MC_ACK) {
			mctelem_cookie_t cookie = ID2COOKIE(mc_fetch.nat->fetch_id);
			mctelem_ack(which, cookie);
		} else {
			if (!is_pv_32on64_vcpu(v)
			    ? guest_handle_is_null(mc_fetch.nat->data)
			    : compat_handle_is_null(mc_fetch.cmp->data))
				return x86_mcerr("do_mca fetch: guest buffer "
				    "invalid", -EINVAL);

			if ((mctc = mctelem_consume_oldest_begin(which))) {
				struct mc_info *mcip = mctelem_dataptr(mctc);
				if (!is_pv_32on64_vcpu(v)
				    ? copy_to_guest(mc_fetch.nat->data, mcip, 1)
				    : copy_to_compat(mc_fetch.cmp->data,
						     mcip, 1)) {
					ret = -EFAULT;
					flags |= XEN_MC_FETCHFAILED;
					mc_fetch.nat->fetch_id = 0;
				} else {
					mc_fetch.nat->fetch_id = COOKIE2ID(mctc);
				}
				mctelem_consume_oldest_end(mctc);
			} else {
				/* There is no data */
				flags |= XEN_MC_NODATA;
				mc_fetch.nat->fetch_id = 0;
			}

			mc_fetch.nat->flags = flags;
			if (copy_to_guest(u_xen_mc, op, 1) != 0)
				ret = -EFAULT;
		}

		break;

	case XEN_MC_notifydomain:
		return x86_mcerr("do_mca notify unsupported", -EINVAL);

	case XEN_MC_physcpuinfo:
		mc_physcpuinfo.nat = &op->u.mc_physcpuinfo;
		nlcpu = num_online_cpus();

		if (!is_pv_32on64_vcpu(v)
		    ? !guest_handle_is_null(mc_physcpuinfo.nat->info)
		    : !compat_handle_is_null(mc_physcpuinfo.cmp->info)) {
			if (mc_physcpuinfo.nat->ncpus <= 0)
				return x86_mcerr("do_mca cpuinfo: ncpus <= 0",
				    -EINVAL);
			nlcpu = min(nlcpu, (int)mc_physcpuinfo.nat->ncpus);
			log_cpus = xmalloc_array(xen_mc_logical_cpu_t, nlcpu);
			if (log_cpus == NULL)
				return x86_mcerr("do_mca cpuinfo", -ENOMEM);

			if (on_each_cpu(do_mc_get_cpu_info, log_cpus, 1)) {
				xfree(log_cpus);
				return x86_mcerr("do_mca cpuinfo", -EIO);
			}
			if (!is_pv_32on64_vcpu(v)
			    ? copy_to_guest(mc_physcpuinfo.nat->info,
					    log_cpus, nlcpu)
			    : copy_to_compat(mc_physcpuinfo.cmp->info,
					    log_cpus, nlcpu))
				ret = -EFAULT;
			xfree(log_cpus);
		}

		mc_physcpuinfo.nat->ncpus = nlcpu;

		if (copy_to_guest(u_xen_mc, op, 1))
			return x86_mcerr("do_mca cpuinfo", -EFAULT);

		break;

	case XEN_MC_msrinject:
		if (nr_mce_banks == 0)
			return x86_mcerr("do_mca inject", -ENODEV);

		mc_msrinject = &op->u.mc_msrinject;
		target = mc_msrinject->mcinj_cpunr;

		if (target >= NR_CPUS)
			return x86_mcerr("do_mca inject: bad target", -EINVAL);

		if (!cpu_isset(target, cpu_online_map))
			return x86_mcerr("do_mca inject: target offline",
			    -EINVAL);

		if (mc_msrinject->mcinj_count == 0)
			return 0;

		if (!x86_mc_msrinject_verify(mc_msrinject))
			return x86_mcerr("do_mca inject: illegal MSR", -EINVAL);

		add_taint(TAINT_ERROR_INJECT);

		on_selected_cpus(cpumask_of(target), x86_mc_msrinject,
				 mc_msrinject, 1);

		break;

	case XEN_MC_mceinject:
		if (nr_mce_banks == 0)
			return x86_mcerr("do_mca #MC", -ENODEV);

		mc_mceinject = &op->u.mc_mceinject;
		target = mc_mceinject->mceinj_cpunr;

		if (target >= NR_CPUS)
			return x86_mcerr("do_mca #MC: bad target", -EINVAL);

		if (!cpu_isset(target, cpu_online_map))
			return x86_mcerr("do_mca #MC: target offline", -EINVAL);

		add_taint(TAINT_ERROR_INJECT);

        if ( mce_broadcast )
            on_each_cpu(x86_mc_mceinject, mc_mceinject, 1);
        else
            on_selected_cpus(cpumask_of(target), x86_mc_mceinject,
                  mc_mceinject, 1);
		break;

	default:
		return x86_mcerr("do_mca: bad command", -EINVAL);
	}

	return ret;
}
void set_poll_bankmask(struct cpuinfo_x86 *c)
{

    if (cmci_support && !mce_disabled) {
        memcpy(&(__get_cpu_var(poll_bankmask)),
                &(__get_cpu_var(no_cmci_banks)), sizeof(cpu_banks_t));
    }
    else {
        memcpy(&(get_cpu_var(poll_bankmask)), &mca_allbanks, sizeof(cpu_banks_t));
        if (mce_firstbank(c))
            clear_bit(0, get_cpu_var(poll_bankmask));
    }
}
void mc_panic(char *s)
{
    is_mc_panic = 1;
    console_force_unlock();
    printk("Fatal machine check: %s\n", s);
    printk("\n"
           "****************************************\n"
           "\n"
           "   The processor has reported a hardware error which cannot\n"
           "   be recovered from.  Xen will now reboot the machine.\n");
    panic("HARDWARE ERROR");
}
