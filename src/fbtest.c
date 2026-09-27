/* fbtest - the framebuffer and the mouse, from ring 3. ROADMAP item 14(h)
 * and the mouse half of 14(j).
 *
 * Freestanding and static, like systest and for the same reason: this checks
 * the kernel's interface, and a libc between the two would retry, translate
 * and hide exactly the answers worth checking.
 *
 *   /bin/fbtest        the suite. Draws a test picture into /dev/fb0 through
 *                      mmap, checks it back three ways (the mapping, read(2)
 *                      at an offset, a forked child's inherited mapping),
 *                      the refusals, then reads /dev/mouse0 while somebody
 *                      moves the mouse - tools/guest_run.py does that
 *                      through the QEMU monitor when it sees the
 *                      "MOUSE-WAIT" lines; at a real machine, follow the
 *                      prompt. Ends "fbtest: N passed, M failed".
 *   /bin/fbtest -i     interactive: a cursor follows the mouse until the
 *                      right button is pressed.
 *
 * The display is taken from the console with KDSETMODE(KD_GRAPHICS) for the
 * duration - so the console does not draw its own text over the picture -
 * and handed back with KD_TEXT, which makes the console repaint itself.
 * Everything printed meanwhile still goes to serial and into the console's
 * grid.
 */

typedef unsigned long  u64;
typedef long           i64;
typedef unsigned int   u32;
typedef int            i32;
typedef unsigned short u16;
typedef unsigned char  u8;

#define NULL ((void *)0)

static i64 sc6(i64 n, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f) {
    i64 ret;
    register i64 r10 __asm__("r10") = d;
    register i64 r8  __asm__("r8")  = e;
    register i64 r9  __asm__("r9")  = f;

    __asm__ volatile ("syscall"
                      : "=a"(ret)
                      : "a"(n), "D"(a), "S"(b), "d"(c),
                        "r"(r10), "r"(r8), "r"(r9)
                      : "rcx", "r11", "memory");
    return ret;
}

#define sc0(n)             sc6((n), 0, 0, 0, 0, 0, 0)
#define sc1(n,a)           sc6((n), (i64)(a), 0, 0, 0, 0, 0)
#define sc2(n,a,b)         sc6((n), (i64)(a), (i64)(b), 0, 0, 0, 0)
#define sc3(n,a,b,c)       sc6((n), (i64)(a), (i64)(b), (i64)(c), 0, 0, 0)
#define sc4(n,a,b,c,d)     sc6((n), (i64)(a), (i64)(b), (i64)(c), (i64)(d), 0, 0)
#define sc6a(n,a,b,c,d,e,f) sc6((n), (i64)(a), (i64)(b), (i64)(c), (i64)(d), (i64)(e), (i64)(f))

#define SYS_read          0
#define SYS_write         1
#define SYS_open          2
#define SYS_close         3
#define SYS_poll          7
#define SYS_lseek         8
#define SYS_mmap          9
#define SYS_munmap       11
#define SYS_rt_sigaction 13
#define SYS_ioctl        16
#define SYS_nanosleep    35
#define SYS_getpid       39
#define SYS_fork         57
#define SYS_exit         60
#define SYS_wait4        61
#define SYS_kill         62
#define SYS_exit_group  231

#define O_RDONLY 0
#define O_RDWR   2

#define PROT_READ   1
#define PROT_WRITE  2
#define MAP_SHARED  0x01
#define MAP_PRIVATE 0x02

#define EINTR   4
#define EACCES 13
#define ENODEV 19
#define EINVAL 22

#define POLLIN 1

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOGET_FSCREENINFO 0x4602
#define KDSETMODE   0x4B3A
#define KDGETMODE   0x4B3B
#define KD_TEXT     0
#define KD_GRAPHICS 1
#define TIOCGWINSZ  0x5413

#define EV_SYN 0
#define EV_KEY 1
#define EV_REL 2
#define REL_X 0
#define REL_Y 1
#define REL_WHEEL 8
#define BTN_LEFT  0x110
#define BTN_RIGHT 0x111

