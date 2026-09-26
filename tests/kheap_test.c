/* Host-side test harness for kernel/kheap.c.
 *
 * Runs the real allocator on a real Linux box so you can shake out
 * coalescing/overlap bugs in a second instead of in QEMU with no debugger.
 * It stubs out vmm_alloc_page() and mmaps the heap range at KHEAP_START so
 * the heap's virtual addresses are actually writable in the test process.
 *
 * Builds and runs BOTH 32-bit and 64-bit. That is the point: it's how the
 * portability of kheap.c is verified rather than asserted. Identical results
 * in both modes means the pointer-width abstraction actually holds.
 *
 * Built freestanding (-nostdlib) for two reasons: the kernel headers typedef
 * size_t, which fights libc, and 32-bit libc dev packages are a nuisance to
 * install. Syscalls go straight to the kernel.
 *
 *   32-bit:  gcc -m32 -ffreestanding -fno-pie -no-pie -fno-stack-protector \
 *                -nostdlib -Ikernel/include -o /tmp/kheap32 \
 *                tests/kheap_test.c kernel/kheap.c
 *   64-bit:  gcc      -ffreestanding -fno-pie -no-pie -fno-stack-protector \
 *                -nostdlib -Ikernel/include -o /tmp/kheap64 \
 *                tests/kheap_test.c kernel/kheap.c
 *
 * Exit status 0 = all checks passed.
 */

#include "kheap.h"
#include "paging.h"
#include "pmm.h"
#include "typesk.h"

typedef __SIZE_TYPE__ host_size;

/* ---- raw syscalls ------------------------------------------------------ */

#if defined(__x86_64__)

static long sys_write(int fd, const char *buf, host_size len) {
    long ret;
    __asm__ volatile ("syscall"
                      : "=a"(ret)
                      : "a"(1L), "D"((long)fd), "S"(buf), "d"(len)
                      : "rcx", "r11", "memory");
    return ret;
}

static void sys_exit(int code) {
    __asm__ volatile ("syscall" : : "a"(60L), "D"((long)code));
    for (;;) { }
}

static unsigned long sys_mmap_fixed(unsigned long addr, unsigned long len) {
    long ret;
    register long r10 __asm__("r10") = 0x32; /* MAP_PRIVATE|ANONYMOUS|FIXED */
    register long r8  __asm__("r8")  = -1;   /* fd */
    register long r9  __asm__("r9")  = 0;    /* offset */
    __asm__ volatile ("syscall"
                      : "=a"(ret)
                      : "a"(9L), "D"(addr), "S"(len), "d"(3L) /* PROT_READ|WRITE */,
                        "r"(r10), "r"(r8), "r"(r9)
                      : "rcx", "r11", "memory");
    return (unsigned long)ret;
}

#else /* i386 */

static long sys_write(int fd, const char *buf, host_size len) {
    long ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(4), "b"(fd), "c"(buf), "d"(len)
                      : "memory");
    return ret;
}

static void sys_exit(int code) {
    __asm__ volatile ("int $0x80" : : "a"(1), "b"(code));
    for (;;) { }
}

/* old_mmap(2), syscall 90: takes a pointer to its six args, which avoids
 * having to load %ebp the way mmap2 does. */
static unsigned long sys_mmap_fixed(unsigned long addr, unsigned long len) {
    unsigned long args[6];
    long ret;
    args[0] = addr;
    args[1] = len;
    args[2] = 0x3;   /* PROT_READ | PROT_WRITE */
    args[3] = 0x32;  /* MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED */
    args[4] = (unsigned long)-1;
    args[5] = 0;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(90), "b"(args)
                      : "memory");
    return (unsigned long)ret;
}

#endif

/* GCC can synthesise calls to these even under -ffreestanding. */
void *memset(void *dst, int c, host_size n) {
    unsigned char *d = dst;
    while (n--) { *d++ = (unsigned char)c; }
    return dst;
}
void *memcpy(void *dst, const void *src, host_size n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) { *d++ = *s++; }
    return dst;
}

/* ---- printing ---------------------------------------------------------- */

static host_size str_len(const char *s) {
    host_size n = 0;
    while (s[n]) { n++; }
    return n;
}

static void print(const char *s) {
    sys_write(1, s, str_len(s));
}

static void print_u(unsigned long v) {
    char buf[24];
    int i = 23;
    buf[23] = '\0';
    if (v == 0) {
        print("0");
        return;
    }
    while (v && i > 0) {
        buf[--i] = (char)('0' + (v % 10));
        v /= 10;
    }
    print(&buf[i]);
}

