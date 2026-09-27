#include "dispatch.h"
#include "kprintf.h"
#include "ksmp.h"
#include "nt.h"
#include "nt_context.h"
#include "object.h"
#include "pmm.h"
#include "process.h"
#include "sched.h"
#include "syscall.h"
#include "teb.h"
#include "timer.h"
#include "typesk.h"
#include "waitq.h"

/* The NT calls about the machine, processes and scheduling - see the block
 * of NT_SYS_ numbers in nt.h for the list and what Win32 builds on each.
 *
 * Every structure written here is laid out as Windows lays it out on x64, at
 * the offsets a program compiled against the SDK reads, and each carries a
 * compile-time check of its size. Classes that are not implemented answer
 * STATUS_INVALID_INFO_CLASS rather than a zeroed buffer: a zero processor
 * count or a zero page size is a wrong answer a program will act on. */

/* --- user memory ------------------------------------------------------------ */

static int range_ok(uint64 p, uint64 len) {
    if (len == 0) {
        return 1;
    }
    return user_ptr_ok(p) && user_ptr_ok(p + len - 1) && p + len > p;
}

static void put_retlen(uint64 retlen_ptr, uint32 n) {
    if (retlen_ptr != 0 && range_ok(retlen_ptr, 4)) {
        *(uint32 *)retlen_ptr = n;
    }
}

static void zero(void *p, uint64 n) {
    uint8 *b = p;
    uint64 i;

    for (i = 0; i < n; i++) {
        b[i] = 0;
    }
}

static int online_count(void) {
    uint64 m = smp_online_mask();
    int n = 0;

    while (m) {
        n += (int)(m & 1);
        m >>= 1;
    }
    return n;
}

/* 100ns units per tick: the unit of every NT time interval. */
static uint64 ticks_to_100ns(uint64 t) {
    return (t * 10000000ULL) / timer_hz();
}

/* --- NtQuerySystemInformation ---------------------------------------------------- */

#define SystemBasicInformation                    0
#define SystemProcessorInformation                1
#define SystemTimeOfDayInformation                3
#define SystemProcessorPerformanceInformation     8
#define SystemLogicalProcessorInformation         73
#define SystemLogicalProcessorAndGroupInformation 107

typedef struct {
    uint32 Reserved;
    uint32 TimerResolution;              /* 100ns units */
    uint32 PageSize;
    uint32 NumberOfPhysicalPages;
    uint32 LowestPhysicalPageNumber;
    uint32 HighestPhysicalPageNumber;
    uint32 AllocationGranularity;
    uint32 Pad;
    uint64 MinimumUserModeAddress;
    uint64 MaximumUserModeAddress;
    uint64 ActiveProcessorsAffinityMask;
    int8   NumberOfProcessors;
    uint8  Pad2[7];
} nt_basic_info_t;
typedef char nt_basic_info_size[(sizeof(nt_basic_info_t) == 0x40) ? 1 : -1];

typedef struct {
    uint16 ProcessorArchitecture;        /* PROCESSOR_ARCHITECTURE_AMD64 = 9 */
    uint16 ProcessorLevel;
    uint16 ProcessorRevision;
    uint16 MaximumProcessors;
    uint32 ProcessorFeatureBits;
} nt_processor_info_t;
typedef char nt_processor_info_size[(sizeof(nt_processor_info_t) == 12) ? 1 : -1];

typedef struct {
    int64  IdleTime;
    int64  KernelTime;                   /* includes IdleTime, as on NT */
    int64  UserTime;
    int64  DpcTime;
    int64  InterruptTime;
    uint32 InterruptCount;
    uint32 Pad;
} nt_cpu_perf_t;
typedef char nt_cpu_perf_size[(sizeof(nt_cpu_perf_t) == 48) ? 1 : -1];

typedef struct {
    uint64 ProcessorMask;
    uint32 Relationship;
    uint32 Pad;
    union {
        uint8  CoreFlags;
        uint32 NodeNumber;
        uint64 Reserved[2];
    } u;
} nt_lpi_t;
typedef char nt_lpi_size[(sizeof(nt_lpi_t) == 32) ? 1 : -1];

#define RelationProcessorCore    0
#define RelationNumaNode         1
#define RelationCache            2
#define RelationProcessorPackage 3
#define RelationGroup            4
#define RelationAll              0xFFFF

/* The PE loader keeps the user half below this. */
#define NT_USER_MIN 0x10000ULL
#define NT_USER_MAX 0x00007FFFFFFEFFFFULL

static uint64 query_system_basic(uint64 buf, uint64 len, uint64 retlen) {
    nt_basic_info_t b;

    if (len < sizeof(b)) {
        put_retlen(retlen, sizeof(b));
        return STATUS_INFO_LENGTH_MISMATCH;
    }
    if (!range_ok(buf, sizeof(b))) {
        return STATUS_ACCESS_VIOLATION;
    }
    zero(&b, sizeof(b));
    b.TimerResolution         = (uint32)ticks_to_100ns(1);
    b.PageSize                = 4096;
    b.NumberOfPhysicalPages   = (uint32)pmm_total_frames();
    b.LowestPhysicalPageNumber  = 1;
    b.HighestPhysicalPageNumber = b.NumberOfPhysicalPages;
    b.AllocationGranularity   = 0x10000;
    b.MinimumUserModeAddress  = NT_USER_MIN;
    b.MaximumUserModeAddress  = NT_USER_MAX;
    b.ActiveProcessorsAffinityMask = smp_online_mask();
    b.NumberOfProcessors      = (int8)online_count();
    *(nt_basic_info_t *)buf = b;
    put_retlen(retlen, sizeof(b));
    return STATUS_SUCCESS;
}

