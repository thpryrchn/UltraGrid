/* dvbm_lpfd.c
 *
 * Linux driver functions for Linear Systems Ltd. DVB Master LP FD.
 *
 * Copyright (C) 2003-2007 Linear Systems Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either Version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public Licence for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 * Linear Systems can be contacted at <http://www.linsys.ca/>.
 *
 */

#include <linux/kernel.h> /* KERN_INFO */
#include <linux/module.h> /* THIS_MODULE */

#include <linux/fs.h> /* inode, file, file_operations */
#include <linux/sched.h> /* pt_regs */
#include <linux/pci.h> /* pci_dev */
#include <linux/slab.h> /* kmalloc () */
#include <linux/list.h> /* INIT_LIST_HEAD () */
#include <linux/spinlock.h> /* spin_lock_init () */
#include <linux/init.h> /* __devinit */
#include <linux/errno.h> /* error codes */
#include <linux/interrupt.h> /* irqreturn_t */
#include <linux/device.h> /* class_device_create_file () */

#include <asm/semaphore.h> /* sema_init () */
#include <asm/uaccess.h> /* put_user () */
#include <asm/bitops.h> /* set_bit () */

#include "asicore.h"
#include "../include/master.h"
#include "miface.h"
#include "mdev.h"
#include "dvbm.h"
#include "lsdma.h"
#include "masterlsdma.h"
#include "dvbm_lpfd.h"

static const char dvbm_lpfd_name[] = DVBM_NAME_LPFD;
static const char dvbm_lpfde_name[] = DVBM_NAME_LPFDE;

/* Static function prototypes */
static ssize_t dvbm_lpfd_show_bypass_mode (struct class_device *cd,
	char *buf);
static ssize_t dvbm_lpfd_store_bypass_mode (struct class_device *cd,
	const char *buf,
	size_t count);
static ssize_t dvbm_lpfd_show_bypass_status (struct class_device *cd,
	char *buf);
static ssize_t dvbm_lpfd_show_blackburst_type (struct class_device *cd,
	char *buf);
static ssize_t dvbm_lpfd_store_blackburst_type (struct class_device *cd,
	const char *buf,
	size_t count);
static ssize_t dvbm_lpfd_show_uid (struct class_device *cd,
	char *buf);
static ssize_t dvbm_lpfd_show_watchdog (struct class_device *cd,
	char *buf);
static ssize_t dvbm_lpfd_store_watchdog (struct class_device *cd,
	const char *buf,
	size_t count);
static irqreturn_t IRQ_HANDLER(dvbm_lpfd_irq_handler,irq,dev_id,regs);
static void dvbm_lpfd_txinit (struct master_iface *iface);
static void dvbm_lpfd_txstart (struct master_iface *iface);
static void dvbm_lpfd_txstop (struct master_iface *iface);
static void dvbm_lpfd_txexit (struct master_iface *iface);
static int dvbm_lpfd_txopen (struct inode *inode, struct file *filp);
static long dvbm_lpfd_txunlocked_ioctl (struct file *filp,
	unsigned int cmd,
	unsigned long arg);
static int dvbm_lpfd_txioctl (struct inode *inode,
	struct file *filp,
	unsigned int cmd,
	unsigned long arg);
static int dvbm_lpfd_txfsync (struct file *filp,
	struct dentry *dentry,
	int datasync);
static int dvbm_lpfd_txrelease (struct inode *inode, struct file *filp);
static void dvbm_lpfd_rxinit (struct master_iface *iface);
static void dvbm_lpfd_rxstart (struct master_iface *iface);
static void dvbm_lpfd_rxstop (struct master_iface *iface);
static void dvbm_lpfd_rxexit (struct master_iface *iface);
static int dvbm_lpfd_rxopen (struct inode *inode, struct file *filp);
static long dvbm_lpfd_rxunlocked_ioctl (struct file *filp,
	unsigned int cmd,
	unsigned long arg);
static int dvbm_lpfd_rxioctl (struct inode *inode,
	struct file *filp,
	unsigned int cmd,
	unsigned long arg);
static int dvbm_lpfd_rxfsync (struct file *filp,
	struct dentry *dentry,
	int datasync);
static int dvbm_lpfd_rxrelease (struct inode *inode, struct file *filp);

struct file_operations dvbm_lpfd_txfops = {
	.owner = THIS_MODULE,
	.llseek = no_llseek,
	.write = masterlsdma_write,
	.poll = masterlsdma_txpoll,
	.ioctl = dvbm_lpfd_txioctl,
#ifdef HAVE_UNLOCKED_IOCTL
	.unlocked_ioctl = dvbm_lpfd_txunlocked_ioctl,
#endif
#ifdef HAVE_COMPAT_IOCTL
	.compat_ioctl = asi_compat_ioctl,
#endif
	.open = dvbm_lpfd_txopen,
	.release = dvbm_lpfd_txrelease,
	.fsync = dvbm_lpfd_txfsync,
	.fasync = NULL
};

struct file_operations dvbm_lpfd_rxfops = {
	.owner = THIS_MODULE,
	.llseek = no_llseek,
	.read = masterlsdma_read,
	.poll = masterlsdma_rxpoll,
	.ioctl = dvbm_lpfd_rxioctl,
#ifdef HAVE_UNLOCKED_IOCTL
	.unlocked_ioctl = dvbm_lpfd_rxunlocked_ioctl,
#endif
#ifdef HAVE_COMPAT_IOCTL
	.compat_ioctl = asi_compat_ioctl,
#endif
	.open = dvbm_lpfd_rxopen,
	.release = dvbm_lpfd_rxrelease,
	.fsync = dvbm_lpfd_rxfsync,
	.fasync = NULL
};

/**
 * dvbm_lpfd_show_bypass_mode - interface attribute read handler
 * @cd: class_device being read
 * @buf: output buffer
 **/
static ssize_t
dvbm_lpfd_show_bypass_mode (struct class_device *cd,
	char *buf)
{
	struct master_dev *card = to_master_dev(cd);

	/* Atomic read of CSR, so we don't lock */
	return snprintf (buf, PAGE_SIZE, "%u\n",
		readl (card->core.addr + DVBM_LPFD_CSR) &
		DVBM_LPFD_CSR_BYPASS_MASK);
}

/**
 * dvbm_lpfd_store_bypass_mode - interface attribute write handler
 * @cd: class_device being written
 * @buf: input buffer
 * @count:
 **/