static void print_i(long v) {
    if (v < 0) { print("-"); print_u((unsigned long)(-v)); }
    else       { print_u((unsigned long)v); }
}

/* ---- VMM stub ---------------------------------------------------------- */

#define TEST_HEAP_BYTES KHEAP_MAX_SIZE

static unsigned long fake_frames_handed_out;
static unsigned long fake_frame_budget;   /* 0 == unlimited */

/* Signature must match paging.h exactly - uint32 in, uint32 out. That the
 * heap can still drive this from a 64-bit build is the point of the
 * kh_commit_page seam and the heap_fits_vmm_api assertion in kheap.c. */
phys_addr_t vmm_alloc_page(virt_addr_t virt_addr, uint32 flags) {
    (void)flags;
    if (virt_addr < KHEAP_START || virt_addr >= KHEAP_START + TEST_HEAP_BYTES) {
        return 0;
    }
    if (fake_frame_budget && fake_frames_handed_out >= fake_frame_budget) {
        return 0;  /* simulated out-of-memory */
    }
    fake_frames_handed_out++;
    return (phys_addr_t)0x100000 + virt_addr;  /* any nonzero "physical address" */
}

/* ---- test scaffolding -------------------------------------------------- */

static int failures;
static int checks;

static void ok(int condition, const char *what) {
    checks++;
    if (!condition) {
        failures++;
        print("  FAIL: ");
        print(what);
        print("\n");
    }
}

static void heap_ok(const char *where) {
    int rc = kheap_check();
    checks++;
    if (rc != 0) {
        failures++;
        print("  FAIL: kheap_check() = ");
        print_i(rc);
        print(" at ");
        print(where);
        print("\n");
    }
}

/* xorshift32 - deterministic, so a failing run reproduces exactly. */
static uint32 rng_state = 0x1234567u;
static uint32 rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* Every live allocation is filled with a byte pattern derived from its slot,
 * and verified before it is freed. This is what catches two allocations
 * overlapping - the classic split/coalesce off-by-one - which a test that
 * only checks for non-NULL pointers would sail straight past. */
static void fill(uint8 *p, kh_size len, uint8 seed) {
    kh_size i;
    for (i = 0; i < len; i++) {
        p[i] = (uint8)(seed + (uint8)i);
    }
}

static int verify(const uint8 *p, kh_size len, uint8 seed) {
    kh_size i;
    for (i = 0; i < len; i++) {
        if (p[i] != (uint8)(seed + (uint8)i)) {
            return 0;
        }
    }
    return 1;
}

/* ---- tests ------------------------------------------------------------- */

static void test_layout(void) {
    void *a, *b;
    kh_size align;

    print("layout and alignment invariants\n");
    kheap_init();

    /* Pointer width abstraction actually holds on this target. */
    ok(sizeof(kh_uptr) == sizeof(void *), "kh_uptr is pointer-sized");

    align = (kh_size)(2u * sizeof(void *));
    a = kmalloc(1);
    b = kmalloc(1);
    ok(a && b, "minimum-size allocations succeed");
    ok(((kh_uptr)a % align) == 0, "kmalloc honours natural alignment");
    ok(((kh_uptr)b % align) == 0, "second allocation aligned too");
    ok((kh_uptr)b > (kh_uptr)a, "distinct blocks");
    ok((kh_uptr)b - (kh_uptr)a >= 3u * align, "blocks respect minimum size");
    kfree(a);
    kfree(b);
    heap_ok("after layout probes");
}

static void test_basic(void) {
    void *a, *b, *c;
    kh_size used, freed, committed;

    print("basic alloc/free\n");
    kheap_init();
    heap_ok("after init");

    kheap_stats(&used, &freed, &committed);
    ok(used == 0, "fresh heap reports 0 used");
    ok(committed == KHEAP_INITIAL, "fresh heap committed KHEAP_INITIAL");

    a = kmalloc(16);
    b = kmalloc(16);
    c = kmalloc(16);
    ok(a && b && c, "three small allocations succeed");
    ok(a != b && b != c && a != c, "allocations are distinct");
    heap_ok("after 3 allocs");

    ok(kmalloc(0) == NULL, "kmalloc(0) returns NULL");

    kfree(b);
    heap_ok("after freeing middle block");
    kfree(a);
    heap_ok("after freeing first block (coalesce forward)");
    kfree(c);
    heap_ok("after freeing last block (coalesce both ways)");

    kheap_stats(&used, &freed, &committed);
    ok(used == 0, "everything freed reports 0 used");
    /* All three merged back into one span, so the free list should hold a
     * single block covering essentially the whole committed heap. */
    ok(freed >= committed - 64, "freed blocks coalesced back into one");
}