#define SIGUSR1 10
#define SIGKILL 9
#define SA_RESTORER 0x04000000UL

/* --- output ------------------------------------------------------------- */

static u64 slen(const char *s) {
    u64 n = 0;
    while (s[n]) n++;
    return n;
}

static void out(const char *s) {
    sc3(SYS_write, 1, s, slen(s));
}

static void out_i64(i64 v) {
    char buf[24];
    int  i = (int)sizeof(buf);
    int  neg = 0;
    u64  u;

    if (v < 0) { neg = 1; u = (u64)(-v); } else { u = (u64)v; }
    buf[--i] = '\0';
    do {
        buf[--i] = (char)('0' + (u % 10));
        u /= 10;
    } while (u != 0);
    if (neg) buf[--i] = '-';
    out(&buf[i]);
}

static int passes;
static int failures;

static void check(int cond, const char *what) {
    out(cond ? "  ok    " : "  FAIL  ");
    out(what);
    out("\n");
    if (cond) passes++; else failures++;
}

static void check_eq(i64 got, i64 want, const char *what) {
    if (got == want) {
        passes++;
        out("  ok    ");
        out(what);
        out("\n");
    } else {
        failures++;
        out("  FAIL  ");
        out(what);
        out("  (got ");
        out_i64(got);
        out(", wanted ");
        out_i64(want);
        out(")\n");
    }
}

static void section(const char *name) {
    out("\n");
    out(name);
    out("\n");
}

static int str_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void sleep_ms(u64 ms) {
    u64 ts[2];

    ts[0] = ms / 1000;
    ts[1] = (ms % 1000) * 1000000UL;
    sc2(SYS_nanosleep, ts, 0);
}

/* --- the framebuffer ----------------------------------------------------- */

static u32 xres, yres, bpp, pitch, smem_len;
static u32 rpos, gpos, bpos;
static volatile u32 *fbmem;
static int fbfd = -1;

static u32 rgb(u32 r, u32 g, u32 b) {
    return (r << rpos) | (g << gpos) | (b << bpos);
}

static volatile u32 *pixel(u32 x, u32 y) {
    return (volatile u32 *)((volatile u8 *)fbmem + (u64)y * pitch + x * 4);
}

static void fill_rect(u32 x0, u32 y0, u32 w, u32 h, u32 c) {
    u32 x, y;

    for (y = y0; y < y0 + h && y < yres; y++) {
        for (x = x0; x < x0 + w && x < xres; x++) {
            *pixel(x, y) = c;
        }
    }
}

/* The picture: a blue-to-black vertical gradient, eight colour bars across
 * the middle, and a white frame one pixel in from the edge. Every value is a
 * pure function of (x, y), so any pixel can be checked without a copy. */
static u32 picture(u32 x, u32 y) {
    static const u32 bars[8][3] = {
        {255, 255, 255}, {255, 255, 0}, {0, 255, 255}, {0, 255, 0},
        {255, 0, 255},   {255, 0, 0},   {0, 0, 255},   {0, 0, 0},
    };

    if (x == 1 || y == 1 || x == xres - 2 || y == yres - 2) {
        return rgb(255, 255, 255);
    }
    if (y >= yres / 3 && y < 2 * yres / 3) {
        u32 i = x * 8 / xres;
        return rgb(bars[i][0], bars[i][1], bars[i][2]);
    }
    return rgb(0, (x * 255) / xres / 4, 64 + (y * 191) / yres);
}

static void draw_picture(void) {
    u32 x, y;

    for (y = 0; y < yres; y++) {
        for (x = 0; x < xres; x++) {
            *pixel(x, y) = picture(x, y);
        }
    }
}

static int picture_intact(void) {
    u32 x, y;

    for (y = 0; y < yres; y += 7) {
        for (x = 0; x < xres; x += 5) {
            if (*pixel(x, y) != picture(x, y)) {
                return 0;
            }
        }
    }
    return 1;
}