static uint64 query_processor_info(uint64 buf, uint64 len, uint64 retlen) {
    nt_processor_info_t p;
    uint32 eax, ebx, ecx, edx;

    if (len < sizeof(p)) {
        put_retlen(retlen, sizeof(p));
        return STATUS_INFO_LENGTH_MISMATCH;
    }
    if (!range_ok(buf, sizeof(p))) {
        return STATUS_ACCESS_VIOLATION;
    }
    /* Family and model/stepping from CPUID leaf 1, in the form NT reports:
     * ProcessorLevel is the family, ProcessorRevision is model << 8 |
     * stepping. */
    __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                              : "a"(1u), "c"(0u));
    p.ProcessorArchitecture = 9;
    p.ProcessorLevel        = (uint16)(((eax >> 8) & 0xF) +
                              ((((eax >> 8) & 0xF) == 0xF) ? ((eax >> 20) & 0xFF) : 0));
    p.ProcessorRevision     = (uint16)((((eax >> 4) & 0xF) | ((eax >> 12) & 0xF0)) << 8 |
                                       (eax & 0xF));
    p.MaximumProcessors     = (uint16)SMP_MAX_CPUS;
    p.ProcessorFeatureBits  = edx;
    *(nt_processor_info_t *)buf = p;
    put_retlen(retlen, sizeof(p));
    return STATUS_SUCCESS;
}

static uint64 query_processor_performance(uint64 buf, uint64 len, uint64 retlen) {
    int n = smp_cpu_count(), i;
    uint64 need = (uint64)n * sizeof(nt_cpu_perf_t);

    if (len < sizeof(nt_cpu_perf_t)) {
        put_retlen(retlen, (uint32)need);
        return STATUS_INFO_LENGTH_MISMATCH;
    }
    if (len < need) {
        n = (int)(len / sizeof(nt_cpu_perf_t));
    }
    if (!range_ok(buf, (uint64)n * sizeof(nt_cpu_perf_t))) {
        return STATUS_ACCESS_VIOLATION;
    }
    for (i = 0; i < n; i++) {
        struct cpu_local *c = smp_cpu(i);
        nt_cpu_perf_t *e = &((nt_cpu_perf_t *)buf)[i];

        zero(e, sizeof(*e));
        /* Ticks this CPU took, split into idle and busy. The busy half is
         * reported as user time: the kernel does not separate time spent in
         * system calls from time in ring 3 per CPU. KernelTime includes the
         * idle time, which is NT's own (documented, surprising) convention -
         * a tool computing busy = Kernel + User - Idle gets the right
         * answer only if that holds. */
        e->IdleTime       = (int64)ticks_to_100ns(c->idle_ticks);
        e->KernelTime     = e->IdleTime;
        e->UserTime       = (int64)ticks_to_100ns(c->ticks - c->idle_ticks);
        e->InterruptCount = (uint32)(c->ticks + c->dev_irqs + c->ipis_received);
    }
    put_retlen(retlen, (uint32)((uint64)n * sizeof(nt_cpu_perf_t)));
    return STATUS_SUCCESS;
}

/* One entry per CPU (a core each - SMT siblings are not told apart), one
 * NUMA node, one package holding them all. */
static uint64 query_lpi(uint64 buf, uint64 len, uint64 retlen) {
    int n = smp_cpu_count(), i, k = 0;
    uint64 need = (uint64)(n + 2) * sizeof(nt_lpi_t);
    nt_lpi_t *out = (nt_lpi_t *)buf;

    if (len < need) {
        put_retlen(retlen, (uint32)need);
        return STATUS_INFO_LENGTH_MISMATCH;
    }
    if (!range_ok(buf, need)) {
        return STATUS_ACCESS_VIOLATION;
    }
    for (i = 0; i < n; i++) {
        if (!smp_cpu(i)->online) {
            continue;
        }
        zero(&out[k], sizeof(out[k]));
        out[k].ProcessorMask = 1ULL << i;
        out[k].Relationship  = RelationProcessorCore;
        k++;
    }
    zero(&out[k], sizeof(out[k]));
    out[k].ProcessorMask = smp_online_mask();
    out[k].Relationship  = RelationNumaNode;
    out[k].u.NodeNumber  = 0;
    k++;
    zero(&out[k], sizeof(out[k]));
    out[k].ProcessorMask = smp_online_mask();
    out[k].Relationship  = RelationProcessorPackage;
    k++;
    put_retlen(retlen, (uint32)((uint64)k * sizeof(nt_lpi_t)));
    return STATUS_SUCCESS;
}

/* SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX records: a relationship and a
 * size, then a body whose shape depends on the relationship. */
typedef struct {
    uint64 Mask;
    uint16 Group;
    uint16 Reserved[3];
} nt_group_affinity_t;

static uint32 lpix_emit(uint8 *out, uint64 room, uint32 relationship,
                        uint64 mask) {
    uint32 size;
    uint8 *body;

    switch (relationship) {
    case RelationProcessorCore:
    case RelationProcessorPackage:
    case RelationNumaNode:
        size = 8 + 24 + (uint32)sizeof(nt_group_affinity_t);     /* 48 */
        break;
    case RelationGroup:
        size = 8 + 24 + 48;                                        /* 80 */
        break;
    default:
        return 0;
    }
    if (room < size) {
        return size;                     /* counted, not written */
    }
    zero(out, size);
    *(uint32 *)(out + 0) = relationship;
    *(uint32 *)(out + 4) = size;
    body = out + 8;
    if (relationship == RelationGroup) {
        uint64 m = smp_online_mask();
        uint8 active = 0;

        while (m) {
            active = (uint8)(active + (m & 1));
            m >>= 1;
        }
        *(uint16 *)(body + 0) = 1;                 /* MaximumGroupCount */
        *(uint16 *)(body + 2) = 1;                 /* ActiveGroupCount  */
        body[24 + 0] = (uint8)SMP_MAX_CPUS;        /* MaximumProcessorCount */
        body[24 + 1] = active;                     /* ActiveProcessorCount  */
        *(uint64 *)(body + 24 + 40) = smp_online_mask();
    } else {
        nt_group_affinity_t *g = (nt_group_affinity_t *)(body + 24);

        if (relationship == RelationNumaNode) {
            *(uint32 *)(body + 0) = 0;             /* NodeNumber */
        }
        *(uint16 *)(body + 22) = 1;                /* GroupCount */
        g->Mask  = mask;
        g->Group = 0;
    }
    return size;
}

