#ifndef _MACHINE_CPU_H_
#define _MACHINE_CPU_H_

/* <machine/cpu.h> - the small machine-dependent surface FreeBSD's
 * <sys/buf_ring.h> and friends reach for. Adapted rather than vendored: the
 * real amd64 one is the whole MD interface (pcb, trapframe, cpu_switch, the
 * MSR accessors) and what these callers want is the pause hint and the
 * critical-section pair.
 */

/* The PAUSE instruction. On a spin loop it tells the CPU this is a spin -
 * which both saves power and, on hyperthreaded parts, stops the spinning
 * thread starving the one doing real work. A no-op here would be correct
 * and measurably worse. */
static __inline void cpu_spinwait(void) {
    __asm__ __volatile__("pause" ::: "memory");
}

#define cpu_ticks()     rdtsc_compat()
static __inline unsigned long long rdtsc_compat(void) {
    unsigned int lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned long long)hi << 32) | lo;
}

#endif
