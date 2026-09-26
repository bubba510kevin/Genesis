/* Host tests for pipe.c.
 *
 * The blocking paths cannot run here - waitq_wait halts the CPU waiting for
 * an interrupt a host process will never receive - so every test below stays
 * on a path where the condition already holds: data present, room available,
 * or an end already closed. That still covers the parts that are expensive to
 * debug on target:
 *
 *   - the ring wrapping, which is wrong in a way that only shows up after the
 *     buffer has been filled and drained once
 *   - a short read returning what is there rather than what was asked for
 *   - end of file, which is a specific value (0) and not an error
 *   - -EPIPE and the SIGPIPE that goes with it
 *
 * The last two are the reason the two ends are separate objects, so they are
 * the ones worth pinning down.
 */

#include <stdio.h>

#include "object.h"
#include "pipe.h"
#include "typesk.h"

static int pipe_failures;

static void check(int cond, const char *what) {
    if (cond) {
        printf("  ok    %s\n", what);
    } else {
        printf("  FAIL  %s\n", what);
        pipe_failures++;
    }
}

static void check_eq(long got, long want, const char *what) {
    if (got == want) {
        printf("  ok    %s\n", what);
    } else {
        printf("  FAIL  %s  (got %ld, wanted %ld)\n", what, got, want);
        pipe_failures++;
    }
}

/* pipe.c calls signal_send when a write finds no reader. kbd_test.c already
 * supplies signal_send_group and signal_pending; this is the one-process
 * form, recorded so the -EPIPE test can assert the signal went with it. */
int  pipe_sigs;
int  pipe_last_sig;

static int64 pwrite(object_t *w, const char *s, uint64 n) {
    uint64 pos = 0;
    return w->type->write(w, s, n, &pos);
}

static int64 pread_(object_t *r, char *buf, uint64 n) {
    uint64 pos = 0;
    return r->type->read(r, buf, n, &pos);
}

static void test_roundtrip(void) {
    object_t *r = NULL, *w = NULL;
    char      buf[64];
    int       i;

    printf("\npipe: bytes in, same bytes out\n");

    check_eq(pipe_create(&r, &w), 0, "a pipe is created");
    if (r == NULL || w == NULL) {
        return;
    }
    check(r != w, "the two ends are separate objects");
    check(pipe_is_pipe(r) && pipe_is_pipe(w), "and both report as pipes");
    check(r->type->write == NULL, "the read end cannot be written");
    check(w->type->read == NULL, "and the write end cannot be read");

    check_eq((long)pwrite(w, "hello", 5), 5, "a write reports every byte");
    check_eq((long)pread_(r, buf, sizeof(buf)), 5,
             "and a read returns what is there, not what was asked for");
    buf[5] = '\0';
    check(buf[0] == 'h' && buf[4] == 'o', "with the bytes intact");

    /* Two writes, one read: a pipe is a byte stream and has no idea where one
     * write ended and the next began. */
    pwrite(w, "abc", 3);
    pwrite(w, "def", 3);
    check_eq((long)pread_(r, buf, sizeof(buf)), 6,
             "two writes come back as one run of bytes");
    for (i = 0; i < 6; i++) {
        if (buf[i] != "abcdef"[i]) break;
    }
    check(i == 6, "in order and unmangled");

    ob_deref(r);
    ob_deref(w);
}

/* The wrap. Fill the ring most of the way, drain it, then push through again
 * so head and tail both cross zero - which is the case a modulo that is right
 * for the first pass and wrong afterwards will fail. */
static void test_wrap(void) {
    object_t *r = NULL, *w = NULL;
    static char out[8192];
    static char in[8192];
    long        n;
    int         i, mismatch = -1;
    int         round;

    printf("\npipe: the ring wraps\n");

    if (pipe_create(&r, &w) != 0) {
        check(0, "a pipe is created");
        return;
    }
    for (i = 0; i < 4096; i++) {
        out[i] = (char)(i * 7 + 3);
    }

    for (round = 0; round < 3; round++) {
        long wrote = (long)pwrite(w, out, 3000);
        long total = 0;

        if (wrote != 3000) {
            check(0, "a 3000-byte write fits in an empty pipe");
            break;
        }
        while (total < 3000) {
            n = (long)pread_(r, in + total, 512);
            if (n <= 0) break;
            total += n;
        }
        if (total != 3000) {
            check(0, "and reads back in full");
            break;
        }
        for (i = 0; i < 3000; i++) {
            if (in[i] != out[i]) { mismatch = i; break; }
        }
        if (mismatch >= 0) break;
    }
    check(round == 3, "three fill-and-drain rounds complete");
    check_eq(mismatch, -1, "and every byte survives the wrap in the right place");

    /* Exactly PIPE_BUF: the boundary between "fits whole" and "blocks". A
     * count field rather than head/tail alone is what lets the last byte in. */
    check_eq((long)pwrite(w, out, 4096), 4096,
             "a write of exactly PIPE_BUF fits - full is not one short of it");
    check_eq((long)pread_(r, in, 4096), 4096, "and reads back whole");

    ob_deref(r);
    ob_deref(w);
}