static ssize_t
dvbm_lpfd_store_bypass_mode (struct class_device *cd,
	const char *buf,
	size_t count)
{
	struct master_dev *card = to_master_dev(cd);
	char *endp;
	unsigned long val = simple_strtoul (buf, &endp, 0);
	const unsigned long max = (card->capabilities & MASTER_CAP_WATCHDOG) ?
		MASTER_CTL_BYPASS_WATCHDOG : MASTER_CTL_BYPASS_DISABLE;
	int retcode = count;
	unsigned int reg;

	if ((endp == buf) || (val > max)) {
		return -EINVAL;
	}
	spin_lock (&card->reg_lock);
	reg = readl (card->core.addr + DVBM_LPFD_CSR) &
		~DVBM_LPFD_CSR_BYPASS_MASK;
	writel (reg | val, card->core.addr + DVBM_LPFD_CSR);
	spin_unlock (&card->reg_lock);
	return retcode;
}

/**
 * dvbm_lpfd_show_bypass_status - interface attribute read handler
 * @cd: class_device being read
 * @buf: output buffer
 **/
static ssize_t
dvbm_lpfd_show_bypass_status (struct class_device *cd,
	char *buf)
{
	struct master_dev *card = to_master_dev(cd);

	return snprintf (buf, PAGE_SIZE, "%u\n",
		(readl (card->core.addr + DVBM_LPFD_ICSR) &
		DVBM_LPFD_ICSR_BYPASS) >> 15);
}

/**
 * dvbm_lpfd_show_blackburst_type - interface attribute read handler
 * @cd: class_device being read
 * @buf: output buffer
 **/
static ssize_t
dvbm_lpfd_show_blackburst_type (struct class_device *cd,
	char *buf)
{
	struct master_dev *card = to_master_dev(cd);

	return snprintf (buf, PAGE_SIZE, "%u\n",
		(readl (card->core.addr + DVBM_LPFD_TCSR) &
		DVBM_LPFD_TCSR_PAL) >> 13);
}

/**
 * dvbm_lpfd_store_blackburst_type - interface attribute write handler
 * @cd: class_device being written
 * @buf: input buffer
 * @count:
 **/
static ssize_t
dvbm_lpfd_store_blackburst_type (struct class_device *cd,
	const char *buf,
	size_t count)
{
	struct master_dev *card = to_master_dev(cd);
	char *endp;
	unsigned long val = simple_strtoul (buf, &endp, 0);
	unsigned int reg;
	const unsigned long max = MASTER_CTL_BLACKBURST_PAL;
	int retcode = count;

	if ((endp == buf) || (val > max)) {
		return -EINVAL;
	}
	spin_lock (&card->reg_lock);
	reg = readl (card->core.addr + DVBM_LPFD_TCSR) & ~DVBM_LPFD_TCSR_PAL;
	writel (reg | (val << 13), card->core.addr + DVBM_LPFD_TCSR);
	spin_unlock (&card->reg_lock);
	return retcode;
}

/**
 * dvbm_lpfd_show_uid - interface attribute read handler
 * @cd: class_device being read
 * @buf: output buffer
 **/
static ssize_t
dvbm_lpfd_show_uid (struct class_device *cd,
	char *buf)
{
	struct master_dev *card = to_master_dev(cd);

	return snprintf (buf, PAGE_SIZE, "0x%08X%08X\n",
		readl (card->core.addr + DVBM_LPFD_UIDR_HI),
		readl (card->core.addr + DVBM_LPFD_UIDR_LO));
}

/**
 * dvbm_lpfd_show_watchdog - interface attribute read handler
 * @cd: class_device being read
 * @buf: output buffer
 **/
static ssize_t
dvbm_lpfd_show_watchdog (struct class_device *cd,
	char *buf)
{
	struct master_dev *card = to_master_dev(cd);

	/* convert 27Mhz ticks to milliseconds */
	return snprintf (buf, PAGE_SIZE, "%u\n",
		(readl (card->core.addr + DVBM_LPFD_WDTLR) / 27000));
}

/**
 * dvbm_lpfd_store_watchdog - interface attribute write handler
 * @cd: class_device being written
 * @buf: input buffer
 * @count:
 **/
static ssize_t
dvbm_lpfd_store_watchdog (struct class_device *cd,
	const char *buf,
	size_t count)
{
	struct master_dev *card = to_master_dev(cd);
	char *endp;
	unsigned long val = simple_strtoul (buf, &endp, 0);
	const unsigned long max = MASTER_WATCHDOG_MAX;
	int retcode = count;

	if ((endp == buf) || (val > max)) {
		return -EINVAL;
	}

	/* Convert val in milliseconds to 27Mhz ticks */
	val = val * 27000;

	writel (val, card->core.addr + DVBM_LPFD_WDTLR);
	return retcode;
}

static CLASS_DEVICE_ATTR(bypass_mode,S_IRUGO|S_IWUSR,
	dvbm_lpfd_show_bypass_mode,dvbm_lpfd_store_bypass_mode);
static CLASS_DEVICE_ATTR(bypass_status,S_IRUGO,
	dvbm_lpfd_show_bypass_status,NULL);
static CLASS_DEVICE_ATTR(blackburst_type,S_IRUGO|S_IWUSR,
	dvbm_lpfd_show_blackburst_type,dvbm_lpfd_store_blackburst_type);
static CLASS_DEVICE_ATTR(uid,S_IRUGO,
	dvbm_lpfd_show_uid,NULL);
static CLASS_DEVICE_ATTR(watchdog,S_IRUGO|S_IWUSR,
	dvbm_lpfd_show_watchdog,dvbm_lpfd_store_watchdog);

/**
 * dvbm_lpfd_pci_probe - PCI insertion handler for a DVB Master LP FD
 * @dev: PCI device
 *
 * Handle the insertion of a DVB Master LP FD.
 * Returns a negative error code on failure and 0 on success.
 **/