static void test_alignment(void) {
    void *p[8];
    int i;

    print("page-aligned allocations\n");
    kheap_init();

    for (i = 0; i < 8; i++) {
        /* Interleave a normal alloc so the aligned ones don't all happen to
         * land on an already-aligned boundary. */
        (void)kmalloc(24 + (kh_size)(rnd() % 100));
        p[i] = kmalloc_a(1000);
        ok(p[i] != NULL, "kmalloc_a succeeds");
        ok(((kh_uptr)p[i] & (PMM_PAGE_SIZE - 1u)) == 0, "kmalloc_a is page-aligned");
        if (p[i]) {
            fill(p[i], 1000, (uint8)i);
        }
        heap_ok("after kmalloc_a");
    }
    for (i = 0; i < 8; i++) {
        if (p[i]) {
            ok(verify(p[i], 1000, (uint8)i), "aligned block contents intact");
            kfree(p[i]);
        }
    }
    heap_ok("after freeing aligned blocks");
}

static void test_realloc(void) {
    uint8 *p;
    int i;

    print("krealloc\n");
    kheap_init();

    p = kmalloc(32);
    fill(p, 32, 0xAA);
    heap_ok("realloc setup");

    p = krealloc(p, 200);
    ok(p != NULL, "krealloc grow succeeds");
    ok(verify(p, 32, 0xAA), "krealloc grow preserves contents");
    heap_ok("after realloc grow");

    fill(p, 200, 0xBB);
    p = krealloc(p, 40);
    ok(p != NULL, "krealloc shrink succeeds");
    ok(verify(p, 40, 0xBB), "krealloc shrink preserves contents");
    heap_ok("after realloc shrink");

    ok(krealloc(NULL, 64) != NULL, "krealloc(NULL, n) allocates");
    ok(krealloc(p, 0) == NULL, "krealloc(p, 0) frees and returns NULL");
    heap_ok("after realloc frees");

    /* Repeated grow-in-place then move, the pattern a growing array hits. */
    p = kmalloc(8);
    for (i = 0; i < 200; i++) {
        fill(p, 8, 0x5A);
        p = krealloc(p, 8 + (kh_size)i * 4);
        if (!p) { break; }
        ok(verify(p, 8, 0x5A), "repeated realloc preserves prefix");
    }
    heap_ok("after repeated realloc");
    kfree(p);
}

static void test_corruption_refused(void) {
    uint8 *p;
    kh_size used_before, used_after;

    print("corruption handling\n");
    kheap_init();

    p = kmalloc(64);
    kfree(p);
    kheap_stats(&used_before, NULL, NULL);
    kfree(p);  /* double free */
    kheap_stats(&used_after, NULL, NULL);
    ok(used_before == used_after, "double free is refused, not applied");
    heap_ok("after double free");

    kfree(NULL);
    heap_ok("after kfree(NULL)");

    kfree((void *)0x100);              /* below the heap */
    kfree((void *)(kh_uptr)(KHEAP_START + KHEAP_MAX_SIZE + 0x1000)); /* above */
    heap_ok("after out-of-range frees");

    /* Stomp the byte just below the payload, the way an underrun would. That
     * lands in the header's size field on a little-endian target; on a
     * big-endian one it hits a different byte of the same field, which is why
     * this only asserts "detected", not which error code. kfree must refuse,
     * and undoing the damage must leave the heap clean again - proving the
     * refusal didn't quietly mangle anything on the way past. */
    p = kmalloc(64);
    {
        uint8 saved = p[-1];
        kheap_stats(&used_before, NULL, NULL);
        p[-1] = 0xFF;
        ok(kheap_check() != 0, "kheap_check spots a trashed header");
        kfree(p);
        kheap_stats(&used_after, NULL, NULL);
        ok(used_before == used_after, "kfree refuses a block with a trashed header");
        p[-1] = saved;
        heap_ok("after repairing the header");
        kfree(p);
        heap_ok("after freeing the repaired block");
    }

    /* Same again for an overrun past the end of the payload, which lands on
     * the footer instead of the header. */
    p = kmalloc(64);
    {
        uint8 *footer = p + 64;  /* payload is 64 bytes; footer follows */
        uint8 saved = *footer;
        kheap_stats(&used_before, NULL, NULL);
        *footer = (uint8)(saved ^ 0xFF);
        kfree(p);
        kheap_stats(&used_after, NULL, NULL);
        ok(used_before == used_after, "kfree refuses a block with a trashed footer");
        *footer = saved;
        heap_ok("after repairing the footer");
        kfree(p);
    }
}

