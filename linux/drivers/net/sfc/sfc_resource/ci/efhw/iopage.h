/****************************************************************************
 * Driver for Solarflare network controllers -
 *          resource management for Xen backend, OpenOnload, etc
 *           (including support for SFE4001 10GBT NIC)
 *
 * This file contains OS-independent API for allocating iopage types.
 * The implementation of these functions is highly OS-dependent.
 * This file is not designed for use outside of the SFC resource driver.
 *
 * Copyright 2005-2007: Solarflare Communications Inc,
 *                      9501 Jeronimo Road, Suite 250,
 *                      Irvine, CA 92618, USA
 *
 * Developed and maintained by Solarflare Communications:
 *                      <linux-xen-drivers@solarflare.com>
 *                      <onload-dev@solarflare.com>
 *
 * Certain parts of the driver were implemented by
 *          Alexandra Kossovsky <Alexandra.Kossovsky@oktetlabs.ru>
 *          OKTET Labs Ltd, Russia,
 *          http://oktetlabs.ru, <info@oktetlabs.ru>
 *          by request of Solarflare Communications
 *
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation, incorporated herein by reference.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 ****************************************************************************
 */

#ifndef __CI_DRIVER_RESOURCE_IOPAGE_H__
#define __CI_DRIVER_RESOURCE_IOPAGE_H__

#include <ci/efhw/efhw_types.h>

/*--------------------------------------------------------------------
 *
 * memory allocation
 *
 *--------------------------------------------------------------------*/

extern int efhw_iopage_alloc(struct efhw_nic *, struct efhw_iopage *p);
extern void efhw_iopage_free(struct efhw_nic *, struct efhw_iopage *p);

extern int efhw_iopages_alloc(struct efhw_nic *, struct efhw_iopages *p,
			      unsigned order);
extern void efhw_iopages_free(struct efhw_nic *, struct efhw_iopages *p);

#endif /* __CI_DRIVER_RESOURCE_IOPAGE_H__ */