int __devinit
dvbm_lpfd_pci_probe (struct pci_dev *dev)
{
	int err;
	unsigned int cap, transport;
	const char *name;
	struct master_dev *card;

	switch (dev->device) {
	case DVBM_PCI_DEVICE_ID_LINSYS_DVBLPFD:
		name = dvbm_lpfd_name;
		break;
	case DVBM_PCI_DEVICE_ID_LINSYS_DVBLPFDE:
		name = dvbm_lpfde_name;
		break;
	default:
		name = "";
		break;
	}

	/* Allocate a board info structure */
	if ((card = (struct master_dev *)
		kmalloc (sizeof (*card), GFP_KERNEL)) == NULL) {
		err = -ENOMEM;
		goto NO_MEM;
	}

	/* Initialize the board info structure */
	memset (card, 0, sizeof (*card));
	/* LS DMA Controller */
	switch (dev->device) {
        case DVBM_PCI_DEVICE_ID_LINSYS_DVBLPFD:
		card->bridge_addr = ioremap_nocache (pci_resource_start (dev, 1),
			pci_resource_len (dev, 1));
		break;
        case DVBM_PCI_DEVICE_ID_LINSYS_DVBLPFDE:
		card->bridge_addr = ioremap_nocache (pci_resource_start (dev, 2),
			pci_resource_len (dev, 2));
		break;
	default:
		break;
	}
	/* ASI Core */
	card->core.addr = ioremap_nocache (pci_resource_start (dev, 0),
		pci_resource_len (dev, 0));
	card->version = readl (card->core.addr + DVBM_LPFD_CSR) >> 16;
	card->name = name;
	card->irq_handler = dvbm_lpfd_irq_handler;
	INIT_LIST_HEAD(&card->iface_list);
	switch (dev->device) {
	default:
	case DVBM_PCI_DEVICE_ID_LINSYS_DVBLPFD:
	case DVBM_PCI_DEVICE_ID_LINSYS_DVBLPFDE:
		card->capabilities = MASTER_CAP_BYPASS | MASTER_CAP_BLACKBURST |
			MASTER_CAP_UID | MASTER_CAP_WATCHDOG;
		break;
	}
	/* Lock for ICSR */
	spin_lock_init (&card->irq_lock);
	/* Lock for IBSTR, IPSTR, FTR, PFLUT, TCSR, RCSR */
	spin_lock_init (&card->reg_lock);
	sema_init (&card->users_sem, 1);
	card->pdev = dev;

	/* Print the firmware version */
	printk (KERN_INFO "%s: %s detected, firmware version %u.%u (0x%04X)\n",
		dvbm_driver_name, name,
		card->version >> 8, card->version & 0x00ff, card->version);

	/* Store the pointer to the board info structure
	 * in the PCI info structure */
	pci_set_drvdata (dev, card);

	/* Reset the FPGA */
	writel (DVBM_LPFD_TCSR_RST, card->core.addr + DVBM_LPFD_TCSR);
	writel (DVBM_LPFD_RCSR_RST, card->core.addr + DVBM_LPFD_RCSR);

	/* Setup the LS DMA */
	writel (LSDMA_INTMSK_CH(0) | LSDMA_INTMSK_CH(1),
		card->bridge_addr + LSDMA_INTMSK);
	writel (LSDMA_CH_CSR_INTDONEENABLE | LSDMA_CH_CSR_INTSTOPENABLE,
		card->bridge_addr + LSDMA_CSR(0));
	writel (LSDMA_CH_CSR_INTDONEENABLE | LSDMA_CH_CSR_INTSTOPENABLE |
		LSDMA_CH_CSR_DIRECTION,
		card->bridge_addr + LSDMA_CSR(1));
	/* Dummy read to flush PCI posted writes */
	readl (card->bridge_addr + LSDMA_INTMSK);

	/* Register a Master device */
	if ((err = mdev_register (card,
		&dvbm_card_list,
		dvbm_driver_name,
		&dvbm_class)) < 0) {
		goto NO_DEV;
	}

	/* Add class_device attributes */
	if (card->capabilities & MASTER_CAP_BYPASS) {
		if ((err = class_device_create_file (&card->class_dev,
			&class_device_attr_bypass_mode)) < 0) {
			printk (KERN_WARNING
				"%s: unable to create file 'bypass_mode'\n",
				dvbm_driver_name);
		}
		if ((err = class_device_create_file (&card->class_dev,
			&class_device_attr_bypass_status)) < 0) {
			printk (KERN_WARNING
				"%s: unable to create file 'bypass_status'\n",
				dvbm_driver_name);
		}
	}
	if (card->capabilities & MASTER_CAP_BLACKBURST) {
		if ((err = class_device_create_file (&card->class_dev,
			&class_device_attr_blackburst_type)) < 0) {
			printk (KERN_WARNING
				"%s: unable to create file 'blackburst_type'\n",
				dvbm_driver_name);
		}
	}
	if (card->capabilities & MASTER_CAP_UID) {
		if ((err = class_device_create_file (&card->class_dev,
			&class_device_attr_uid)) < 0) {
			printk (KERN_WARNING
				"%s: unable to create file 'uid'\n",
				dvbm_driver_name);
		}
	}
	if (card->capabilities & MASTER_CAP_WATCHDOG) {
		if ((err = class_device_create_file (&card->class_dev,
			&class_device_attr_watchdog)) < 0) {
			printk (KERN_WARNING
				"%s: unable to create file 'watchdog'\n",
				dvbm_driver_name);
		}
	}

	/* Register a transmit interface */
	cap = ASI_CAP_TX_SETCLKSRC | ASI_CAP_TX_FIFOUNDERRUN |
		ASI_CAP_TX_DATA | ASI_CAP_TX_RXCLKSRC;
	switch (dev->device) {
	case DVBM_PCI_DEVICE_ID_LINSYS_DVBLPFD:
	case DVBM_PCI_DEVICE_ID_LINSYS_DVBLPFDE:
		cap |= ASI_CAP_TX_MAKE204 | ASI_CAP_TX_FINETUNING |
			ASI_CAP_TX_LARGEIB |
			ASI_CAP_TX_INTERLEAVING |
			ASI_CAP_TX_TIMESTAMPS |
			ASI_CAP_TX_NULLPACKETS |
			ASI_CAP_TX_PTIMESTAMPS;
		transport = ASI_CTL_TRANSPORT_DVB_ASI;
		break;
	default:
		transport = 0xff;
		break;
	}
	if ((err = asi_register_iface (card,
		MASTER_DIRECTION_TX,
		&dvbm_lpfd_txfops,
		cap,
		4,
		transport)) < 0) {
		goto NO_IFACE;
	}

	/* Register a receive interface */
	cap = ASI_CAP_RX_SYNC | ASI_CAP_RX_INVSYNC | ASI_CAP_RX_CD;
	switch (dev->device) {
	case DVBM_PCI_DEVICE_ID_LINSYS_DVBLPFD:
	case DVBM_PCI_DEVICE_ID_LINSYS_DVBLPFDE:
		cap |= ASI_CAP_RX_MAKE188 |
			ASI_CAP_RX_DATA |
			ASI_CAP_RX_PIDFILTER |
			ASI_CAP_RX_TIMESTAMPS |
			ASI_CAP_RX_PTIMESTAMPS |
			ASI_CAP_RX_NULLPACKETS;
		transport = ASI_CTL_TRANSPORT_DVB_ASI;
		break;
	default:
		transport = 0xff;
		break;
	}
	if ((err = asi_register_iface (card,
		MASTER_DIRECTION_RX,
		&dvbm_lpfd_rxfops,
		cap,
		4,
		transport)) < 0) {
		goto NO_IFACE;
	}

	return 0;

NO_IFACE:
	dvbm_pci_remove (dev);
NO_DEV:
NO_MEM:
	return err;
}

