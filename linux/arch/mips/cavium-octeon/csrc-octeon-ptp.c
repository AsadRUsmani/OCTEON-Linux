/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 2009, 2010 Cavium Networks
 */
#include <linux/clocksource.h>
#include <linux/init.h>
#include <asm/time.h>

#include <asm/octeon/octeon.h>
#include <asm/octeon/cvmx-mio-defs.h>

static cycle_t octeon_ptp_clock_read(struct clocksource *cs)
{
	/* CN63XX pass 1.x has an errata where you must read this register
		twice to get the correct result */
	if (OCTEON_IS_MODEL(OCTEON_CN63XX_PASS1_X))
		cvmx_read_csr(CVMX_MIO_PTP_CLOCK_HI);
	return cvmx_read_csr(CVMX_MIO_PTP_CLOCK_HI);
}

static struct clocksource clocksource_ptp_clock = {
	.name		= "OCTEON_PTP_CLOCK",
	.read		= octeon_ptp_clock_read,
	.mask		= CLOCKSOURCE_MASK(64),
	.flags		= CLOCK_SOURCE_IS_CONTINUOUS,
};

int __init ptp_clock_init(void)
{
	union cvmx_mio_ptp_clock_cfg ptp_clock_cfg;

	/* Chips prior to CN6XXX don't support the PTP clock source */
	if (!OCTEON_IS_MODEL(OCTEON_CN6XXX))
		return 0;

	/* FIXME: Remove this when PTP is implemented in the simulator */
	if (octeon_is_simulation())
		return 0;

	/* Get the current state of the PTP clock */
	ptp_clock_cfg.u64 = cvmx_read_csr(CVMX_MIO_PTP_CLOCK_CFG);
	if (!ptp_clock_cfg.s.ext_clk_en) {
		/*
		 * The clock has not been configured to use an
		 * external source.  Program it to use the main clock
		 * reference.
		 */
		unsigned long long clock_comp = (NSEC_PER_SEC << 32) / octeon_get_io_clock_rate();
		cvmx_write_csr(CVMX_MIO_PTP_CLOCK_COMP, clock_comp);
		pr_info("PTP Clock: Using sclk reference at %lld Hz\n",
			(NSEC_PER_SEC << 32) / clock_comp);
	} else {
		/* The clock is already programmed to use an external GPIO */
		unsigned long long clock_comp = cvmx_read_csr(CVMX_MIO_PTP_CLOCK_COMP);
		pr_info("PTP Clock: Using GPIO %d at %lld Hz\n",
			ptp_clock_cfg.s.ext_clk_in,
			(NSEC_PER_SEC << 32) / clock_comp);
	}

	/* Enable the clock if it wasn't done already */
	if (!ptp_clock_cfg.s.ptp_en) {
		ptp_clock_cfg.s.ptp_en = 1;
		cvmx_write_csr(CVMX_MIO_PTP_CLOCK_CFG, ptp_clock_cfg.u64);
	}

	/* Add PTP as a high quality clocksource with nano second granularity */
	clocksource_ptp_clock.rating = 400;
	clocksource_set_clock(&clocksource_ptp_clock, NSEC_PER_SEC);
	clocksource_register(&clocksource_ptp_clock);
	return 0;
}

arch_initcall(ptp_clock_init);
