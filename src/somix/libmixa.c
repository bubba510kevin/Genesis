/* libmixa.so - a Linux shared object for Windows programs to load (ROADMAP
 * item 19, stage 3). src/winmix/mix.c loads it with LoadLibrary and checks
 * each export.
 *
 * Freestanding (see libmixb.c), and built to need every relocation a
 * shared object ordinarily does:
 *
 *   R_X86_64_RELATIVE    names[] - pointers to its own strings
 *   R_X86_64_64          a_fp - a data word holding b_add's address
 *   R_X86_64_GLOB_DAT    b_counter, reached through the GOT
 *   R_X86_64_JUMP_SLOT   the call to b_add through the PLT
 *
 * plus a DT_NEEDED on libmixb.so and a constructor in .init_array. Its
 * system calls are raw Linux ones, made from inside a Windows process -
 * which is what stage 1's per-call routing is for. */

extern int b_counter;
extern int b_add(int a, int b);

static int ctor_ran;

__attribute__((constructor))
static void init(void) {
    ctor_ran = 42;
}

int a_ctor(void) {
    return ctor_ran;
}

int a_add(int x, int y) {
    return b_add(x, y);                  /* through the PLT into libmixb */
}

int a_bump(void) {
    return ++b_counter;                  /* libmixb's data, through the GOT */
}

static const char *const names[] = { "zero", "one", "two" };

const char *a_name(int i) {
    return names[i];
}

int (*a_fp)(int, int) = b_add;

int a_call_fp(int x) {
    return a_fp(x, 1);
}

long a_sum14(long a, long b, long c, long d, long e, long f, long g,
             long h, long i, long j, long k, long l, long m, long n) {
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h +
           9 * i + 10 * j + 11 * k + 12 * l + 13 * m + 14 * n;
}

double a_hyp(double x, double y) {
    return x * x + y * y;
}

double a_mixed(int k, double x) {
    return (double)k * x;
}

/* A callback INTO the Windows program: a System V function pointer. */
int a_callback(int (*cb)(int), int v) {
    return cb(v) * 2;
}

static long lsys3(long n, long a, long b, long c) {
    long r;

    __asm__ volatile ("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c)
                      : "rcx", "r11", "memory");
    return r;
}

long a_getpid(void) {
    return lsys3(39, 0, 0, 0);
}

long a_write(const char *s) {
    long n = 0;

    while (s[n] != '\0') {
        n++;
    }
    return lsys3(1, 1, (long)s, n);
}

/* Clobbers every register Win64 expects preserved and System V does not
 * (RSI, RDI, XMM6-XMM15), so a caller checking them after the call can
 * tell whether the adapter saved them. */
long a_clobber(void) {
    __asm__ volatile ("xor %%esi, %%esi\n\txor %%edi, %%edi\n\t"
                      "pxor %%xmm6, %%xmm6\n\tpxor %%xmm7, %%xmm7\n\t"
                      "pxor %%xmm8, %%xmm8\n\tpxor %%xmm9, %%xmm9\n\t"
                      "pxor %%xmm10, %%xmm10\n\tpxor %%xmm11, %%xmm11\n\t"
                      "pxor %%xmm12, %%xmm12\n\tpxor %%xmm13, %%xmm13\n\t"
                      "pxor %%xmm14, %%xmm14\n\tpxor %%xmm15, %%xmm15"
                      ::: "rsi", "rdi", "xmm6", "xmm7", "xmm8", "xmm9",
                          "xmm10", "xmm11", "xmm12", "xmm13", "xmm14",
                          "xmm15");
    return 7;
}