static uint64 query_lpi_ex(uint32 wanted, uint64 buf, uint64 len, uint64 retlen) {
    uint8 *out = (uint8 *)buf;
    uint64 used = 0, need = 0;
    int n = smp_cpu_count(), i;
    uint32 sz;

#define EMIT(rel, mask)                                                   \
    do {                                                                  \
        if (wanted == RelationAll || wanted == (rel)) {                   \
            sz = lpix_emit(used < len ? out + used : out,                 \
                           used < len ? len - used : 0, (rel), (mask));   \
            need += sz;                                                   \
            if (need <= len) {                                            \
                used = need;                                              \
            } else {                                                      \
                used = len;                                               \
            }                                                             \
        }                                                                 \
    } while (0)

    if (len != 0 && !range_ok(buf, len)) {
        return STATUS_ACCESS_VIOLATION;
    }
    for (i = 0; i < n; i++) {
        if (smp_cpu(i)->online) {
            EMIT(RelationProcessorCore, 1ULL << i);
        }
    }
    EMIT(RelationNumaNode, smp_online_mask());
    EMIT(RelationProcessorPackage, smp_online_mask());
    EMIT(RelationGroup, smp_online_mask());
#undef EMIT
    put_retlen(retlen, (uint32)need);
    if (need > len) {
        return STATUS_INFO_LENGTH_MISMATCH;
    }
    return STATUS_SUCCESS;
}

static uint64 nt_query_system_information(uint64 cls, uint64 buf, uint64 len,
                                          uint64 retlen) {
    switch ((uint32)cls) {
    case SystemBasicInformation:
        return query_system_basic(buf, (uint32)len, retlen);
    case SystemProcessorInformation:
        return query_processor_info(buf, (uint32)len, retlen);
    case SystemProcessorPerformanceInformation:
        return query_processor_performance(buf, (uint32)len, retlen);
    case SystemLogicalProcessorInformation:
        return query_lpi(buf, (uint32)len, retlen);
    case SystemLogicalProcessorAndGroupInformation:
        return query_lpi_ex(RelationAll, buf, (uint32)len, retlen);
    default:
        return STATUS_INVALID_INFO_CLASS;
    }
}

static uint64 nt_query_system_information_ex(uint64 cls, uint64 in, uint64 inlen,
                                             uint64 buf, uint64 len,
                                             uint64 retlen) {
    uint32 wanted = RelationAll;

    if ((uint32)cls != SystemLogicalProcessorAndGroupInformation) {
        return nt_query_system_information(cls, buf, len, retlen);
    }
    if (in != 0 && (uint32)inlen >= 4) {
        if (!range_ok(in, 4)) {
            return STATUS_ACCESS_VIOLATION;
        }
        wanted = *(const uint32 *)in;
    }
    return query_lpi_ex(wanted, buf, (uint32)len, retlen);
}

/* --- threads and processes by handle --------------------------------------------- */

/* The thread a handle names, in the CALLING process only - NtCurrentThread,
 * or a Thread object handle whose thread belongs to this process. */
static process_t *thread_of_handle(uint64 handle) {
    process_t *me = proc_current();
    object_t *obj;
    int tid = 0;
    uint32 code = 0;
    process_t *t;

    if (handle == NT_CURRENT_THREAD) {
        return me;
    }
    obj = nt_object_of(handle);
    if (obj == NULL || thread_object_query(obj, &tid, &code) != 0) {
        return NULL;                         /* not a thread, or it exited */
    }
    t = proc_find(tid);
    if (t == NULL || t->tgid != me->tgid || t->state == PROC_ZOMBIE) {
        return NULL;
    }
    return t;
}

static int is_current_process(uint64 handle) {
    return handle == NT_CURRENT_PROCESS;
}

/* Per-thread NT scheduling attributes the kernel stores but does not use to
 * schedule (it has no priority levels): kept so a Set is honestly followed
 * by a Get that returns it. Keyed by slot, invalidated by pid. */
static int32 nt_prio[MAX_PROCESSES];
static int   nt_prio_pid[MAX_PROCESSES];
static uint8 nt_prio_class[MAX_PROCESSES];     /* by the group leader's slot */
static int   nt_class_pid[MAX_PROCESSES];

static int32 thread_priority(process_t *t) {
    int s = proc_index(t);

    return nt_prio_pid[s] == t->pid ? nt_prio[s] : 0;
}

/* --- NtQueryInformationProcess / NtSetInformationProcess ---------------------------- */

#define ProcessBasicInformation   0
#define ProcessTimes              4
#define ProcessPriorityClass      18
#define ProcessAffinityMask       21

typedef struct {
    uint32 ExitStatus;
    uint32 Pad0;
    uint64 PebBaseAddress;
    uint64 AffinityMask;
    int32  BasePriority;
    uint32 Pad1;
    uint64 UniqueProcessId;
    uint64 InheritedFromUniqueProcessId;
} nt_pbi_t;
typedef char nt_pbi_size[(sizeof(nt_pbi_t) == 48) ? 1 : -1];

typedef struct {
    int64 CreateTime;
    int64 ExitTime;
    int64 KernelTime;
    int64 UserTime;
} nt_times_t;