/**
 * dvbm_lpfd_pci_remove - PCI removal handler for a DVB Master LP FD
 * @card: Master device
 *
 * Handle the removal of a DVB Master LP FD.
 **/
void
dvbm_lpfd_pci_remove (struct master_dev *card)
{
	if (card->capabilities & MASTER_CAP_BYPASS) {
		writel (0, card->core.addr + DVBM_LPFD_CSR);
	}
	iounmap (card->core.addr);
	return;
}

/**
 * dvbm_lpfd_irq_handler - DVB Master LP FD interrupt service routine
 * @irq: interrupt number
 * @dev_id: pointer to the device data structure
 * @regs: processor context
 **/
static irqreturn_t
IRQ_HANDLER(dvbm_lpfd_irq_handler,irq,dev_id,regs)
{
	struct master_dev *card = dev_id;
	unsigned int dmaintsrc = readl (card->bridge_addr + LSDMA_INTSRC);
	unsigned int status, interrupting_iface = 0;
	struct master_iface *txiface = list_entry (card->iface_list.next,
		struct master_iface, list);
	struct master_iface *rxiface = list_entry (card->iface_list.prev,
		struct master_iface, list);

	if (dmaintsrc & LSDMA_INTSRC_CH(0)) {
		/* Read the interrupt type and clear it */
		spin_lock (&card->irq_lock);
		status = readl (card->bridge_addr + LSDMA_CSR(0));
		writel (status, card->bridge_addr + LSDMA_CSR(0));
		spin_unlock (&card->irq_lock);

		/* Increment the buffer pointer */
		if (status & LSDMA_CH_CSR_INTSRCBUFFER) {
			lsdma_advance (txiface->dma);
		}

		/* Flag end-of-chain */
		if (status & LSDMA_CH_CSR_INTSRCDONE) {
			set_bit (ASI_EVENT_TX_BUFFER_ORDER, &txiface->events);
			set_bit (0, &txiface->dma_done);
		}

		/* Flag DMA abort */
		if (status & LSDMA_CH_CSR_INTSRCSTOP) {
			set_bit (0, &txiface->dma_done);
		}

		interrupting_iface |= 0x1;

	}

	if (dmaintsrc & LSDMA_INTSRC_CH(1)) {
		struct lsdma_dma *dma = rxiface->dma;

		/* Read the interrupt type and clear it */
		spin_lock (&card->irq_lock);
		status = readl (card->bridge_addr + LSDMA_CSR(1));
		writel (status, card->bridge_addr + LSDMA_CSR(1));
		spin_unlock (&card->irq_lock);

		/* Increment the buffer pointer */
		if (status & LSDMA_CH_CSR_INTSRCBUFFER) {
			lsdma_advance (dma);
			if (lsdma_rx_isempty (dma)) {
				set_bit (ASI_EVENT_RX_BUFFER_ORDER,
					&rxiface->events);
			}
		}

		/* Flag end-of-chain */
		if (status & LSDMA_CH_CSR_INTSRCDONE) {
			set_bit (0, &rxiface->dma_done);
		}

		/* Flag DMA abort */
		if (status & LSDMA_CH_CSR_INTSRCSTOP) {
			set_bit (0, &rxiface->dma_done);
		}

		interrupting_iface |= 0x2;
	}

	/* Check and clear the source of the interrupt */
	spin_lock (&card->irq_lock);
	status = readl (card->core.addr + DVBM_LPFD_ICSR);
	writel (status, card->core.addr + DVBM_LPFD_ICSR);
	spin_unlock (&card->irq_lock);

	if (status & DVBM_LPFD_ICSR_TXUIS) {
		set_bit (ASI_EVENT_TX_FIFO_ORDER,
			&txiface->events);
		interrupting_iface |= 0x1;
	}
	if (status & DVBM_LPFD_ICSR_TXDIS) {
		set_bit (ASI_EVENT_TX_DATA_ORDER,
			&txiface->events);
		interrupting_iface |= 0x1;
	}
	if (status & DVBM_LPFD_ICSR_RXCDIS) {
		set_bit (ASI_EVENT_RX_CARRIER_ORDER,
			&rxiface->events);
		interrupting_iface |= 0x2;
	}
	if (status & DVBM_LPFD_ICSR_RXAOSIS) {
		set_bit (ASI_EVENT_RX_AOS_ORDER,
			&rxiface->events);
		interrupting_iface |= 0x2;
	}
	if (status & DVBM_LPFD_ICSR_RXLOSIS) {
		set_bit (ASI_EVENT_RX_LOS_ORDER,
			&rxiface->events);
		interrupting_iface |= 0x2;
	}
	if (status & DVBM_LPFD_ICSR_RXOIS) {
		set_bit (ASI_EVENT_RX_FIFO_ORDER,
			&rxiface->events);
		interrupting_iface |= 0x2;
	}
	if (status & DVBM_LPFD_ICSR_RXDIS) {
		set_bit (ASI_EVENT_RX_DATA_ORDER,
			&rxiface->events);
		interrupting_iface |= 0x2;
	}

	if (interrupting_iface) {
		/* Dummy read to flush PCI posted writes */
		readl (card->bridge_addr + LSDMA_INTMSK);

		if (interrupting_iface & 0x1) {
			wake_up (&txiface->queue);
		}
		if (interrupting_iface & 0x2) {
			wake_up (&rxiface->queue);
		}
		return IRQ_HANDLED;
	}
	return IRQ_NONE;
}