static void test_eof(void) {
    object_t *r = NULL, *w = NULL;
    char      buf[32];

    printf("\npipe: the last write end closing is end of file\n");

    if (pipe_create(&r, &w) != 0) {
        check(0, "a pipe is created");
        return;
    }
    pwrite(w, "tail", 4);

    /* Closing the writer must not discard what it already sent. A shell
     * pipeline where the producer exits before the consumer runs depends on
     * this entirely. */
    ob_deref(w);
    check_eq((long)pread_(r, buf, sizeof(buf)), 4,
             "data written before the close is still readable after it");
    check_eq((long)pread_(r, buf, sizeof(buf)), 0,
             "and the next read is 0 - end of file, not an error");
    check_eq((long)pread_(r, buf, sizeof(buf)), 0, "and stays that way");
    ob_deref(r);
}

static void test_epipe(void) {
    object_t *r = NULL, *w = NULL;

    printf("\npipe: the last read end closing is -EPIPE\n");

    if (pipe_create(&r, &w) != 0) {
        check(0, "a pipe is created");
        return;
    }
    pipe_sigs = 0;
    ob_deref(r);

    check_eq((long)pwrite(w, "nobody home", 11), -32,
             "a write with no reader left is -EPIPE");
    check_eq(pipe_sigs, 1, "and it raised exactly one signal");
    check_eq(pipe_last_sig, 13, "which was SIGPIPE");

    /* Every subsequent write, not just the first. */
    pipe_sigs = 0;
    check_eq((long)pwrite(w, "still nobody", 12), -32, "and so is the next one");
    check_eq(pipe_sigs, 1, "with its own signal");
    ob_deref(w);
}

/* The pool is finite and exhaustion has to be an errno rather than a crash.
 * MAX_PIPES is 8; ask for more and the ninth must fail cleanly and the pool
 * must come back when the ends are closed. */
static void test_exhaustion(void) {
    object_t *r[16], *w[16];
    int       i, made = 0;

    printf("\npipe: a finite pool fails cleanly\n");

    for (i = 0; i < 16; i++) {
        r[i] = NULL;
        w[i] = NULL;
        if (pipe_create(&r[i], &w[i]) != 0) break;
        made++;
    }
    check(made > 0 && made < 16, "the pool runs out at a specific limit");
    check_eq(pipe_create(&r[15], &w[15]), -23, "and one more is -ENFILE");

    for (i = 0; i < made; i++) {
        ob_deref(r[i]);
        ob_deref(w[i]);
    }
    check_eq(pipe_create(&r[0], &w[0]), 0,
             "closing both ends returns the pipe to the pool");
    ob_deref(r[0]);
    ob_deref(w[0]);

    /* Closing only one end must NOT return it: the other end is still usable
     * and reusing the buffer underneath it would hand one process another's
     * bytes. With one pipe half-closed, exactly one fewer than the pool holds
     * should be available. */
    if (pipe_create(&r[0], &w[0]) == 0) {
        int held = 0;

        ob_deref(w[0]);                  /* write end gone, read end open */
        for (i = 1; i < 16; i++) {
            r[i] = NULL;
            w[i] = NULL;
            if (pipe_create(&r[i], &w[i]) != 0) break;
            held++;
        }
        check_eq(held, made - 1, "a half-closed pipe still holds its slot");
        for (i = 1; i <= held; i++) {
            ob_deref(r[i]);
            ob_deref(w[i]);
        }
        ob_deref(r[0]);
    }
}

int pipe_run_tests(void) {
    pipe_failures = 0;
    test_roundtrip();
    test_wrap();
    test_eof();
    test_epipe();
    test_exhaustion();
    return pipe_failures;
}
