#include "ntdll.h"
#include "ntsyscalls.h"

/* The system-call stubs.
 *
 * Four instructions each, identical for every call, which is why they are
 * generated from a macro rather than written out seven times:
 *
 *     mov r10, rcx      the first argument; SYSCALL destroys RCX, because
 *                       the CPU puts the return address there
 *     mov eax, <number>
 *     syscall
 *     ret
 *
 * Everything else is already where it needs to be. The Win64 ABI put
 * arguments two to four in RDX, R8 and R9 and the rest on the stack from
 * [rsp+0x28], and the kernel reads them from exactly there - see
 * kernel/include/nt.h. That is the entire reason the convention was changed
 * to match Win64 before this file existed: under any other arrangement each
 * stub would be a dozen instructions of register shuffling with an
 * RSP-relative read whose offset moves as it pushes.
 *
 * Written as top-level asm rather than with __attribute__((naked)), which GCC
 * does not support on x86-64. A C function body would prologue and epilogue
 * around the syscall and destroy the frame the kernel is reading arguments
 * out of.
 *
 * The numbers come from ntsyscalls.h, generated from the kernel header. There
 * is no second list to keep in step. */

#define NT_STUB(name, number)                    \
    __asm__(".globl " #name "\n"                 \
            ".def " #name "; .scl 2; .type 32; .endef\n" \
            #name ":\n"                          \
            "    movq %rcx, %r10\n"              \
            "    movl $" #number ", %eax\n"      \
            "    syscall\n"                      \
            "    ret\n")

/* The number has to be pasted as a literal, so it goes through one extra
 * expansion - otherwise the macro parameter arrives as the token
 * NT_SYS_CLOSE rather than as 0x04. */
#define NT_STUB_X(name, number) NT_STUB(name, number)

NT_STUB_X(NtDisplayString,         NT_SYS_DISPLAY_STRING);
NT_STUB_X(NtTerminateProcess,      NT_SYS_TERMINATE_PROCESS);
NT_STUB_X(NtOpenFile,              NT_SYS_OPEN_FILE);
NT_STUB_X(NtClose,                 NT_SYS_CLOSE);
NT_STUB_X(NtReadFile,              NT_SYS_READ_FILE);
NT_STUB_X(NtWriteFile,             NT_SYS_WRITE_FILE);
NT_STUB_X(NtAllocateVirtualMemory, NT_SYS_ALLOCATE_VIRTUAL);

/* The dispatcher objects. Identical stubs to the seven above - which is the
 * point of generating them: adding a system call is one line here, one line
 * in ntdll.def and one number in the kernel header the numbers are read
 * from. */
NT_STUB_X(NtCreateEvent,           NT_SYS_CREATE_EVENT);
NT_STUB_X(NtOpenEvent,             NT_SYS_OPEN_EVENT);
NT_STUB_X(NtSetEvent,              NT_SYS_SET_EVENT);
NT_STUB_X(NtResetEvent,            NT_SYS_RESET_EVENT);
NT_STUB_X(NtWaitForSingleObject,   NT_SYS_WAIT_SINGLE);
NT_STUB_X(NtWaitForMultipleObjects, NT_SYS_WAIT_MULTIPLE);
NT_STUB_X(NtContinue,              NT_SYS_CONTINUE);
NT_STUB_X(NtQueueApcThread,        NT_SYS_QUEUE_APC);
NT_STUB_X(NtTestAlert,             NT_SYS_TEST_ALERT);
NT_STUB_X(NtRaiseException,        NT_SYS_RAISE_EXCEPTION);
NT_STUB_X(NtGenesisLoadImage,      NT_SYS_GENESIS_LOAD_IMAGE);
NT_STUB_X(NtCreateSemaphore,       NT_SYS_CREATE_SEMAPHORE);
NT_STUB_X(NtReleaseSemaphore,      NT_SYS_RELEASE_SEMAPHORE);
NT_STUB_X(NtCreateMutant,          NT_SYS_CREATE_MUTANT);
NT_STUB_X(NtReleaseMutant,         NT_SYS_RELEASE_MUTANT);

/* Threads - ROADMAP item 14(a). */
NT_STUB_X(NtCreateThreadEx,        NT_SYS_CREATE_THREAD);
NT_STUB_X(NtTerminateThread,       NT_SYS_TERMINATE_THREAD);
NT_STUB_X(NtQueryInformationThread, NT_SYS_QUERY_THREAD);
NT_STUB_X(NtSuspendThread,         NT_SYS_SUSPEND_THREAD);
NT_STUB_X(NtResumeThread,          NT_SYS_RESUME_THREAD);

/* The machine, processes and scheduling. */
NT_STUB_X(NtQuerySystemInformation,   NT_SYS_QUERY_SYSTEM_INFO);
NT_STUB_X(NtQuerySystemInformationEx, NT_SYS_QUERY_SYSTEM_INFO_EX);
NT_STUB_X(NtQueryInformationProcess,  NT_SYS_QUERY_PROCESS);
NT_STUB_X(NtSetInformationProcess,    NT_SYS_SET_PROCESS);
NT_STUB_X(NtSetInformationThread,     NT_SYS_SET_THREAD);
NT_STUB_X(NtYieldExecution,           NT_SYS_YIELD);
NT_STUB_X(NtDelayExecution,           NT_SYS_DELAY);
NT_STUB_X(NtGetCurrentProcessorNumber, NT_SYS_CURRENT_PROCESSOR);
NT_STUB_X(NtGetCurrentProcessorNumberEx, NT_SYS_CURRENT_PROCESSOR_EX);
NT_STUB_X(NtQueryPerformanceCounter,  NT_SYS_PERF_COUNTER);
NT_STUB_X(NtQuerySystemTime,          NT_SYS_SYSTEM_TIME);
NT_STUB_X(NtWaitForAlertByThreadId,   NT_SYS_WAIT_ALERT_BY_TID);
NT_STUB_X(NtAlertThreadByThreadId,    NT_SYS_ALERT_BY_TID);
