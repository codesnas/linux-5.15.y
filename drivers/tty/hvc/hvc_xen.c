// SPDX-License-Identifier: GPL-2.0+
/*
 * xen console driver interface to hvc_console.c
 *
 * (c) 2007 Gerd Hoffmann <kraxel@suse.de>
 */

#include <linux/console.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/irq.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/list.h>
#include <linux/serial_core.h>

#include <asm/io.h>
#include <asm/xen/hypervisor.h>

#include <xen/xen.h>
#include <xen/interface/xen.h>
#include <xen/hvm.h>
#include <xen/grant_table.h>
#include <xen/page.h>
#include <xen/events.h>
#include <xen/interface/io/console.h>
#include <xen/interface/sched.h>
#include <xen/hvc-console.h>
#include <xen/xenbus.h>

#include "hvc_console.h"

#define HVC_COOKIE   0x58656e /* "Xen" in hex */

struct xencons_info {
	struct list_head list;
	struct xenbus_device *xbdev;
	struct xencons_interface *intf;
	unsigned int evtchn;
	XENCONS_RING_IDX out_cons;
	unsigned int out_cons_same;
	struct hvc_struct *hvc;
	int irq;
	int vtermno;
	grant_ref_t gntref;
	spinlock_t ring_lock;
};

static LIST_HEAD(xenconsoles);
static DEFINE_SPINLOCK(xencons_lock);

/* ------------------------------------------------------------------ */

static struct xencons_info *vtermno_to_xencons(int vtermno)
{
	struct xencons_info *entry, *ret = NULL;
	unsigned long flags;

	spin_lock_irqsave(&xencons_lock, flags);
	if (list_empty(&xenconsoles)) {
		spin_unlock_irqrestore(&xencons_lock, flags);
		return NULL;
	}

	list_for_each_entry(entry, &xenconsoles, list) {
		if (entry->vtermno == vtermno) {
			ret  = entry;
			break;
		}
	}
	spin_unlock_irqrestore(&xencons_lock, flags);

	return ret;
}

static inline int xenbus_devid_to_vtermno(int devid)
{
	return devid + HVC_COOKIE;
}

static inline void notify_daemon(struct xencons_info *cons)
{
	/* Use evtchn: this is called early, before irq is set up. */
	notify_remote_via_evtchn(cons->evtchn);
}

static int __write_console(struct xencons_info *xencons,
		const char *data, int len)
{
	XENCONS_RING_IDX cons, prod;
	struct xencons_interface *intf = xencons->intf;
	int sent = 0;
	unsigned long flags;

	spin_lock_irqsave(&xencons->ring_lock, flags);
	cons = intf->out_cons;
	prod = intf->out_prod;
	mb();			/* update queue values before going on */

	if ((prod - cons) > sizeof(intf->out)) {
		spin_unlock_irqrestore(&xencons->ring_lock, flags);
		pr_err_once("xencons: Illegal ring page indices");
		return -EINVAL;
	}

	while ((sent < len) && ((prod - cons) < sizeof(intf->out)))
		intf->out[MASK_XENCONS_IDX(prod++, intf->out)] = data[sent++];

	wmb();			/* write ring before updating pointer */
	intf->out_prod = prod;
	spin_unlock_irqrestore(&xencons->ring_lock, flags);

	if (sent)
		notify_daemon(xencons);
	return sent;
}

static int domU_write_console(uint32_t vtermno, const char *data, int len)
{
	int ret = len;
	struct xencons_info *cons = vtermno_to_xencons(vtermno);
	if (cons == NULL)
		return -EINVAL;

	/*
	 * Make sure the whole buffer is emitted, polling if
	 * necessary.  We don't ever want to rely on the hvc daemon
	 * because the most interesting console output is when the
	 * kernel is crippled.
	 */
	while (len) {
		int sent = __write_console(cons, data, len);

		if (sent < 0)
			return sent;

		data += sent;
		len -= sent;

		if (unlikely(len))
			HYPERVISOR_sched_op(SCHEDOP_yield, NULL);
	}

	return ret;
}

