#include "backtrace.h"
#include "idt.h"
#include "interrupt.h"
#include "nt_context.h"
#include "irq.h"
#include "lapic.h"
#include "keyboard.h"
#include "kstack.h"
#include "object.h"
#include "paging.h"
#include "process.h"
#include "sched.h"
#include "signal.h"
#include "syscall.h"
#include "timer.h"
#include "io.h"
#include "pic.h"
#include "screen.h"
#include "typesk.h"
#include "bkl.h"
#include "ksmp.h"

/* Single dispatch point, replacing the split isr_handler.c / irq_handler.c of
 * the 32-bit tree. There is one stub table and one frame layout now, so a
 * single function on the C side is the natural shape. */

static const char *exception_name(uint64 vector) {
    switch (vector) {
        case 0:  return "Divide by Zero";
        case 1:  return "Debug";
        case 2:  return "NMI";
        case 3:  return "Breakpoint";
        case 4:  return "Overflow";
        case 5:  return "Bound Range Exceeded";
        case 6:  return "Invalid Opcode";
        case 7:  return "Device Not Available";
        case 8:  return "Double Fault";
        case 10: return "Invalid TSS";
        case 11: return "Segment Not Present";
        case 12: return "Stack-Segment Fault";
        case 13: return "General Protection Fault";
        case 14: return "Page Fault";
        case 16: return "x87 Floating-Point";
        case 17: return "Alignment Check";
        case 18: return "Machine Check";
        case 19: return "SIMD Floating-Point";
        default: return "Reserved/Unknown";
    }
}

/* Spell out the page-fault error code.
 *
 * A raw hex error code is a number you have to decode by hand, from a manual,
 * at the exact moment you are least inclined to - and the RSVD bit in
 * particular describes a cause (an entry with bits set above the physical
 * address width) that looks nothing like the permissions problem the other
 * bits suggest. Printing words costs ten lines and removes a step.
 *
 * Note that P=0 together with RSVD=1 should not occur: reserved bits are only
 * checked in an entry that is present. If you see that combination, suspect
 * the emulator's error-code composition before your own tables. */
static void print_pf_error(uint64 err) {
    print_string("\n  cause ", 0x0C);
    print_string((err & 0x1) ? "protection violation" : "page not present", 0x0C);
    print_string((err & 0x2) ? ", write"  : ", read", 0x0C);
    print_string((err & 0x4) ? ", user"   : ", supervisor", 0x0C);
    if (err & 0x8)  print_string(", RESERVED BIT SET IN AN ENTRY", 0x0C);
    if (err & 0x10) print_string(", instruction fetch", 0x0C);
    if (err & 0x20) print_string(", protection key", 0x0C);
    if (err & 0x40) print_string(", shadow stack", 0x0C);
}

static uint64 read_cr2(void) {
    uint64 value;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(value));
    return value;
}

/* Counted, not printed - see the dispatch branches at the bottom of
 * interrupt_dispatch for why. Reported once at boot by
 * interrupt_report(), which is where a non-zero value becomes visible. */
static uint64 spurious_count;
static uint64 unhandled_vector_count;

void interrupt_report(uint8 color) {
    print_string("interrupts: spurious ", color);
    print_hex64(spurious_count, color);
    print_string("  unhandled dynamic vectors ", color);
    print_hex64(unhandled_vector_count, color);
    print_string("\n", color);
}

/* The tick counter moved to timer.c, which owns the hardware. This wrapper
 * stays because callers already use it. */
extern void timer_tick(void);

uint64 timer_get_ticks(void) {
    return timer_ticks_now();
}

/* Ring 3 has CS with the low two bits set to the requested privilege level.
 * Anything else means the interrupt landed inside the kernel. */
static int frame_from_user(const struct interrupt_frame *frame) {
    return (frame->cs & 3) == 3;
}

/* A fault in ring 3 that could not be resolved kills the process, not the
 * machine.
 *
 * This used to halt unconditionally, which meant a null dereference in a user
 * program took the whole kernel down - the single most common way to lose a
 * debugging session to something that was never the kernel's fault. The
 * report is kept, because a segfault the user cannot see is worse than one
 * that prints; what changes is what happens afterwards.
 *
 * The process is killed here rather than sent a signal, because a signal is
 * delivered at a syscall boundary and a faulting process never reaches one -
 * it would re-execute the faulting instruction and fault again forever. A
 * process with a SIGSEGV handler is a thing to support once signal delivery
 * can rewrite an interrupt frame as well as a syscall frame; until then,
 * dying is honest and looping is not. */
