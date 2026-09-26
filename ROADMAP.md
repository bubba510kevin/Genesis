Ordered TODO — WHAT IS LEFT

This file holds only what is unfinished. Items 1, 2, 3, 5, 8, 11 and the
completed halves of 4, 6, 7 and 13 were removed when it was rewritten;
ROADMAP-archive.md has the full history, and the bug records that mattered
live in src/verif.c, which is where this tree's own standing instruction says
they belong.

THE NUMBERS ARE NOT RENUMBERED, deliberately. Source comments and src/verif.c
refer to "ROADMAP item 11", "item 6", "item 7" by number in dozens of places;
renumbering would make every one of them point at the wrong thing. The gaps in
the sequence are the completed items.

THE ONE DEPENDENCY WORTH SEEING FIRST is DONE. It said: there is no KERNEL
THREAD, sched.c schedules only processes with user address spaces, and that
single absence blocks ZFS's write path (item 7), the driver half of the
dispatcher objects (item 14), a real taskqueue, and is why sleep(9) IDLES the
CPU on hlt rather than descheduling. Built first, as that entry argued for,
rather than worked around four separate times.

WHAT EXISTS NOW: kernel/include/kthread.h and kernel/proc/kthread.c. A kernel
thread is a process_t with is_kthread set, running in vmm_kernel_space() on
its own kstack slot, entered through a fabricated stack that lands in a C
trampoline instead of syscall_return. It is a process_t deliberately - the
table scan, the readiness states, sched_wake, the wait queues and the tick
accounting ULE scores on all already worked on one, and a second schedulable
type would have meant a second copy of every one of them.

Three things came with it that were not obviously part of it:

  RFLAGS IS NOW PART OF THE CONTEXT SWITCH. switch_context saves and restores
  it (pushfq/popfq). Without that the interrupt flag simply leaks across a
  switch, and the two directions fail differently and both fail quietly: a
  kernel thread entered with IF clear never receives a timer tick and the
  machine stops with no output, and a user syscall resumed with IF set runs
  with interrupts on in a kernel that has no locking around what it mutates.
  This was a latent bug before there was anything to expose it.

  SLEEP(9) REALLY DESCHEDULES, for a kernel thread. kern_synch.c asks
  ksleep_can_block() per call rather than answering once, because the same
  tsleep() in the same driver is reached from a kernel thread in one path and
  from an interrupt handler in another, and the handler has nothing to
  deschedule. The idling path is therefore not legacy and is not going away.
  The bridge is kernel/include/ksleep.h, which exists because <sys/proc.h>
  and kernel/include/process.h both define `struct thread` and cannot be in
  one translation unit.

  THE CHECK. kthread_selftest() at boot, per the standing instruction below.
  It asserts the private stack, the interrupt state on entry, the deschedule
  (the sleeping thread is PROC_BLOCKED while its creator is still running -
  the pair is what distinguishes a deschedule from the old hlt idle), a real
  wakeup returning 0 rather than a ten-second timeout, the exit reclaim, and
  the cap in both directions. Passes, including under -smp 2.

WHAT IS STILL OPEN HERE, and it is the reason this entry has not simply moved
to the archive:

  KERNEL THREADS ARE NOT PREEMPTIBLE. sched_tick sets the reschedule flag for
  one exactly as for a user process; return_to_user never acts on it, because
  a kernel thread is always running in the kernel. This is a decision rather
  than a gap - nothing here locks the heap, the handle tables or the mount
  table, and preempting kernel code is what turns that into corruption - but
  it means a kernel thread that computes in a loop hangs the machine with no
  output, and every caller has to be written knowing it.

  ONE RUNS FOR WORK NOW: the taskqueue's servicing thread, created at boot
  (kernel/bsd/kern_taskqueue.c). It is the first and only kernel thread with a
  job rather than a test behind it, and it holds one of KTHREAD_MAX's six
  slots for the life of the machine. The txg syncer is still the first ZFS
  one and does not exist.


CONDITION VARIABLES AND THE TASKQUEUE ARE DONE, which was item 7's second
blocker and the thing kernel threads existed to unblock. Both were the same
shape of problem: a header with no implementation, or an implementation that
could not defer anything because there was nowhere to defer it to.

  CONDVARS - kernel/bsd/kern_condvar.c. <sys/condvar.h> was vendored with
  nothing behind it at all. It is a mapping rather than a mechanism: a condvar
  IS a wait channel, so cv_wait is a sleep on the condvar's own address with
  the caller's lock dropped across it, and cv_signal is a wakeup on that
  address. The race a condvar normally needs machinery for - a signal landing
  between the predicate test and the sleep - cannot happen here, because
  gsleep reads the channel's generation counter BEFORE it drops the caller's
  lock, and a signaller must hold that same lock. That argument is written out
  in the file; it is the reason there is no sleepqueue.

  THE LOCK CLASS, which came with it. sleep(9) and condvars both have to drop
  and retake a lock they are handed as a bare `struct lock_object *`.
  kern_synch.c's _sleep() assumed a mutex, which was invisible only because
  nothing reached _sleep() directly - every vendored caller goes through
  mtx_sleep/sx_sleep/rm_sleep, each of which knows its own type. A condvar
  cannot dodge it: cv_wait is ONE entry point for all three. So lock_object
  carries lo_class now, set by the three init wrappers.

  THE TASKQUEUE - kernel/bsd/kern_taskqueue.c. taskqueue_enqueue was a macro
  that CALLED the task inline on the enqueuing CPU. It is a queue and a kernel
  thread now, so a task runs after its enqueuer returned and may block while
  it runs; enqueues coalesce the way upstream's do, and taskqueue_drain is a
  real wait rather than a function that returned immediately because there was
  never anything in flight. taskqueue_enqueue_timeout is real too - a callout
  fires and hands the task to the thread, which is what "a timer with a thread
  context attached" means - so UMA's periodic uma_timeout now actually runs,
  every twenty seconds, having never run before.

  THE FALLBACK IS NOT A STUB. With no thread - before genesis_taskqueue_init,
  or if no kernel thread could be created - taskqueue_enqueue still runs the
  task inline. Boot enqueues before the thread exists, and the choice there is
  between running the work at the call site and dropping it.

WHAT IS OPEN IN THIS PART:

  ONE QUEUE, ONE THREAD, THREE NAMES. taskqueue_thread, taskqueue_swi and
  taskqueue_fast are the same object. Upstream separates them so a task that
  blocks cannot delay one that must not. That is a latency cost rather than a
  correctness one and the fix is a second thread on a second queue, but it is
  a real difference and it is not made.

  taskqueue_drain FROM A NON-THREAD CONTEXT SPINS. A kernel thread waits on a
  condvar; anything else cannot deschedule through sleep(9) at all, so it
  yields in a loop instead - because cv_wait there would idle the CPU, and the
  thing it is waiting for is a kernel thread that needs that CPU. The general
  fix is for an ordinary process context to be able to deschedule, which is a
  question about detecting interrupt-handler context and not about taskqueues.
  See the Owed section.

  THE NETWORK STACK'S USE OF IT IS EXERCISED, and the evidence is an ordering
  rather than a test: do_link_state_change - which IS if_linktask's handler -
  used to print "re: link state changed to UP" during the driver's attach,
  before sti. It now prints sixty lines of boot later, after the ULE selftest,
  because it runs on the taskqueue thread. if_addmultitask still has not run;
  it needs a multicast join.

  Reaching it at all needed the DEVCLASS_MAX fix below.


THE BOOT WITH THE DATA DISK ATTACHED IS FIXED, which was blocking every one of
the above from being tested and is not a numbered item either. The machine
stopped during kld_load_directories at "bus: DEVCLASS_MAX exceeded", before
sti, so no selftest after it ran and neither systest nor verif could be
started. devclass_find HALTS on exhaustion by design - bus.h calls it a
build-time sizing bug rather than a runtime condition - and the pool was 8
where the boot needs 9.

What was in those eight is the part worth keeping: FOUR WERE THE BUS
SELFTEST'S OWN (st_pass, st_child, st_inherit, st_nowild), nothing ever frees
a devclass, and a test's devclass is as permanent as a driver's. Half the pool
was spent before any real driver asked for one. Now 32, with
bus_devclass_report printing the live count at boot so the headroom is a line
in the log instead of something learned by halting.

With that fixed: systest 234/234 and verif PART A 109/109, both run from the
shell for the first time in this sequence of changes.


4. Driver model — REMAINDERS ONLY

The three models and all four load directories are done. What is left:

NO PHY DRIVERS. Upstream ships around forty; Genesis has none, so link state is read straight out of the BMSR — clause 22, correct on any PHY — and autonegotiation is not driven. That is fine against QEMU's emulated PHY and is the first thing to break on real hardware needing a vendor-specific reset or a link-up workaround.

MODULE .TEXT IS NO LONGER W+X - the oldest open security remainder in the tree, and why it stayed open is worth keeping: it could not be fixed where the memory was. Page protections are per PAGE, and a kmalloc_a allocation shares its first and last page with whatever the allocator put next to it, so marking a module's text read-execute would have marked somebody else's bytes read-execute too.

So module images left the heap. They get their own 4MB VA window reserved from the kernel VA allocator, exactly as PE driver images already did (kernel/exec/pe.c). It was carved by a bump pointer, on the argument that a free list for memory nothing frees is a structure whose only correct value is its initial one - true right up until unload landed, and then exactly wrong: a bump pointer that cannot rewind turns every unload into a permanent window leak. It is a page bitmap now, sixteen uint64s over 1024 pages, first fit.

The sections are placed in TWO GROUPS with a page-aligned seam: executable first, everything else after. That grouping is what makes W^X expressible at all - text and data interleaved in address order means every page holding any text must be executable and every page holding any data must be writable, and the pages holding both must be both, which is the W+X being fixed.

THE ORDER IS THE PROPERTY. An image is mapped writable and NON-EXECUTABLE, relocated, and only then sealed to read-execute. There is no instant at which a page is both writable and executable; a W^X window that opens for one instruction is not one.

THE SEAL GOES BEFORE THE FIRST INSTRUCTION, and getting that wrong is how it was first written: the seal sat after the load completed, but pass 4a CALLS INTO THE MODULE - .genesis_modinit is a list of function pointers that run immediately - so the module executed while still NX. The symptom was exact: an instruction-fetch page fault at a present page, in the second module loaded.

IT IS FAIL-CLOSED, which is a stronger guarantee than the check that guards it. Because an image starts non-executable, a load that never seals faults on its first call rather than producing a writable-and-executable module. Measured by disabling the seal: the kernel dies inside kld_scan_entry. kld_wx_selftest therefore exists to catch the OPPOSITE error - a seal covering too much, leaving the data half unwritable - which is the silent one. It reads the protections back out of the page tables rather than trusting the loader, and prints "kld: W^X selftest passed (4 modules)" at boot.

MODULES UNLOAD NOW, which was the last thing this item was waiting on. kld_unload calls the module's exit functions (a .genesis_modexit section, the mirror of .genesis_modinit, so module_exit in real Linux driver source is finally wired to something), takes its drivers off the bus through bus.c's new bus_unregister_range, checks that nothing in five registries still points into the image, and unmaps it. The leak that entry named was the least of it - see the Owed section for what a pointer left behind actually costs and for the limit of what the five sweeps can promise. KLD_MAX_MODULES is 16 and now means "at once" rather than "ever", because a slot is reusable.

Still open here: eventhandler lists are not walked, so a module that registered one unloads cleanly and faults later. The sysctl tree WAS on that list and came off it later the same round - it is the fifth sweep now, and bus.c calls device_sysctl_fini on detach so it normally finds nothing rather than normally refusing. taskqueue_drain from a non-kernel-thread context still spins (see Owed), which is the same ksleep_can_block limitation, not a new one.

NO PHY DRIVERS is therefore the only thing left in this item that is a missing FEATURE rather than a sweep that does not reach far enough.


6. netinet — REMAINDERS ONLY

IPv4, ICMP, ARP, UDP, routing and the socket layer are vendored and verified
end to end at boot. What is left:

STILL OPEN: TCP. netinet/tcp_*.c is not vendored, and netinet/in_proto.c's protocol switch names a tcp_protosw whose pr_attach returns EPROTONOSUPPORT — so socket(AF_INET, SOCK_STREAM, 0) fails with the correct error rather than misbehaving. Every dependency TCP has below it now exists (in_pcb, the socket layer, the routing table, sleep, sysctl, callout), which is why this is a next step rather than a rewrite.

SOCKET(2) IS DONE, and the entry that said it was blocked was reading upstream's shape rather than this kernel's. It read: "bridging a socket to a file descriptor is VFS work... it needs Genesis's fileobj/vfs layer to grow a fileops vector."

FreeBSD needs a fileops vector because its descriptors point at struct file, which dispatches through a per-type table it has to be taught about sockets. Genesis's descriptors point at an object_t behind an object_type_t vtable - which IS a fileops vector, and has been since the object manager existed. Nothing had to grow: a socket becomes a descriptor by being an object type, the same way pipes, eventfds and socketpairs already are. That was not obvious until three other types had been written that way; recorded here because otherwise this reads as a thing that got easier by itself.

socket, bind, connect, sendto and recvfrom are in (kernel/bsd/kern_socketfd.c plus the descriptor half in syscall.c), and read(2)/write(2) work on a socket without appearing anywhere in that list - they go through the object vtable like any other descriptor, which is the property that made the whole thing small.