static void test_fb(void) {
    u32 var[40];
    u8  fix[80];
    u16 ws[4];
    i64 r;

    section("framebuffer: /dev/fb0");

    fbfd = (int)sc2(SYS_open, "/dev/fb0", O_RDWR);
    check(fbfd >= 0, "open /dev/fb0");
    if (fbfd < 0) {
        return;
    }

    check_eq(sc3(SYS_ioctl, fbfd, FBIOGET_VSCREENINFO, var), 0,
             "FBIOGET_VSCREENINFO");
    xres = var[0]; yres = var[1]; bpp = var[6];
    rpos = var[8]; gpos = var[11]; bpos = var[14];
    check(xres >= 640 && yres >= 480, "a mode at least 640x480");
    check_eq(bpp, 32, "32 bits per pixel");
    check(var[9] == 8 && var[12] == 8 && var[15] == 8,
          "8-bit red, green and blue");
    check(rpos != gpos && gpos != bpos && rpos != bpos,
          "three distinct channel positions");

    check_eq(sc3(SYS_ioctl, fbfd, FBIOGET_FSCREENINFO, fix), 0,
             "FBIOGET_FSCREENINFO");
    smem_len = *(u32 *)(fix + 24);
    pitch    = *(u32 *)(fix + 48);
    check(str_eq((const char *)fix, "Genesis VBE"), "id says Genesis VBE");
    check_eq(*(u32 *)(fix + 36), 2, "visual is TRUECOLOR");
    check(pitch >= xres * 4, "line_length covers a scanline");
    check(smem_len >= pitch * yres, "smem_len covers the screen");
    check(*(u64 *)(fix + 16) != 0, "smem_start is the physical address");

    check_eq(sc3(SYS_ioctl, 0, TIOCGWINSZ, ws), 0, "TIOCGWINSZ on the console");
    check(ws[1] == xres / 8 && ws[0] == yres / 16,
          "the console grid is the framebuffer in 8x16 cells");

    /* The refusals, before anything is mapped. */
    {
        int ro = (int)sc2(SYS_open, "/dev/fb0", O_RDONLY);
        int nul = (int)sc2(SYS_open, "/dev/null", O_RDWR);

        check_eq(sc6a(SYS_mmap, 0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE,
                      fbfd, 0), -EINVAL, "mmap MAP_PRIVATE of a device is refused");
        check_eq(sc6a(SYS_mmap, 0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
                      fbfd, 100), -EINVAL, "an unaligned offset is refused");
        check_eq(sc6a(SYS_mmap, 0, 8192, PROT_READ | PROT_WRITE, MAP_SHARED,
                      fbfd, (i64)smem_len - 4096), -EINVAL,
                 "a mapping running past the end is refused");
        check_eq(sc6a(SYS_mmap, 0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
                      ro, 0), -EACCES,
                 "PROT_WRITE through a read-only descriptor is refused");
        check_eq(sc6a(SYS_mmap, 0, 4096, PROT_READ, MAP_SHARED, nul, 0),
                 -ENODEV, "a device with no mmap (/dev/null) is -ENODEV");
        sc1(SYS_close, ro);
        sc1(SYS_close, nul);
    }

    check_eq(sc3(SYS_ioctl, 0, KDSETMODE, KD_GRAPHICS), 0,
             "KDSETMODE KD_GRAPHICS takes the display");
    {
        int mode = -1;
        check(sc3(SYS_ioctl, 0, KDGETMODE, &mode) == 0 && mode == KD_GRAPHICS,
              "KDGETMODE says graphics");
    }

    r = sc6a(SYS_mmap, 0, smem_len, PROT_READ | PROT_WRITE, MAP_SHARED,
             fbfd, 0);
    check(r > 0 && (r & 0xFFF) == 0, "mmap MAP_SHARED of the whole framebuffer");
    if (r <= 0) {
        return;
    }
    fbmem = (volatile u32 *)r;

    draw_picture();
    check(picture_intact(), "the picture reads back through the mapping");

    /* read(2) at an offset returns what the mapping wrote: the mapping is
     * the device's memory, not a private page that happens to hold the same
     * bytes because this process put them there. */
    {
        u32 x = xres / 2 + 3, y = yres / 2 + 5, v = 0;
        i64 off = (i64)y * pitch + x * 4;

        *pixel(x, y) = rgb(12, 34, 56);
        check_eq(sc3(SYS_lseek, fbfd, off, 0), off, "lseek /dev/fb0 to a pixel");
        check_eq(sc3(SYS_read, fbfd, &v, 4), 4, "read one pixel");
        check_eq(v, rgb(12, 34, 56), "read(2) sees what the mapping wrote");

        v = rgb(200, 100, 50);
        sc3(SYS_lseek, fbfd, off, 0);
        check_eq(sc3(SYS_write, fbfd, &v, 4), 4, "write one pixel");
        check_eq(*pixel(x, y), rgb(200, 100, 50),
                 "the mapping sees what write(2) wrote");
        *pixel(x, y) = picture(x, y);
    }

    /* A second mapping, one page in: the same pixels at a different
     * address. */
    {
        i64 r2 = sc6a(SYS_mmap, 0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
                      fbfd, 4096);
        volatile u32 *second = (volatile u32 *)r2;

        check(r2 > 0, "a second mapping at offset 4096");
        if (r2 > 0) {
            check_eq(second[0], fbmem[1024],
                     "its first pixel is the first mapping's pixel 1024");
            second[1] = rgb(1, 2, 3);
            check_eq(fbmem[1025], rgb(1, 2, 3),
                     "a write through one shows through the other");
            fbmem[1025] = picture(1025 % (pitch / 4), 1025 / (pitch / 4));
            check_eq(sc2(SYS_munmap, r2, 4096), 0, "munmap the second");
            check(picture_intact(), "the first mapping survives it");
        }
    }

    /* fork: the child inherits the mapping SHARED - a copy-on-write fork of
     * video memory would give the child a private RAM copy nobody sees. */
    {
        i64 pid;
        int status = 0;

        *pixel(10, 10) = rgb(0, 0, 0);
        pid = sc0(SYS_fork);
        if (pid == 0) {
            int ok = *pixel(20, 20) == picture(20, 20) &&
                     *pixel(10, 10) == rgb(0, 0, 0);
            *pixel(10, 10) = rgb(250, 128, 7);
            sc1(SYS_exit, ok ? 0 : 1);
        }
        check(pid > 0, "fork with the framebuffer mapped");
        if (pid > 0) {
            sc4(SYS_wait4, pid, &status, 0, 0);
            check_eq((status >> 8) & 0xFF, 0,
                     "the child sees the parent's picture");
            check_eq(*pixel(10, 10), rgb(250, 128, 7),
                     "the parent sees the child's write (shared, not COW)");
        }
        *pixel(10, 10) = picture(10, 10);
    }
}