static void kill_faulting_process(uint64 vector) {
    process_t *p = proc_current();

    if (p == NULL) {
        return;
    }

    print_string("\n[process ", 0x0E);
    print_hex((uint32)p->pid, 0x0E);
    print_string(" killed: ", 0x0E);
    print_string(exception_name(vector), 0x0E);
    print_string("]\n", 0x0E);

    /* One retirement path for every way a process dies without unwinding -
     * see proc_retire. This function used to open-code it, and drifted from
     * the copy in signal.c almost immediately. */
    proc_retire(p, 128 + SIGSEGV);
}

static void interrupt_dispatch_locked(struct interrupt_frame *frame) {
    /* --- the user RSP, for whichever door the kernel was entered by ------
     *
     * The per-CPU block's user_rsp slot is written by exactly one instruction
     * today: `movq %rsp, %gs:8` at the top of syscall_entry. An interrupt
     * writes nothing there, so a thread that entered by an IDT gate leaves
     * the slot holding whatever thread last made a SYSCALL - and schedule()
     * reads that slot to park the outgoing thread's stack pointer. It was
     * therefore saving one thread's user RSP into another thread's process
     * structure.
     *
     * That has been harmless, because a thread that entered by interrupt
     * leaves by iretq, which takes RSP from its own frame and never consults
     * the slot. It stops being harmless the moment anything wants the user
     * RSP of a thread interrupted in user mode - a stack trace across the
     * boundary, a signal delivered from the timer rather than at a syscall
     * return, or a thread that entered one way and left the other.
     *
     * The fix belongs here rather than in schedule(). schedule() would have
     * to ask how the thread entered, which means remembering it, which means
     * a second piece of state that can disagree with the first. Filling the
     * slot on the way in makes the question unnecessary: the invariant
     * becomes "gs:8 holds the current thread's user RSP whenever the kernel
     * is running on its behalf", true from either entry path, and every
     * reader is correct without knowing which one it was.
     *
     * Conditional on the frame's CS for the same reason isr_common's swapgs
     * is: an interrupt taken while the kernel was already running has no user
     * RSP in its frame, and the slot already holds the right value from the
     * syscall that got us here. Overwriting it with a kernel stack pointer is
     * how a syscall returns to ring 3 on the wrong stack. */
    if (frame->vector < 32) {
        int from_user = frame_from_user(frame);

        /* Copy-on-write gets first refusal on every page fault, from ring 0
         * as well as ring 3: with CR0.WP set, a syscall writing into a forked
         * process's buffer faults here exactly as user code would, and
         * resolving it is what lets read(2) into a shared page work at all. */
        if (frame->vector == 14 &&
            vmm_handle_write_fault(read_cr2(), frame->error_code)) {
            return_to_user(from_user);
            return;
        }

        if (from_user) {
            /* A Windows process handles its own faults (ROADMAP 14(c)):
             * the fault goes to ntdll's exception dispatcher, and only a
             * process that cannot be told - no dispatcher, no stack left -
             * dies here. An unhandled one comes back as NtRaiseException's
             * last chance and dies there, with the same kind of report. */
            if (nt_exception_deliver(frame,
                                     frame->vector == 14 ? read_cr2() : 0)) {
                return_to_user(1);
                return;
            }
            print_string("\nUser fault: ", 0x0C);
            print_string(exception_name(frame->vector), 0x0C);
            print_string("  RIP ", 0x0C);
            print_hex64(frame->rip, 0x0C);
            if (frame->vector == 14) {
                print_string("  CR2 ", 0x0C);
                print_hex64(read_cr2(), 0x0C);
                print_pf_error(frame->error_code);
            }
            kill_faulting_process(frame->vector);

            /* return_to_user refuses to resume a zombie, so this schedules
             * away and never comes back for this process. */
            return_to_user(1);
            return;
        }

        print_string("\nException: ", 0x4F);
        print_string(exception_name(frame->vector), 0x4F);
        print_string("\n  RIP ", 0x0C);
        print_hex64(frame->rip, 0x0C);
        print_string("  err ", 0x0C);
        print_hex64(frame->error_code, 0x0C);

        /* CR2 holds the faulting address, and it is the single most useful
         * number when debugging paging. Reading it costs nothing and saves
         * guessing which dereference did it. */
        if (frame->vector == 14) {
            print_string("\n  CR2 ", 0x0C);
            print_hex64(read_cr2(), 0x0C);
            print_pf_error(frame->error_code);

            /* The guard page paying for itself. Without this the report is
             * "page fault at 0xFFFFFFFFA0007FF8", which looks like a wild
             * pointer and sends you looking in the wrong place entirely. */
            if (kstack_is_guard(read_cr2())) {
                print_string("\n  KERNEL STACK OVERFLOW - a thread ran off "
                             "the bottom of its stack", 0x4F);
            }
        }
        print_string("\n", 0x0C);

        /* ROADMAP item 11: this used to stop at the bare RIP above. rbp is
         * the frame pointer live at the fault, captured in the interrupt
         * frame the same way rip is - see backtrace.h for why walking it
         * is safe to trust on this build. */
        backtrace_print(frame->rip, frame->rbp, 0x0C);

        __asm__ volatile ("cli");
        for (;;) {
            __asm__ volatile ("hlt");
        }
    }

    if (frame->vector < 48) {
        uint64 irq = frame->vector - 32;

        if (irq == 0) {
            /* ticks itself already advanced, before the lock - see
             * interrupt_dispatch. This is the rest of the tick. */
            timer_tick();
            /* Only flags a pending switch. Switching here would unwind an
             * interrupt frame from underneath the handler that is still
             * running on it; the actual switch happens on the way out to
             * user mode, where the stack is clean. */
            sched_tick();
        } else if (irq == 1) {
            kbd_irq();
        } else {
            /* Every other legacy line - PCI-attached drivers register here
             * via irq_register (see irq.h); a spuriously-firing unmasked
             * line with nothing registered is a no-op, not this file's
             * problem to diagnose. */
            irq_dispatch((uint8)irq);
        }

        /* Whichever controller actually delivered it - the 8259 pair, or the
         * Local APIC if the IOAPIC took over the legacy lines at boot. See
         * irq_eoi in irq.h for why this is one call and not a branch here. */
        irq_eoi((uint8)irq);
    } else if (frame->vector == IDT_SPURIOUS_VECTOR) {
        /* The LAPIC's spurious vector. Raised when an interrupt is withdrawn
         * between the CPU accepting it and reading the vector - a real,
         * architecturally expected event, not an error.
         *
         * It takes NO EOI, and that is the whole reason it needs its own
         * branch rather than falling into the one below. Acknowledging a
         * spurious interrupt acknowledges whatever real interrupt is
         * genuinely in service instead, which loses it. */
        spurious_count++;
    } else if (frame->vector > 47) {
        /* Dynamically allocated vectors - MSI today (see pci.h's
         * pci_msi_alloc), IPIs when Part 10 lands. These are delivered by
         * the Local APIC, so they take lapic_eoi and NOT pic_send_eoi;
         * sending the PIC's would acknowledge a legacy line that never
         * fired, and leave the LAPIC's in-service bit set so nothing at or
         * below that priority is ever delivered again.
         *
         * EOI after the handler, not before: the handler is what quiesces
         * the device, and acknowledging first opens a window where the
         * device can re-raise before it has been told to stop. */
        if (!idt_dispatch_vector((uint8)frame->vector)) {
            /* Nothing bound. Counted rather than printed - an unbound
             * vector firing repeatedly would otherwise fill the screen with
             * the same line and take the machine down by way of the
             * console. */
            unhandled_vector_count++;
        }
        lapic_eoi();
    }

    /* The preemption point for user code - taken by the caller,
     * interrupt_dispatch, which owns the big kernel lock's release and so
     * must be the one to decide when this thread leaves the kernel. */
    (void)0;
}