/**
 * dvbm_lpfd_txinit - Initialize the DVB Master LP FD transmitter
 * @iface: interface
 **/
static void
dvbm_lpfd_txinit (struct master_iface *iface)
{
	struct master_dev *card = iface->card;
	unsigned int reg = iface->null_packets ? DVBM_LPFD_TCSR_NP : 0;

	switch (iface->timestamps) {
	default:
	case ASI_CTL_TSTAMP_NONE:
		reg |= 0;
		break;
	case ASI_CTL_TSTAMP_APPEND:
		reg |= DVBM_LPFD_TCSR_TSS;
		break;
	case ASI_CTL_TSTAMP_PREPEND:
		reg |= DVBM_LPFD_TCSR_PRC;
		break;
	}
	switch (iface->mode) {
	default:
	case ASI_CTL_TX_MODE_188:
		reg |= 0;
		break;
	case ASI_CTL_TX_MODE_204:
		reg |= DVBM_LPFD_TCSR_204;
		break;
	case ASI_CTL_TX_MODE_MAKE204:
		reg |= DVBM_LPFD_TCSR_MAKE204;
		break;
	}
	switch (iface->clksrc) {
	default:
	case ASI_CTL_TX_CLKSRC_ONBOARD:
		reg |= 0;
		break;
	case ASI_CTL_TX_CLKSRC_EXT:
		reg |= DVBM_LPFD_TCSR_EXTCLK;
		break;
	case ASI_CTL_TX_CLKSRC_RX:
		reg |= DVBM_LPFD_TCSR_RXCLK;
		break;
	}
	/* There will be no races on IBSTR, IPSTR, FTR, and TCSR
	 * until this code returns, so we don't need to lock them */
	writel (reg | DVBM_LPFD_TCSR_RST, card->core.addr + DVBM_LPFD_TCSR);
	wmb ();
	writel (reg, card->core.addr + DVBM_LPFD_TCSR);
	wmb ();
	writel (DVBM_LPFD_TFSL << 16, card->core.addr + DVBM_LPFD_TFCR);
	writel (0, card->core.addr + DVBM_LPFD_IBSTR);
	writel (0, card->core.addr + DVBM_LPFD_IPSTR);
	writel (0, card->core.addr + DVBM_LPFD_FTR);

	return;
}

/**
 * dvbm_lpfd_txstart - Activate the DVB Master LP FD transmitter
 * @iface: interface
 **/
static void
dvbm_lpfd_txstart (struct master_iface *iface)
{
	struct master_dev *card = iface->card;
	unsigned int reg;

	/* Enable DMA */
	writel (LSDMA_CH_CSR_INTDONEENABLE | LSDMA_CH_CSR_INTSTOPENABLE,
		card->bridge_addr + LSDMA_CSR(0));

	/* Enable transmitter interrupts */
	spin_lock_irq (&card->irq_lock);
	reg = readl (card->core.addr + DVBM_LPFD_ICSR) &
		DVBM_LPFD_ICSR_RXCTRL_MASK;
	reg |= DVBM_LPFD_ICSR_TXUIE | DVBM_LPFD_ICSR_TXDIE;
	writel (reg, card->core.addr + DVBM_LPFD_ICSR);
	spin_unlock_irq (&card->irq_lock);
	/* Enable the transmitter.
	 * There will be no races on TCSR
	 * until this code returns, so we don't need to lock it */
	reg = readl (card->core.addr + DVBM_LPFD_TCSR);
	writel (reg | DVBM_LPFD_TCSR_EN, card->core.addr + DVBM_LPFD_TCSR);

	return;
}

/**
 * dvbm_lpfd_txstop - Deactivate the DVB Master LP FD transmitter
 * @iface: interface
 **/
static void
dvbm_lpfd_txstop (struct master_iface *iface)
{
	struct master_dev *card = iface->card;
	struct lsdma_dma *dma = iface->dma;
	unsigned int reg;

	lsdma_tx_link_all (dma);
	wait_event (iface->queue, test_bit (0, &iface->dma_done));
	lsdma_reset (dma);

	if (!iface->null_packets) {
		/* Wait for the onboard FIFOs to empty */
		/* Atomic read of ICSR, so we don't need to lock */
		wait_event (iface->queue,
			!(readl (card->core.addr + DVBM_LPFD_ICSR) &
			DVBM_LPFD_ICSR_TXD));
	}

	/* Disable the transmitter.
	 * There will be no races on TCSR here,
	 * so we don't need to lock it */
	reg = readl (card->core.addr + DVBM_LPFD_TCSR);
	writel (reg & ~DVBM_LPFD_TCSR_EN, card->core.addr + DVBM_LPFD_TCSR);

	/* Disable transmitter interrupts */
	spin_lock_irq (&card->irq_lock);
	reg = readl (card->core.addr + DVBM_LPFD_ICSR) &
		DVBM_LPFD_ICSR_RXCTRL_MASK;
	reg |= DVBM_LPFD_ICSR_TXUIS | DVBM_LPFD_ICSR_TXDIS;
	writel (reg, card->core.addr + DVBM_LPFD_ICSR);
	spin_unlock_irq (&card->irq_lock);

	/* Disable DMA */
	writel ((LSDMA_CH_CSR_INTDONEENABLE | LSDMA_CH_CSR_INTSTOPENABLE) &
		~LSDMA_CH_CSR_ENABLE,
		card->bridge_addr + LSDMA_CSR(0));

	return;
}

/**
 * dvbm_lpfd_txexit - Clean up the DVB Master LP FD transmitter
 * @iface: interface
 **/
static void
dvbm_lpfd_txexit (struct master_iface *iface)
{
	struct master_dev *card = iface->card;

	/* Reset the transmitter */
	writel (DVBM_LPFD_TCSR_RST, card->core.addr + DVBM_LPFD_TCSR);

	return;
}

/**
 * dvbm_lpfd_txopen - DVB Master LP FD transmitter open() method
 * @inode: inode
 * @filp: file
 *
 * Returns a negative error code on failure and 0 on success.
 **/
static int
dvbm_lpfd_txopen (struct inode *inode, struct file *filp)
{
	return masterlsdma_open (inode,
		filp,
		dvbm_lpfd_txinit,
		dvbm_lpfd_txstart,
		DVBM_LPFD_FIFO,
		0);
}

