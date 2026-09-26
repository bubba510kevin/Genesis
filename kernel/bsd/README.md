# kernel/bsd - vendored FreeBSD kernel subsystems

Every file here is either **vendored** (FreeBSD source, unmodified) or
**Genesis's** (the seam that lets the vendored source build). Nothing is in
between, and this file is the manifest that keeps it that way.

The rule: if a vendored file needs an edit to compile, the fix belongs in
`compat/`, not in the vendored file. An edited vendored file cannot be
re-fetched from a newer upstream without a merge, which is the whole thing
vendoring was supposed to avoid.

## Vendored - do not edit

| File | Source | How to re-check |
|---|---|---|
| `compat/sys/mbuf.h`     | `vendsrc/sys/sys/mbuf.h`     | `md5sum` against the source; must match exactly |
| `compat/sys/queue.h`    | `vendsrc/sys/sys/queue.h`    | `md5sum` against the source; must match exactly |
| `compat/sys/callout.h`  | `vendsrc/sys/sys/callout.h`  | `md5sum` against the source; must match exactly |
| `compat/sys/_callout.h` | `vendsrc/sys/sys/_callout.h` | `md5sum` against the source; must match exactly |
| `vendor/kern_mbuf.inc`   | `vendsrc/sys/kern/kern_mbuf.c`   | whole functions, verbatim; see the file's header for the list |
| `vendor/uipc_mbuf.inc`   | `vendsrc/sys/kern/uipc_mbuf.c`   | as above |
| `vendor/uipc_mbuf2.inc`  | `vendsrc/sys/kern/uipc_mbuf2.c`  | as above |

### The protocol layer

One Genesis `.c` per upstream `.c`, so the mapping is one-to-one and every
`.inc` can be `md5sum`ed against `vendsrc`. Split that way rather than merged
into a few translation units because it has to be: `nhop_ctl.c` and `nhgrp.c`
each define a static `djb_hash()`, and merging them is a redefinition error.
Keeping them separate is what lets the files stay unmodified.

| Genesis file | Upstream |
|---|---|
| `if.c` | `net/if.c` (whole, 5139 lines) plus `net/if_dead.c` |
| `ethersubr.c` | `net/if_ethersubr.c` |
| `if_llatbl.c` | `net/if_llatbl.c` |
| `radix.c` | `net/radix.c`, `kern/subr_unit.c` |
| `domain.c` | `kern/uipc_domain.c`, `net/toeplitz.c` |
| `ifmedia.c` | `net/if_media.c` |
| `route.c` and the twelve files beside it | the whole `net/route/` tree, `net/route.c` |
| `in_rmx.c`, `in_fib.c` | `netinet/in_rmx.c`, `netinet/in_fib.c` |
| `ip.c` | `netinet/ip_input.c`, `ip_output.c`, `ip_reass.c`, `ip_icmp.c`, `ip_id.c`, `ip_options.c` |
| `in.c` | `netinet/in.c`, `netinet/in_proto.c` |
| `in_pcb.c` | `netinet/in_pcb.c` |
| `if_ether.c` | `netinet/if_ether.c` |
| `in_mcast.c`, `igmp.c` | `netinet/in_mcast.c`, `netinet/igmp.c` |
| `udp_usrreq.c`, `raw_ip.c` | `netinet/udp_usrreq.c`, `netinet/raw_ip.c` |
| `uipc_socket.c`, `uipc_sockbuf.c` | `kern/uipc_socket.c`, `kern/uipc_sockbuf.c` |
| `subr_uio.c` | `kern/subr_uio.c` (the `uiomove` family) |
| `libkern.c` | `libkern/jenkins_hash.c`, `arc4random_uniform.c`, `inet_ntop.c`, `inet_ntoa.c`, `inet_pton.c`, `qsort.c`, `kern/subr_hash.c` |
| `kern_counter.c` | the rate-limiting half of `kern/subr_counter.c` |

`route_prelude.h` is the shared include set those files use. It is Genesis's,
and it exists so the split above costs one line per file rather than twenty.

The two headers are whole files and can be diffed byte for byte. The three
`.inc` files are function-for-function extracts - upstream's files pull in
the protocol-switch table, the domain table, the VM page daemon and the MAC
framework, so taking them whole would mean taking those too. Each `.inc`
header names exactly which functions were taken and what was left behind,
with the reason.

`.inc` rather than `.c` because they are `#include`d into one translation
unit (`mbuf.c`) - the constructors they define are `static`, which is
upstream's arrangement. `build.py`'s `sources()` skips any directory named
`vendor`, so they are never compiled on their own. `kernel/zfs/vendor/`
already works this way for the same reason.

## Genesis's - edit freely