/* The interrupt entry, and where the big kernel lock is taken and dropped
 * for it (see bkl.h's table).
 *
 * THE LOCK-FREE PATH. The SMP IPIs and an AP's LAPIC timer run without it:
 * a shootdown or remote-call IPI is by definition answered while the CPU
 * that sent it holds the lock and waits, and the AP's tick is 100 interrupts
 * a second per CPU that almost never need anything but a counter. Each takes
 * the lock only if, coming from ring 3, it leaves something for the return
 * path to do - a reschedule, a thread killed from another CPU, a fatal
 * signal - and otherwise goes straight back to the user.
 *
 * The PIT tick advances `ticks` before the lock for the same reason: the
 * machine's clock must not lose a tick because another CPU was in the
 * kernel when it fired.
 *
 * EVERYTHING ELSE takes the lock unless this CPU already holds it - an
 * interrupt from ring 3 always, an interrupt that woke an idle CPU always,
 * an interrupt nested in kernel code on this CPU never (the lock is this
 * CPU's already). */
static int wants_kernel(struct cpu_local *c) {
    process_t *me = c->current;

    return c->resched || smp_deferred_pending() ||
           (me != NULL && !me->is_kthread &&
            (me->state == PROC_ZOMBIE || signal_pending(me) ||
             me->nt_suspend_count > 0));
}