static uint64 process_affinity(process_t *leader) {
    uint64 m = 0;
    int i;

    /* The union over the process's live threads: what SetProcessAffinityMask
     * set, unless a thread has been given its own, narrower mask. */
    for (i = 0; i < MAX_PROCESSES; i++) {
        process_t *t = proc_at(i);

        if (t != NULL && !t->is_kthread && t->tgid == leader->tgid &&
            t->state != PROC_ZOMBIE) {
            m |= t->affinity;
        }
    }
    return m & smp_online_mask();
}

static uint64 nt_query_information_process(uint64 handle, uint64 cls, uint64 buf,
                                           uint64 len, uint64 retlen) {
    process_t *me = proc_current();
    process_t *leader = proc_find(me->tgid);

    if (!is_current_process(handle)) {
        return STATUS_INVALID_HANDLE;
    }
    if (leader == NULL) {
        leader = me;
    }
    switch ((uint32)cls) {
    case ProcessBasicInformation: {
        nt_pbi_t b;

        if ((uint32)len < sizeof(b)) {
            put_retlen(retlen, sizeof(b));
            return STATUS_INFO_LENGTH_MISMATCH;
        }
        if (!range_ok(buf, sizeof(b))) {
            return STATUS_ACCESS_VIOLATION;
        }
        zero(&b, sizeof(b));
        b.ExitStatus     = STATUS_PENDING;
        b.PebBaseAddress = NT_PEB_BASE;
        b.AffinityMask   = process_affinity(leader);
        b.BasePriority   = 8;
        b.UniqueProcessId = (uint64)me->tgid;
        b.InheritedFromUniqueProcessId = (uint64)leader->ppid;
        *(nt_pbi_t *)buf = b;
        put_retlen(retlen, sizeof(b));
        return STATUS_SUCCESS;
    }
    case ProcessTimes: {
        nt_times_t t;
        uint64 run = 0;
        int i;

        if ((uint32)len < sizeof(t)) {
            put_retlen(retlen, sizeof(t));
            return STATUS_INFO_LENGTH_MISMATCH;
        }
        if (!range_ok(buf, sizeof(t))) {
            return STATUS_ACCESS_VIOLATION;
        }
        for (i = 0; i < MAX_PROCESSES; i++) {
            process_t *p = proc_at(i);

            if (p != NULL && !p->is_kthread && p->tgid == me->tgid) {
                run += p->cpu_ticks;
            }
        }
        zero(&t, sizeof(t));
        t.UserTime = (int64)ticks_to_100ns(run);
        *(nt_times_t *)buf = t;
        put_retlen(retlen, sizeof(t));
        return STATUS_SUCCESS;
    }
    case ProcessPriorityClass: {
        int s = proc_index(leader);

        if ((uint32)len < 2) {
            put_retlen(retlen, 2);
            return STATUS_INFO_LENGTH_MISMATCH;
        }
        if (!range_ok(buf, 2)) {
            return STATUS_ACCESS_VIOLATION;
        }
        ((uint8 *)buf)[0] = 0;                                  /* Foreground */
        ((uint8 *)buf)[1] = nt_class_pid[s] == leader->pid ? nt_prio_class[s] : 2;
        put_retlen(retlen, 2);
        return STATUS_SUCCESS;
    }
    default:
        return STATUS_INVALID_INFO_CLASS;
    }
}

static uint64 nt_set_information_process(uint64 handle, uint64 cls, uint64 buf,
                                         uint64 len) {
    process_t *me = proc_current();
    process_t *leader = proc_find(me->tgid);

    if (!is_current_process(handle)) {
        return STATUS_INVALID_HANDLE;
    }
    if (leader == NULL) {
        leader = me;
    }
    switch ((uint32)cls) {
    case ProcessAffinityMask: {
        uint64 mask;
        int i;

        if ((uint32)len != sizeof(uint64)) {
            return STATUS_INFO_LENGTH_MISMATCH;
        }
        if (!range_ok(buf, 8)) {
            return STATUS_ACCESS_VIOLATION;
        }
        mask = *(const uint64 *)buf;
        /* NT refuses a mask that names a processor that does not exist,
         * rather than trimming it. */
        if (mask == 0 || (mask & ~smp_online_mask()) != 0) {
            return STATUS_INVALID_PARAMETER;
        }
        for (i = 0; i < MAX_PROCESSES; i++) {
            process_t *t = proc_at(i);

            if (t != NULL && !t->is_kthread && t->tgid == me->tgid &&
                t->state != PROC_ZOMBIE) {
                (void)sched_set_affinity(t, mask);
            }
        }
        sched_migrate_self();
        return STATUS_SUCCESS;
    }
    case ProcessPriorityClass: {
        int s = proc_index(leader);
        uint8 cls_value;

        if ((uint32)len < 2) {
            return STATUS_INFO_LENGTH_MISMATCH;
        }
        if (!range_ok(buf, 2)) {
            return STATUS_ACCESS_VIOLATION;
        }
        cls_value = ((const uint8 *)buf)[1];
        if (cls_value == 0 || cls_value > 6) {
            return STATUS_INVALID_PARAMETER;
        }
        nt_prio_class[s] = cls_value;
        nt_class_pid[s]  = leader->pid;
        return STATUS_SUCCESS;
    }
    default:
        return STATUS_INVALID_INFO_CLASS;
    }
}

/* --- NtSetInformationThread (and the extra NtQueryInformationThread classes) --------- */

#define ThreadTimes               1
#define ThreadPriority            2
#define ThreadBasePriority        3
#define ThreadAffinityMask        4
#define ThreadZeroTlsCell         10
#define ThreadIdealProcessor      13
#define ThreadHideFromDebugger    17
#define ThreadGroupInformation    30
#define ThreadIdealProcessorEx    33

