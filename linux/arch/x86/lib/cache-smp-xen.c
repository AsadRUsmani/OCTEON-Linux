#include <linux/smp.h>
#include <linux/module.h>

static void __wbinvd(void *dummy)
{
	wbinvd();
}

#ifndef CONFIG_XEN /* XXX Needs hypervisor support. */
void wbinvd_on_cpu(int cpu)
{
	smp_call_function_single(cpu, __wbinvd, NULL, 1);
}
EXPORT_SYMBOL(wbinvd_on_cpu);
#endif

int wbinvd_on_all_cpus(void)
{
	/* XXX Best effort for now - needs hypervisor support. */
	return on_each_cpu(__wbinvd, NULL, 1);
}
EXPORT_SYMBOL(wbinvd_on_all_cpus);