/* A signal that arrives while its target is in ring 3 on another CPU used
 * to wait for the target's next system call. The one kind that cannot wait
 * - a signal whose action is to terminate - is acted on here, on the way
 * back to ring 3 from any interrupt. Handled signals still wait for the
 * next syscall boundary, which needs a syscall frame to rewrite. */
static void kill_on_fatal_signal(void) {
    process_t *me = proc_current();

    if (me != NULL && !me->is_kthread && me->state != PROC_ZOMBIE &&
        signal_pending(me)) {
        signal_kill_if_fatal(me);
    }
}

void interrupt_dispatch(struct interrupt_frame *frame) {
    struct cpu_local *c = smp_this_cpu();
    int from_user = frame_from_user(frame);
    int vec = (int)frame->vector;
    int took = 0;

    /* The user RSP, for whichever door the kernel was entered by: schedule()
     * parks the per-CPU slot into the outgoing thread, so an interrupt from
     * ring 3 must fill it just as SYSCALL does. Conditional on the frame's
     * CS: an interrupt taken inside the kernel has no user RSP in its frame. */
    if (from_user) {
        syscall_set_user_rsp(frame->rsp);
    }

    if (vec >= 48 && smp_is_ipi_vector(vec)) {
        c->irq_depth++;
        idt_dispatch_vector((uint8)vec);
        lapic_eoi();
        c->irq_depth--;
        if (!from_user || !wants_kernel(c)) {
            return;
        }
        bkl_acquire();
        c->user_entries++;
        kill_on_fatal_signal();
        return_to_user(1);
        bkl_exit_to_user();
        return;
    }

    if (vec == 32) {
        timer_advance();
    }

    if (from_user || c->bkl_depth == 0) {
        bkl_acquire();
        took = 1;
        if (from_user) {
            c->user_entries++;
        }
    }
    if (vec >= 32) {
        c->irq_depth++;
    }
    if (vec > 32 && vec != IDT_SPURIOUS_VECTOR) {
        c->dev_irqs++;
    }
    interrupt_dispatch_locked(frame);
    if (vec >= 32) {
        c->irq_depth--;
    }
    /* Deferred work (NT DPCs) queued by the handler, run now that the
     * handler is done - but only from the OUTERMOST level: an interrupt
     * nested in kernel code that holds the lock must not run work under the
     * interrupted code's feet. took, or from ring 3, is exactly "outermost". */
    if ((took || from_user) && c->irq_depth == 0) {
        smp_run_deferred();
    }

    /* The preemption point for user code.
     *
     * Without this, only a syscall could yield the CPU, so a compute-bound
     * loop that never enters the kernel would hold it forever. The EOI is
     * sent first: schedule() may not return for a long time, and leaving the
     * PIC masked for the whole of another process's quantum stops every
     * interrupt including the timer.
     *
     * Only when returning to ring 3. An interrupt taken while the kernel was
     * already running has that kernel's frames live below this one, and
     * switching away would abandon them. */
    if (from_user) {
        kill_on_fatal_signal();
        return_to_user(1);
        /* Possibly on another CPU now - schedule() may have moved this
         * thread - so the release goes through the CPU it is on. */
        bkl_exit_to_user();
    } else if (took) {
        bkl_release();
    }
}
