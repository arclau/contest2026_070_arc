#include "autoconf.h"
#include <osdep_service.h>
#include <nuttx/mqueue.h>
#include <syslog.h>
#include <nuttx/config.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <arpa/inet.h>
#include <nuttx/arch.h>
#include <nuttx/wqueue.h>
#include <nuttx/kthread.h>
#include <nuttx/mutex.h>
#include <nuttx/spinlock.h>
#include <semaphore.h>
#include <sched.h>
#include "customer_rtos_service.h"
#include <nuttx/syslog/syslog.h>
/********************* os depended utilities ********************/

//PRIORITIE_OFFSET  defined to adjust the priority of threads in wlan_lib
unsigned int g_prioritie_offset = 4;
static uint8_t g_rtos_service_stack[409600];
static struct kwork_wqueue_s * g_worker;

//----- ------------------------------------------------------------------
// Misc Function
//----- ------------------------------------------------------------------

int rtw_printf(const char *format, ...)
{
    /* Quiet: suppress the chatty RTL871X info logs (scan/join/essid dumps,
     * passphrase echo, BT mp polling) that flood the console. Keep only
     * error-ish lines so real failures stay visible. */
    if (format != NULL &&
        (strstr(format, "FAIL") || strstr(format, "fail") ||
         strstr(format, "ERROR") || strstr(format, "error") ||
         strstr(format, "Assert")))
    {
        va_list args;
        va_start(args, format);
        vsyslog(LOG_INFO, format, args);
        va_end(args);
    }
    return 0;
}

#if defined(CONFIG_MP_LOG_FUNC_INDEPENDENT) && CONFIG_MP_LOG_FUNC_INDEPENDENT
int rtw_mplog(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	vsyslog(LOG_INFO, format, args);
	va_end(args);
	return 0;
}
#endif


void save_and_cli(_irqL* irql)
{
	*irql=enter_critical_section();
}

void restore_flags(_irqL irql)
{
	leave_critical_section(irql);
}

void cli()
{
}

/* Not needed on 64bit architectures */
static unsigned int __div64_32(u64 *n, unsigned int base)
{
	u64 rem = *n;
	u64 b = base;
	u64 res, d = 1;
	unsigned int high = rem >> 32;

	/* Reduce the thing a bit first */
	res = 0;
	if (high >= base) {
		high /= base;
		res = (u64) high << 32;
		rem -= (u64) (high * base) << 32;
	}

	while ((u64)b > 0 && b < rem) {
		b = b+b;
		d = d+d;
	}

	do {
		if (rem >= b) {
			rem -= b;
			res += d;
		}
		b >>= 1;
		d >>= 1;
	} while (d);

	*n = res;
	return rem;
}

/********************* os depended service ********************/

u8* _freertos_malloc(u32 sz)
{
	return malloc(sz);
}

u8* _freertos_zmalloc(u32 sz)
{
	return  calloc(1, sz);
}

void _freertos_mfree(u8 *pbuf, u32 sz)
{
	free(pbuf);
}

static void _freertos_memcpy(void* dst, const void* src, u32 sz)
{
	memcpy(dst, src, sz);
}

static int _freertos_memcmp(void *dst, void *src, u32 sz)
{
//under Linux/GNU/GLibc, the return value of memcmp for two same mem. chunk is 0
	if (!(memcmp(dst, src, sz)))
		return 1;

	return 0;
}

static void _freertos_memset(void *pbuf, int c, u32 sz)
{
	memset(pbuf, c, sz);
}

static void _freertos_init_sema(_sema *sema, int init_val)
{
	sem_t *_sema_t;
	_sema_t = calloc(1, sizeof(sem_t));
  	if (!_sema_t)
    {
      return;
    }

	if (sem_init(_sema_t, 0, init_val))
	    {
      free(_sema_t);
      return;
    }
	*sema = _sema_t;
}

static void _freertos_free_sema(_sema *sema)
{
	sem_destroy(*sema);
  	free(*sema);
  	*sema = NULL;
}

static void _freertos_up_sema(_sema *sema)
{
	sem_post(*sema);
}

static void _freertos_up_sema_from_isr(_sema *sema)
{
	_freertos_up_sema(sema);
}