/**
 * dvbm_lpfd_txunlocked_ioctl - DVB Master LP FD transmitter unlocked_ioctl() method
 * @filp: file
 * @cmd: ioctl command
 * @arg: ioctl argument
 *
 * Returns a negative error code on failure and 0 on success.
 **/
static long
dvbm_lpfd_txunlocked_ioctl (struct file *filp,
	unsigned int cmd,
	unsigned long arg)
{
	struct master_iface *iface = filp->private_data;
	struct master_dev *card = iface->card;
	struct asi_txstuffing stuffing;

	switch (cmd) {
	case ASI_IOC_TXGETBUFLEVEL:
		if (put_user (lsdma_tx_buflevel (iface->dma),
			(unsigned int *)arg)) {
			return -EFAULT;
		}
		break;
	case ASI_IOC_TXSETSTUFFING:
		if (iface->transport != ASI_CTL_TRANSPORT_DVB_ASI) {
			return -ENOTTY;
		}
		if (copy_from_user (&stuffing, (struct asi_txstuffing *)arg,
			sizeof (stuffing))) {
			return -EFAULT;
		}
		if ((stuffing.ib > 0xffff) ||
			(stuffing.ip > 0xffffff) ||
			(stuffing.normal_ip > 0xff) ||
			(stuffing.big_ip > 0xff) ||
			((stuffing.il_normal + stuffing.il_big) > 0xf) ||
			(stuffing.il_normal > stuffing.normal_ip) ||
			(stuffing.il_big > stuffing.big_ip)) {
			return -EINVAL;
		}
		spin_lock (&card->reg_lock);
		writel (stuffing.ib, card->core.addr + DVBM_LPFD_IBSTR);
		writel (stuffing.ip, card->core.addr + DVBM_LPFD_IPSTR);
		writel ((stuffing.il_big << DVBM_LPFD_FTR_ILBIG_SHIFT) |
			(stuffing.big_ip << DVBM_LPFD_FTR_BIGIP_SHIFT) |
			(stuffing.il_normal << DVBM_LPFD_FTR_ILNORMAL_SHIFT) |
			stuffing.normal_ip, card->core.addr + DVBM_LPFD_FTR);
		spin_unlock (&card->reg_lock);
		break;
	case ASI_IOC_TXGETTXD:
		/* Atomic read of ICSR, so we don't need to lock */
		if (put_user ((readl (card->core.addr + DVBM_LPFD_ICSR) &
			DVBM_LPFD_ICSR_TXD) ? 1 : 0, (int *)arg)) {
			return -EFAULT;
		}
		break;
	default:
		return asi_txioctl (iface, cmd, arg);
	}
	return 0;
}

/**
 * dvbm_lpfd_txioctl - DVB Master LP FD transmitter ioctl() method
 * @inode: inode
 * @filp: file
 * @cmd: ioctl command
 * @arg: ioctl argument
 *
 * Returns a negative error code on failure and 0 on success.
 **/
static int
dvbm_lpfd_txioctl (struct inode *inode,
	struct file *filp,
	unsigned int cmd,
	unsigned long arg)
{
	return dvbm_lpfd_txunlocked_ioctl (filp, cmd, arg);
}

/**
 * dvbm_lpfd_txfsync - DVB Master LP FD transmitter fsync() method
 * @filp: file to flush
 * @dentry: directory entry associated with the file
 * @datasync: used by filesystems
 *
 * Returns a negative error code on failure and 0 on success.
 **/
static int
dvbm_lpfd_txfsync (struct file *filp,
	struct dentry *dentry,
	int datasync)
{
	struct master_iface *iface = filp->private_data;
	struct lsdma_dma *dma = iface->dma;

	if (down_interruptible (&iface->buf_sem)) {
		return -ERESTARTSYS;
	}
	lsdma_tx_link_all (dma);
	wait_event (iface->queue, test_bit (0, &iface->dma_done));
	lsdma_reset (dma);

	if (!iface->null_packets) {
		struct master_dev *card = iface->card;

		/* Wait for the onboard FIFOs to empty */
		/* Atomic read of ICSR, so we don't need to lock */
		wait_event (iface->queue,
			!(readl (card->core.addr + DVBM_LPFD_ICSR) &
			DVBM_LPFD_ICSR_TXD));
	}

	up (&iface->buf_sem);
	return 0;
}

/**
 * dvbm_lpfd_txrelease - DVB Master LP FD transmitter release() method
 * @inode: inode
 * @filp: file
 *
 * Returns a negative error code on failure and 0 on success.
 **/
static int
dvbm_lpfd_txrelease (struct inode *inode, struct file *filp)
{
	struct master_iface *iface = filp->private_data;

	return masterlsdma_release (iface, dvbm_lpfd_txstop, dvbm_lpfd_txexit);
}

/**
 * dvbm_lpfd_rxinit - Initialize the DVB Master LP FD receiver
 * @iface: interface
 **/
static void
dvbm_lpfd_rxinit (struct master_iface *iface)
{
	struct master_dev *card = iface->card;
	unsigned int i, reg = DVBM_LPFD_RCSR_RF |
		(iface->null_packets ? DVBM_LPFD_RCSR_NP : 0);

	switch (iface->timestamps) {
	default:
	case ASI_CTL_TSTAMP_NONE:
		reg |= 0;
		break;
	case ASI_CTL_TSTAMP_APPEND:
		reg |= DVBM_LPFD_RCSR_TSE;
		break;
	case ASI_CTL_TSTAMP_PREPEND:
		reg |= DVBM_LPFD_RCSR_PTSE;
		break;
	}
	switch (iface->mode) {
	default:
	case ASI_CTL_RX_MODE_RAW:
		reg |= 0;
		break;
	case ASI_CTL_RX_MODE_188:
		reg |= DVBM_LPFD_RCSR_188 | DVBM_LPFD_RCSR_PFE;
		break;
	case ASI_CTL_RX_MODE_204:
		reg |= DVBM_LPFD_RCSR_204 | DVBM_LPFD_RCSR_PFE;
		break;
	case ASI_CTL_RX_MODE_AUTO:
		reg |= DVBM_LPFD_RCSR_AUTO | DVBM_LPFD_RCSR_PFE;
		break;
	case ASI_CTL_RX_MODE_AUTOMAKE188:
		reg |= DVBM_LPFD_RCSR_AUTO | DVBM_LPFD_RCSR_RSS |
			DVBM_LPFD_RCSR_PFE;
		break;
	case ASI_CTL_RX_MODE_204MAKE188:
		reg |= DVBM_LPFD_RCSR_204 | DVBM_LPFD_RCSR_RSS |
			DVBM_LPFD_RCSR_PFE;
		break;
	}

	/* There will be no races on RCSR
	 * until this code returns, so we don't need to lock it */
	writel (reg | DVBM_LPFD_RCSR_RST, card->core.addr + DVBM_LPFD_RCSR);
	wmb ();
	writel (reg, card->core.addr + DVBM_LPFD_RCSR);

	/* Reset PID filter.
	 * There will be no races on PFLUT
	 * until this code returns, so we don't need to lock it */
	for (i = 0; i < 256; i++) {
		writel (i, card->core.addr + DVBM_LPFD_PFLUTAR);
		wmb ();
		writel (0xffffffff, card->core.addr + DVBM_LPFD_PFLUTR);
		wmb ();
	}

	return;
}