/* --- the mouse ------------------------------------------------------------ */

typedef struct {
    u64 sec, usec;
    u16 type, code;
    i32 value;
} input_event;

struct totals {
    i64 dx, dy, wheel;
    int left_down, left_up, right_down, right_up;
    int events, reports, bad_order;
    u64 last_sec, last_usec;
    int open_report;              /* events since the last SYN */
};

static i32 cur_x, cur_y;          /* the drawn cursor */

static void draw_cursor(u32 colour) {
    fill_rect((u32)cur_x, (u32)cur_y, 8, 8, colour);
}

static void account(struct totals *t, const input_event *e) {
    t->events++;
    if (e->sec < t->last_sec ||
        (e->sec == t->last_sec && e->usec < t->last_usec)) {
        t->bad_order++;
    }
    t->last_sec = e->sec;
    t->last_usec = e->usec;
    if (e->type == EV_SYN) {
        t->reports++;
        t->open_report = 0;
        return;
    }
    t->open_report++;
    if (e->type == EV_REL) {
        if (e->code == REL_X) t->dx += e->value;
        if (e->code == REL_Y) t->dy += e->value;
        if (e->code == REL_WHEEL) t->wheel += e->value;
    } else if (e->type == EV_KEY) {
        if (e->code == BTN_LEFT)  { if (e->value) t->left_down++;  else t->left_up++; }
        if (e->code == BTN_RIGHT) { if (e->value) t->right_down++; else t->right_up++; }
    }
}