static u32 _freertos_down_sema(_sema *sema, u32 timeout)
{
	struct timespec abstime;
	int ret;
	if(timeout == RTW_MAX_DELAY) {
		ret = sem_wait(*sema);
	} else {
		clock_gettime(CLOCK_MONOTONIC, &abstime);
        abstime.tv_sec += timeout / 1000;
        abstime.tv_nsec += (timeout % 1000) * 1000 * 1000;
        if (abstime.tv_nsec >= (1000 * 1000000))
        {
            abstime.tv_sec += 1;
            abstime.tv_nsec -= (1000 * 1000000);
        }
		ret = sem_clockwait(*sema, CLOCK_MONOTONIC, &abstime);

	}

	return !ret;
}

static void _freertos_mutex_init(_mutex *pmutex)
{
	mutex_t *_mutex_t;
	_mutex_t = calloc(1, sizeof(mutex_t));
  	if (!_mutex_t)
    {
      return;
    }

	if (nxmutex_init(_mutex_t))
	{
      free(_mutex_t);
      return;
    }

	*pmutex = _mutex_t;
}

static void _freertos_mutex_free(_mutex *pmutex)
{
	nxmutex_destroy(*pmutex);
  	free(*pmutex);
	*pmutex = NULL;
}

static void _freertos_mutex_get(_lock *plock)
{
	nxmutex_lock(*plock);
}

static int _freertos_mutex_get_timeout(_lock *plock, u32 timeout_ms)
{
	return nxmutex_timedlock(*plock, timeout_ms);
}

static void _freertos_mutex_put(_lock *plock)
{
	nxmutex_unlock(*plock);
}

static void _freertos_enter_critical(_lock *plock, _irqL *pirqL)
{
	save_and_cli(pirqL);
}

static void _freertos_exit_critical(_lock *plock, _irqL *pirqL)
{
	restore_flags(*pirqL);
}

static void _freertos_enter_critical_from_isr(_lock *plock, _irqL *pirqL)
{

}

static void _freertos_exit_critical_from_isr(_lock *plock, _irqL *pirqL)
{

}

static int _freertos_enter_critical_mutex(_mutex *pmutex, _irqL *pirqL)
{
	return nxmutex_lock(*pmutex);
}

static void _freertos_exit_critical_mutex(_mutex *pmutex, _irqL *pirqL)
{
	nxmutex_unlock(*pmutex);
}

static void _freertos_cpu_lock(void)
{

}

static void _freertos_cpu_unlock(void)
{

}

static void _freertos_spinlock_init(_lock *plock)
{
	#if USE_MUTEX_FOR_SPINLOCK
		rtw_mutex_init(plock);
	#endif
}

static void _freertos_spinlock_free(_lock *plock)
{
	#if USE_MUTEX_FOR_SPINLOCK
		rtw_mutex_free(plock);
	#endif
}
static void _freertos_spinlock(_lock *plock)
{
#if USE_MUTEX_FOR_SPINLOCK
	while(rtw_mutex_get_timeout(plock, 60 * 1000) != 0)
		DBG_INFO("%s(%p) failed, retry\n", __FUNCTION__, plock);
#endif
}

static void _freertos_spinunlock(_lock *plock)
{
	#if USE_MUTEX_FOR_SPINLOCK
		rtw_mutex_put(plock);
	#endif
}


static void _freertos_spinlock_irqsave(_lock *plock, _irqL *irqL)
{
}

static void _freertos_spinunlock_irqsave(_lock *plock, _irqL *irqL)
{

}

static int _freertos_init_xqueue( _xqueue* queue, const char* name, u32 message_size, u32 number_of_messages )
{
  struct mq_attr attr;
  struct file *mq;
  int ret;
  mq = malloc(sizeof(struct file));
  if (!mq)
    {
      return -ENOMEM;
    }

  attr.mq_maxmsg = number_of_messages;
  attr.mq_msgsize = message_size;
  attr.mq_curmsgs = 0;
  attr.mq_flags = 0;
  ret = file_mq_open(mq, name, O_RDWR | O_CREAT, 0644, &attr);
  if (ret < 0)
    {
      free(mq);
      return -ENOMEM;
    }

  *queue = mq;
  return 0;
}

static int _freertos_push_to_xqueue( _xqueue* queue, void* message, u32 timeout_ms )
{
  struct file *mq = *queue;
  struct mq_attr attr;
  file_mq_getattr(mq, &attr);
  return file_mq_send(mq, message, attr.mq_msgsize, 1);
}