static uint64 set_thread_affinity(process_t *t, uint64 mask) {
    if (mask == 0 || (mask & ~smp_online_mask()) != 0) {
        return STATUS_INVALID_PARAMETER;
    }
    if (sched_set_affinity(t, mask) != 0) {
        return STATUS_INVALID_PARAMETER;
    }
    if (t == proc_current()) {
        sched_migrate_self();
    }
    return STATUS_SUCCESS;
}

static uint64 nt_set_information_thread(uint64 handle, uint64 cls, uint64 buf,
                                        uint64 len) {
    process_t *t = thread_of_handle(handle);

    if (t == NULL) {
        return STATUS_INVALID_HANDLE;
    }
    switch ((uint32)cls) {
    case ThreadAffinityMask:
        if ((uint32)len != sizeof(uint64)) {
            return STATUS_INFO_LENGTH_MISMATCH;
        }
        if (!range_ok(buf, 8)) {
            return STATUS_ACCESS_VIOLATION;
        }
        return set_thread_affinity(t, *(const uint64 *)buf);

    case ThreadGroupInformation: {
        const nt_group_affinity_t *g;

        if ((uint32)len != sizeof(nt_group_affinity_t)) {
            return STATUS_INFO_LENGTH_MISMATCH;
        }
        if (!range_ok(buf, sizeof(*g))) {
            return STATUS_ACCESS_VIOLATION;
        }
        g = (const nt_group_affinity_t *)buf;
        if (g->Group != 0) {
            return STATUS_INVALID_PARAMETER;
        }
        return set_thread_affinity(t, g->Mask);
    }

    case ThreadIdealProcessor: {
        uint32 cpu, previous;

        if ((uint32)len != sizeof(uint32)) {
            return STATUS_INFO_LENGTH_MISMATCH;
        }
        if (!range_ok(buf, 4)) {
            return STATUS_ACCESS_VIOLATION;
        }
        cpu = *(const uint32 *)buf;
        previous = t->ideal_cpu >= 0 ? (uint32)t->ideal_cpu : 0;
        /* MAXIMUM_PROCESSORS asks for the current value without changing
         * it. The PREVIOUS ideal processor is the return value - a success
         * status that is a number, which is why SetThreadIdealProcessor
         * hands this straight back. */
        if (cpu == 64) {
            return previous;
        }
        if (cpu >= (uint32)smp_cpu_count() || !smp_cpu((int)cpu)->online) {
            return STATUS_INVALID_PARAMETER;
        }
        t->ideal_cpu = (int)cpu;
        return previous;
    }

    case ThreadIdealProcessorEx: {
        const uint8 *pn;

        if ((uint32)len != 4) {
            return STATUS_INFO_LENGTH_MISMATCH;
        }
        if (!range_ok(buf, 4)) {
            return STATUS_ACCESS_VIOLATION;
        }
        pn = (const uint8 *)buf;
        if (*(const uint16 *)pn != 0 || pn[2] >= smp_cpu_count() ||
            !smp_cpu(pn[2])->online) {
            return STATUS_INVALID_PARAMETER;
        }
        t->ideal_cpu = pn[2];
        return STATUS_SUCCESS;
    }

    case ThreadPriority:
    case ThreadBasePriority: {
        int32 v;
        int s = proc_index(t);

        if ((uint32)len != sizeof(int32)) {
            return STATUS_INFO_LENGTH_MISMATCH;
        }
        if (!range_ok(buf, 4)) {
            return STATUS_ACCESS_VIOLATION;
        }
        v = *(const int32 *)buf;
        /* ThreadBasePriority is SetThreadPriority's -15..+15 relative value
         * (±15 is "saturate": idle / time critical); ThreadPriority an
         * absolute 1..31. Out of range is refused, as NT refuses it. */
        if ((uint32)cls == ThreadBasePriority ? (v < -15 || v > 15)
                                              : (v < 1 || v > 31)) {
            return STATUS_INVALID_PARAMETER;
        }
        nt_prio[s]     = (uint32)cls == ThreadBasePriority ? v : v - 8;
        nt_prio_pid[s] = t->pid;
        return STATUS_SUCCESS;
    }

    case ThreadZeroTlsCell: {
        /* TlsFree's other half: the index goes back to the pool, and every
         * thread's copy of that slot is cleared so the next TlsAlloc hands
         * out a slot that reads NULL everywhere - Windows' guarantee. The
         * slots are in each thread's TEB (the 64) or in the expansion array
         * a TEB points at (the next 1024), all in this process's own space,
         * which is the one loaded. */
        uint32 index;
        int i;

        if ((uint32)len != sizeof(uint32)) {
            return STATUS_INFO_LENGTH_MISMATCH;
        }
        if (!range_ok(buf, 4)) {
            return STATUS_ACCESS_VIOLATION;
        }
        index = *(const uint32 *)buf;
        if (index >= NT_TLS_MINIMUM_SLOTS + NT_TLS_EXPANSION_SLOTS) {
            return STATUS_INVALID_PARAMETER;
        }
        for (i = 0; i < MAX_PROCESSES; i++) {
            process_t *o = proc_at(i);
            uint64 teb, cell;

            if (o == NULL || o->is_kthread || o->tgid != t->tgid ||
                o->state == PROC_ZOMBIE || o->thread.gs_base == 0) {
                continue;
            }
            teb = o->thread.gs_base;
            if (index < NT_TLS_MINIMUM_SLOTS) {
                cell = teb + NT_TEB_TLS_SLOTS + (uint64)index * 8;
            } else {
                uint64 exp;

                if (!range_ok(teb + NT_TEB_TLS_EXPANSION, 8)) {
                    continue;
                }
                exp = *(const uint64 *)(teb + NT_TEB_TLS_EXPANSION);
                if (exp == 0) {
                    continue;                 /* never used: already NULL */
                }
                cell = exp + (uint64)(index - NT_TLS_MINIMUM_SLOTS) * 8;
            }
            if (range_ok(cell, 8)) {
                *(uint64 *)cell = 0;
            }
        }
        return STATUS_SUCCESS;
    }

    case ThreadHideFromDebugger:
        /* There is no debugger to hide from: accepted, as a length-0 set. */
        return (uint32)len == 0 ? STATUS_SUCCESS : STATUS_INFO_LENGTH_MISMATCH;

    default:
        return STATUS_INVALID_INFO_CLASS;
    }
}