/* Move the drawn cursor by what the events said: the "reads mouse events and
 * draws" loop every pointer-driven program is, in miniature. */
static void follow(const input_event *e) {
    if (e->type != EV_REL || fbmem == NULL) {
        return;
    }
    draw_cursor(picture((u32)cur_x, (u32)cur_y));
    if (e->code == REL_X) cur_x += e->value;
    if (e->code == REL_Y) cur_y += e->value;
    if (cur_x < 0) cur_x = 0;
    if (cur_y < 0) cur_y = 0;
    if (cur_x > (i32)xres - 8) cur_x = (i32)xres - 8;
    if (cur_y > (i32)yres - 8) cur_y = (i32)yres - 8;
    draw_cursor(rgb(255, 64, 0));
}

static int drain(int fd, struct totals *t) {
    input_event ev[16];
    i64 n = sc3(SYS_read, fd, ev, sizeof(ev));
    int i;

    if (n < 0) {
        return (int)n;
    }
    for (i = 0; i < (int)(n / (i64)sizeof(input_event)); i++) {
        account(t, &ev[i]);
        follow(&ev[i]);
    }
    return (int)n;
}

struct pollfd {
    i32   fd;
    short events;
    short revents;
};

static volatile int woke;
static void on_usr1(int sig) { (void)sig; woke = 1; }

__asm__(
".text\n"
".globl fbtest_restorer\n"
"fbtest_restorer:\n"
"    movq $15, %rax\n"
"    syscall\n"
);
extern void fbtest_restorer(void);

struct kernel_sigaction {
    u64 handler, flags, restorer, mask;
};

static void test_mouse(void) {
    struct totals t;
    struct pollfd pfd;
    int fd, i;
    u8 small[16];

    section("mouse: /dev/mouse0");

    fd = (int)sc2(SYS_open, "/dev/mouse0", O_RDONLY);
    check(fd >= 0, "open /dev/mouse0");
    if (fd < 0) {
        return;
    }
    check_eq(sc3(SYS_read, fd, small, sizeof(small)), -EINVAL,
             "a read shorter than one event is -EINVAL");

    pfd.fd = fd; pfd.events = POLLIN; pfd.revents = 0;
    check_eq(sc3(SYS_poll, &pfd, 1, 0), 0,
             "poll says not readable while nothing moves");

    for (i = 0; i < (int)sizeof(t); i++) ((u8 *)&t)[i] = 0;
    cur_x = (i32)xres / 2;
    cur_y = (i32)yres / 2;
    if (fbmem != NULL) draw_cursor(rgb(255, 64, 0));

    /* Phase 1: poll wakes for the events, and they add up. The harness
     * injects exactly this; a person at the machine does it by hand. */
    out("fbtest: MOUSE-WAIT-1 move right 7 and down 5, click left, "
        "wheel up one notch\n");
    {
        int rounds = 0;

        while (rounds++ < 40 &&
               !(t.left_up >= 1 && t.dx == 7 && t.dy == 5 && t.wheel >= 1)) {
            pfd.revents = 0;
            if (sc3(SYS_poll, &pfd, 1, 500) <= 0) {
                continue;
            }
            if (drain(fd, &t) <= 0) {
                break;
            }
        }
    }
    check(t.events > 0, "events arrived through poll + read");
    check_eq(t.dx, 7, "REL_X adds up to 7 (right)");
    check_eq(t.dy, 5, "REL_Y adds up to 5 (down is positive)");
    check_eq(t.wheel, 1, "REL_WHEEL adds up to 1 (away from the user)");
    check(t.left_down == 1 && t.left_up == 1, "BTN_LEFT went down, then up");
    check(t.open_report == 0, "every report ended with SYN_REPORT");
    check_eq(t.bad_order, 0, "timestamps never went backwards");
    if (fbmem != NULL) {
        check_eq(*pixel((u32)cur_x + 3, (u32)cur_y + 3), rgb(255, 64, 0),
                 "the cursor was drawn where the motion put it");
    }

    /* Phase 2: a read that BLOCKS until the mouse does something. A forked
     * watchdog sends SIGUSR1 after fifteen seconds, so a read that never
     * wakes fails with -EINTR instead of hanging the suite. */
    {
        struct kernel_sigaction sa;
        i64 me = sc0(SYS_getpid);
        i64 dog;
        int status = 0, got;

        sa.handler = (u64)&on_usr1;
        sa.flags = SA_RESTORER;
        sa.restorer = (u64)&fbtest_restorer;
        sa.mask = 0;
        sc4(SYS_rt_sigaction, SIGUSR1, &sa, 0, 8);

        dog = sc0(SYS_fork);
        if (dog == 0) {
            sleep_ms(15000);
            sc2(SYS_kill, me, SIGUSR1);
            sc1(SYS_exit, 0);
        }
        out("fbtest: MOUSE-WAIT-2 click right\n");
        t.right_down = t.right_up = 0;
        for (i = 0; i < 8 && t.right_up == 0 && !woke; i++) {
            got = drain(fd, &t);
            if (got < 0) {
                break;
            }
        }
        check(!woke, "a blocking read woke for the mouse (not the watchdog)");
        check(t.right_down == 1 && t.right_up == 1,
              "BTN_RIGHT went down, then up");
        sc2(SYS_kill, dog, SIGKILL);
        sc4(SYS_wait4, dog, &status, 0, 0);
    }
    sc1(SYS_close, fd);
}