static int _freertos_pop_from_xqueue( _xqueue* queue, void* message, u32 timeout_ms )
{
  struct file *mq = *queue;
  struct mq_attr attr;
  unsigned int prio;
  file_mq_getattr(mq, &attr);
  return !file_mq_receive(mq, message, attr.mq_msgsize, &prio);
}

static int _freertos_deinit_xqueue( _xqueue* queue )
{
  struct file *mq = *queue;
  int ret;
  ret = file_mq_close(mq);
  if (ret >= 0)
    {
      free(mq);
    }

  return ret;
}

static u32 _freertos_get_current_time(void)
{
	return clock();
}

static u32 _freertos_systime_to_ms(u32 systime_t)
{
	return TICK2MSEC(systime_t);
}

static u32 _freertos_systime_to_sec(u32 systime_t)
{
	return TICK2SEC(systime_t);
}

static u32 _freertos_ms_to_systime(u32 ms)
{
	return MSEC2TICK(ms);
}

static u32 _freertos_sec_to_systime(u32 sec)
{
	return SEC2TICK(sec);
}

static void _freertos_msleep_os(int ms)
{
	rtw_usleep_os(ms * 1000);
}

static void _freertos_usleep_os(int us)
{
  if (us >= CONFIG_USEC_PER_TICK)
    {
      usleep(us);
    }
  else
    {
      up_udelay(us);
	  //extern void delay_us_block(uint32_t us);
	  //delay_us_block(us);
    }

}

static void _freertos_mdelay_os(int ms)
{
	rtw_msleep_os(ms);
}

static void _freertos_udelay_os(int us)
{
	rtw_usleep_os(us);
}

static void _freertos_yield_os(void)
{
	sched_yield();
}

static void _freertos_ATOMIC_SET(ATOMIC_T *v, int i)
{
	v->counter = i;
}

static int _freertos_ATOMIC_READ(ATOMIC_T *v)
{
	return v->counter;
}

static void _freertos_ATOMIC_ADD(ATOMIC_T *v, int i)
{
	_irqL irql;
	save_and_cli(&irql);
	v->counter += i;
	restore_flags(irql);
}

static void _freertos_ATOMIC_SUB(ATOMIC_T *v, int i)
{
	_irqL irql;
	save_and_cli(&irql);
	v->counter -= i;
	restore_flags(irql);
}

static void _freertos_ATOMIC_INC(ATOMIC_T *v)
{
	_freertos_ATOMIC_ADD(v, 1);
}

static void _freertos_ATOMIC_DEC(ATOMIC_T *v)
{
	_freertos_ATOMIC_SUB(v, 1);
}

static int _freertos_ATOMIC_ADD_RETURN(ATOMIC_T *v, int i)
{
	int temp;
	_irqL irql;
	save_and_cli(&irql);
	temp = v->counter;
	temp += i;
	v->counter = temp;
	restore_flags(irql);

	return temp;
}

static int _freertos_ATOMIC_SUB_RETURN(ATOMIC_T *v, int i)
{
	int temp;
	_irqL irql;
	save_and_cli(&irql);
	temp = v->counter;
	temp -= i;
	v->counter = temp;
	restore_flags(irql);

	return temp;
}

static int _freertos_ATOMIC_INC_RETURN(ATOMIC_T *v)
{
	return _freertos_ATOMIC_ADD_RETURN(v, 1);
}

static int _freertos_ATOMIC_DEC_RETURN(ATOMIC_T *v)
{
	return _freertos_ATOMIC_SUB_RETURN(v, 1);
}

static u64 _freertos_modular64(u64 n, u64 base)
{
	unsigned int __base = (base);
	unsigned int __rem;

	if (((n) >> 32) == 0) {
		__rem = (unsigned int)(n) % __base;
		(n) = (unsigned int)(n) / __base;
	}
	else
		__rem = __div64_32(&(n), __base);

	return __rem;
}

/* Refer to ecos bsd tcpip codes */
static int _freertos_arc4random(void)
{
	u32 res = rtw_get_current_time();
	static unsigned long seed = 0xDEADB00B;

	seed = ((seed & 0x007F00FF) << 7) ^
	    ((seed & 0x0F80FF00) >> 8) ^ // be sure to stir those low bits
	    (res << 13) ^ (res >> 9);    // using the clock too!
	return (int)seed;
}