/* The NtQueryInformationThread classes beyond ThreadBasicInformation (which
 * nt.c answers). Returns STATUS_INVALID_INFO_CLASS for anything else. */
uint64 nt_query_thread_more(uint64 handle, uint64 cls, uint64 buf, uint64 len,
                            uint64 retlen) {
    process_t *t = thread_of_handle(handle);
    uint64 dead_ticks = 0;

    if (t == NULL) {
        /* A thread that has exited still answers ThreadTimes, from what its
         * object kept - as on Windows, where timing a worker after joining
         * it is the usual way to ask. Every other class needs it alive. */
        object_t *obj = nt_object_of(handle);
        int tid;
        uint32 code;

        if ((uint32)cls != ThreadTimes || obj == NULL ||
            thread_object_query(obj, &tid, &code) != 1 ||
            thread_object_cpu(obj, &dead_ticks) != 0) {
            return STATUS_INVALID_HANDLE;
        }
    }
    switch ((uint32)cls) {
    case ThreadTimes: {
        nt_times_t tt;

        if ((uint32)len < sizeof(tt)) {
            put_retlen(retlen, sizeof(tt));
            return STATUS_INFO_LENGTH_MISMATCH;
        }
        if (!range_ok(buf, sizeof(tt))) {
            return STATUS_ACCESS_VIOLATION;
        }
        zero(&tt, sizeof(tt));
        tt.UserTime = (int64)ticks_to_100ns(t != NULL ? t->cpu_ticks
                                                      : dead_ticks);
        *(nt_times_t *)buf = tt;
        put_retlen(retlen, sizeof(tt));
        return STATUS_SUCCESS;
    }
    case ThreadGroupInformation: {
        nt_group_affinity_t g;

        if ((uint32)len < sizeof(g)) {
            put_retlen(retlen, sizeof(g));
            return STATUS_INFO_LENGTH_MISMATCH;
        }
        if (!range_ok(buf, sizeof(g))) {
            return STATUS_ACCESS_VIOLATION;
        }
        zero(&g, sizeof(g));
        g.Mask = t->affinity & smp_online_mask();
        *(nt_group_affinity_t *)buf = g;
        put_retlen(retlen, sizeof(g));
        return STATUS_SUCCESS;
    }
    case ThreadIdealProcessorEx: {
        uint8 *pn = (uint8 *)buf;

        if ((uint32)len < 4) {
            put_retlen(retlen, 4);
            return STATUS_INFO_LENGTH_MISMATCH;
        }
        if (!range_ok(buf, 4)) {
            return STATUS_ACCESS_VIOLATION;
        }
        *(uint16 *)pn = 0;
        pn[2] = (uint8)(t->ideal_cpu >= 0 ? t->ideal_cpu : t->cpu);
        pn[3] = 0;
        put_retlen(retlen, 4);
        return STATUS_SUCCESS;
    }
    case ThreadPriority:
    case ThreadBasePriority: {
        if ((uint32)len < 4) {
            put_retlen(retlen, 4);
            return STATUS_INFO_LENGTH_MISMATCH;
        }
        if (!range_ok(buf, 4)) {
            return STATUS_ACCESS_VIOLATION;
        }
        *(int32 *)buf = (uint32)cls == ThreadBasePriority ? thread_priority(t)
                                                          : 8 + thread_priority(t);
        put_retlen(retlen, 4);
        return STATUS_SUCCESS;
    }
    default:
        return STATUS_INVALID_INFO_CLASS;
    }
}

/* For ThreadBasicInformation in nt.c: the real affinity and priority. */
uint64 nt_thread_affinity(uint64 handle) {
    process_t *t = thread_of_handle(handle);

    return t != NULL ? (t->affinity & smp_online_mask()) : smp_online_mask();
}

int32 nt_thread_priority(uint64 handle) {
    process_t *t = thread_of_handle(handle);

    return 8 + (t != NULL ? thread_priority(t) : 0);
}

/* --- yielding, sleeping, where am I, what time is it --------------------------------- */

static uint64 nt_yield_execution(void) {
    struct cpu_local *c = smp_this_cpu();
    uint64 before = c->switches;
    process_t *me = proc_current();

    schedule();
    /* Did a switch happen? The count moved on THIS CPU only if something
     * else ran - and if the thread was moved, it certainly did. */
    if (smp_this_cpu() != c || c->switches != before || me->cpu != (int)c->index) {
        return STATUS_SUCCESS;
    }
    return STATUS_NO_YIELD_PERFORMED;
}

/* The NT epoch is 1601-01-01; Unix's is 1970-01-01: 11644473600 seconds. */
#define NT_EPOCH_DELTA_100NS 116444736000000000ULL

static uint64 nt_now_100ns(void) {
    return timer_realtime_ns() / 100 + NT_EPOCH_DELTA_100NS;
}

static int apc_waiting(void *ctx);

