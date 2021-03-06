
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stdint.h>
#include <stddef.h>

#include <xen/xen.h>
#include <xen_bitops.h>
#include <hypervisor.h>
#include <xen_events.h>
#include <xen_time.h>
#include <sched.h>
#include <kernel_timer.h>
#include <kdebug.h>

extern shared_info_t * xen_shared_info;

int numTicks;

/************************************************************************
 * Time functions
 *************************************************************************/

/* These are peridically updated in shared_info, and then copied here. */
struct shadow_time_info {
	u64 tsc_timestamp;     /* TSC at last update of time vals.  */
	u64 system_timestamp;  /* Time, in nanosecs, since boot.    */
	u32 tsc_to_nsec_mul;
	u32 tsc_to_usec_mul;
	int tsc_shift;
	u32 version;
};
static struct timespec shadow_ts;
static u32 shadow_ts_version;

static struct shadow_time_info shadow;


#ifndef rmb
#define rmb()  __asm__ __volatile__ ("lock; addl $0,0(%%esp)": : :"memory")
#endif

#define HANDLE_USEC_OVERFLOW(_tv)          \
    do {                                   \
        while ( (_tv)->tv_usec >= 1000000 ) \
        {                                  \
            (_tv)->tv_usec -= 1000000;      \
            (_tv)->tv_sec++;                \
        }                                  \
    } while ( 0 )

static inline int time_values_up_to_date(void)
{
	struct vcpu_time_info *src = &xen_shared_info->vcpu_info[0].time; 

	return (shadow.version == src->version);
}


/*
 * Scale a 64-bit delta by scaling and multiplying by a 32-bit fraction,
 * yielding a 64-bit result.
 */
static inline u64 scale_delta(u64 delta, u32 mul_frac, int shift)
{
	u64 product;
#ifdef __i386__
	u32 tmp1, tmp2;
#endif

	if ( shift < 0 )
		delta >>= -shift;
	else
		delta <<= shift;

#ifdef __i386__
	__asm__ (
		"mul  %5       ; "
		"mov  %4,%%eax ; "
		"mov  %%edx,%4 ; "
		"mul  %5       ; "
		"add  %4,%%eax ; "
		"xor  %5,%5    ; "
		"adc  %5,%%edx ; "
		: "=A" (product), "=r" (tmp1), "=r" (tmp2)
		: "a" ((u32)delta), "1" ((u32)(delta >> 32)), "2" (mul_frac) );
#else
	__asm__ (
		"mul %%rdx ; shrd $32,%%rdx,%%rax"
		: "=a" (product) : "0" (delta), "d" ((u64)mul_frac) );
#endif

	return product;
}


static unsigned long get_nsec_offset(void)
{
	u64 now, delta;
	rdtscll(now);
	delta = now - shadow.tsc_timestamp;
	return scale_delta(delta, shadow.tsc_to_nsec_mul, shadow.tsc_shift);
}


static void get_time_values_from_xen(void)
{
	struct vcpu_time_info    *src = &xen_shared_info->vcpu_info[0].time;

 	do {
		shadow.version = src->version;
		rmb();
		shadow.tsc_timestamp     = src->tsc_timestamp;
		shadow.system_timestamp  = src->system_time;
		shadow.tsc_to_nsec_mul   = src->tsc_to_system_mul;
		shadow.tsc_shift         = src->tsc_shift;
		rmb();
	}
	while ((src->version & 1) | (shadow.version ^ src->version));

	shadow.tsc_to_usec_mul = shadow.tsc_to_nsec_mul / 1000;
}




/* monotonic_clock(): returns # of nanoseconds passed since time_init()
 *		Note: This function is required to return accurate
 *		time even in the absence of multiple timer ticks.
 */
u64 monotonic_clock(void)
{
	u64 time;
	u32 local_time_version;

	do {
		local_time_version = shadow.version;
		rmb();
		time = shadow.system_timestamp + get_nsec_offset();
        if (!time_values_up_to_date())
			get_time_values_from_xen();
		rmb();
	} while (local_time_version != shadow.version);

	return time;
}

static void update_wallclock(void)
{
	shared_info_t *s = xen_shared_info;

	do {
		shadow_ts_version = s->wc_version;
		rmb();
		shadow_ts.ts_sec  = s->wc_sec;
		shadow_ts.ts_nsec = s->wc_nsec;
		rmb();
	}
	while ((s->wc_version & 1) | (shadow_ts_version ^ s->wc_version));
}


void gettimeofday(struct timeval *tv)
{
    u64 nsec = monotonic_clock();
    nsec += shadow_ts.ts_nsec;
    
    
    tv->tv_sec = shadow_ts.ts_sec;
    tv->tv_sec += NSEC_TO_SEC(nsec);
    tv->tv_usec = NSEC_TO_USEC(nsec % 1000000000UL);
}


void block_domain(s_time_t until)
{
    struct timeval tv;
    gettimeofday(&tv);
    if(monotonic_clock() < until)
    {
        HYPERVISOR_set_timer_op(until);
        HYPERVISOR_sched_op(SCHEDOP_block, 0);
    }
}


/*
 * Just a dummy 
 */
static void timer_handler(evtchn_port_t ev, struct pt_regs *regs, void *ign)
{
	/* Timer Top Half */
    get_time_values_from_xen();
    update_wallclock();

	kernel_tick(++numTicks);

	/* Reenable event delivery */
	clear_evtchn(ev);
	__sti();

	/* Call the Bottom Half here */
	sched_bottom_half();
}



void xen_init_time(void)
{
	numTicks = 0;
    printk("Initialising timer interface\n");
    bind_virq(VIRQ_TIMER, &timer_handler, NULL);
}