static int _freertos_get_random_bytes(void *buf, uint32_t len)
{
#if 1 //becuase of 4-byte align, we use the follow code style.
	unsigned int ranbuf;
	unsigned int *lp;
	int i, count;
	count = len / sizeof(unsigned int);
	lp = (unsigned int *) buf;

	for(i = 0; i < count; i ++) {
		lp[i] = _freertos_arc4random();
		len -= sizeof(unsigned int);
	}

	if(len > 0) {
		ranbuf = _freertos_arc4random();
		_freertos_memcpy(&lp[i], &ranbuf, len);
	}
	return 0;
#else
	unsigned long ranbuf, *lp;
	lp = (unsigned long *)buf;
	while (len > 0) {
		ranbuf = _freertos_arc4random();
		*lp++ = ranbuf; //this op need the pointer is 4Byte-align!
		len -= sizeof(ranbuf);
	}
	return 0;
#endif
}

static u32 _freertos_GetFreeHeapSize(void)
{
	struct mallinfo mem;
	mem = mallinfo();
	return mem.arena;
}

static int nuttx_task_hook(int argc, FAR char *argv[])
{
  struct task_struct *ptask;
  struct nthread_wrapper *wrap;
  ptask = (FAR struct task_struct *)
         ((uintptr_t)strtoul(argv[1], NULL, 0));
  if (!ptask || !ptask->task)
    {
      return 0;
    }

  wrap = ptask->task;
  if (wrap->func)
  {
	wrap->func(wrap->thctx);
  }

  return 0;
}

static int _freertos_create_task(struct task_struct *ptask, const char *name,
	u32  stack_size, u32 priority, thread_func_t func, void *thctx)
{
	struct nthread_wrapper *wrap;
	char *argv[2];
	char arg1[16];
	int pid;
	snprintf(arg1, 16, "0x%" PRIxPTR, (uintptr_t)ptask);
	argv[0] = arg1;
	argv[1] = NULL;
	wrap = malloc(sizeof(*wrap));
	if (!wrap)
    {
		return -ENOMEM;
    }
	ptask->task_name = name;

	if(func){
		wrap->func = func;
		wrap->thctx = thctx;
	}

	priority += SCHED_PRIORITY_DEFAULT;
	ptask->task = wrap;

	pid = kthread_create(name,
                       priority,
                       stack_size * sizeof(int) * 5,
                       nuttx_task_hook, argv);
  if (pid < 0)
    {
      free(wrap);
	  DBG_INFO("Create Task \"%s\" Failed! ret=%d\n", ptask->task_name, pid);
      return pid;
    }

	wrap->pid = pid;

	DBG_TRACE("Create Task \"%s\"\n", ptask->task_name);
	return 1;
}

static void _freertos_delete_task(struct task_struct *ptask)
{
	if (!ptask->task){
		DBG_INFO("_freertos_delete_task(): ptask is NULL!\n");
		return;
	}
	struct nthread_wrapper *wrap = ptask->task;

	free(wrap);
	ptask->task = 0;

	DBG_TRACE("Delete Task \"%s\"\n", ptask->task_name);
}
static void _freertos_wakeup_task(struct task_struct *ptask)
{
	sem_post(*(&ptask->wakeup_sema));
}

static void _freertos_thread_enter(char *name)
{
	DBG_TRACE("RTKTHREAD %s\n", name);
}

static void _freertos_thread_exit(void)
{
	DBG_TRACE("RTKTHREAD exit %s\n", __FUNCTION__);
}

static inline void _freertos_worker_init_once(void)
{
	if (g_worker != NULL)
		return;

	g_worker = work_queue_create("WIFIWORKER", SCHED_PRIORITY_DEFAULT+10, g_rtos_service_stack, CONFIG_SCHED_LPWORKSTACKSIZE, 1);
}

_timerHandle _freertos_timerCreate( const signed char *pcTimerName,
							  osdepTickType xTimerPeriodInTicks,
							  u32 uxAutoReload,
							  void * pvTimerID,
							  TIMER_FUN pxCallbackFunction )
{
	struct ntimer_wrapper *wrap;
	wrap = calloc(1, sizeof(*wrap));
	if (!wrap)
    {
		return NULL;
    }
	wrap->callback = pxCallbackFunction;
	_freertos_worker_init_once();
	return wrap;
}

