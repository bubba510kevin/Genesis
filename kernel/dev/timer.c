#include "bsd.h"
#include "io.h"
#include "serial.h"
#include "timer.h"
#include "typesk.h"

/* The PIT's input is 1193182Hz - the NTSC colour burst divided by three, for
 * reasons that stopped mattering in about 1984 and are now load-bearing
 * anyway. */
#define PIT_FREQUENCY 1193182u

#define PIT_CH0       0x40
#define PIT_COMMAND   0x43

static volatile uint64 ticks;
static uint32 tick_hz = TIMER_HZ;

void timer_init(uint32 hz) {
    uint32 divisor;

    if (hz == 0) {
        hz = TIMER_HZ;
    }
    divisor = PIT_FREQUENCY / hz;
    /* A divisor of 0 means 65536 to the hardware, so the useful range stops
     * at 65535; clamping rather than wrapping keeps a silly argument from
     * producing a wildly wrong tick rate. */
    if (divisor == 0) {
        divisor = 1;
    }
    if (divisor > 0xFFFF) {
        divisor = 0xFFFF;
    }
    tick_hz = PIT_FREQUENCY / divisor;

    /* 0x36: channel 0, access lo/hi, mode 3 (square wave), binary. */
    outb(PIT_COMMAND, 0x36);
    outb(PIT_CH0, (uint8)(divisor & 0xFF));
    outb(PIT_CH0, (uint8)((divisor >> 8) & 0xFF));
}

/* The clock itself, split out of timer_tick so the interrupt path can run
 * it before taking the big kernel lock: a tick that had to wait for another
 * CPU to leave the kernel would otherwise be late, and a second tick arriving
 * while the first waited would be lost - the PIT latches one. */
void timer_advance(void) {
    ticks++;
}

void timer_tick(void) {

    /* The callout wheel's only clock source. Deliberately after the
     * increment, so a handler that reads timer_ticks_now() sees the tick it
     * was scheduled for rather than the one before it.
     *
     * This runs in interrupt context and calls straight into every due
     * callout handler - see kernel/bsd/callout.c. It is a no-op until
     * net_callout_init() has run, so the ordering in flk.c is not load-
     * bearing for correctness, only for when callouts start working. */
    net_callout_tick();

    /* The serial console's fallback drain. See serial.h: on hardware whose
     * IRQ 4 routing this tree has never seen, an interrupt that never arrives
     * would mean a machine with no keyboard AND no serial - which is a
     * machine nobody can reach. One port read per tick when the FIFO is
     * empty, and serial_rx_report prints how many bytes came each way so a
     * dead interrupt is visible rather than silently carried by this. */
    serial_poll();
}

uint32 timer_hz(void) {
    return tick_hz;
}

uint64 timer_ns(void) {
    /* Scaled before dividing, and in 64 bits: ticks * 1000000000 overflows a
     * 32-bit intermediate after four seconds, and dividing first would throw
     * away everything below a whole second. */
    return (ticks * 1000000000ULL) / tick_hz;
}

static uint64 boot_epoch_ns;

void timer_set_boot_epoch(uint64 seconds) {
    /* Recorded as the wall time AT BOOT rather than as "now", so the answer
     * does not depend on how long after the read this was called. */
    boot_epoch_ns = seconds * 1000000000ULL - timer_ns();
}

uint64 timer_realtime_ns(void) {
    return boot_epoch_ns + timer_ns();
}

uint64 timer_ticks_now(void) {
    return ticks;
}

uint64 timer_ms(void) {
    return (ticks * 1000ULL) / tick_hz;
}

/* --- the TSC, calibrated ---------------------------------------------------
 *
 * The tick is 10ms; a performance counter has to resolve far less than
 * that. The TSC does, and runs regardless of interrupt state, but its rate
 * is not architecturally known - so it is measured once against the PIT,
 * over the same ten ticks the LAPIC timer is calibrated across (see
 * smp_start_scheduling). Until then timer_tsc_hz() is 0 and callers fall
 * back to the tick or to a deliberately high assumed rate. */
static uint64 tsc_hz;

static uint64 rdtsc_raw(void) {
    uint32 lo, hi;

    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64)hi << 32) | lo;
}

void timer_calibrate_tsc(uint32 pit_ticks) {
    uint64 t0, c0, c1;

    if (pit_ticks == 0) {
        return;
    }
    t0 = ticks;
    while (ticks == t0) {
        __asm__ volatile ("pause");
    }
    c0 = rdtsc_raw();
    t0 = ticks;
    while (ticks - t0 < pit_ticks) {
        __asm__ volatile ("pause");
    }
    c1 = rdtsc_raw();
    tsc_hz = ((c1 - c0) * (uint64)tick_hz) / pit_ticks;
}

uint64 timer_tsc_hz(void) {
    return tsc_hz;
}

uint64 timer_tsc(void) {
    return rdtsc_raw();
}