Verified from RING 3, not just from the kernel: systest creates a UDP socket, binds, connects to QEMU's resolver, sends a DNS query with both write(2) and sendto(2), and gets the reply back with the transaction id and the source address intact. There is no loopback interface, so that peer is the only UDP partner this machine has; the boot check already depends on it.

Two decisions worth keeping. SOCK_STREAM is refused BY THE PROTOCOL SWITCH - in_proto.c's tcp_protosw pr_attach returns EPROTONOSUPPORT - and NOT by a check in the syscall layer, so the day TCP is vendored it starts working with no edit. And sin_len is REWRITTEN from the caller's addrlen rather than trusted, because the vendored code reads it and a Linux-shaped sockaddr has no such field; the test passes sin_len = 0 deliberately to prove it.

STILL OPEN: TCP. netinet/tcp_*.c is not vendored. Every dependency it has below it now exists (in_pcb, the socket layer, the routing table, sleep, sysctl, callout, and now a descriptor to hang it on), which is why this is a next step rather than a rewrite. Also still open: listen/accept/getsockname/sendmsg/recvmsg, and there is no DHCP client, so the address is still compiled in — though it is now APPLIED through the real SIOCAIFADDR ioctl, exactly as ifconfig(8) would, rather than assigned to a variable.


7. zfs — reads done, permissions done both ways, allocation and commit done, the object layer above them is not

Read-only second volume: done. Feature gating on the superblock: done, at
eighteen read features. Refuse a dirty journal, then write, THEN THE VFS WORK
BELOW, then flip root: all open.

systest's filesystem section must run identically on both volumes - DONE, and it was reachable long before anyone ran it. The pool fixture (tests/host/fixtures/genesispool.dat.gz, built by OpenZFS's own libzpool) has been in the tree, the vendored reader mounts it unaided at /mnt/d, and systest's fs_checks() was already parameterised by base path. The only missing piece was ATTACHING THE DISK, which build.py now does by default.

That is the important part rather than the 19 checks: with no second volume, systest printed "skip - no second volume is mounted" and still reported "all passed". test_volumes' own comment calls that out as "the failure mode this tree has already had once" - and it was live, in this item's own check, for as long as the check existed. 333 passed, 0 failed, with the ZFS half really running: open, read, SEEK_END, read-at-EOF, fstat, stat, getdents64, -ENOTDIR, -ENOENT, all on ZFS rather than FAT.

NEW — PERMISSIONS, BOTH KINDS, AND THEY ARE ONE MECHANISM. Genesis now reads a ZFS file's real ACL, enforces it, and presents it as either POSIX mode bits or a Windows SECURITY_DESCRIPTOR. That is one stored form with two views, not two systems kept in step, and the reason it can be is that ZFS's native ACL is the NFSv4 ACL, NFSv4's model was taken from NT's, and the ACCESS MASK is the same field in both — ACE_READ_DATA and FILE_READ_DATA are both 0x1, ACE_WRITE_ACL and WRITE_DAC are both 0x40000. kernel/include/acl_abi.h writes each constant down once with its NT name in the comment, and #if-verifies every one against the vendored zfsimpl.h's copy, so a disagreement is a build failure rather than a wrong answer.

THE POSIX MODE IS A PROJECTION, never a second source of truth. acl_to_mode is OpenZFS's zfs_mode_compute reproduced: the first entry to mention a permission for a class settles that class, and an entry naming a specific user or group contributes NOTHING because a mode word has three classes and such an entry is about none of them. That loss is the point rather than a limitation — it is what makes it possible to tell an ACL-aware kernel from one that is reciting st_mode.

WHAT WAS ACTUALLY MISSING WAS THE WAY IN, not the ACL support. The vendored reader gets a v5 file's mode, size, uid and gid from CONSTANTS - SA_MODE_OFFSET 0, SA_SIZE_OFFSET 8, SA_UID_OFFSET 24, SA_GID_OFFSET 32 - which are right for the default ZPL layout and can never reach an ACL, because ZPL_DACL_ACES is VARIABLE LENGTH: its size is a property of the individual file and lives in the SA header's sa_lengths[] array. No number of extra constants fixes that. kernel/zfs/zfs_sa.inc is the real walk — header to layout number, layout number to the ordered attribute list in the LAYOUTS zap, then a running offset with each length coming from the registry if fixed and from sa_lengths[] if not. The registry is read BY NAME rather than iterated, which is what sa_setup does in the real ZFS and the only part of the arrangement that is stable: attribute numbers are assigned per filesystem, the names are compiled into OpenZFS.

THE FIXTURE IS THE HALF THAT MADE IT TESTABLE, and building it turned up the trap. tests/host/fixtures/genesisacl.dat.gz is a ZPL v5 pool made by real `zpool create`, with one non-trivial ACL written through libzpool's own sa.c by tests/host/zplsetacl.c. Non-trivial matters: chmod can only ever produce owner@/group@/everyone@, which is the mode word wearing a hat, and a reader that "passes" against one of those has proved only that it can rediscover st_mode the long way round. secret.txt is mode 0600 owned by uid 1000 and its ACL additionally grants uid 1001 read — something no mode word can express, so a POSIX-only reader and an ACL-aware one MUST disagree about uid 1001, and a test can tell which is running.

THE TRAP WAS ZFS_ACL_TRIVIAL. Bit 0x4 in the znode's pflags means "this file's ACL says exactly what its mode bits say", and zfs_zaccess uses it to skip loading the ACL at all. The first version of zplsetacl wrote a correct non-trivial ACL and left the flag set, and real ZFS then denied uid 1001 the access the ACL granted — it never looked. Nothing was corrupt and no read failed. Genesis honours the flag too, which is why four of the fixture's five objects cost no I/O at all.

AND LINUX WILL NOT ENFORCE THIS ACL, which is worth knowing before anyone tries to check the fixture against the host. module/os/linux/zfs/zfs_vfsops.c:acltype_changed_cb collapses ZFS_ACLTYPE_NFSV4 into ZFS_ACLTYPE_OFF, so Linux stores and preserves NFSv4 ACLs and then falls back to mode bits. The bytes are right — OpenZFS wrote them — and FreeBSD or illumos would honour them. Enforcing this ACL is something Genesis does that Linux-on-ZFS does not.

THREE BUGS FELL OUT OF WIRING IT UP, all of the same shape: dead code that was correct only because nothing read it. proc_alloc never initialised uid, gid, sid or umask, so a REUSED process slot kept the dead process's credentials. Neither fork path copied them, so a child of a process that had dropped privilege got whatever was in the slot. setuid and setgid sat in the accepted-and-ignored list while setresuid actually worked, so the same intent expressed two ways gave two different answers. None of it was reachable while every access check said yes; all of it is a privilege leak the moment one says no.

WHAT IS CHECKED. Host suite: the four ACEs of secret.txt decoded field by field (including the 16-byte named-user entry, which is the one that catches a reader assuming a fixed stride), the mode projecting back to exactly the 0100600 ZFS recorded, and 28 assertions on the SECURITY_DESCRIPTOR — the Samba idmap SIDs, the DACL in the SAME ORDER as the source (Windows calls denies-first "canonical" and reordering this ACL would take the file away from its owner), the mask crossing unchanged, and the ACE FLAG BYTE not crossing unchanged, because NFSv4's inherited bit is 0x80 and NT's is 0x10 while NT's failed-access bit is 0x80 — a byte copy turns an inherited ACE into an auditing one. systest, in the guest, through real syscalls, as real uids: 382 passed, 0 failed, including uid 1001 opening a mode-0600 file it does not own and uid 1002 being refused the same file.

BOTH ON-DISK ACL FORMATS ARE PARSED, and the older one needed a fixture built for it. A filesystem older than ZPL version 5 has no SA registry: its ACL is inside the znode's own 88 bytes of zp_acl, and TWO structures overlay those 88 bytes with z_acl_version deciding which — version 0 puts a uint32 COUNT at offset 8 and stores fixed 12-byte entries, version 1 puts a uint32 SIZE there, a uint16 count at offset 14, and stores the same variable-width entries a v5 pool does. Both are handled, and which applies is decided per OBJECT from the dnode's bonus type rather than per filesystem, because a pool upgraded to version 5 still has old objects in it until something rewrites them.

THE FIELD ORDER IS THE TRAP, and it is a quiet one. An old ACE is {uint32 who, uint32 mask, uint16 flags, uint16 type}; a modern one is {uint16 type, uint16 flags, uint32 mask} with the who afterwards if there is one. The who comes FIRST in one and last in the other, and the type moves from offset 0 to offset 10. Decode an old ACE with the modern layout and the low half of the uid is read as the ACE type — for uid 1001 that is 0x03E9, which is not a recognised type, so the entry is SKIPPED rather than misapplied. The result is an ACL that has quietly lost its most interesting entry and still looks well-formed. tests/host/mkzpl.c now writes exactly that entry into genesispool.dat so the mistake cannot pass, and the assertions name the offsets they are pinning.

TWO ENCODINGS, ONE ANSWER, which is the check worth having. genesispool.dat (v1, old format) and genesisacl.dat (v5, SA format) carry secret.txt with the SAME four entries deliberately: same mode, same owner, same grant to uid 1001. They agree about what the ACL says and disagree about how it is written down, so the decoded results are directly comparable and any difference is the decoder's fault rather than the fixture's. systest checks both volumes from userland, where only the result is visible: uid 1001 opens a mode-0600 file on each, uid 1002 is refused on each. z_acl_extern_obj — the escape hatch for an ACL too big for the six inline slots — is followed rather than refused, because a file with a seventh entry would otherwise read as having no ACL at all, and that is the failure that grants access rather than denying it.

STILL OPEN on this: setting an ACL (there is no write path to set it through), inheritance on create (same reason), and SACL/auditing, which NtQuerySecurityObject refuses outright rather than returning an empty one — an empty SACL means "audit nothing", which is a claim. An unknown z_acl_version is also refused rather than guessed, since the two known versions disagree about what the uint32 at offset 8 means and a third could disagree about anything.

NEW — SUPREME, THE PRIVILEGE acl_access's ROOT BYPASS ALREADY PREDICTED. That bypass carried a comment saying that the day this kernel grew privileges as a real concept, the line would become a privilege test instead of a uid test. It now is one: cred_is_supreme (kernel/include/acl.h, kernel/fs/acl.c) is root OR the one uid a root process has designated, and acl_access's bypass reads it instead of testing euid==0 directly. Root is unaffected — this is not a second code path, it is the same line — and the designated uid gets exactly what root already had: an explicit deny does not bind it either, which is closer to how NT AUTHORITY\SYSTEM and NT SERVICE\TrustedInstaller actually behave than to how a merely-Administrator account does, and is the whole point of fusing the three into one exemption rather than layering a Windows-shaped privilege check on top of a POSIX-shaped one.

EXACTLY ONE UID HOLDS IT, which is enforced by there being one kernel-global to hold (genesis_supreme_uid_get/set, kernel/proc/process.c) rather than a check on top of a list — "give one user the permission" asked for singular and this is singular by construction. Not stored on process_t: cred_t assembles it on demand in proc_cred the same way it already does uid/gid, so a granted-then-revoked process cannot carry a stale copy of the fact.

THE GRANT MECHANISM IS prctl, NOT A NEW SYSCALL NUMBER. This kernel's syscall numbers are real Linux amd64 ones throughout (kernel/include/syscall.h); inventing a new one for this risks colliding with a number Linux assigns later. prctl already existed in the dispatch table doing nothing (accepted-and-ignored, sharing a case with mprotect) — real Linux's prctl is exactly the designed-for-this extension point, an op code plus arguments, and it is real now. PR_GENESIS_GRANT_SUPREME and PR_GENESIS_REVOKE_SUPREME are root-only (a non-root process granting itself supreme would just be a syscall-shaped backdoor); PR_GENESIS_QUERY_SUPREME is open to anyone, the same posture getresuid already has. Every op this kernel does not recognise still falls through to the old unconditional 0, so no existing caller's prctl(2) of some other op sees a new answer. Every grant and revoke is kprintf-logged.

CHECKED AT BOOT (kernel/fs/acl_selftest.c, acl_selftest — called from flk.c after dispatch_selftest, with no ordering dependency on anything else): a credential with the flag clear stays denied by a 0000 ACL; the same credential with it set bypasses that ACL; root's own bypass is unchanged. STILL OPEN: the syscall gate itself — that a non-root caller's PR_GENESIS_GRANT_SUPREME is refused with -EPERM — is reachable from ring 3 and does not yet have a systest.c or host check exercising it, only direct code inspection.