u32  _freertos_timerStop( _timerHandle xTimer,
							   osdepTickType xBlockTime )
{
	struct ntimer_wrapper *wrap = xTimer;
	if (!work_available(&wrap->work))
    {
		work_cancel_wq(g_worker, &wrap->work);
    }
	return 1;
}

u32 _freertos_timerDelete( _timerHandle xTimer,
							   osdepTickType xBlockTime )
{
	struct ntimer_wrapper *wrap = xTimer;
	_freertos_timerStop(xTimer, xBlockTime);
	free(wrap);
	return 1;
}

u32 _freertos_timerIsTimerActive( _timerHandle xTimer )
{
	struct ntimer_wrapper *wrap = xTimer;
	return !work_available(&wrap->work);
}


u32  _freertos_timerChangePeriod( _timerHandle xTimer,
							   osdepTickType xNewPeriod,
							   osdepTickType xBlockTime )
{
	struct ntimer_wrapper *wrap = xTimer;
	/*if(xNewPeriod == 0)
		xNewPeriod += 1;	*/
	if (work_available(&wrap->work))
    {
      work_queue_wq(g_worker, &wrap->work, wrap->callback, wrap, xNewPeriod);
    }

	return 1;
}
void *_freertos_timerGetID( _timerHandle xTimer ){

	return xTimer;
}

u32  _freertos_timerStart( _timerHandle xTimer,
							   osdepTickType xBlockTime )
{
	return _freertos_timerChangePeriod(xTimer, 0, xBlockTime);
}

u32  _freertos_timerStartFromISR( _timerHandle xTimer,
							   osdepBASE_TYPE *pxHigherPriorityTaskWoken )
{
	return _freertos_timerStart(xTimer, 0);
}

u32  _freertos_timerStopFromISR( _timerHandle xTimer,
							   osdepBASE_TYPE *pxHigherPriorityTaskWoken )
{
	return _freertos_timerStop(xTimer, 0);
}

u32  _freertos_timerResetFromISR( _timerHandle xTimer,
							   osdepBASE_TYPE *pxHigherPriorityTaskWoken )
{
	return _freertos_timerStart(xTimer, 0);
}

u32  _freertos_timerChangePeriodFromISR( _timerHandle xTimer,
							   osdepTickType xNewPeriod,
							   osdepBASE_TYPE *pxHigherPriorityTaskWoken )
{
	return _freertos_timerChangePeriod(xTimer, xNewPeriod, 0);
}

u32  _freertos_timerReset( _timerHandle xTimer,
							   osdepTickType xBlockTime )
{
	return _freertos_timerStart(xTimer, 0);
}

void _freertos_acquire_wakelock(void)
{

}

void _freertos_release_wakelock(void)
{

}

void _freertos_wakelock_timeout(uint32_t timeout)
{

}

u8 _freertos_get_scheduler_state(void)
{
	return 1;
}