/* --- interactive ----------------------------------------------------------- */

static void interactive(void) {
    struct totals t;
    int fd, i;

    fd = (int)sc2(SYS_open, "/dev/mouse0", O_RDONLY);
    if (fd < 0 || fbmem == NULL) {
        out("fbtest: needs /dev/fb0 and /dev/mouse0\n");
        return;
    }
    for (i = 0; i < (int)sizeof(t); i++) ((u8 *)&t)[i] = 0;
    cur_x = (i32)xres / 2;
    cur_y = (i32)yres / 2;
    draw_cursor(rgb(255, 64, 0));
    out("fbtest: move the mouse; the right button ends\n");
    while (t.right_down == 0) {
        if (drain(fd, &t) < 0) {
            break;
        }
        if (t.left_down) {
            fill_rect((u32)cur_x, (u32)cur_y, 8, 8, rgb(0, 255, 0));
        }
    }
    sc1(SYS_close, fd);
}

static void release_display(void) {
    int mode = -1;

    if (fbmem != NULL) {
        sc2(SYS_munmap, (i64)fbmem, smem_len);
    }
    check_eq(sc3(SYS_ioctl, 0, KDSETMODE, KD_TEXT), 0,
             "KDSETMODE KD_TEXT hands the display back");
    check(sc3(SYS_ioctl, 0, KDGETMODE, &mode) == 0 && mode == KD_TEXT,
          "KDGETMODE says text");
    check_eq(sc3(SYS_ioctl, 0, KDSETMODE, 7), -EINVAL,
             "KDSETMODE with a nonsense mode is -EINVAL");
}

void fbtest_main(u64 *sp) {
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);

    if (argc > 1 && str_eq(argv[1], "-i")) {
        test_fb();
        interactive();
        release_display();
        sc1(SYS_exit_group, 0);
    }

    out("fbtest: the framebuffer and the mouse, from ring 3\n");
    test_fb();
    test_mouse();
    section("handing the display back");
    release_display();

    out("\nfbtest: ");
    out_i64(passes);
    out(" passed, ");
    out_i64(failures);
    out(" failed\n");
    sc1(SYS_exit_group, failures > 255 ? 255 : failures);
}

/* The entry: argc/argv are on the stack the kernel built, so _start hands
 * the stack pointer to C before anything pushes. */
__asm__(
".text\n"
".globl _start\n"
"_start:\n"
"    movq %rsp, %rdi\n"
"    andq $-16, %rsp\n"
"    call fbtest_main\n"
"    hlt\n"
);