| File | What it is |
|---|---|
| `mbuf.c` | The translation unit. Holds `panic`, `malloc`/`free`, the globals the vendored fragments read, the stubs for branches this tree cannot reach, and the init/selftest entry points. |
| `callout.c` | The timer wheel behind the vendored `callout(9)` API. `vendsrc/sys/kern/kern_timeout.c` is not vendored because it is built on per-CPU state, softclock kernel threads, `sleepqueue(9)` and `mtx(9)` - Parts 10 and 11 of this pass. Every signature and return value matches upstream so that file can replace this one later. |
| `uma_vendor.c` | The one translation unit for the REAL allocator - `vendor/uma_core.inc` is `#include`d into it, on the `zfs_vendor.c` / `mbuf.c` model. Holds the glue: a `vm_page` token table, `kva_alloc`/`pmap_*` over `vmalloc.c` and `paging.c`, and `genesis_uma_init` in place of upstream's SYSINIT. |
| `vendor/uma_core.inc` | **Vendored, byte-for-byte** `vendsrc/sys/vm/uma_core.c` - 6042 lines, the largest vendored file in this tree. `md5sum` it against vendsrc. |
| `compat/vm/uma.h`, `uma_int.h`, `uma_dbg.h` | **Vendored** from `vendsrc/sys/vm/`. `uma.h` carries ONE clearly-marked Genesis block at the top adding two includes it otherwise gets from the kernel's global include order; everything else is upstream. |
| `sysinit.c` | Walks the `set_sysinit_set` link set in subsystem order. Upstream's mechanism, which this tree had compiled out; see the file for why that was wrong and what it cost. |
| `kern_eventhandler.c` | The named publish/subscribe lists. Also previously compiled out, and `netinet/in.c` reaches `in_ifattach` only through one. |
| `kern_sysctl.c` | The MIB tree, the handlers and `sbuf`. `<sys/sysctl.h>` is vendored; `kern/kern_sysctl.c` is not, because two thirds of it is the `__sysctl(2)` syscall. |
| `kern_synch.c` | `tsleep`/`msleep`/`wakeup`. Idles rather than deschedules - there is no kernel-thread abstraction to deschedule. |
| `kern_smr.c` | Safe memory reclamation, because `in_pcb.c` looks sockets up without a lock. Conservative: waits for every reader, not just the ones that could matter. |
| `kern_time.c` | Everything `<sys/time.h>` declares, over `kernel/dev/timer.c`. |
| `kern_env.c` | The kernel environment (over `kernel/driver/hints.c`), `log()`, and `copyin`/`copyout`. |
| `net_absences.c` | Every FreeBSD subsystem the vendored stack calls into that is NOT here - credentials, kqueue, AIO, file descriptors, the routing socket, TCP - each with what is missing, what it costs, and what closing it would take. The list is the boundary of the port. |
| `netstack.c` | Configuration only: the address, the default route and `ifconfig up`, each issued through the same entry point the userland tool would use. |
| `compat/` | ~250 headers standing in for (or vendored from) FreeBSD's kernel environment. Each says at the top what it leaves out and why. The ones added for UMA: `smp.h`, `pcpu.h`, `proc.h`, `sched.h`, `mutex.h`, `rwlock.h`, `sx.h`, `smr.h`, `sysctl.h`, `taskqueue.h`, `sleepqueue.h`, `domainset.h`, `ktr.h`, `asan.h`, `msan.h`, `sbuf.h`, `stack.h`, `bitset.h`, `random.h`, `limits.h`, `kernel.h`, `eventhandler.h`, `vmmeter.h`, and the `vm/` set. |

### The three name collisions this port forced

Worth stating together, because each was a silent-wrong-file hazard rather
than a compile error waiting to happen:

- Genesis's lock header was `sys/mutex.h`. FreeBSD's `uma_core.c` needs a
  FreeBSD `<sys/mutex.h>`, and `-Ikernel/bsd/compat` is searched first - so
  the same include named two different files depending on the including
  file's directory. Genesis's is **`klock.h`** now.
- Genesis's lock FUNCTIONS were `mtx_lock` and friends. FreeBSD makes those
  MACROS, and a macro beats a declaration wherever both are visible. They are
  **`kmtx_*`/`krw_*`** now.
- Genesis's SMP header was `smp.h`, and `compat/sys/smp.h` includes it -
  which resolved to itself. It is **`ksmp.h`** now.

These are the same trap this file already documented for `sys/param.h` and
`sys/queue.h` across the two vendored trees, hit three more times.

## The include-path trap

`compat/` is **not** on the global include path. It is added only for sources
under `kernel/bsd/`, by `build.py`'s `EXTRA_INCLUDES`.

This matters because `kernel/zfs/compat/` also answers `#include <sys/param.h>`,
`<sys/types.h>` and `<sys/queue.h>` - with different files. That tree is the
standalone-loader (`stand/libsa`) flavour; this one is the kernel flavour, and
its `sys/param.h` has to define `MSIZE` and `MCLBYTES`, which the loader's does
not. A global include path would hand one tree the other's headers, and the
failure would not be a missing file - it would be a file that parses and is
wrong.

## Scope

mbuf(9) and callout(9). No `ifnet`, no protocols, no sockets, no driver,
nothing wired to hardware - those are ROADMAP items 6 and 12. This provides
the two subsystems item 6 says it needs; it does not consume them.

The one hardware tie is `timer.c` calling `net_callout_tick()` from the tick
handler, which is the clock the wheel cannot exist without.