/**
 * dvbm_lpfd_rxstart - Activate the DVB Master LP FD receiver
 * @iface: interface
 **/
static void
dvbm_lpfd_rxstart (struct master_iface *iface)
{
	struct master_dev *card = iface->card;
	unsigned int reg;

	/* Enable and start DMA */
	writel (lsdma_dma_to_desc_low (lsdma_head_desc_bus_addr (iface->dma)),
		card->bridge_addr + LSDMA_DESC(1));
	clear_bit (0, &iface->dma_done);
	wmb ();
	writel (LSDMA_CH_CSR_INTDONEENABLE | LSDMA_CH_CSR_INTSTOPENABLE |
		LSDMA_CH_CSR_DIRECTION | LSDMA_CH_CSR_ENABLE,
		card->bridge_addr + LSDMA_CSR(1));

	/* Dummy read to flush PCI posted writes */
	readl (card->bridge_addr + LSDMA_INTMSK);

	/* Enable receiver interrupts */
	spin_lock_irq (&card->irq_lock);
	reg = readl (card->core.addr + DVBM_LPFD_ICSR) &
		DVBM_LPFD_ICSR_TXCTRL_MASK;
	reg |= DVBM_LPFD_ICSR_RXCDIE | DVBM_LPFD_ICSR_RXAOSIE |
		DVBM_LPFD_ICSR_RXLOSIE | DVBM_LPFD_ICSR_RXOIE |
		DVBM_LPFD_ICSR_RXDIE;
	writel (reg, card->core.addr + DVBM_LPFD_ICSR);
	spin_unlock_irq (&card->irq_lock);

	/* Enable the receiver */
	spin_lock (&card->reg_lock);
	reg = readl (card->core.addr + DVBM_LPFD_RCSR);
	writel (reg | DVBM_LPFD_RCSR_EN, card->core.addr + DVBM_LPFD_RCSR);

	spin_unlock (&card->reg_lock);

	return;
}

/**
 * dvbm_lpfd_rxstop - Deactivate the DVB Master LP FD receiver
 * @iface: interface
 **/
static void
dvbm_lpfd_rxstop (struct master_iface *iface)
{
	struct master_dev *card = iface->card;
	unsigned int reg;

	/* Disable the receiver */
	spin_lock (&card->reg_lock);
	reg = readl (card->core.addr + DVBM_LPFD_RCSR);
	writel (reg & ~DVBM_LPFD_RCSR_EN, card->core.addr + DVBM_LPFD_RCSR);
	spin_unlock (&card->reg_lock);

	/* Disable receiver interrupts */
	spin_lock_irq (&card->irq_lock);
	reg = readl (card->core.addr + DVBM_LPFD_ICSR) &
		DVBM_LPFD_ICSR_TXCTRL_MASK;
	reg |= DVBM_LPFD_ICSR_RXCDIS | DVBM_LPFD_ICSR_RXAOSIS |
		DVBM_LPFD_ICSR_RXLOSIS | DVBM_LPFD_ICSR_RXOIS |
		DVBM_LPFD_ICSR_RXDIS;
	writel (reg, card->core.addr + DVBM_LPFD_ICSR);
	spin_unlock_irq (&card->irq_lock);

	/* Disable and abort DMA */
	writel ((LSDMA_CH_CSR_INTDONEENABLE | LSDMA_CH_CSR_INTSTOPENABLE |
		LSDMA_CH_CSR_DIRECTION) & ~LSDMA_CH_CSR_ENABLE,
		card->bridge_addr + LSDMA_CSR(1));
	wmb ();
	writel ((LSDMA_CH_CSR_INTDONEENABLE | LSDMA_CH_CSR_INTSTOPENABLE |
		LSDMA_CH_CSR_DIRECTION | LSDMA_CH_CSR_STOP) &
		~LSDMA_CH_CSR_ENABLE,
		card->bridge_addr + LSDMA_CSR(1));

	/* Dummy read to flush PCI posted writes */
	readl (card->bridge_addr + LSDMA_INTMSK);
	wait_event (iface->queue, test_bit (0, &iface->dma_done));
	writel ((LSDMA_CH_CSR_INTDONEENABLE | LSDMA_CH_CSR_INTSTOPENABLE |
		LSDMA_CH_CSR_DIRECTION) & ~LSDMA_CH_CSR_ENABLE,
		card->bridge_addr + LSDMA_CSR(1));

	return;
}

/**
 * dvbm_lpfd_rxexit - Clean up the DVB Master LP FD receiver
 * @iface: interface
 **/
static void
dvbm_lpfd_rxexit (struct master_iface *iface)
{
	struct master_dev *card = iface->card;

	/* Reset the receiver.
	 * There will be no races on RCSR here,
	 * so we don't need to lock it */
	writel (DVBM_LPFD_RCSR_RST, card->core.addr + DVBM_LPFD_RCSR);

	return;
}

/**
 * dvbm_lpfd_rxopen - DVB Master LP FD receiver open() method
 * @inode: inode
 * @filp: file
 *
 * Returns a negative error code on failure and 0 on success.
 **/
static int
dvbm_lpfd_rxopen (struct inode *inode, struct file *filp)
{
	return masterlsdma_open (inode,
		filp,
		dvbm_lpfd_rxinit,
		dvbm_lpfd_rxstart,
		DVBM_LPFD_FIFO,
		0);
}