static int domU_read_console(uint32_t vtermno, char *buf, int len)
{
	struct xencons_interface *intf;
	XENCONS_RING_IDX cons, prod;
	int recv = 0;
	struct xencons_info *xencons = vtermno_to_xencons(vtermno);
	unsigned int eoiflag = 0;
	unsigned long flags;

	if (xencons == NULL)
		return -EINVAL;
	intf = xencons->intf;

	spin_lock_irqsave(&xencons->ring_lock, flags);
	cons = intf->in_cons;
	prod = intf->in_prod;
	mb();			/* get pointers before reading ring */

	if ((prod - cons) > sizeof(intf->in)) {
		spin_unlock_irqrestore(&xencons->ring_lock, flags);
		pr_err_once("xencons: Illegal ring page indices");
		return -EINVAL;
	}

	while (cons != prod && recv < len)
		buf[recv++] = intf->in[MASK_XENCONS_IDX(cons++, intf->in)];

	mb();			/* read ring before consuming */
	intf->in_cons = cons;

	/*
	 * When to mark interrupt having been spurious:
	 * - there was no new data to be read, and
	 * - the backend did not consume some output bytes, and
	 * - the previous round with no read data didn't see consumed bytes
	 *   (we might have a race with an interrupt being in flight while
	 *   updating xencons->out_cons, so account for that by allowing one
	 *   round without any visible reason)
	 */
	if (intf->out_cons != xencons->out_cons) {
		xencons->out_cons = intf->out_cons;
		xencons->out_cons_same = 0;
	}
	if (!recv && xencons->out_cons_same++ > 1) {
		eoiflag = XEN_EOI_FLAG_SPURIOUS;
	}
	spin_unlock_irqrestore(&xencons->ring_lock, flags);

	if (recv) {
		notify_daemon(xencons);
	}

	xen_irq_lateeoi(xencons->irq, eoiflag);

	return recv;
}

static const struct hv_ops domU_hvc_ops = {
	.get_chars = domU_read_console,
	.put_chars = domU_write_console,
	.notifier_add = notifier_add_irq,
	.notifier_del = notifier_del_irq,
	.notifier_hangup = notifier_hangup_irq,
};

static int dom0_read_console(uint32_t vtermno, char *buf, int len)
{
	return HYPERVISOR_console_io(CONSOLEIO_read, len, buf);
}

/*
 * Either for a dom0 to write to the system console, or a domU with a
 * debug version of Xen
 */
static int dom0_write_console(uint32_t vtermno, const char *str, int len)
{
	int rc = HYPERVISOR_console_io(CONSOLEIO_write, len, (char *)str);
	if (rc < 0)
		return rc;

	return len;
}

static const struct hv_ops dom0_hvc_ops = {
	.get_chars = dom0_read_console,
	.put_chars = dom0_write_console,
	.notifier_add = notifier_add_irq,
	.notifier_del = notifier_del_irq,
	.notifier_hangup = notifier_hangup_irq,
};

static int xen_hvm_console_init(void)
{
	int r;
	uint64_t v = 0;
	unsigned long gfn, flags;
	struct xencons_info *info;

	if (!xen_hvm_domain())
		return -ENODEV;

	info = vtermno_to_xencons(HVC_COOKIE);
	if (!info) {
		info = kzalloc(sizeof(struct xencons_info), GFP_KERNEL);
		if (!info)
			return -ENOMEM;
		spin_lock_init(&info->ring_lock);
	} else if (info->intf != NULL) {
		/* already configured */
		return 0;
	}
	/*
	 * If the toolstack (or the hypervisor) hasn't set these values, the
	 * default value is 0. Even though gfn = 0 and evtchn = 0 are
	 * theoretically correct values, in practice they never are and they
	 * mean that a legacy toolstack hasn't initialized the pv console correctly.
	 */
	r = hvm_get_parameter(HVM_PARAM_CONSOLE_EVTCHN, &v);
	if (r < 0 || v == 0)
		goto err;
	info->evtchn = v;
	v = 0;
	r = hvm_get_parameter(HVM_PARAM_CONSOLE_PFN, &v);
	if (r < 0 || v == 0)
		goto err;
	gfn = v;
	info->intf = memremap(gfn << XEN_PAGE_SHIFT, XEN_PAGE_SIZE, MEMREMAP_WB);
	if (info->intf == NULL)
		goto err;
	info->vtermno = HVC_COOKIE;

	spin_lock_irqsave(&xencons_lock, flags);
	list_add_tail(&info->list, &xenconsoles);
	spin_unlock_irqrestore(&xencons_lock, flags);

	return 0;
err:
	kfree(info);
	return -ENODEV;
}

static int xencons_info_pv_init(struct xencons_info *info, int vtermno)
{
	spin_lock_init(&info->ring_lock);
	info->evtchn = xen_start_info->console.domU.evtchn;
	/* GFN == MFN for PV guest */
	info->intf = gfn_to_virt(xen_start_info->console.domU.mfn);
	info->vtermno = vtermno;

	list_add_tail(&info->list, &xenconsoles);

	return 0;
}