static void test_exhaustion(void) {
    void *blocks[512];
    int n = 0, i;

    print("exhaustion and recovery\n");
    kheap_init();
    fake_frames_handed_out = 0;
    fake_frame_budget = 40;  /* only 40 pages of "physical memory" */

    for (i = 0; i < 512; i++) {
        blocks[i] = kmalloc(4096);
        if (!blocks[i]) { break; }
        n++;
    }
    ok(n > 0, "some allocations succeed before OOM");
    ok(n < 512, "allocator eventually reports OOM instead of lying");
    heap_ok("at OOM");

    for (i = 0; i < n; i++) {
        kfree(blocks[i]);
    }
    heap_ok("after freeing everything at OOM");

    /* After a full free the heap should be reusable for a big block again. */
    blocks[0] = kmalloc(4096);
    ok(blocks[0] != NULL, "heap is reusable after full free");
    kfree(blocks[0]);

    fake_frame_budget = 0;
}

#define SLOTS 256

static void test_random(void) {
    struct { uint8 *p; kh_size len; uint8 seed; } live[SLOTS];
    uint32 op;
    int i;

    print("randomised stress (40000 ops)\n");
    kheap_init();
    fake_frames_handed_out = 0;
    fake_frame_budget = 0;

    for (i = 0; i < SLOTS; i++) {
        live[i].p = NULL;
        live[i].len = 0;
        live[i].seed = 0;
    }

    for (op = 0; op < 40000; op++) {
        uint32 slot = rnd() % SLOTS;
        uint32 action = rnd() % 100;

        if (live[slot].p) {
            /* Always verify before touching an existing allocation. */
            if (!verify(live[slot].p, live[slot].len, live[slot].seed)) {
                failures++;
                checks++;
                print("  FAIL: allocation corrupted at op ");
                print_u(op);
                print("\n");
                return;
            }
            if (action < 60) {
                kfree(live[slot].p);
                live[slot].p = NULL;
            } else if (action < 75) {
                kh_size newlen = 1 + (rnd() % 3000);
                uint8 *np = krealloc(live[slot].p, newlen);
                if (np) {
                    kh_size keep = live[slot].len < newlen ? live[slot].len : newlen;
                    if (!verify(np, keep, live[slot].seed)) {
                        failures++;
                        checks++;
                        print("  FAIL: krealloc lost data at op ");
                        print_u(op);
                        print("\n");
                        return;
                    }
                    live[slot].p = np;
                    live[slot].len = newlen;
                    live[slot].seed = (uint8)rnd();
                    fill(np, newlen, live[slot].seed);
                }
            }
        } else {
            /* Mostly small allocations with an occasional large one, which is
             * roughly what a kernel actually does, and mixes in the aligned
             * path so its splits get stressed too. */
            kh_size len = (rnd() % 20 == 0) ? (1 + rnd() % 60000)
                                            : (1 + rnd() % 256);
            uint8 *p = (rnd() % 16 == 0) ? kmalloc_a(len) : kmalloc(len);
            if (p) {
                live[slot].p = p;
                live[slot].len = len;
                live[slot].seed = (uint8)rnd();
                fill(p, len, live[slot].seed);
            }
        }

        if ((op % 500) == 0) {
            int rc = kheap_check();
            if (rc != 0) {
                failures++;
                checks++;
                print("  FAIL: kheap_check() = ");
                print_i(rc);
                print(" at op ");
                print_u(op);
                print("\n");
                return;
            }
        }
    }

    for (i = 0; i < SLOTS; i++) {
        if (live[i].p) {
            ok(verify(live[i].p, live[i].len, live[i].seed),
               "final contents intact");
            kfree(live[i].p);
        }
    }
    heap_ok("after draining random test");

    {
        kh_size used;
        kheap_stats(&used, NULL, NULL);
        ok(used == 0, "all bytes accounted for after draining");
    }
}

void _start(void) {
    unsigned long mapped = sys_mmap_fixed((unsigned long)KHEAP_START,
                                          (unsigned long)TEST_HEAP_BYTES);
    print(sizeof(void *) == 8 ? "== 64-bit build ==\n" : "== 32-bit build ==\n");

    if (mapped != (unsigned long)KHEAP_START) {
        print("mmap of the test heap failed\n");
        sys_exit(2);
    }

    test_layout();
    test_basic();
    test_alignment();
    test_realloc();
    test_corruption_refused();
    test_exhaustion();
    test_random();

    print("\n");
    print_i(checks - failures);
    print("/");
    print_i(checks);
    print(" checks passed\n");

    if (failures) {
        print("FAILED\n");
        sys_exit(1);
    }
    print("OK\n");
    sys_exit(0);
}