static uint64 nt_delay_execution(uint64 alertable, uint64 interval_ptr) {
    process_t *me = proc_current();
    int64 interval;
    uint64 deadline, hz = timer_hz();

    alertable = (uint8)alertable;
    if (alertable && nt_apc_pending(me)) {
        return STATUS_USER_APC;             /* SleepEx(..., TRUE) with one queued */
    }
    if (!range_ok(interval_ptr, 8)) {
        return STATUS_ACCESS_VIOLATION;
    }
    interval = *(const int64 *)interval_ptr;
    if (interval == 0) {
        /* Sleep(0): give the rest of the slice to anything ready. */
        schedule();
        return STATUS_SUCCESS;
    }
    if (interval > 0) {
        /* Absolute, in NT time: the distance from now. */
        uint64 now = nt_now_100ns();

        if ((uint64)interval <= now) {
            return STATUS_SUCCESS;
        }
        interval = -(int64)((uint64)interval - now);
    }
    /* Relative, negative, 100ns units; rounded UP to whole ticks, plus one:
     * "now" is somewhere inside the current tick, so counting whole ticks
     * from it could end up to a tick early. A sleep is never shorter than
     * asked - Sleep(50) measured by QueryPerformanceCounter caught exactly
     * that. */
    deadline = timer_ticks_now() + 1 +
               (((uint64)(-interval)) * hz + 9999999ULL) / 10000000ULL;
    if (alertable) {
        /* On the readiness queue, which nt_apc_queue wakes: the sleep ends
         * at the deadline or at the first APC, whichever is first. */
        for (;;) {
            int r = waitq_wait_until(waitq_readiness(), apc_waiting, me,
                                     deadline);

            if (nt_apc_pending(me)) {
                return STATUS_USER_APC;
            }
            if (r == WAITQ_TIMEOUT || timer_ticks_now() >= deadline) {
                return STATUS_SUCCESS;
            }
            if (r == WAITQ_SIGNAL) {
                return STATUS_ALERTED;
            }
        }
    }
    while (timer_ticks_now() < deadline) {
        sched_sleep_until(me, deadline);
        if (me->state == PROC_BLOCKED) {
            me->state = PROC_RUNNING;
        }
    }
    return STATUS_SUCCESS;
}

static uint64 nt_current_processor_ex(uint64 pn_ptr) {
    uint32 n = (uint32)smp_cpu_index();

    if (pn_ptr != 0) {
        uint8 *pn;

        if (!range_ok(pn_ptr, 4)) {
            return n;
        }
        pn = (uint8 *)pn_ptr;
        *(uint16 *)pn = 0;
        pn[2] = (uint8)n;
        pn[3] = 0;
    }
    return n;
}

static uint64 nt_query_performance_counter(uint64 counter_ptr, uint64 freq_ptr) {
    uint64 hz = timer_tsc_hz();
    uint64 value, freq;

    if (counter_ptr == 0 || !range_ok(counter_ptr, 8)) {
        return STATUS_ACCESS_VIOLATION;
    }
    if (freq_ptr != 0 && !range_ok(freq_ptr, 8)) {
        return STATUS_ACCESS_VIOLATION;
    }
    if (hz != 0) {
        value = timer_tsc();
        freq  = hz;
    } else {
        value = timer_ticks_now();
        freq  = timer_hz();
    }
    *(uint64 *)counter_ptr = value;
    if (freq_ptr != 0) {
        *(uint64 *)freq_ptr = freq;
    }
    return STATUS_SUCCESS;
}

static uint64 nt_query_system_time(uint64 time_ptr) {
    if (!range_ok(time_ptr, 8)) {
        return STATUS_ACCESS_VIOLATION;
    }
    *(uint64 *)time_ptr = nt_now_100ns();
    return STATUS_SUCCESS;
}

/* --- NtWaitForAlertByThreadId / NtAlertThreadByThreadId ----------------------------
 *
 * One flag per thread, consumed by the wait. All sleepers share one wait
 * queue and each wakes to test only its own flag - a spurious wake costs a
 * re-test, and the flag is what guarantees an alert delivered before the
 * sleep begins is not lost. */
#include "waitq.h"

static wait_queue_t alert_q;
static int          alert_q_ready;

static int alerted(void *ctx) {
    return ((process_t *)ctx)->nt_alerted != 0;
}

static uint64 nt_wait_for_alert(uint64 address, uint64 timeout_ptr) {
    process_t *me = proc_current();
    uint64 deadline = 0;
    int r;

    (void)address;
    if (!alert_q_ready) {
        waitq_init(&alert_q);
        alert_q_ready = 1;
    }
    if (timeout_ptr != 0) {
        int64 t;

        if (!range_ok(timeout_ptr, 8)) {
            return STATUS_ACCESS_VIOLATION;
        }
        t = *(const int64 *)timeout_ptr;
        if (t > 0) {
            uint64 now = nt_now_100ns();

            t = (uint64)t > now ? -(int64)((uint64)t - now) : 0;
        }
        if (t == 0) {
            if (me->nt_alerted) {
                me->nt_alerted = 0;
                return STATUS_ALERTED;
            }
            return STATUS_TIMEOUT;
        }
        deadline = timer_ticks_now() + 1 +
                   (((uint64)(-t)) * timer_hz() + 9999999ULL) / 10000000ULL;
    }
    for (;;) {
        if (me->nt_alerted) {
            me->nt_alerted = 0;
            return STATUS_ALERTED;
        }
        r = deadline == 0 ? waitq_wait(&alert_q, alerted, me)
                          : waitq_wait_until(&alert_q, alerted, me, deadline);
        if (r == WAITQ_TIMEOUT && !me->nt_alerted) {
            return STATUS_TIMEOUT;
        }
        if (r == WAITQ_SIGNAL && !me->nt_alerted) {
            return STATUS_ALERTED;      /* interrupted: the caller re-tests */
        }
    }
}

