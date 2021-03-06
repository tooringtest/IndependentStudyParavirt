
#include <hypervisor.h>
#include <xen/xen.h>
#include <xen_events.h>
#include <xen/io/console.h>
#include <kdebug.h>

struct xencons_interface * xen_console;

void xencons_rx(char *buf, unsigned len, struct pt_regs *regs);
void xencons_tx(void);

/* TODO - need to define BUG_ON for whole mini-os, need crash-dump as well */
#define BUG_ON(_cond)   do{if(_cond) while(1);} while(0);

static inline struct xencons_interface *xencons_interface(void)
{
    return xen_console;
}

static inline void notify_daemon(void)
{
    /* Use evtchn: this is called early, before irq is set up. */
    notify_remote_via_evtchn(xen_start_info->console.domU.evtchn);
}

int xencons_ring_send_no_notify(const char *data, unsigned len)
{	
    int sent = 0;
	struct xencons_interface *intf = xencons_interface();
	XENCONS_RING_IDX cons, prod;
	cons = intf->out_cons;
	prod = intf->out_prod;
	mb();
	BUG_ON((prod - cons) > sizeof(intf->out));

	while ((sent < len) && ((prod - cons) < sizeof(intf->out)))
		intf->out[MASK_XENCONS_IDX(prod++, intf->out)] = data[sent++];

	wmb();
	intf->out_prod = prod;
    
    return sent;
}

int xencons_ring_send(const char *data, unsigned len)
{
    int sent;
    sent = xencons_ring_send_no_notify(data, len);
	notify_daemon();

	return sent;
}	



static void handle_input(evtchn_port_t port, 
			 struct pt_regs *regs, 
			 void *ign)
{
	struct xencons_interface *intf = xencons_interface();
	XENCONS_RING_IDX cons, prod;

	cons = intf->in_cons;
	prod = intf->in_prod;
	mb();
	BUG_ON((prod - cons) > sizeof(intf->in));

	while (cons != prod) {
		xencons_rx(intf->in+MASK_XENCONS_IDX(cons,intf->in), 
			   1, 
			   regs);
		cons++;
	}

	mb();
	intf->in_cons = cons;

	notify_daemon();

	xencons_tx();
}

int xencons_ring_init(void)
{
	int err;

	if (!xen_start_info || !xen_start_info->console.domU.evtchn)
		return 0;

	err = bind_evtchn(xen_start_info->console.domU.evtchn, 
			  handle_input,
			  NULL);
	if (err <= 0) {
		printk("XEN console request chn bind failed %i\n", err);
		return err;
	}

	/* In case we have in-flight data after save/restore... */
	notify_daemon();

	return 0;
}

void xencons_resume(void)
{
	(void)xencons_ring_init();
}