NEW — GNFS: A NATIVE COW FILESYSTEM, KEEPING WHAT ZFS IS FOR AND NONE OF WHAT MADE IT EXPENSIVE HERE. Decided against continuing to chase OpenZFS write-path compatibility: the object layer that compatibility requires is what made the write-path cascade below (zfs_write.inc) cost ~7 blocks per allocation and never converge. kernel/gnfs/ is new code, BSD-2-Clause, with no CDDL boundary to keep and no aim of being read by real `zdb` — the vendored reader in kernel/zfs/ keeps that job, unchanged, for real external disks. See kernel/include/gnfs_layout.h for the full design argument; the shape in one line: a fixed ring of GNFS_RING_SLOTS root records (txg % count, exactly the uberblock ring's own crash-consistency rule, reused rather than reinvented) each naming one of two fixed free-space BITMAP regions, alternating by commit rather than a bitmap with unbounded history. No space-map log to replay, no multi-level meta-dnode, no compression/dedup/encryption.

THIS SESSION BUILT THE FOUNDATION: the on-disk layout (kernel/include/gnfs_layout.h), the pure logic over it — a 64-bit FNV-1a checksum, root record seal/validate, a first-fit bitmap allocator, and gnfs_format (kernel/gnfs/gnfs_format.c, no I/O, no allocation, callback-based so tools/mkgnfs.c and the kernel driver share one formatting implementation rather than two that could disagree) — and the device-facing half (kernel/gnfs/gnfs_vfs.c): a real fs_ops_t, a prober registered with volume.c through kernel/include/gnfs.h (one opaque call, the same shape as zfs_init, even though gnfs carries none of its CDDL concerns), and gnfs_txg_commit, the ring-advancing half of the crash-safety argument.

WHAT THE FOUNDATION MOUNTED, AT FIRST: an empty root directory and nothing else — no object table, fs_ops_t::write/create/mkdir all NULL. That is item 7's history now, not gnfs's present: the object layer landed the same session, below.

NEW — THE OBJECT LAYER: FILES AND DIRECTORIES, REAL COW THROUGHOUT. kernel/gnfs/gnfs_object.c adds a flat object table (kernel/include/gnfs_layout.h's own header comment on why flat rather than indirected is the point: an object number is a fixed-stride array index, never a chain of blocks pointing at blocks, which is exactly the depth the old OpenZFS-compatible cascade did not have and paid for) — onodes with up to GNFS_OBJ_DIRECT direct block pointers (48KB per file or directory in this foundation, refused past that with -EFBIG rather than silently truncated), and single-block directories (gnfs_dir_add/find/remove/iterate, bounded and refusing -ENOSPC when full, the same policy FAT's own fixed-size root directory already uses in this tree).

THE OBJECT TABLE IS COW BY THE SAME MECHANISM AS THE BITMAP: a whole-structure ping-pong between two fixed regions, committed together with the bitmap and a new root record in one gnfs_txg_commit call (extended this session to carry both). Every block a write touches — file data, or a directory's own dirent block — is a FRESH block; gnfs_cow_write_block never overwrites one already referenced by the mounted root, and the old one is freed only after the new one is durably in the in-memory table about to be committed. That is what makes a retained old root record (a future snapshot) a genuine, untouched copy of the graph as it stood, for free, rather than something a snapshot feature has to construct later.

fs_ops_t is real now: lookup (multi-component path resolution, kernel/gnfs/gnfs_vfs.c's gnfs_resolve), read and write (direct blocks, gaps read as zero — the exact lesson kernel/fs/fat.c's write path already learned, applied here from the start rather than re-learned), iterate, statfs, create, truncate, mkdir, rmdir (refuses -ENOTEMPTY, refuses removing the root), unlink. rename and a real setacl/getacl are still NULL — rename deliberately deferred, ACL work waiting on this layer settling first, exactly as planned.

CHECKED, end to end. tests/host/gnfs_test.c now also covers: onode init/bounds (objnum 0 and out-of-range both refused), monotonic onode allocation exhausting at GNFS_MAX_OBJECTS, directory add/find/remove/iterate including the EEXIST/ENOSPC/hole-reuse cases, and a full object-layer exercise through the real fs_ops_t — create, EEXIST on a duplicate, write, a write past the current end with the gap verified byte-for-byte zero, mkdir, a nested create two directories deep, rmdir refusing a non-empty directory then succeeding once it is empty, unlink, and — the check that actually matters, the same standard the ring already had to meet — tearing the mount down and remounting from the same bytes to confirm the unlink really reached the medium rather than only ever existing in memory. All passing, no existing selftest regressed, and real device I/O verified again in QEMU with a freshly formatted disk (`gnfs: volume on \Device\HarddiskVolume1, txg 1, ... objects in use`).

A REAL BUG IN SHARED TEST INFRASTRUCTURE WAS FOUND BUILDING THIS, worth recording because of its shape rather than its size: tests/host/dev_stub.c's dev_stub_attach never initialised dev_stub_t::file, relying on every existing caller declaring its dev_stub_t `static` (which zero-initialises) rather than as an ordinary local. gnfs_test.c was the first caller to use a stack-local one, and stub_write's `if (s->file != NULL)` then dereferenced whatever garbage the stack held as a FILE*. Non-deterministic in exactly the way that shape of bug always is: reproduced reliably in a plain build, vanished under AddressSanitizer (different stack layout, not a fix), and was only pinned down by bisecting with direct trace prints down to the exact statement. Fixed at the source — dev_stub_attach now sets the field explicitly — rather than worked around in gnfs_test.c, since every future caller with a non-static dev_stub_t would otherwise hit the same thing.

STILL OPEN ON THE OBJECT LAYER ITSELF, precisely: the dataset directory and snapshot retention (a snapshot is a retained old root record — the object layer's real COW already makes that a genuine untouched copy of the graph, for free — but allocation does not yet know to refuse a block a retained snapshot still references, since there is only one live bitmap; that is the one piece deferred, not hidden).

NEW — THE ACL WRITE PATH IS REAL, ON GNFS. fs_ops_t gains a `setacl` slot (kernel/include/fs.h) mirroring `getacl`, and kernel/fs/vfs.c's fs_setacl is the one place that both gates it (fs_access(..., ACE_WRITE_ACL) — the SAME check root's and the supreme privilege's bypass already cover, so both can always set an ACL, exactly like TrustedInstaller/SYSTEM) and calls it, the same split fs_getacl/fs_access already draws for reading. gnfs_onode_t gains `acl_block`: an object's ACL, when it has one, lives in its own COW block (kernel/gnfs/gnfs_vfs.c's gnfs_store_acl/gnfs_read_stored_acl/gnfs_effective_acl), with `acl_block == 0` meaning "none stored" and handled by fs_getacl's existing projection fallback — the identical convention ZFS_ACL_TRIVIAL already established one layer down.

TWO NEW PURE FUNCTIONS IN kernel/fs/acl.c, alongside acl_from_mode and acl_to_mode: acl_inherit builds a new object's initial ACL from its parent's — real NT/NFSv4 inheritance, ACE_FILE_INHERIT_ACE/ACE_DIRECTORY_INHERIT_ACE copied in with ACE_INHERIT_ONLY_ACE cleared and ACE_INHERITED_ACE set, and a child's own inherit bits cleared when it cannot have descendants (a file) or the source said ACE_NO_PROPAGATE_INHERIT_ACE — with a plain acl_from_mode as the floor underneath whatever is inherited, never replaced by it. acl_apply_chmod is chmod's own half: it rewrites exactly the owner@/group@/everyone@ ALLOW entries to a new mode and leaves every other entry — a named grant, a deny, anything inherited — untouched, falling back to a fresh projection if the ACL is not shaped the way this codebase's own acl_from_mode/acl_inherit always shape it.

WIRED IN: gnfs_op_create and gnfs_op_mkdir call acl_inherit against the parent's effective ACL and store the result only when it is non-trivial (nothing inherited stays unstored, projecting from the mode exactly as if this never ran). chmod(2) and fchmod(2) are genuinely new syscalls (SYS_chmod 90, SYS_fchmod 91, real Linux amd64 numbers — this kernel invents none of its own) built entirely on fs_getacl + acl_apply_chmod + fs_setacl.

CHOWN WAS DELIBERATELY NOT HERE AT FIRST. It needs ACE_WRITE_OWNER as its own gate, not a reuse of ACE_WRITE_ACL: an ordinary owner holds WRITE_ACL unconditionally (acl_from_mode grants it) and should still not thereby be able to give a file away to an arbitrary other uid, which is exactly the distinction POSIX draws between chmod and chown. Getting that gate right is its own small piece of work, not a rename of this one.

CHOWN, ADDED THE NEXT SESSION (2026-09-26), on exactly that separate gate. chown(2) and fchown(2) (SYS_chown 92, SYS_fchown 93) go through fs_setowner in kernel/fs/vfs.c, NOT fs_setacl, and fs_setowner's decision is a new pure function, acl_chown_permitted in kernel/fs/acl.c, whose rule is deliberately more than "holds ACE_WRITE_OWNER". A UID change requires WRITE_OWNER AND that the new uid be the caller's own: WRITE_OWNER lets you TAKE ownership, never GIVE it, which is NT's rule for that right without SeRestorePrivilege and the only reading under which granting it cannot be used to plant a file under someone else's name. A GID change requires the new group be one the caller is in, and either being the current owner (POSIX chgrp, no WRITE_OWNER needed) or holding WRITE_OWNER. Supreme (and so root) may set any pair; a request that changes nothing is permitted to anyone. The errno is -EPERM, POSIX's, not the -EACCES an access check answers, and a read-only volume answers -EROFS before the policy is consulted. The WRITE_OWNER test goes through acl_access itself, so a deny ahead of a grant binds it like any other bit and the supreme bypass needs no second copy.

THE TRAP WAS THE SECOND COPY OF THE OWNER. acl_t carries its own owner/group, and acl_access resolves owner@ and group@ against THOSE, not the onode's. A chown that updated only gnfs_onode_t::uid would leave any object with a stored ACL still granting owner@'s rights to the OLD owner - nothing corrupt, every stat correct, and the wrong person able to write. Hence fs_ops_t grew its own setowner slot rather than riding on setacl (an object with no stored ACL has nowhere to put an owner in one), and gnfs_op_setowner rewrites the stored ACL's pair FIRST, COW, and the onode's second, so a failed ACL write leaves nothing changed. CHECKED: gnfs_test.c's test_acl_chown_permitted pairs every refusal with the change that flips it (owner cannot give away / root and supreme can; chgrp into own group / not into another; WRITE_OWNER take-for-self / not give-to-third; a prior deny revokes it / supreme ignores the deny), and test_chown_end_to_end drives fs_setowner against a real gnfs volume, including a stored-ACL file whose new owner then really gets owner@'s access, and a remount showing both copies reached the medium. acl_selftest.c checks the give-away refusal and its supreme control at boot.

STILL OPEN AROUND CHOWN: POSIX clears S_ISUID/S_ISGID on a non-privileged chown; that is not done, and is harmless only because exec does not honour those bits at all yet - the day it does, this becomes a real hole and must land with it. And gnfs_op_create/gnfs_op_mkdir still create every object as uid 0 gid 0, because fs_ops_t::create takes no credential - so on gnfs today only root or supreme can usefully chmod anything, and chown is the only way a non-root user comes to own a file there. Passing the creator's credential into create/mkdir is the natural next step here. No ring-3 systest exercises chown yet either; the host and boot checks cover the policy, not the syscall plumbing.

CHECKED: tests/host/gnfs_test.c covers acl_inherit's three cases (a file child, a directory child, and NO_PROPAGATE even on a directory child) against a fabricated parent ACL, acl_apply_chmod rewriting a mode twice around a named grant that survives untouched both times (and its fallback when the expected shape is not there), and a full end-to-end exercise through the real fs_ops_t: a plain file with nothing inherited correctly has no stored ACL; setacl on a directory with an inheritable entry persists and reads back; a file AND a directory created inside it both inherit the entry, the file's copy loses its own propagation bits and the directory's keeps them; and — the check that distinguishes "the bytes are stored" from "the permission is real" — a named uid granted only by the inherited entry can do something the object's default mode denies everyone else, verified with acl_access directly, with an arbitrary uninvolved uid refused as the control. All passing, kernel build and boot unaffected.

NEW — THE WRITE PATH: ALLOCATION AND COMMIT WORK, THE OBJECT LAYER DOES NOT. Genesis can now allocate real space from a real pool, write a checksummed block into it, and commit a transaction group that real OpenZFS accepts. What it cannot yet do is make a FILE point at that block, and the gap between those two sentences is the honest status of item 7's second half.

FIRST, WHAT WAS MISSING WAS NOT A WRITE CALL. The vendored reader has vdev_write_phys and even a label writer (the bootenv path uses it), so bytes could always reach the disk. What it has none of - the string appears zero times in zfsimpl.c - is SPACE MAPS, because a boot loader reads and therefore never allocates. Everything below is new code against a documented format rather than a port of something already present.

SPACE MAPS ARE A LOG, NOT A BITMAP. Each metaslab holds allocations and frees appended in txg order, and "what is free" is only available after replaying the whole thing. Three entry shapes share the encoding and one of them is two words wide, so mistaking a one-word debug entry for the first half of a two-word entry desynchronises everything after it - producing a range tree that is confidently wrong rather than obviously broken. Offsets and runs are in units of ashift and relative to the metaslab's own start; forgetting either yields ranges that look plausible and are in the wrong place. Verified by summing every entry of every log and holding the total against `zdb -mmm`, which sums smp_alloc independently: both say 258560 bytes.

THE ALLOCATOR HAS TO SEE WHAT THE SPACE MAPS CANNOT. The map describes the last COMMITTED txg, so a second allocation in the same transaction must be checked against the first one in memory or the two overlap. Also, ms_count is a TRUNCATING divide of asize, so the tail of the vdev belongs to no metaslab and must never be handed out - a test asserting asize == ms_count << ms_shift was this tree's own mistake and the corrected form is kept. First-fit, deliberately: a wrong allocation POLICY costs performance and a wrong allocation IMPLEMENTATION costs the pool.

ONE BUG, AND IT IS THE ONE WORTH RECORDING. The byteorder bit in a blkptr is not "0 means little-endian". It records the order the block was written in using ZFS_HOST_BYTEORDER's encoding, which is 1 on x86, and BP_SHOULD_BYTESWAP is `BP_GET_BYTEORDER(bp) != ZFS_HOST_BYTEORDER`. Writing 0 told every reader the block came from a big-endian machine, so the checksum was verified against a byteswapped copy. It presented as: the bytes reached the disk correctly, a raw read of them matched byte for byte, and zio_read still returned EIO - the checksum would not verify against the very buffer it had just been computed from. The probe now reports the raw-medium check and the zio_read check separately, because three different failures all surface as EIO and one signal cannot distinguish them.

THE COMMIT WORKS AND REAL ZFS AGREES. An uberblock goes into all four labels at ring slot `txg % count` - that modulo IS the crash-safety story, since a new uberblock never overwrites its predecessor and a torn commit leaves the previous txg intact in another slot. Uberblocks are self-checksumming: the checksum covers the block with a verifier derived from the block's own physical offset sitting where the checksum will go, which is why one cannot be relocated and why the four copies have four different checksums. tests/host/check_zfs_write.sh imports the resulting image with real OpenZFS and scrubs it: uberblock txg 36 (up from 35), pool ONLINE, `scrub repaired 0B ... 0 errors`, every file intact. That is the only verification that counts - Genesis reading back its own writes proves two halves of one program agree, and a pool only Genesis can read is a pool Genesis has corrupted in a way only Genesis forgives.

WHAT IS STILL MISSING, PRECISELY. fs_ops_t::write is still NULL for ZFS and `zfs_writable` still answers no, which is accurate. Making a file point at a written block means rewriting the whole chain above it - the dnode, the dnode array's indirect blocks, the objset, the DSL dataset in the MOS, and the MOS tree above that - and persisting the space map changes in the SAME txg, or the next mount hands out blocks that are already in use. There is no shortcut past that; it IS what copy-on-write means. Also absent: gang blocks (so a fragmented pool gets ENOSPC where real ZFS would succeed - the safe direction), compression on write, and more than one DVA per block (legal on disk, since BP_GET_NDVAS counts the filled slots, but less redundant than what ZFS would have written - a difference in durability, not in format). Multi-vdev pools are REFUSED rather than written to badly.

NEW — THE COW CASCADE IS BUILT, RUNS END TO END, AND DOES NOT YET CONVERGE. zfsg_write_block walks the whole chain a real write needs: the file's data block, its indirect blocks, the file's dnode, the dnode-array block holding it, the filesystem's objset, the DSL dataset's ds_bp in the MOS, the space maps for every allocation made along the way, the MOS's own objset, and the uberblock. It is roughly 400 more lines and every layer of it is exercised. It returns EAGAIN.

WHAT EAGAIN MEANS HERE, and it is not a euphemism. The transaction detected that its allocation accounting had not settled within its pass budget and ABANDONED itself - which costs nothing, because nothing is visible until an uberblock is written. The pool afterwards is byte-for-byte what it was, minus some blocks written into free space that nothing references. That is asserted in the host suite (every file still resolves and reads) and then checked by real OpenZFS: import, `scrub repaired 0B ... 0 errors`, everything intact. An abort being a non-event is the property that made attempting this defensible at all, and it is the one thing here that is fully proven.

WHY IT DOES NOT CONVERGE, which is the useful finding. Recording an allocation means appending to a metaslab's space map, which is writing to a DMU object, which allocates. That recursion terminates only if each pass allocates strictly less than the one before, and this implementation guarantees the opposite: it copy-on-writes the MOS meta-dnode afresh on EVERY dnode update, and that array is SIX levels deep on a real pool, so each update costs six blocks. Since each space-map append is itself a dnode update, recording N allocations creates roughly 7N more.

The first attempt was worse still - one space-map append per allocation rather than per metaslab - and exhausted a 64-allocation budget before finishing a single pass. Batching by metaslab fixed that and moved the failure from "out of budget" to "does not converge", which is the more honest failure and the one that names what is missing.

WHAT IS MISSING IS A DIRTY-BUFFER LAYER, and that is the DMU. Real ZFS does not copy-on-write a block every time it is touched; it dirties the buffer in memory for the whole txg and writes it ONCE at sync time. Six blocks per dnode update becomes six blocks for the entire transaction, the recursion collapses, and the passes converge. That is not a tweak to the code above - it is the piece of the DMU this tree does not have, and it is what item 7's "vendor module/zfs" has been pointing at all along.

SO: FS_OPS_T::WRITE IS STILL NULL FOR ZFS, and `access(W_OK)` still answers no. That remains accurate and is deliberately not being flipped. Genesis can allocate real space, write checksummed blocks real ZFS accepts, commit an uberblock real ZFS imports and scrubs clean, and walk the entire metadata cascade - and it still cannot change a file, because the last thing standing between those two sentences is a cache with transaction semantics.

NEW — WHAT "ALL FEATURES" ACTUALLY MEANS, because the gap between what is here and what ZFS is has not been written down and the number matters. What is vendored is the BOOT LOADER's reader. It gates on eighteen read features (features_for_read[] in zfsimpl.c: embedded_data, hole_birth, large_blocks, large_dnode, extensible_dataset, device_removal, lz4/zstd/blake3/skein/sha512, encryption, bookmark_v2, head_errlog, multi_vdev_crash_dump, vdev_zaps_v2, dynamic_gang_header) and REFUSES a pool using anything else. It cannot write, and it is not a reduced version of the real thing — it is a different, simpler program that happens to parse the same on-disk format.

The real thing is vendsrc/sys/contrib/openzfs/module, which is 346,837 lines. module/zfs alone is 205,345. Getting to "all features" means essentially all of it: spa.c/vdev*.c (the pool), metaslab.c/space_map.c (allocation), dmu*.c/dbuf.c/dnode*.c (the object layer), arc.c (the cache), zil.c (the log), txg.c/dsl_*.c (transaction groups and the dataset layer), zap*.c, zio*.c (the pipeline), plus module/icp (crypto), module/lua, module/zcommon, module/nvpair, module/zstd and module/avl.

NEW — AND IT IS BLOCKED, on four things Genesis does not have, each of which is its own item rather than a detail of this one. Listing them because "vendor more ZFS" is not actionable and "build these four, then vendor ZFS" is:
(1) KERNEL THREADS - DONE. See the dependency note at the top of this file. The txg sync thread, the ARC reclaim thread and the ZIL commit path are real threads that block and are woken, and there is now something for them to be. sleep(9) deschedules for a kernel thread rather than idling. Everything else on this list depended on this one, so the rest of the list is now reachable in order.
(2) CONDITION VARIABLES and sleepable taskqueues - DONE. See the note at the top of this file. cv_wait/cv_broadcast/cv_timedwait exist (kernel/bsd/kern_condvar.c) and taskqueue(9) defers to a kernel thread instead of calling the task inline (kernel/bsd/kern_taskqueue.c). The DMU and the ARC use both throughout, and neither is now the reason they cannot be vendored.
(3) A PAGE CACHE WITH WRITEBACK - THE CACHE IS BUILT, THE WRITEBACK IS NOT, and the split is deliberate. kernel/fs/pcache.c caches by (volume, inode, page) ABOVE the filesystem, which is the distinction this entry was drawing against bcache: a block cache keyed on (device, LBA) does nothing for "have I already read this part of this FILE", because answering that means walking the filesystem's structures to turn an offset into an LBA - which is most of the cost. That is 7(d)'s "exec re-reads it through the block cache every time".

64 pages, clock/second-chance eviction, wired under fs_read so no filesystem has to know it exists, and invalidated by every path that mutates: write (the written range only), truncate and unlink (the whole file), unmount (the whole volume). Unlink resolves the inode BEFORE unlinking, because FAT reuses directory slots and pages keyed on a freed inode can otherwise be inherited by the next file created.

WRITES ARE WRITE-THROUGH. Deferring a write means deferring the ALLOCATION behind it, and on FAT those are the same act - fat_write_entry_at extends the chain, links clusters and rewrites the directory record as it goes. A dirty page past EOF describes bytes with no cluster, so flushing later has to allocate later, and until it does the file's size on disk disagrees with what every reader is told. That does not produce a slow filesystem, it produces a file whose length is a lie. ZFS does not have this problem - a txg commit allocates and writes together and nothing is visible until it does - so the writeback half belongs with the DMU rather than here, and building it against FAT first would mean building it twice.

Still open here: file-backed mmap (sys_mmap still answers -ENODEV for anything but MAP_ANONYMOUS).
(4) A REAL kmem_cache WITH RECLAIM - DONE. kernel/bsd/kern_lowmem.c. The reclaim machinery was never the missing part: uma_core.c ships all of it, and uma_vendor.c's own comment had already narrowed the gap to "the trigger rather than the context". This is the trigger - a kernel thread that watches free memory and raises vm_lowmem.

A THREAD RATHER THAN A HOOK IN THE ALLOCATOR, because reclaim CALLS FREE: the vm_lowmem handlers are arbitrary subsystem code that allocates and frees while running, so firing from inside pmm_alloc_frame means re-entering the allocator from its own failure path with its lock held. Upstream splits it the same way and for the same reason. Kernel threads did not exist when this entry was written, which is why it is only buildable now.

TWO POOLS ARE WATCHED. UMA's slabs come from the KERNEL HEAP (genesis_kmem_malloc is kmalloc_a), not from frames directly, so heap exhaustion arrives first and is the pressure a cache can act on; frame exhaustion arrives later and matters more. The threshold is a FRACTION (one eighth) rather than a number, because an absolute floor never stops firing on a small machine and fires far too late on a large one. Rate limited to once a second, upstream's number, so a handler that can free nothing does not turn low memory into a spin.

THE CHECK THAT MADE IT REAL was measured on the wrong pool first and therefore could not fail: it watched pmm_free_frames while UMA hands memory back to the HEAP, and reported "nothing cached" while sitting next to a zone with 128KB cached. Measured on the heap it reports 0x3a000 bytes returned, and the assertion is that the number moves. Both directions of the threshold are checked too - a should_fire that answered yes always would turn the watcher into a permanent reclaim loop and would pass a one-sided test.

NEW — the honest sequencing, then, is: kernel threads first (they unblock (2) and make sleep(9) real rather than an idle loop) - DONE; then condvars and a real taskqueue - DONE; then vendor module/zfs's READ path, which is where this now stands and is the first step whose size is measured in tens of thousands of lines rather than hundreds (dmu/dbuf/dnode/zap/dsl/zio) which gets the full read feature set and is testable against the same pools the bootloader reader already mounts; then the ARC once there is something to reclaim under; then the write path, which is the first point where a bug destroys data rather than failing a read. Feature-flag gating stays the entry check throughout: a pool with a feature this build does not implement must be REFUSED, not mounted read-only-and-hope, because ZFS's whole compatibility story is that an unknown incompatible feature means the on-disk format is not the one this code understands.

NEW — THE VFS WORK THE ROOT FLIP NEEDS, which is a separate prerequisite from any of the above and is small enough to do first. Flipping root to ZFS means the root filesystem is one whose vtable slots are mostly NULL today, and four things break at that moment. (a) is now done; (b), (c) and (d) are not:
(a) fs_ops_t HAS NO unmount SLOT - FIXED, and it was not harmless for FAT either. fs_ops_t::unmount exists, fs_unmount_volume and fs_unmount_at both call it, and fatfs and zfs both implement it. zfsg_unmount had been sitting on the vendored side the whole time never called, which is the shape this bug had: not a missing implementation, a missing CALL.

    THE PART THAT IS NOT OBVIOUS, and which the naive fix gets wrong: the slot is RETIRED, not freed. kernel/dev/volume.c already documented why - handles on files that lived on the volume still hold fs_node_t bodies pointing at that fs_volume_t, and `mounted` is what turns their next read into -ENODEV. Handing the same fs_volume_t to a different disk turns that honest -ENODEV into a successful read of somebody else's filesystem, which is strictly worse than the leak. So an unmount releases the expensive state and leaves the slot claimed-but-retired; a retired slot is reused only when no free one is left. That is exactly the policy volume.c always stated - "slots are recovered when the pool is exhausted, not when a device goes away" - and it was true of nothing, because nothing recovered them at all.

    THE SAME LEAK WAS ONE LAYER UP. volume.c's own volume_pool had it too: vol_detach never cleared in_use, so eight insert/remove cycles exhausted the pool for the life of the machine. Same retire-and-reuse-on-exhaustion policy applied there.

    THE CHECK, measured against the bug rather than assumed to catch it: tests/host/volume_test.c inserts and pulls the same disk seven times - more than either pool is deep - and the disk must mount every time. With fat_ops.unmount forced back to NULL it fails and names the cycle: "first cycle that failed to mount: 4". Four rather than five because the mount earlier in that file already spent one slot, which is itself the point - the leak is per-machine, not per-test.
(b) NO write SLOT - DONE FOR fat16, still open for zfs. fatfs's `write` is fat_write_entry_at: it extends a file, allocates and links clusters as it grows, and updates the 32-byte directory record last so a crash leaves allocated-but-unreferenced clusters (an fsck-able leak) rather than a file reporting garbage as content.

    fs_writable derives its answer from that pointer, so access(W_OK) flipped from -EACCES to 0 with no change to access(2) itself - which is what having one source of the fact was for. write(2) works end to end from ring 3: open(O_RDWR), fileobj, fs_write, fatfs_write, the block layer, the disk. Checked by verif.c's "write(2) on a file", which is self-restoring because it runs against the real staged root.

    THE THING THAT WAS WRONG FIRST, because it is the part that is easy to miss: a write past the end of file must make the skipped bytes read as ZERO, and those bytes live in two different kinds of place - clusters this write allocated (which are zeroed on allocation) and THE TAIL OF THE CLUSTER THAT ALREADY HELD THE END OF FILE, which is stale data from whatever was there before. Zeroing only the new clusters leaves that tail, and it reads back as somebody's deleted data inside a file that never wrote it. The gap is now written as zeroes through the same path as the data, which makes both cases correct without either being a special case.

    That is checked, and the check was MEASURED rather than assumed: tests/host/fat_write_test.c poisons the bytes past a file's end before writing past them, because on a freshly formatted image every byte past every EOF is already zero and a wrong implementation passes by luck. With the gap fill removed the check fails; with it, it passes.

    THE WAY IN IS OPEN TOO. sys_openat used to refuse O_CREAT, O_TRUNC and O_APPEND with -EROFS, so no program that opens a file for output could get started at all. All three work now, on two more vtable slots:

      fs_ops_t::create   - fat_create writes a directory record with first
                           cluster 0 and size 0, which is FAT's own spelling
                           of "empty". No cluster is allocated: the first
                           write does that, and allocating here would give
                           every touched-and-never-written file one it does
                           not use. O_EXCL distinguishes create-or-open from
                           create-or-fail, which is the difference between a
                           lock file working and not.
      fs_ops_t::truncate - shrink cuts the chain and frees the tail; grow
                           zero-fills by delegating to fat_write_entry_at, so
                           "the gap reads as zero" has one implementation
                           rather than two that can disagree. The record is
                           written BEFORE the chain is cut, which is the
                           opposite of fat_unlink's order and for the mirror
                           reason: unlink must not leave an entry naming freed
                           clusters, truncate must not leave a size claiming
                           clusters that are gone. Both choose to leak space
                           rather than describe space that is not theirs.

    O_APPEND is a status flag on the open instance, and do_write seeks to the
    end immediately before EVERY write rather than once at open - which is the
    whole content of the flag, and what makes two descriptors appending to one
    file interleave instead of overwriting. F_SETFL accepts it now too.

    truncate(2) and ftruncate(2) are in the syscall table, which clears two of
    the three entries the Owed list said were waiting on the content path.

    THE END-TO-END PROOF is a shell doing what a shell does:

        # /bin/ls > /t.txt
        # /bin/ls /
        BIN  BOOT  ETC  LIB  SBIN  T.TXT  USR  WSR

    A real program, real redirection, a file that was not there before, on the
    real disk.

    ZFS still has no write slot and that is a different problem entirely - the vendored reader's vdev write callback returns EROFS, so "then write" is a different piece of software rather than a flag flipped here.
(c) NO SYMLINKS BELOW THE VFS. The object namespace has them (ns.c); fs_ops_t does not. readlinkat is in the syscall table with nothing on disk to resolve. ZFS has symlinks and a root filesystem that silently cannot represent one is a root that cannot hold /bin/sh -> busybox.
(d) NO PAGE CACHE, so exec of a large binary off ZFS re-reads it through the block cache every time. Tolerable for a second volume, not for the volume every execve comes from.

NEW — and one thing to check rather than assume when the flip happens: the NT side reaches a volume through the object namespace (\Device\HarddiskVolumeN + an unparsed remainder, consumed by dev_open_object -> vol_parse -> fs_lookup_on) and DELIBERATELY skips the mount table. That path is filesystem-agnostic today because vol_parse only converts separators, but it has never been exercised against a filesystem whose lookup is not FAT's. The check is the one already written down for this item - systest's filesystem section running identically on both volumes - extended to run through both spellings.


9. Write all missing syscalls

88 of 107 as of item 11's capability audit. THE SIX THE OWED LIST NAMED ARE DONE - mremap, sigaltstack, waitid, execveat, eventfd2 and socketpair - with 61 new checks in systest (238 -> 299).

Notes worth keeping, because in each case the interesting part was not the syscall:

MREMAP is implementable only because the caller passes old_size. sys_mmap keeps no record of what it handed out - the address space is a bump pointer and the page tables are the whole account - so the extent to resize has to be an argument. Grow-in-place is tried before moving, and the discriminating test is the SHRINK: it must return the same address, which an implementation of "allocate small, copy, free" fails while passing every contents check.

SIGALTSTACK needed the delivery path, not just the syscall. sigalt_on is a DEPTH rather than a flag: a second SA_ONSTACK signal arriving inside the first handler must stack below the outer frame rather than restart at the top of the alternate stack, and a flag gets that right only until signals nest. The frame records whether IT was the delivery that stepped on, so the depth comes back down exactly as many times as it went up.

WAITID reuses wait4's blocking loop rather than copying it - that loop is four lines of policy around one line of hard-won `sti; hlt; cli` detail, and a second copy is a second place to get it wrong in a function whose bugs present as a shell that hangs. The check that matters is the WNOHANG one: si_pid must be ZEROED, because that is the only way a caller distinguishes "nothing ready" from a real report, and the test fills the buffer with 0xEE first so a kernel that leaves it alone fails.

EVENTFD2 is the self-pipe trick with the pipe taken out. Two wait queues rather than one, for kern_taskqueue.c's reason - readers and writers are woken by opposite events. POLLOUT is CONDITIONAL, which is the half an implementation skips because a counter is almost never full; the one caller that hits the ceiling would otherwise be told it may write and then block inside a call poll just promised would not.

SOCKETPAIR IS TWO CROSSED PIPES, and that is the design rather than a shortcut. The hard part is that each end must be readable AND writable, which no single pipe object can be. A fresh type with its own rings would reimplement the wait queues, the EOF rule and SIGPIPE - and the reimplementation is the one that gets the edge cases wrong. Shutdown semantics then come out for free and correct: closing one end leaves the peer's read at EOF and its write at EPIPE, neither of which is code in that file.

Still open: thirteen or so others, and the musl ptrace-trace method below is still the systematic way to find which ones matter.

NEW — POLL(2) ON A SOCKET NEEDED A WAKEUP, found while testing the above and worth its own line because it is a whole CLASS of hole rather than one bug. Genesis's poll parks on ONE shared readiness queue that every waitq_wake_all pokes - waitq.c argues for a single queue exactly so that no wake site can be forgotten. The socket layer does not use waitq at all; it wakes through selwakeuppri, which the compat header defined as a no-op with the comment "a socket layer built on this would block through waitq instead". True until socket(2) existed. Afterwards a poll on a socket slept to its timeout with the data already in the receive buffer.

selwakeuppri pokes the readiness queue now. THE FIX IS NOT COVERED BY A TEST and that is recorded in verif's PART B rather than glossed: proving a wakeup needs data to arrive on a schedule, this machine has no loopback interface, and its only UDP peer answers only when the HOST has an upstream resolver. A loopback interface is the thing to build before that line can be claimed - and it would make the receive path testable without any peer at all, which is worth more than this one check.

NEW — the musl ptrace-trace method that already found poll/access/nanosleep/fcntl is a more systematic way to fill this than reading a syscall table; running it against a wider corpus (busybox/sbase, both of which now run) would do the same job again.

NEW — the musl ptrace-trace method that already found poll/access/nanosleep/fcntl is a more systematic way to fill this than reading a syscall table; running it against a wider corpus (busybox/sbase, both of which now run) would do the same job again.


10. Clean up the root that gets staged, and add what needs to be added. Clean up the source code dir.

THE COLLISION CHECK IS DONE - tests/host/check_staged_tree.py - and it is a BUILD check rather than the systest this entry asked for, deliberately. The collision is a property of the source tree and the naming rules, both known before the image exists, so this refuses to build a bad image rather than reporting a bad one after boot. It is also the only place the check can be complete: by the time the guest sees the volume one of the two colliding names is simply gone, so a walk of the mounted filesystem cannot tell a collision from a file nobody staged. It uses fatfs.Fat16.name_to_11 - the stager's own encoder - rather than a reimplementation, so it cannot drift from the thing it checks. Verified by making it fail: both the historical System32/system32 case and an over-long name are caught by name.

AND IT UNCOVERED A CHECK THAT HAD BEEN FAILING FOR A LONG TIME. tests/host/check_zfs_boundary.py reported 294 failures on every run, ALL OF THEM FALSE. It matched includes on BASENAME against every file under kernel/zfs/ including kernel/zfs/compat/ - a vendored shim layer full of deliberately generic names (sys/param.h, sys/types.h, sys/queue.h) that kernel/bsd/compat/ also has. So every BSD file including <sys/param.h> was reported as reaching into ZFS when it resolves to the BSD header.

The second consequence was worse than the noise: run.sh has `set -e` and that check exits 1, so it silently GATED EVERYTHING AFTER IT. The new staged-tree check appended below it never ran until this was fixed, and run.sh had been exiting 1 while the C suite's "all checks passed" line made it look green. Basename matching cannot work here - colliding basenames across the two vendored trees is exactly why build.py hands out include paths per directory - so the compat subtree is excluded and the path form ("zfs/...") is kept. Verified by making it fail on a real violation in both spellings. run.sh now runs to completion and exits 0.


12. AHCI IS DONE, NATIVELY, AND IT IS THE GATE THE TARGET MACHINE WAS BEHIND

kernel/dev/ahci.c is a polled AHCI driver: controller reset, port bring-up, IDENTIFY, and READ/WRITE DMA EXT through a PRDT. `ahci: selftest passed - 32768 sectors, DMA write/read round trip on lba 32767`, and the disk registers through disk.c as \Device\Harddisk4\DR4 / sde with its volume scanned like any other. Boot 52 selftests, verif 160/0, systest 382/0, host rc=0.

WHY NATIVE AND NOT VENDORED, which is the decision rather than the code. vendsrc/sys/dev/ahci/ahci.c is 2,911 lines and is a CAM SIM driver - it includes cam/cam_ccb.h, cam/cam_sim.h and cam/cam_xpt_sim.h and never touches a disk directly. Vendoring it means vendoring CAM, which is 104,469 lines, or shimming the twenty-four xpt_ and cam_sim_ entry points it uses. AHCI's own programming model is small enough that writing it is cheaper than either, and the result fits the shape ata.c already established. The DRIVER MODEL is still Newbus - what was rejected is FreeBSD's ahci.c specifically, not FreeBSD's driver model. Linux was worse (libata sits on the SCSI midlayer, and none of it is in a FreeBSD vendsrc tree) and WDM was categorically wrong (Windows AHCI is a Storport miniport, and WDM's value is binary compat with drivers whose source does not exist - AHCI's does).

TWO MISTAKES WORTH KEEPING. The first: phys_to_virt was used to reach the BAR, and it page-faulted on the first register read. paging.h says why in as many words - it "resolves for ANY physical address IN RAM" and explicitly names an MMIO address as something it is not for. The direct map is built over E820 RAM; a BAR at 0xFEBD1000 is in none of it. ioremap is the right call and also maps the window UNCACHED, which matters independently: a cached mapping of PxCI would let the CPU satisfy a read from a cache line and never see the controller clear it, which is a hang indistinguishable from a dead controller.

The second was in build.py rather than in the driver: `-device ich9-ahci` had been attached since the MSI work with NO DRIVE BEHIND IT, so the driver found an HBA with zero occupied ports and its entire transfer path would never have run. A driver whose transfer code has never executed is untested however many registers it read correctly. The IDE drives stay attached alongside, so a boot proves the two storage paths coexist rather than one having replaced the other.

STILL POLLED, like ata.c and for the reason ata.h gives: polling has no concurrency, so a misbehaviour is in the storage protocol rather than in interrupt handling. Also still one command slot, one PRD entry, and a bounce buffer - AHCI is DMA, so the controller walks physical addresses with the MMU bypassed, and a caller's kernel pointer may straddle pages that are not adjacent in RAM. A real scatter list built from the caller's mappings is the obvious next step and is what removes the 8-sector-at-a-time ceiling.

12c. SERIAL IS A CONSOLE NOW, NOT JUST A TRANSCRIPT

kernel/dev/serial.c had a transmit half and no receive half - it says so in its
own header, and that was right while the only input device was PS/2. The
bare-metal target broke that assumption: it has a USB keyboard and NO PS/2
device at all, and kernel/dev/keyboard.c is PS/2-only, so the shell would come
up on a machine nobody could type at. The alternatives were xHCI plus a USB HID
stack, or fifty lines and the 16550A the box already reports at 0x3f8 IRQ 4.

RECEIVED BYTES GO INTO THE KEYBOARD'S OWN RING, through a new kbd_inject. That
is the whole integration and it is the part worth defending: the line
discipline, the Ctrl-C handling, the echo and every reader above sit on that
ring, so a serial console behaves IDENTICALLY to the keyboard rather than being
a parallel input path that drifts. A second ring would need all of it
duplicated and the copies would eventually disagree about something - most
likely about what ends a line. Almost no translation is needed either, because
the existing line reader already treats '\n' and '\r' alike and '\b' and 0x7F
alike, which is exactly what a terminal sends.

THE INTERRUPT PATH WORKS AND WAS PROVEN TO, rather than assumed. There is also
a timer-tick fallback drain, and the two are counted separately (serial_rx_report
prints "rx N by irq, M by poll") so a dead interrupt shows up instead of being
silently carried by the poll. To check it was not the poll doing the work, the
fallback was disabled and the guest driven again: typing /bin/hello over serial
still ran it. The fallback stays as insurance for hardware this tree has not
seen - a machine where IRQ 4 does not route would otherwise have no keyboard
AND no serial, which is unreachable - but it is insurance, not the mechanism.
The handler also returns whether it CLAIMED the interrupt, because IRQ 4 is
conventionally shared with COM3 and irq.h is explicit that a handler which
cannot tell must return zero.

AND IT MADE THE TEST HARNESS TEN TIMES FASTER. tools/guest_run.py drove the
guest with QEMU monitor `sendkey` - one command per character with a delay
between them, because nothing read the serial port back. It is a pipe now:
verif and systest together went from minutes to 28 seconds, and the whole class
of bug where a missing KEYMAP entry silently mistyped a command is gone.

One bug in that rewrite is worth keeping because it is the same shape as the
last one: a suite's TALLY LINE is not the end of its output. verif prints
"verification: N passed, M failed" and then keeps going with
print_manual_queue(), so sending the next command on the tally alone typed it
into the still-running program's stdin. systest never ran, and the driver
reported one clean result and exited zero - success-shaped failure again. Fixed
by waiting for the output to go quiet rather than for a line to appear.

12b. Write the remaining drivers needed for basic operation (choose what driver model best suits what the driver does the best based on their ecosystem strengths, ABI stability, and performance characteristics.)

The DECISION below is made and written down. No driver has been written to it
yet, and that is the whole of what this item is now. AHCI and NVMe are the
named gaps; ATA is still PIO-only.

NEW — keyboard.c, rtc.c, disk.c, and timer.c already exist as ad hoc drivers predating any of the three models; worth an explicit pass deciding which model each migrates to versus staying bespoke because they're boot-critical, rather than starting from a blank page

NEW — the decision, worked per category rather than left as an abstract principle: storage (ATA/AHCI/NVMe) is Newbus — ata.c+disk.c is already conceptually a two-layer Newbus driver (raw hardware talk, then a device_ops_t adapter), it is the same ecosystem ZFS is already vendored from, and its direct synchronous call shape fits this kernel's uniprocessor polling-first storage path better than Linux's block layer or WDM's IRP-per-request overhead. The two NICs item 6 already names, virtio-net and e1000, are Newbus too, ported straight from the real drivers already sitting in vendsrc/sys/dev/virtio and vendsrc/sys/dev/e1000 — no reason to reach for a translation layer when a native, well-tested driver for the exact target hardware already exists in the vendored tree. Wifi and anything newer or less common is Linux-shaped instead: vendsrc/sys/contrib/dev (rtw88, rtw89, iwlwifi, iwm, iwn, mwl, athk, mediatek, ...) is FreeBSD's own admission that these are a source-compat-with-Linux-driver-code problem, not a from-scratch-native one, and the same reasoning will apply to any GPU driver beyond a basic framebuffer — FreeBSD's own DRM story is LinuxKPI-hosted for exactly this reason. Input (PS/2 now, USB HID later) and RTC/timer/system peripherals stay Newbus: simple, standardized, already native, no ecosystem reason to diverge. WDM/KMDF's role is different in kind from the other two rather than a third slice of the same pie — it is the only path to binary compatibility with a driver that has no available source anywhere, the proprietary-Windows-only case (laptop ACPI/embedded controllers, audio codecs, some GPUs), which is also why /wsr/Windows/System32/Drivers is a real load path in item 4's text rather than a decoration: it names loading a driver out of an actual Windows installation tree. Until the kernel-mode PE loader and NT export surface item 4 already flags exist, WDM-shaped code here means driver source written natively against that shape same as the other two, but the eventual payoff — unmodified binaries — is categorically different from what Newbus or the Linux shim ever offer.

NEW — the matching engine underneath all three models stays Newbus-shaped (bus.c's devclass/probe/attach), but two pieces of the real WDM architecture are genuinely better than anything Newbus has, not just differently-shaped: IRP-based dispatch, where a request is a queueable, cancelable object with a completion routine rather than a blocking function call — device_ops_t and Newbus's attach() are both fundamentally synchronous-call-shaped, and neither has an answer for overlapped or in-flight-cancelable I/O if a NIC's TX ring or USB's concurrent transfers ever need one; and device stacking (IoAttachDeviceToDeviceStack), layering a filter driver — disk encryption, RAID — on top of another device's object, which Newbus's flat parent-child devclass tree cannot express at all (FreeBSD itself does not solve this in newbus either; it reaches for GEOM, a separate stacking system, for exactly this reason on the storage side). Neither is worth building now — Genesis is uniprocessor, synchronous, and one-driver-per-device by device.h's own existing design — but the day a real hot-pluggable bus (USB) or a filter-driver use case (disk encryption above the storage stack) shows up is the day reaching for WDM's actual IRP/stacking model, not just its type shapes, over bolting the same thing onto Newbus is the right call.


13. SMP bring-up — REMAINDER ONLY

AP startup, per-CPU data, TLB shootdown, the LAPIC and the IOAPIC are all done
and verified under -smp 2. What is left:

NO INTERRUPT BALANCING. Every line is routed to the BSP and stays there. Choosing a destination per line is possible now (ioapic_route_irq takes an APIC ID) but deciding it dynamically is policy with no measured problem behind it.


NEW — 14. Finish the NT object manager and namespace

What exists: kernel/obj/object.c (refcounted object_t behind an object_type_t vtable - read, write, getdents, poll, destroy) and kernel/obj/ns.c (a tree of ns_entry_t, kinds DIRECTORY/LINK/OBJECT, backslash-separated, case-insensitive, with the UNPARSED REMAINDER rule that lets one namespace address filesystems and character devices without knowing what either is). dev_open_object is the single place a remainder is consumed. ns.h states the design decision that makes this item worth finishing rather than trimming: the namespace is NT's and the POSIX spelling is layered ON TOP rather than beside it, because two namespaces would mean two sets of names for one console.

What is in it today: \Device\Console, \Device\Null, the disks and volumes, and the \??\ links (CON, console, tty, stdin, stdout, stderr, null, NUL, C:, sda, sdb). Devices and links to devices, and nothing else.

NEW — DISPATCHER OBJECTS - THE KERNEL HALF IS DONE (kernel/obj/dispatch.c). Events, semaphores and mutants exist, are nameable under \BaseNamedObjects, and a wait on one really blocks. object_type_t grew the two slots this entry asked for, and the pairing with poll is written out at the slot: poll asks "would this block" and must not change anything; wait PERFORMS the operation and CONSUMES, so wait cannot be built out of poll - "poll says ready, therefore take it" has a window in between where somebody else takes it.

    The three semantics are all implemented as this entry specified them: a notification event stays set and releases everybody, a synchronisation event auto-resets so exactly one waiter gets through per signal, a semaphore REFUSES a release past its limit rather than clamping (and the check asserts the count did not move, because a clamp would pass the refusal test alone), and a mutant is recursive with release-by-a-non-owner an error rather than a no-op.

    \BaseNamedObjects, \KernelObjects, \ObjectTypes and \Sessions all exist now, and \ObjectTypes is populated from a type registry - so the namespace is self-describing, including describing its own "Type" type. NS_MAX_ENTRIES went 64 -> 256, which this entry also asked for.

    THE RING-3 HALF IS DONE TOO. Nine NT syscalls (kernel/exec/nt.c): NtCreateEvent, NtOpenEvent, NtSetEvent, NtResetEvent, NtWaitForSingleObject, NtCreateSemaphore, NtReleaseSemaphore, NtCreateMutant, NtReleaseMutant - with the matching stubs and exports in src/ntdll, whose numbers are generated from kernel/include/nt.h so there is no second list to keep in step.

    Three details in there that are decisions rather than transcription:

      STATUS_TIMEOUT IS A SUCCESS CODE. NT_SUCCESS() is true for it, so a
      caller that only tests NT_SUCCESS and then uses the object has a bug -
      which is precisely why it is a different value rather than an error, and
      why the check compares it exactly instead of testing NT_SUCCESS.

      AN ABSOLUTE TIMEOUT IS REFUSED. NT's timeout is signed: NULL is forever,
      negative is a relative interval in 100ns units, positive is an absolute
      time since 1601. Only the first two are implemented, and the third
      returns STATUS_NOT_IMPLEMENTED rather than being read as a relative
      interval - which is what dropping the sign would do, and would be a hang
      wearing a plausible number.

      A NAMED CREATE THAT COLLIDES DESTROYS THE OBJECT rather than handing
      back the existing one. Silently opening someone else's mutex when you
      asked to create one is how two programs each end up believing they own
      it; that is NtOpenEvent's job and it is a separate call.

    THE CHECK is src/winsync/sync.c - a MinGW-built PE importing from
    ntdll.dll, so like src/winhello it knows no syscall number and no register
    convention. Twenty-nine checks, all passing, covering what the boot test
    cannot see: argument marshalling across the Win64 boundary,
    OBJECT_ATTRIBUTES parsing, handle allocation and the NTSTATUS values.

    STILL NOT THE TWO-PROCESS CHECK. Item 14's written-down test wants a named
    event created by one process and opened by name from another, and that
    still cannot be done: there is no NtCreateProcess, so a second NT process
    cannot be started. sync.exe does the naming path in ONE process - create by
    name, open by name, collide on a second create - which is every step
    except the second process, and dispatch_selftest does the blocking half
    with a kernel thread. Between them every mechanism is covered; the literal
    shape of the check is not, and that is worth keeping written down rather
    than declaring the item finished.

    THE CHECK asked for "a named event created by one process and opened by name from another, with the second one BLOCKING until the first signals". It is dispatch_selftest(), with one honest difference: the second context is a KERNEL THREAD, not a second process, because nothing in ring 3 can reach these yet. The mechanism under test - name -> object -> blocking wait - is exercised in full: the opener finds the event BY NAME having never been handed a pointer, and the creator observes it in PROC_BLOCKED while itself running, which is the pair that distinguishes a real wait from a function that returns immediately. The negative is there too: waiting on a console is -EINVAL, asserted next to a wait on an event that succeeds, so "correctly refused" is distinguishable from "nothing is waitable yet".

    AND IT FOUND A PRE-EXISTING BUG, in kernel/proc/waitq.c and not in any of this. When nothing else is runnable, schedule() returns immediately and never touches the state - so a process that then exits the loop ON ITS DEADLINE returns to ordinary code still marked PROC_BLOCKED, and the next schedule() refuses to pick it, permanently. A process wedged by a TIMEOUT, on an idle machine, which is the machine where "nothing else was runnable" is most likely. poll(2) with a timeout takes that path. sys_nanosleep had already worked around it by writing PROC_RUNNING back by hand; the fix belongs in the one blocking loop rather than in each caller that remembers, and that is where it is now.

NEW — the original text of this entry, kept because the argument is the reason the work was worth doing:
DISPATCHER OBJECTS are the biggest gap, and they are the half of the object manager that has no substitute anywhere else in the kernel. NT's namespace exists at least as much to let two unrelated processes agree on a synchronisation object BY NAME (\BaseNamedObjects\SomeMutex) as it does to name hardware - a Win32 program's first kernel call is very often CreateMutexW rather than CreateFileW. Three types, and the differences are visible to a waiter so they cannot be collapsed into one: an EVENT is a flag, where a notification event stays set and releases every waiter and a synchronisation event releases exactly one and auto-resets; a SEMAPHORE is a count with a limit, and a release past the limit must be REFUSED rather than clamped, because a caller that over-releases has a counting bug and clamping hides it forever; a MUTANT is ownership and is RECURSIVE - the owner may take it again and must release it as many times, which is exactly why it is not a semaphore of one. object_type_t needs two more slots for this (a wait that BLOCKS and a signal), and the pairing with the existing poll slot is the thing to get right: poll asks "would this block", wait performs the operation and CONSUMES - a synchronisation event resets, a semaphore decrements, a mutant takes ownership - so wait cannot be built out of poll and must not be called speculatively.

NEW — and the prerequisite that decided when is now MET. It read: waitq.c blocks a PROCESS, so a wait through it is only legitimate from a syscall path, and a driver waiting from kernel context has nothing to deschedule - so the userland half (NtCreateEvent/NtWaitForSingleObject) was buildable on waitq and the driver half (KeWaitForSingleObject with a real KEVENT) waited on kernel threads. Kernel threads exist, so BOTH halves are buildable, and the order in that sentence is still the right one to build them in - the userland half is testable from ring 3 by the check at the bottom of this item, and the driver half is not testable by anything until a driver wants it. The boundary that remains is a different one and is worth stating so it is not rediscovered: a kernel thread may wait, an INTERRUPT HANDLER still may not, and KeWaitForSingleObject at IRQL >= DISPATCH_LEVEL must therefore refuse rather than block. kern_synch.c makes exactly that distinction per call through ksleep_can_block(), and the dispatcher objects should make it the same way rather than inventing a second answer.

NEW — THE HANDLE TABLE IS NOT ONE, and the half of this entry that said the access mask is "never enforced again" IS NO LONGER TRUE. do_read and do_write both test open_file_t.access, and verif asserts it in both directions - writing through an O_RDONLY descriptor is refused, and writing through an O_WRONLY one works, so the refusal is the access mode and not a read-only filesystem.

What remains is a MODEL DIFFERENCE rather than a missing check, and it is worth stating that way because "add a mask to handle_t" would be the wrong fix. Genesis enforces access on the OPEN INSTANCE, which is POSIX's model and is what object.h's three-levels-of-flags comment carefully justifies: access is fixed at open and SHARED by every descriptor that dups it. NT's model is a mask on each HANDLE, so two handles to one object can carry different rights and a duplicate can be granted less than its source. Bolting the NT mask on beside the POSIX one would give two answers to "may this descriptor write", and the day they disagree is a security decision made by whichever check ran first. Choosing between them is the actual work, and it is downstream of there being a second user to protect anything from. NT's model is that the mask is checked at every operation, and that is what makes a read-only handle to a writable file mean anything. Also absent: OBJ_INHERIT semantics beyond the one HANDLE_INHERITABLE bit, and any notion of a handle table that is not the fd table.

NEW — NO SECURITY DESCRIPTORS, so OBJECT_ATTRIBUTES carries a path and nothing else. That is defensible while there are no credentials (kernel/bsd/net_absences.c makes the same point from the network side: there is one ucred and it is the unrestricted one), and it stops being defensible the day there is a second user. Sequencing note: this is downstream of a real credential in struct process, which item 9's syscall work is the natural home for.

NEW — TYPES ARE NOT NAMED - DONE, and it registers itself. ob_create registers a type the first time an object of it is created, so the list cannot drift: the alternative was a hand-written registration call in each of the eight files that define a type, and that is a list somebody forgets to add to - which would leave a new type missing from the one directory whose entire job is to say what types exist. A list that can silently be incomplete is worse than no list, because it looks complete.

    The consequence is worth stating rather than hiding: \ObjectTypes holds the types that have been INSTANTIATED, not the ones compiled in. A type nobody has created an object of does not appear. That is a slightly different question from NT's and the more useful one here - "what is on this machine" rather than "what could be". At boot it reads: console, null, device, Event, Semaphore, Mutant, Type.

NEW — the original entry, kept for its argument: TYPES ARE NOT NAMED - done at the same time as the dispatcher objects exactly as this entry suggested. ob_register_type/ob_publish_types keep a registry and publish it as \ObjectTypes\<name>, including \ObjectTypes\Type, so the list describes its own membership. Registration is separated from publication because of boot order: a type is a file-scope constant and can register before the namespace exists, so registering only records it and one sweep does the inserting. A type registered after that sweep - a driver loaded at runtime - publishes immediately.

    THAT LAST NOTE IS NOW STALE and is kept only because the paragraph above it is quoted history: it said "still all-dispatcher rather than all-devices: the device, file, pipe and console types do not register yet". They do. ob_create registers a type the first time an object of it is created, so registration is not a call anybody has to remember - which is why the boot listing reads console, null, device, Event, Semaphore, Mutant, Type rather than dispatcher types alone. The socket, eventfd and socketpair types added since register themselves on the same terms, without a line of registration code anywhere.

NEW — MISSING DIRECTORIES - DONE. \BaseNamedObjects, \KernelObjects, \ObjectTypes and \Sessions are created by dispatch_init(). NS_MAX_ENTRIES is 256 rather than 64, for the reason this entry gave: fine for eleven entries, not fine once every named event is one. NS_NAME_MAX 32 and NS_PATH_MAX 128 are unchanged and have not been a constraint yet.

NEW — ONE INCONSISTENCY THAT IS ALREADY A BUG-SHAPED THING - FIXED, and it was hiding a second bug. device_ops's `control` slot was documented, wired and had no caller: sys_ioctl was a hardcoded `fd > 2 -> -EBADF` plus a termios table. It dispatches through dev_control now, with arg_size decoded from the Linux ioctl encoding so a driver is told how big its argument is without a central table of every code.

    THE SECOND BUG was the `fd > 2` half. Descriptors 0, 1 and 2 got the terminal answers WHATEVER THEY POINTED AT - so with stdin redirected from a file, TCGETS returned a fully populated termios describing a console that was not there, and isatty() said yes for a redirected stdin. That is exactly the question isatty exists to answer, and every program deciding whether to colourise or to prompt asks it. The answer follows the OBJECT now, not the descriptor number. Checked in systest with the pair that makes it a test: a regular file must be -ENOTTY, and a descriptor that really is the console must succeed even though it is not 0, 1 or 2.

NEW — the check for this item, in the shape this tree uses: a named event created by one process and opened by name from another, with the second one BLOCKING until the first signals - a test that only creates and signals in one process cannot tell a real wait from a function that returns immediately. Plus the negative: waiting on a console must answer -EINVAL rather than hanging, and that has to be asserted or the "not waitable" path is indistinguishable from a type nobody has got to yet.


Owed

NEW — THE NIC GOT EXACTLY ONE INTERRUPT PER BOOT - FIXED, and it is the most interesting bug in this round because nothing that changed was wrong.

Symptom: `net: selftest FAILED - no ARP reply from the gateway (requests sent 2, replies 0)`, with `ipackets 0`. THE WIRE WAS FINE - a QEMU filter-dump pcap showed the guest's ARP request leaving at t=0 and slirp's reply arriving at t=+23ms, every run. Instrumenting newbus_compat.c's interrupt trampoline gave the number that cracked it: entered ONCE for the whole boot, and never again.

The mechanism, and all three parts are needed:

  1. THE KERNEL IS NOT PREEMPTIVE. A kernel thread runs only when something calls schedule(); the timer tick sets the reschedule flag and nothing acts on it while the kernel is on the CPU.
  2. net_selftest's wait_for() SPUN WITHOUT YIELDING - `pause` in a loop bounded by the tick counter. So for the whole 200-tick wait, no kernel thread could run at all.
  3. if_re registers re_intr as a FILTER. On an interrupt it MASKS THE CHIP (`CSR_WRITE_2(sc, RL_IMR, 0)`) and hands the receive work to rl_inttask on taskqueue_fast; re_int_task is the only thing that re-arms the mask.

One interrupt arrives, the chip goes quiet, the task sits on a queue whose service thread the waiter is starving, and the reply sits in the RX ring for the entire time the counter reads zero. Raising the wait from 200 ticks to 2000 did not help, which is what ruled out "the machine got slower" and pointed at a deadlock rather than a race.

IT WAS LATENT UNTIL THE TASKQUEUE BECAME REAL. taskqueue_enqueue used to be a macro that called the task INLINE on the enqueuing CPU, so re_int_task ran inside the interrupt handler and re-armed the chip before the waiter got another turn. Deferring the work was the correct change and it turned a working spin into a deadlock. Both halves were right; the combination was not.

AND THE TEST HAD BEEN PASSING ON LUCK BEFORE THAT - on where the single interrupt happened to fall relative to the reply landing in the ring. A green line that depends on timing is worse than a red one, because it is the same green line either way.

Fix: wait_for() calls kthread_yield() in its loop. Four net checks pass, ipackets 5, reproducible across runs.

THE GENERAL SHAPE IS WORTH KEEPING: in a non-preemptive kernel, any loop that waits for work produced by a kernel thread must yield, or it cannot succeed however long it waits. wait_for was the only caller in this class that had the bug, but it is not the only spin loop in the tree.

NEW — tools/build_user.sh EXITS 0 - it was exiting 1 on the rtl8139 module, which the Owed list has carried for a while. The cause was not the script: vendsrc/sys/dev/rl/if_rl.c and vendsrc/sys/dev/re/if_re.c both call device_get_sysctl_ctx and device_get_sysctl_tree in attach, neither existed, and `set -e` stopped src/kmod/build.sh dead at that file - before the two modules after it. Per-device sysctl contexts are real now (kernel/bsd/kern_devsysctl.c): dev.<name>.<unit>, created on first use, and the same device gets the same pair every time. Both drivers compile and all five modules build.

The naming trap that shaped it: sysctl_add_oid DOES NOT COPY THE NAME. So the obvious implementation of the unit level - snprintf the number into a buffer - would leave the node holding a dangling name. Unit names come out of a static table of decimal strings instead.


FILE CONTENT WRITING — DONE for fat16, and so is the way in: O_CREAT, O_EXCL, O_TRUNC, O_APPEND, truncate(2) and ftruncate(2) all work (see item 7(b)). statfs(2) and fstatfs(2) are done too, which clears that whole group.

statfs needed FREE SPACE and never needed the write path at all - it counts zero entries across the whole FAT, deliberately uncached, because a value captured at mount is right once and confident forever after, and df reporting a full disk as empty is worse than df being slow. f_files and f_ffree are 0, which is FAT being answered honestly rather than a gap: there is no inode table to count, and Linux's own vfat says the same.

The check that makes it a test rather than a plausible-looking table is the last one: it writes 8KB, requires the free count to have gone DOWN, then unlinks and requires it back EXACTLY where it started. A statfs reciting the superblock passes everything before that and fails both.

AND IT FOUND ONE: statfs on a path that does not exist returned SUCCESS. volume_ok answers "which volume owns this name", and with one volume mounted at "/" that is every name, including names of files that are not there - so statfs("/no/such/path") described the root volume perfectly. statfs names a file in order to identify a filesystem, and a name that resolves to nothing identifies nothing; it is -ENOENT now.

Also still open: execveat, waitid, mremap, socketpair, eventfd2, sigaltstack.

NEW — fstatfs(2) answers from the CWD rather than from the descriptor, and that is a real limitation written down rather than hidden: an open file object holds a node and a node does not carry a path, so there is no way from the descriptor to name its volume. Right whenever the descriptor and the working directory are on the same volume, which with one mounted volume is always. The fix is small - fs_node_t already carries its fs_volume_t, so it wants an fs_statfs_vol() taking that - and is not done because nothing calls fstatfs yet

NEW — the shell finds no `echo`, `cat` or `rm`, and this is NOT a staging gap, which is what it looks like. root/bin/busybox is a SHELL-ONLY build: 140KB, and `strings` finds no applet names in it at all - not even `echo`, which every ordinary ash has as a builtin. There are no applets to link to. Staging symlinks would therefore fix nothing, and FAT16 has no symlinks to stage anyway. What is actually needed is a busybox rebuilt with applets enabled, and its source is not in this tree - so this is a BUILD INPUT to acquire rather than a line to change. Recorded this way round because "the applet links are missing" sends the next person to tools/ and root/, which is the wrong place

Clock resolution is one tick
NEW — and the CPU-TIME CLOCK WAS DECAYING, which is a different thing and was a real bug rather than a stated limitation. clock_gettime(CLOCK_PROCESS_CPUTIME_ID) and times(2) both read run_ticks, and kernel/proc/sched_ule.c HALVES run_ticks every two seconds because it is a scheduling heuristic that must reflect recent behaviour. So the CPU time a process reported went DOWN, which times(2) is defined never to do, and a process that spun for 1.4 seconds with the machine to itself was charged 46% of it. One field was serving two purposes with incompatible lifetimes. process_t::cpu_ticks is the accounting total now and nothing decays it; run_ticks stays ULE's. Measured 46% before and 100% after
NEW — the way it presented is worth keeping: an INTERMITTENT verification failure that looked like flakiness, because the answer depended on where the two-second decay boundary fell inside the measurement. The check's floor was 50% and the value sat at 46-51%. A threshold set generously enough to tolerate an order-of-magnitude error is a threshold that turns a bug into noise; it is 80% now, against a measured 100%
CLOCK_PROCESS_CPUTIME_ID is per-thread, not per-group
F_GETLK/F_SETLK decline
MAX_PROCESSES 16, threads take slots
NEW — and KERNEL THREADS take slots too, out of the same sixteen. KTHREAD_MAX is 6, which is a ceiling rather than a reservation: it stops a leaking kernel thread from surfacing as "cannot fork" in a shell, and it does not stop six legitimate ones from leaving ten slots for everything else. The txg syncer, an ARC reclaimer and a taskqueue worker are three of the six before anything else asks
NEW — SLEEP(9) STILL IDLES FOR ANYTHING THAT IS NOT A KERNEL THREAD. ksleep_can_block() answers "am I a kernel thread", and the question it actually wants to ask is "am I in an interrupt handler" - an interrupt handler has nothing to deschedule, and every other context does. The difference is not academic: it is why taskqueue_drain has two loops, one waiting on a condvar and one yielding, and why the second one spins. Closing it means a per-context interrupt-depth count, which means bracketing interrupt_dispatch - and that function has several returns and calls return_to_user (which schedules) from inside them, so a global counter would be held across a context switch and read by the wrong thread. Not hard, but not a one-liner, and getting it wrong in that file produces no output at all
NEW — a kernel thread that does not yield or block hangs the machine, because the kernel is non-preemptive and kernel threads inherit that. Not a bug to fix - preempting kernel code in a kernel with no locking around the heap, the handle tables or the mount table is what turns that into corruption - but it is a real cost, it is in verif.c's PART B as a JUDGEMENT entry, and the failure it produces names nothing
NEW — this is also tracked in verification.c's PART B already; worth picking one home so the two don't drift apart
Readiness queue wakes every poller on every wake
NEW — this reads as an already-justified design tradeoff (documented at the time poll was built) rather than open debt; may belong under a "known tradeoffs" heading instead of Owed
Roadmap Part 1 status table still describes a 32-bit kernel (in ROADMAP-archive.md now)
NEW — NO MODULE UNLOAD PATH - DONE, and it was item 4's last remainder. kld_unload calls the module's exit functions, takes its drivers off the bus, checks that nothing else still points into the image, and unmaps it. module_exit is wired to a .genesis_modexit section, the mirror of .genesis_modinit, so a Linux driver's source is unchanged and its exit function is now actually called.

THE LEAK WAS NEVER THE DANGEROUS HALF. What kills the machine is a POINTER LEFT BEHIND: a module's driver_t, its interrupt handlers, its callout functions and its softc all live inside the image being unmapped, and the fault arrives later, in a timer tick, naming nothing. So kld_unload asks four registries - the newbus driver tables, the IRQ handler pool, the callout wheel, the taskqueue - "do you hold anything inside these bytes?", and REFUSES if any does. The question is an ADDRESS RANGE and not a list the module declares, because a declared list is one somebody forgets to add to and a missing entry is not a compile error, it is a stale pointer that faults later.

Honest limit: this is not a proof that no pointer survives. A module that stashed one somewhere none of those four walk passes every check and still faults. What holds is the useful half - a refused unload is certainly unsafe, an accepted one has been checked against everything this kernel can enumerate, and the list grows as the registries do.

THE CASCADE WAS bus.c, as expected: devclass_add_driver had no inverse, and a driver_t once registered was permanent. bus_unregister_range is that inverse. Three answers, all three checked in bus_selftest: a driver with a detach comes off cleanly; a detach that FORGETS a resource is reported and the resource reclaimed (checked by allocating the same bytes again, which a slot merely counted would refuse); a driver with no detach method, attached, is refused without changing anything. The first and third are a deliberate pair - a can_unregister that returned -1 for everything would pass the refusal test alone.

THE BUMP ALLOCATOR was the other half, and it is a page bitmap now. Sixteen uint64s, first fit, 1024 pages. A bump pointer that cannot rewind turns every unload into a permanent window leak. The assertion that catches a stub: unload hellokm, reload it, and require the SAME BASE ADDRESS - which an unload that cleared the table entry and forgot the bitmap fails and nothing else here would catch. KLD_MAX_MODULES is 16 rather than 8, and it means "at once" now rather than "ever".

NEW — a REFUSED unload can leave a module STOPPED BUT LOADED. Unavoidable rather than sloppy: the exit function is what releases the registrations, so it must run before the residual sweep is meaningful, and it cannot be un-run if the sweep then finds something. The module stays mapped, in the table, with its exit already called. Reported as the module bug it is. Linux avoids this with refcounts checked BEFORE exit, which needs every registration to take one - a change to every registry rather than to the loader

NEW — module .text is W+X - FIXED, see item 4. It was the one Owed-shaped entry that was a SECURITY property rather than a missing feature, and the oldest still open. Module images are off the heap and in their own mapped window: text read-execute, data no-execute, checked at boot by reading the page tables back
NEW — and the ACCESS MODE on a descriptor is enforced, which only started mattering when the volume became writable. Asserted in verif in both directions: writing through an O_RDONLY descriptor is refused, and writing through an O_WRONLY one works - so the refusal is the access mode and not a read-only filesystem. While nothing could write, that check would have passed for the wrong reason


The standing instruction
If it isn't verifiable at compile time, it's a test in verification.c. Not a note, not a checklist line, not something to remember at the prompt.
PART B is only for what ring 3 genuinely cannot reach:
BUILD — host tests, SYSCALL_TRACE=1 compiling
BOOT — boot-time output
HUMAN — needs someone at a keyboard
DESTRUCTIVE — would damage the live disk
JUDGEMENT — a real cost written down, not a suspected bug
Everything else is a check. Corollaries worth keeping:
A test that explains away its own zero isn't a test — measure a control
A negative-only test can't distinguish enforcement from blanket refusal — assert the positive case too
Test what the section header claims
A PART A check that passes once moves to systest and is deleted — verification.c is a queue, not a second suite


To know the file tree is the standard Linux tree with /wsr (Windows system repository) that stores all rewritten NT core components and the standard Windows file tree. Now, in root, there should be symlinks to every dir in wsr so there would be a /wsr/Windows/System32 and /Windows/System32. /wsr/Users and /Users should be symlinked to /home
NEW — with symlinks running both directions (/wsr/X ↔ /X), ns_lookup needs a cycle guard / max-redirect-depth or a path bouncing between the two roots resolves forever. The /dev \??\-vs-\Device\ merge already produced one real bug in exactly this class of double-view resolution


14. THE LONG-TERM GOAL, WRITTEN DOWN SO IT STOPS BEING IMPLICIT: a daily-drivable desktop running the Windows 7 DE, real Win32 binaries included. Nothing below this line is started. It is written as a dependency-ordered inventory, not as history the way the rest of this file is - there is no "DONE" to report yet on any of it, and no numbered sub-item here is safe from being renumbered, split, or dropped once work on it actually begins. It exists so the size of the gap is a fact everyone can see, not a feeling.

THE ARCHITECTURAL FORK THAT DECIDES EVERYTHING BELOW IT, and it has to be decided once, on purpose, before any of the GUI work starts: Windows Vista and later - including 7 - moved USER32/GDI32's actual implementation INTO THE KERNEL, as win32k.sys. A real, unmodified user32.dll/gdi32.dll from a real Windows 7 install does not talk to a subsystem server process for a window or a DC - it issues Nt/Zw syscalls straight into win32k.sys. Reusing those exact DLL files therefore means reimplementing win32k.sys's entire undocumented kernel-mode syscall surface bit-for-bit, which is exactly the part of Windows ReactOS has spent the most years on and still has the most open bugs in, because it is reverse-engineered rather than documented.

  THE TRACTABLE ALTERNATIVE, and the one worth committing to now rather than discovering the hard way later: write Genesis's OWN clean-room user32.dll/gdi32.dll/shell32.dll - the same strategy already proven at small scale for ntdll.dll and kernel32.dll (src/ntdll, src/kernel32) - implementing the DOCUMENTED Win32 export surface, backed by whatever primitives Genesis's own kernel actually has underneath. A third-party .exe that only calls documented Win32 APIs cannot tell the difference; it does not see win32k.sys, it sees exports named CreateWindowExW and BitBlt. This is ReactOS's actual strategy, not a shortcut invented here, and it is the only version of this goal that does not require reverse-engineering an entire undocumented kernel subsystem first. It also means "the Windows 7 DE" more precisely means "Genesis's own reimplementation of what the Windows 7 DE does," not the literal explorer.exe-plus-win32k.sys pair - a distinction worth being honest about up front.

(a) FULL MULTITHREADING, and it comes before anything graphical because everything graphical assumes it. Genesis is one-thread-per-process today (see the sig_handlers/sig_blocked split in process.h, whose own comment already names clone() as the thing that will need this). Needed: CreateThread/ExitThread, a real per-thread stack and TEB (not just the process-wide one), thread-local storage (the FS-segment TEB slot model NT uses), and thread-aware scheduling under the existing ULE port rather than beside it.

(b) DISPATCHER OBJECTS, completed. Event/Semaphore/Mutant already exist as object TYPES in the namespace (kernel/obj/) - what is missing is WaitForSingleObject/WaitForMultipleObjects actually blocking a THREAD (not a whole process) on them, and APCs (asynchronous procedure calls), which real Win32 I/O completion and thread termination both lean on.

(c) STRUCTURED EXCEPTION HANDLING, table-based, x64-style. Already named as deferred once (ROADMAP item 2's note on RtlUnwindEx). Not optional for real binaries: __try/__except is pervasive in real Windows system code, and KiUserExceptionDispatcher is how a real access violation or divide-by-zero becomes something a normal Win32 program's handler ever sees rather than an instant kernel-chosen death.

(d) NT MEMORY COMPLETENESS: VirtualAlloc/VirtualFree/VirtualProtect matching NT's page-state model (free/reserved/committed, not just POSIX-shaped mmap), Section objects for MapViewOfFile and shared memory - a first-class object type this kernel does not have yet - and the PEB filled in well enough that GetModuleHandle and friends work off it rather than off nothing.

(e) THE LOADER, completed rather than stubbed: LdrLoadDll/LdrGetProcedureAddress for real (ROADMAP item 2 already named moving this out of kernel context as owed), a real DLL search path, and enough of SxS/manifest handling that a real system DLL - which Windows 7 leans on for even its own C runtime - resolves instead of refusing to load.

(f) KERNEL32.DLL, actually completed. What exists (src/kernel32: err.c, file.c, proc.c - 571 lines) is proof of the approach, not the surface: full process/thread creation, VirtualAlloc family, memory-mapped files, Unicode/ANSI conversion (MultiByteToWideChar/WideCharToMultiByte - NT is UTF-16 native throughout, and nothing above this layer works without this), environment variables, time/locale, and the console API completing rather than stubbing.

(g) ADVAPI32.DLL: the registry (RegOpenKeyEx/RegQueryValueEx/RegSetValueEx and the hive format itself - HKLM/HKCU do not exist anywhere in this tree yet, and nearly every real Windows binary, not just the shell, reads the registry on startup), the Service Control Manager's CLIENT api, and LSA stubs. The security half of this DLL is the one place already half-built - kernel/fs/acl.c and ntsec.c's SID/ACL work is exactly what a real RegGetKeySecurity or a real SetNamedSecurityInfo would sit on top of.

(h) THE DISPLAY, de-scoped on purpose: not a WDDM driver, not a GPU driver - a linear framebuffer Genesis's own kernel exposes directly (VESA/VBE mode-set is enough), with GDI32 rendering into it in software. This is the same simplification this tree already made once for ZFS (keep the idea, drop the part that is expensive to get right) and it is a real Windows 7 mode, not an invented one: Windows 7's "Basic" theme is exactly classic, non-composited GDI rendering with no Aero/DWM required, which is the right target before any compositor is - if one ever is.

(i) GDI32.DLL: device contexts, bitmaps and blitting, line/rect/text primitives, and a font rasterizer (an open one like FreeType is a legitimate vendoring target the way LZ4 already is for ZFS - the font ENGINE is not Microsoft's proprietary part, the fonts themselves are a separate question for later).

(j) USER32.DLL: window classes, CreateWindowEx, the message queue and GetMessage/DispatchMessage/window procedures, real input plumbed from the keyboard driver (which exists) and A MOUSE DRIVER (which does not - nothing in this tree touches PS/2 or USB mouse input, or USB HID at all, and a GUI desktop without one is not drivable), timers, and enough of SetWindowsHookEx/menus/dialogs for ordinary apps to function.

(k) COM/OLE (combase, ole32): IUnknown, CoCreateInstance, IDispatch, the apartment threading model. Foundational to shell32 and to most non-trivial real applications, not an Explorer-only dependency.

(l) RPC (RPCRT4.dll): MSRPC, used as IPC between system services and by plenty of ordinary applications. A real protocol implementation, not a shim.

(m) COMCTL32.DLL: the common controls (buttons, listviews, treeviews, the taskbar's own building blocks) that Explorer and nearly every real Win32 GUI app is built from.

(n) THE SESSION/SERVICE ARCHITECTURE, radically simplifiable for a single-user daily-driver target rather than a real multi-user Windows install: a minimal smss.exe-equivalent bring-up, a minimal winlogon-equivalent that goes straight to a desktop session (auto-logon, no real credential UI needed for this goal), and the Service Control Manager only to the extent real system DLLs expect SOME services to be queryable, not a faithful reimplementation of every Windows 7 service.

(o) A NATIVE SHELL, and the honest fallback if shell32/explorer.exe's real dependency chain (registry-defined shell namespace, CLSIDs for "Computer"/"Documents", drag-drop, icon handlers) turns out to be its own multi-year project on top of everything above: a Genesis-native window manager and taskbar that is A desktop environment capable of hosting real Win32 apps, rather than literally explorer.exe. This is the same "keep the idea, not the exact implementation" move as (a) the display and (h) the write path already made.

(p) NTFS, at least read support, since real Windows 7 media and most Windows-authored software assume NTFS semantics (junctions, alternate data streams for some apps, NTFS ACLs) that FAT16/gnfs do not have. Lower priority than the GUI stack above it - a Windows 7 desktop could plausibly run its OWN system files off gnfs if step (o) is taken (a native shell needs no NTFS-specific behavior), deferring NTFS to "run an unmodified retail Windows 7 install" specifically.

(q) TCP, already tracked as item 6's own open piece and worth restating here: a "daily-drivable" desktop is an internet-connected one, and every dependency TCP has below it (in_pcb, the socket layer, the routing table, sleep, sysctl, callout, and now a descriptor to hang it on) already exists per item 6's own note - it is a next step there, not a new dependency invented by this item.

(r) USB, entirely absent from this tree today - no controller driver (UHCI/OHCI/EHCI/xHCI), no HID class driver, no mass storage class driver. Real modern peripherral hardware, the mouse in particular ((j) above), is USB far more often than PS/2 now, which makes this a practical dependency for "daily-drivable" on real hardware even though nothing else on this list requires it directly.

(s) THE HONEST EXCLUSIONS, stated rather than silently assumed away: the .NET Framework (an entire second runtime - the CLR - that a real share of Windows 7-era software depends on) and a modern web browser (nothing built from scratch here is remotely competitive with Chromium/WebKit's own scope) are both out of scope for this item. "Runs the Windows 7 DE" means the desktop, the shell, and ordinary native Win32 applications - not every binary that ever shipped for Windows 7.

WHERE THIS STANDS RELATIVE TO EVERYTHING ELSE IN THIS FILE: every other item here is a real, load-bearing piece of the eventual answer - the NT object manager, the PE loader, WDM driver loading, and this session's ACL work in particular are exactly the foundation (a), (b) and (g) build on. But (a) through (r) above are, collectively, larger than everything this tree has built so far, and ReactOS's own multi-decade timeline on almost exactly this problem is the honest comparison, not a discouraging one - it is evidence this is a real, hard, well-precedented problem rather than one this tree is failing to solve quickly.