static uint64 nt_alert_by_tid(uint64 tid) {
    process_t *me = proc_current();
    process_t *t = proc_find((int)tid);

    if (t == NULL || t->tgid != me->tgid || t->is_kthread ||
        t->state == PROC_ZOMBIE) {
        return STATUS_INVALID_CID;
    }
    t->nt_alerted = 1;
    if (alert_q_ready) {
        waitq_wake_all(&alert_q);
    }
    return STATUS_SUCCESS;
}

/* --- APCs ------------------------------------------------------------------------- */

/* NtQueueApcThread(HANDLE, PPS_APC_ROUTINE, PVOID, PVOID, PVOID) */
static uint64 nt_queue_apc(uint64 handle, uint64 routine, uint64 arg1,
                           uint64 arg2, uint64 arg3) {
    process_t *t = thread_of_handle(handle);

    if (t == NULL || t->is_kthread) {
        return STATUS_INVALID_HANDLE;
    }
    if (routine == 0 || !range_ok(routine, 1)) {
        return STATUS_INVALID_PARAMETER;
    }
    if (nt_apc_queue(t, routine, arg1, arg2, arg3) != 0) {
        return STATUS_NO_MEMORY;
    }
    return STATUS_SUCCESS;
}

static int apc_waiting(void *ctx) {
    return nt_apc_pending((const process_t *)ctx);
}

/* --- suspend and resume ------------------------------------------------------------ */

static uint64 nt_suspend_resume(uint64 handle, uint64 prev_ptr, int suspend) {
    process_t *t = thread_of_handle(handle);
    int prev;

    if (t == NULL || t->is_kthread) {
        return STATUS_INVALID_HANDLE;
    }
    if (prev_ptr != 0 && !range_ok(prev_ptr, 4)) {
        return STATUS_ACCESS_VIOLATION;
    }
    prev = t->nt_suspend_count;
    if (suspend) {
        if (prev >= NT_MAXIMUM_SUSPEND_COUNT) {
            return STATUS_SUSPEND_COUNT_EXCEEDED;
        }
        t->nt_suspend_count = prev + 1;
        if (t != proc_current() && t->state == PROC_READY && !t->oncpu) {
            /* Waiting for a CPU: taken off the run queue here and now. Not
             * left to return_to_user, because a thread that has never run
             * does not pass through it - its first switch lands straight in
             * syscall_return and ring 3. */
            sched_dequeue(t);
            t->state     = PROC_BLOCKED;
            t->nt_parked = 1;
        } else {
            /* Running in ring 3 on another CPU: make that CPU enter the
             * kernel, where return_to_user parks it. Blocked, or this thread
             * itself: it parks the next time it heads for ring 3. */
            sched_poke(t);
        }
    } else if (prev > 0) {
        t->nt_suspend_count = prev - 1;
        if (prev == 1 && t->nt_parked) {
            t->nt_parked = 0;
            sched_wake(t);
        }
    }
    if (prev_ptr != 0) {
        *(uint32 *)prev_ptr = (uint32)prev;
    }
    return STATUS_SUCCESS;
}

/* --- dispatch ----------------------------------------------------------------------- */

uint64 nt_sys_dispatch(struct syscall_frame *frame, int *handled) {
    uint64 a5 = 0, a6 = 0;

    *handled = 1;
    switch (frame->rax) {
    case NT_SYS_QUERY_SYSTEM_INFO:
        return nt_query_system_information(frame->r10, frame->rdx, frame->r8,
                                           frame->r9);
    case NT_SYS_QUERY_SYSTEM_INFO_EX:
        (void)nt_stack_arg(syscall_get_user_rsp(), 5, &a5);
        (void)nt_stack_arg(syscall_get_user_rsp(), 6, &a6);
        return nt_query_system_information_ex(frame->r10, frame->rdx, frame->r8,
                                              frame->r9, a5, a6);
    case NT_SYS_QUERY_PROCESS:
        (void)nt_stack_arg(syscall_get_user_rsp(), 5, &a5);
        return nt_query_information_process(frame->r10, frame->rdx, frame->r8,
                                            frame->r9, a5);
    case NT_SYS_SET_PROCESS:
        return nt_set_information_process(frame->r10, frame->rdx, frame->r8,
                                          frame->r9);
    case NT_SYS_SET_THREAD:
        return nt_set_information_thread(frame->r10, frame->rdx, frame->r8,
                                          frame->r9);
    case NT_SYS_YIELD:
        return nt_yield_execution();
    case NT_SYS_DELAY:
        return nt_delay_execution(frame->r10, frame->rdx);
    case NT_SYS_CURRENT_PROCESSOR:
        return (uint64)smp_cpu_index();
    case NT_SYS_CURRENT_PROCESSOR_EX:
        return nt_current_processor_ex(frame->r10);
    case NT_SYS_PERF_COUNTER:
        return nt_query_performance_counter(frame->r10, frame->rdx);
    case NT_SYS_SYSTEM_TIME:
        return nt_query_system_time(frame->r10);
    case NT_SYS_WAIT_ALERT_BY_TID:
        return nt_wait_for_alert(frame->r10, frame->rdx);
    case NT_SYS_ALERT_BY_TID:
        return nt_alert_by_tid(frame->r10);
    case NT_SYS_QUEUE_APC:
        (void)nt_stack_arg(syscall_get_user_rsp(), 5, &a5);
        return nt_queue_apc(frame->r10, frame->rdx, frame->r8, frame->r9, a5);
    case NT_SYS_SUSPEND_THREAD:
        return nt_suspend_resume(frame->r10, frame->rdx, 1);
    case NT_SYS_RESUME_THREAD:
        return nt_suspend_resume(frame->r10, frame->rdx, 0);
    default:
        *handled = 0;
        return 0;
    }
}