/**
 * dvbm_lpfd_rxunlocked_ioctl - DVB Master LP FD receiver unlocked_ioctl() method
 * @filp: file
 * @cmd: ioctl command
 * @arg: ioctl argument
 *
 * Returns a negative error code on failure and 0 on success.
 **/
static long
dvbm_lpfd_rxunlocked_ioctl (struct file *filp,
	unsigned int cmd,
	unsigned long arg)
{
	struct master_iface *iface = filp->private_data;
	struct master_dev *card = iface->card;
	int val;
	unsigned int reg = 0, pflut[256], i;

	switch (cmd) {
	case ASI_IOC_RXGETBUFLEVEL:
		if (put_user (lsdma_rx_buflevel (iface->dma),
			(unsigned int *)arg)) {
			return -EFAULT;
		}
		break;
	case ASI_IOC_RXGETSTATUS:
		/* Atomic reads of ICSR and RCSR, so we don't need to lock */
		reg = readl (card->core.addr + DVBM_LPFD_ICSR);
		switch (readl (card->core.addr + DVBM_LPFD_RCSR) &
			DVBM_LPFD_RCSR_SYNC_MASK) {
		case 0:
			val = 1;
			break;
		case DVBM_LPFD_RCSR_188:
			val = (reg & DVBM_LPFD_ICSR_RXPASSING) ? 188 : 0;
			break;
		case DVBM_LPFD_RCSR_204:
			val = (reg & DVBM_LPFD_ICSR_RXPASSING) ? 204 : 0;
			break;
		case DVBM_LPFD_RCSR_AUTO:
			if (reg & DVBM_LPFD_ICSR_RXPASSING) {
				val = (reg & DVBM_LPFD_ICSR_RX204) ? 204 : 188;
			} else {
				val = 0;
			}
			break;
		default:
			return -EIO;
		}
		if (put_user (val, (int *)arg)) {
			return -EFAULT;
		}
		break;
	case ASI_IOC_RXSETINVSYNC:
		if (get_user (val, (int *)arg)) {
			return -EFAULT;
		}
		switch (val) {
		case 0:
			reg |= 0;
			break;
		case 1:
			reg |= DVBM_LPFD_RCSR_INVSYNC;
			break;
		default:
			return -EINVAL;
		}
		spin_lock (&card->reg_lock);
		writel ((readl (card->core.addr + DVBM_LPFD_RCSR) &
			~DVBM_LPFD_RCSR_INVSYNC) | reg,
			card->core.addr + DVBM_LPFD_RCSR);
		spin_unlock (&card->reg_lock);
		break;
	case ASI_IOC_RXGETCARRIER:
		/* Atomic read of ICSR, so we don't need to lock */
		if (put_user ((readl (card->core.addr + DVBM_LPFD_ICSR) &
			DVBM_LPFD_ICSR_RXCD) ? 1 : 0, (int *)arg)) {
			return -EFAULT;
		}
		break;
	case ASI_IOC_RXSETDSYNC:
		if (get_user (val, (int *)arg)) {
			return -EFAULT;
		}
		if (val) {
			return -EINVAL;
		}
		break;
	case ASI_IOC_RXGETRXD:
		/* Atomic read of ICSR, so we don't need to lock */
		if (put_user ((readl (card->core.addr + DVBM_LPFD_ICSR) &
			DVBM_LPFD_ICSR_RXD) ? 1 : 0, (int *)arg)) {
			return -EFAULT;
		}
		break;
	case ASI_IOC_RXSETPF:
		if (!(iface->capabilities & ASI_CAP_RX_PIDFILTER)) {
			return -ENOTTY;
		}
		if (copy_from_user (pflut, (unsigned int *)arg,
			sizeof (unsigned int [256]))) {
			return -EFAULT;
		}
		spin_lock (&card->reg_lock);
		for (i = 0; i < 256; i++) {
			writel (i, card->core.addr + DVBM_LPFD_PFLUTAR);
			wmb ();
			writel (pflut[i], card->core.addr + DVBM_LPFD_PFLUTR);
			wmb ();
		}
		spin_unlock (&card->reg_lock);
		break;
	default:
		return asi_rxioctl (iface, cmd, arg);
	}
	return 0;
}

/**
 * dvbm_lpfd_rxioctl - DVB Master LP FD receiver ioctl() method
 * @inode: inode
 * @filp: file
 * @cmd: ioctl command
 * @arg: ioctl argument
 *
 * Returns a negative error code on failure and 0 on success.
 **/
static int
dvbm_lpfd_rxioctl (struct inode *inode,
	struct file *filp,
	unsigned int cmd,
	unsigned long arg)
{
	return dvbm_lpfd_rxunlocked_ioctl (filp, cmd, arg);
}

/**
 * dvbm_lpfd_rxfsync - DVB Master LP FD receiver fsync() method
 * @filp: file to flush
 * @dentry: directory entry associated with the file
 * @datasync: used by filesystems
 *
 * Returns a negative error code on failure and 0 on success.
 **/
static int
dvbm_lpfd_rxfsync (struct file *filp,
	struct dentry *dentry,
	int datasync)
{
	struct master_iface *iface = filp->private_data;
	struct master_dev *card = iface->card;
	unsigned int reg;

	if (down_interruptible (&iface->buf_sem)) {
		return -ERESTARTSYS;
	}

	/* Stop the receiver */
	dvbm_lpfd_rxstop (iface);

	/* Reset the onboard FIFO and driver buffers */
	spin_lock (&card->reg_lock);
	reg = readl (card->core.addr + DVBM_LPFD_RCSR);
	writel (reg | DVBM_LPFD_RCSR_RST, card->core.addr + DVBM_LPFD_RCSR);
	wmb ();
	writel (reg, card->core.addr + DVBM_LPFD_RCSR);
	spin_unlock (&card->reg_lock);
	iface->events = 0;
	lsdma_reset (iface->dma);

	/* Start the receiver */
	dvbm_lpfd_rxstart (iface);

	up (&iface->buf_sem);
	return 0;
}

/**
 * dvbm_lpfd_rxrelease - DVB Master LP FD receiver release() method
 * @inode: inode
 * @filp: file
 *
 * Returns a negative error code on failure and 0 on success.
 **/
static int
dvbm_lpfd_rxrelease (struct inode *inode, struct file *filp)
{
	struct master_iface *iface = filp->private_data;

	return masterlsdma_release (iface, dvbm_lpfd_rxstop, dvbm_lpfd_rxexit);
}