static int xen_pv_console_init(void)
{
	struct xencons_info *info;
	unsigned long flags;

	if (!xen_pv_domain())
		return -ENODEV;

	if (!xen_start_info->console.domU.evtchn)
		return -ENODEV;

	info = vtermno_to_xencons(HVC_COOKIE);
	if (!info) {
		info = kzalloc(sizeof(struct xencons_info), GFP_KERNEL);
		if (!info)
			return -ENOMEM;
	} else if (info->intf != NULL) {
		/* already configured */
		return 0;
	}
	spin_lock_irqsave(&xencons_lock, flags);
	xencons_info_pv_init(info, HVC_COOKIE);
	spin_unlock_irqrestore(&xencons_lock, flags);

	return 0;
}

static int xen_initial_domain_console_init(void)
{
	struct xencons_info *info;
	unsigned long flags;

	if (!xen_initial_domain())
		return -ENODEV;

	info = vtermno_to_xencons(HVC_COOKIE);
	if (!info) {
		info = kzalloc(sizeof(struct xencons_info), GFP_KERNEL);
		if (!info)
			return -ENOMEM;
		spin_lock_init(&info->ring_lock);
	}

	info->irq = bind_virq_to_irq(VIRQ_CONSOLE, 0, false);
	info->vtermno = HVC_COOKIE;

	spin_lock_irqsave(&xencons_lock, flags);
	list_add_tail(&info->list, &xenconsoles);
	spin_unlock_irqrestore(&xencons_lock, flags);

	return 0;
}

static void xen_console_update_evtchn(struct xencons_info *info)
{
	if (xen_hvm_domain()) {
		uint64_t v = 0;
		int err;

		err = hvm_get_parameter(HVM_PARAM_CONSOLE_EVTCHN, &v);
		if (!err && v)
			info->evtchn = v;
	} else
		info->evtchn = xen_start_info->console.domU.evtchn;
}

void xen_console_resume(void)
{
	struct xencons_info *info = vtermno_to_xencons(HVC_COOKIE);
	if (info != NULL && info->irq) {
		if (!xen_initial_domain())
			xen_console_update_evtchn(info);
		rebind_evtchn_irq(info->evtchn, info->irq);
	}
}

#ifdef CONFIG_HVC_XEN_FRONTEND
static void xencons_disconnect_backend(struct xencons_info *info)
{
	if (info->hvc != NULL)
		hvc_remove(info->hvc);
	info->hvc = NULL;
	if (info->irq > 0) {
		evtchn_put(info->evtchn);
		info->irq = 0;
		info->evtchn = 0;
	}
	/* evtchn_put() will also close it so this is only an error path */
	if (info->evtchn > 0)
		xenbus_free_evtchn(info->xbdev, info->evtchn);
	info->evtchn = 0;
	if (info->gntref > 0)
		gnttab_free_grant_references(info->gntref);
	info->gntref = 0;
}

static void xencons_free(struct xencons_info *info)
{
	free_page((unsigned long)info->intf);
	info->intf = NULL;
	info->vtermno = 0;
	kfree(info);
}

static int xen_console_remove(struct xencons_info *info)
{
	unsigned long flags;

	xencons_disconnect_backend(info);
	spin_lock_irqsave(&xencons_lock, flags);
	list_del(&info->list);
	spin_unlock_irqrestore(&xencons_lock, flags);
	if (info->xbdev != NULL)
		xencons_free(info);
	else {
		if (xen_hvm_domain())
			iounmap(info->intf);
		kfree(info);
	}
	return 0;
}

static int xencons_remove(struct xenbus_device *dev)
{
	return xen_console_remove(dev_get_drvdata(&dev->dev));
}

static int xencons_connect_backend(struct xenbus_device *dev,
				  struct xencons_info *info)
{
	int ret, evtchn, devid, ref, irq;
	struct xenbus_transaction xbt;
	grant_ref_t gref_head;

	ret = xenbus_alloc_evtchn(dev, &evtchn);
	if (ret)
		return ret;
	info->evtchn = evtchn;
	irq = bind_evtchn_to_irq_lateeoi(evtchn);
	if (irq < 0)
		return irq;
	info->irq = irq;
	devid = dev->nodename[strlen(dev->nodename) - 1] - '0';
	info->hvc = hvc_alloc(xenbus_devid_to_vtermno(devid),
			irq, &domU_hvc_ops, 256);
	if (IS_ERR(info->hvc))
		return PTR_ERR(info->hvc);
	ret = gnttab_alloc_grant_references(1, &gref_head);
	if (ret < 0)
		return ret;
	info->gntref = gref_head;
	ref = gnttab_claim_grant_reference(&gref_head);
	if (ref < 0)
		return ref;
	gnttab_grant_foreign_access_ref(ref, info->xbdev->otherend_id,
					virt_to_gfn(info->intf), 0);

