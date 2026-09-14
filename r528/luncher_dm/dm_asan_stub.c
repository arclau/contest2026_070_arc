/****************************************************************************
 * apps/vendor/allwinnertech/apps/luncher_dm/dm_asan_stub.c
 *
 * No-op AddressSanitizer stubs for the prebuilt libcedarc.a
 * (compiled with -fsanitize=address). The firmware has no ASAN runtime;
 * these no-op hooks effectively disable the instrumentation checks, which
 * is harmless here (the checks only report, they never alter data).
 *
 ****************************************************************************/

void __asan_handle_no_return(void)
{
}

void __asan_load1_noabort(void *addr)
{
    (void)addr;
}

void __asan_load2_noabort(void *addr)
{
    (void)addr;
}

void __asan_load4_noabort(void *addr)
{
    (void)addr;
}

void __asan_load8_noabort(void *addr)
{
    (void)addr;
}

void __asan_loadN_noabort(void *addr, unsigned long size)
{
    (void)addr;
    (void)size;
}

void __asan_store1_noabort(void *addr)
{
    (void)addr;
}

void __asan_store2_noabort(void *addr)
{
    (void)addr;
}

void __asan_store4_noabort(void *addr)
{
    (void)addr;
}

void __asan_store8_noabort(void *addr)
{
    (void)addr;
}

void __asan_storeN_noabort(void *addr, unsigned long size)
{
    (void)addr;
    (void)size;
}

/* Melis RTOS kernel API used by the prebuilt libcedarc.a.
 * NuttX has no enter_critical_section/leave_critical_section in this
 * config (critmon gated out), so provide lightweight no-op equivalents
 * that keep interrupts enabled — safe for the single-threaded music
 * playback path. */

typedef unsigned long irqstate_t;

irqstate_t enter_critical_section(void)
{
    return 0;
}

void leave_critical_section(irqstate_t flags)
{
    (void)flags;
}