const struct osdep_service_ops osdep_service = {
	_freertos_malloc,			//rtw_vmalloc
	_freertos_zmalloc,			//rtw_zvmalloc
	_freertos_mfree,			//rtw_vmfree
	_freertos_malloc,			//rtw_malloc
	_freertos_zmalloc,			//rtw_zmalloc
	_freertos_mfree,			//rtw_mfree
	_freertos_memcpy,			//rtw_memcpy
	_freertos_memcmp,			//rtw_memcmp
	_freertos_memset,			//rtw_memset
	_freertos_init_sema,		//rtw_init_sema
	_freertos_free_sema,		//rtw_free_sema
	_freertos_up_sema,			//rtw_up_sema
	_freertos_up_sema_from_isr,	//rtw_up_sema_from_isr
	_freertos_down_sema,		//rtw_down_sema
	_freertos_mutex_init,		//rtw_mutex_init
	_freertos_mutex_free,		//rtw_mutex_free
	_freertos_mutex_get,		//rtw_mutex_get
	_freertos_mutex_get_timeout,//rtw_mutex_get_timeout
	_freertos_mutex_put,		//rtw_mutex_put
	_freertos_enter_critical,	//rtw_enter_critical
	_freertos_exit_critical,	//rtw_exit_critical
	_freertos_enter_critical_from_isr,	//rtw_enter_critical_from_isr
	_freertos_exit_critical_from_isr,	//rtw_exit_critical_from_isr
	NULL,		//rtw_enter_critical_bh
	NULL,		//rtw_exit_critical_bh
	_freertos_enter_critical_mutex,	//rtw_enter_critical_mutex
	_freertos_exit_critical_mutex,	//rtw_exit_critical_mutex
	_freertos_cpu_lock,
	_freertos_cpu_unlock,
	_freertos_spinlock_init,		//rtw_spinlock_init
	_freertos_spinlock_free,		//rtw_spinlock_free
	_freertos_spinlock,				//rtw_spin_lock
	_freertos_spinunlock,			//rtw_spin_unlock
	_freertos_spinlock_irqsave,		//rtw_spinlock_irqsave
	_freertos_spinunlock_irqsave,	//rtw_spinunlock_irqsave
	_freertos_init_xqueue,			//rtw_init_xqueue
	_freertos_push_to_xqueue,		//rtw_push_to_xqueue
	_freertos_pop_from_xqueue,		//rtw_pop_from_xqueue
	_freertos_deinit_xqueue,		//rtw_deinit_xqueue
	_freertos_get_current_time,		//rtw_get_current_time
	_freertos_systime_to_ms,		//rtw_systime_to_ms
	_freertos_systime_to_sec,		//rtw_systime_to_sec
	_freertos_ms_to_systime,		//rtw_ms_to_systime
	_freertos_sec_to_systime,		//rtw_sec_to_systime
	_freertos_msleep_os,	//rtw_msleep_os
	_freertos_usleep_os,	//rtw_usleep_os
	_freertos_mdelay_os,	//rtw_mdelay_os
	_freertos_udelay_os,	//rtw_udelay_os
	_freertos_yield_os,		//rtw_yield_os

	_freertos_ATOMIC_SET,	//ATOMIC_SET
	_freertos_ATOMIC_READ,	//ATOMIC_READ
	_freertos_ATOMIC_ADD,	//ATOMIC_ADD
	_freertos_ATOMIC_SUB,	//ATOMIC_SUB
	_freertos_ATOMIC_INC,	//ATOMIC_INC
	_freertos_ATOMIC_DEC,	//ATOMIC_DEC
	_freertos_ATOMIC_ADD_RETURN,	//ATOMIC_ADD_RETURN
	_freertos_ATOMIC_SUB_RETURN,	//ATOMIC_SUB_RETURN
	_freertos_ATOMIC_INC_RETURN,	//ATOMIC_INC_RETURN
	_freertos_ATOMIC_DEC_RETURN,	//ATOMIC_DEC_RETURN

	_freertos_modular64,			//rtw_modular64
	_freertos_get_random_bytes,		//rtw_get_random_bytes
	_freertos_GetFreeHeapSize,		//rtw_getFreeHeapSize

	_freertos_create_task,			//rtw_create_task
	_freertos_delete_task,			//rtw_delete_task
	_freertos_wakeup_task,			//rtw_wakeup_task

	_freertos_thread_enter,			//rtw_thread_enter
	_freertos_thread_exit,			//rtw_thread_exit

	_freertos_timerCreate,			//rtw_timerCreate,
	_freertos_timerDelete,			//rtw_timerDelete,
	_freertos_timerIsTimerActive,	//rtw_timerIsTimerActive,
	_freertos_timerStop,			//rtw_timerStop,
	_freertos_timerChangePeriod,	//rtw_timerChangePeriod
	_freertos_timerGetID,			//rtw_timerGetID
	_freertos_timerStart,			//rtw_timerStart
	_freertos_timerStartFromISR,	//rtw_timerStartFromISR
	_freertos_timerStopFromISR,		//rtw_timerStopFromISR
	_freertos_timerResetFromISR,	//rtw_timerResetFromISR
	_freertos_timerChangePeriodFromISR,	//rtw_timerChangePeriodFromISR
	_freertos_timerReset,			//rtw_timerReset

	_freertos_acquire_wakelock,		//rtw_acquire_wakelock
	_freertos_release_wakelock,		//rtw_release_wakelock
	_freertos_wakelock_timeout,		//rtw_wakelock_timeout
	_freertos_get_scheduler_state	//rtw_get_scheduler_state
};