 again:
	ret = xenbus_transaction_start(&xbt);
	if (ret) {
		xenbus_dev_fatal(dev, ret, "starting transaction");
		return ret;
	}
	ret = xenbus_printf(xbt, dev->nodename, "ring-ref", "%d", ref);
	if (ret)
		goto error_xenbus;
	ret = xenbus_printf(xbt, dev->nodename, "port", "%u",
			    evtchn);
	if (ret)
		goto error_xenbus;
	ret = xenbus_transaction_end(xbt, 0);
	if (ret) {
		if (ret == -EAGAIN)
			goto again;
		xenbus_dev_fatal(dev, ret, "completing transaction");
		return ret;
	}

	xenbus_switch_state(dev, XenbusStateInitialised);
	return 0;

 error_xenbus:
	xenbus_transaction_end(xbt, 1);
	xenbus_dev_fatal(dev, ret, "writing xenstore");
	return ret;
}

static int xencons_probe(struct xenbus_device *dev,
				  const struct xenbus_device_id *id)
{
	int ret, devid;
	struct xencons_info *info;
	unsigned long flags;

	devid = dev->nodename[strlen(dev->nodename) - 1] - '0';
	if (devid == 0)
		return -ENODEV;

	info = kzalloc(sizeof(struct xencons_info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;
	spin_lock_init(&info->ring_lock);
	dev_set_drvdata(&dev->dev, info);
	info->xbdev = dev;
	info->vtermno = xenbus_devid_to_vtermno(devid);
	info->intf = (void *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
	if (!info->intf)
		goto error_nomem;

	ret = xencons_connect_backend(dev, info);
	if (ret < 0)
		goto error;
	spin_lock_irqsave(&xencons_lock, flags);
	list_add_tail(&info->list, &xenconsoles);
	spin_unlock_irqrestore(&xencons_lock, flags);

	return 0;

 error_nomem:
	ret = -ENOMEM;
	xenbus_dev_fatal(dev, ret, "allocating device memory");
 error:
	xencons_disconnect_backend(info);
	xencons_free(info);
	return ret;
}

static int xencons_resume(struct xenbus_device *dev)
{
	struct xencons_info *info = dev_get_drvdata(&dev->dev);

	xencons_disconnect_backend(info);
	memset(info->intf, 0, XEN_PAGE_SIZE);
	return xencons_connect_backend(dev, info);
}

static void xencons_backend_changed(struct xenbus_device *dev,
				   enum xenbus_state backend_state)
{
	switch (backend_state) {
	case XenbusStateReconfiguring:
	case XenbusStateReconfigured:
	case XenbusStateInitialising:
	case XenbusStateInitialised:
	case XenbusStateUnknown:
		break;

	case XenbusStateInitWait:
		break;

	case XenbusStateConnected:
		xenbus_switch_state(dev, XenbusStateConnected);
		break;

	case XenbusStateClosed:
		if (dev->state == XenbusStateClosed)
			break;
		fallthrough;	/* Missed the backend's CLOSING state */
	case XenbusStateClosing: {
		struct xencons_info *info = dev_get_drvdata(&dev->dev);;

		/*
		 * Don't tear down the evtchn and grant ref before the other
		 * end has disconnected, but do stop userspace from trying
		 * to use the device before we allow the backend to close.
		 */
		if (info->hvc) {
			hvc_remove(info->hvc);
			info->hvc = NULL;
		}

		xenbus_frontend_closed(dev);
		break;
	}
	}
}

static const struct xenbus_device_id xencons_ids[] = {
	{ "console" },
	{ "" }
};

static struct xenbus_driver xencons_driver = {
	.name = "xenconsole",
	.ids = xencons_ids,
	.probe = xencons_probe,
	.remove = xencons_remove,
	.resume = xencons_resume,
	.otherend_changed = xencons_backend_changed,
};
#endif /* CONFIG_HVC_XEN_FRONTEND */

static int __init xen_hvc_init(void)
{
	int r;
	struct xencons_info *info;
	const struct hv_ops *ops;

	if (!xen_domain())
		return -ENODEV;

	if (xen_initial_domain()) {
		ops = &dom0_hvc_ops;
		r = xen_initial_domain_console_init();
		if (r < 0)
			goto register_fe;
		info = vtermno_to_xencons(HVC_COOKIE);
	} else {
		ops = &domU_hvc_ops;
		if (xen_hvm_domain())
			r = xen_hvm_console_init();
		else
			r = xen_pv_console_init();
		if (r < 0)
			goto register_fe;

		info = vtermno_to_xencons(HVC_COOKIE);
		info->irq = bind_evtchn_to_irq_lateeoi(info->evtchn);
	}
	if (info->irq < 0)
		info->irq = 0; /* NO_IRQ */
	else
		irq_set_noprobe(info->irq);

	info->hvc = hvc_alloc(HVC_COOKIE, info->irq, ops, 256);
	if (IS_ERR(info->hvc)) {
		unsigned long flags;

		r = PTR_ERR(info->hvc);
		spin_lock_irqsave(&xencons_lock, flags);
		list_del(&info->list);
		spin_unlock_irqrestore(&xencons_lock, flags);
		if (info->irq)
			evtchn_put(info->evtchn);
		kfree(info);
		return r;
	}

	r = 0;
 register_fe:
#ifdef CONFIG_HVC_XEN_FRONTEND
	r = xenbus_register_frontend(&xencons_driver);
#endif
	return r;
}
device_initcall(xen_hvc_init);

static int xen_cons_init(void)
{
	const struct hv_ops *ops;

	if (!xen_domain())
		return 0;

	if (xen_initial_domain())
		ops = &dom0_hvc_ops;
	else {
		int r;
		ops = &domU_hvc_ops;

		if (xen_hvm_domain())
			r = xen_hvm_console_init();
		else
			r = xen_pv_console_init();
		if (r < 0)
			return r;
	}

	hvc_instantiate(HVC_COOKIE, 0, ops);
	return 0;
}
console_initcall(xen_cons_init);

#ifdef CONFIG_X86
static void xen_hvm_early_write(uint32_t vtermno, const char *str, int len)
{
	if (xen_cpuid_base())
		outsb(0xe9, str, len);
}
#else
static void xen_hvm_early_write(uint32_t vtermno, const char *str, int len) { }
#endif

#ifdef CONFIG_EARLY_PRINTK
static int __init xenboot_console_setup(struct console *console, char *string)
{
	static struct xencons_info xenboot;

	if (xen_initial_domain() || !xen_pv_domain())
		return 0;

	return xencons_info_pv_init(&xenboot, 0);
}

static void xenboot_write_console(struct console *console, const char *string,
				  unsigned len)
{
	unsigned int linelen, off = 0;
	const char *pos;

	if (dom0_write_console(0, string, len) >= 0)
		return;

	if (!xen_pv_domain()) {
		xen_hvm_early_write(0, string, len);
		return;
	}

	if (domU_write_console(0, "(early) ", 8) < 0)
		return;
	while (off < len && NULL != (pos = strchr(string+off, '\n'))) {
		linelen = pos-string+off;
		if (off + linelen > len)
			break;
		domU_write_console(0, string+off, linelen);
		domU_write_console(0, "\r\n", 2);
		off += linelen + 1;
	}
	if (off < len)
		domU_write_console(0, string+off, len-off);
}

struct console xenboot_console = {
	.name		= "xenboot",
	.write		= xenboot_write_console,
	.setup		= xenboot_console_setup,
	.flags		= CON_PRINTBUFFER | CON_BOOT | CON_ANYTIME,
	.index		= -1,
};
#endif	/* CONFIG_EARLY_PRINTK */

void xen_raw_console_write(const char *str)
{
	ssize_t len = strlen(str);
	int rc = 0;

	if (xen_domain()) {
		rc = dom0_write_console(0, str, len);
		if (rc != -ENOSYS || !xen_hvm_domain())
			return;
	}
	xen_hvm_early_write(0, str, len);
}

void xen_raw_printk(const char *fmt, ...)
{
	static char buf[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	xen_raw_console_write(buf);
}

static void xenboot_earlycon_write(struct console *console,
				  const char *string,
				  unsigned len)
{
	dom0_write_console(0, string, len);
}

static int __init xenboot_earlycon_setup(struct earlycon_device *device,
					    const char *opt)
{
	device->con->write = xenboot_earlycon_write;
	return 0;
}
EARLYCON_DECLARE(xenboot, xenboot_earlycon_setup);
