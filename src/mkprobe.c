/* Create a directory tree and LEAVE it, so the host can check the on-disk
 * result with mtools and fsck.fat.
 *
 * This is the verification that matters for a filesystem writer: systest
 * proves Genesis agrees with itself, which a driver that wrote a consistent
 * but non-standard layout would also pass. Agreeing with an INDEPENDENT
 * implementation of FAT16 is the part that says the bytes are right. */
typedef unsigned long u64;
typedef long          i64;

static i64 sc3(i64 n, i64 a, i64 b, i64 c) {
    i64 r;
    __asm__ volatile ("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c)
                      : "rcx", "r11", "memory");
    return r;
}

#define SYS_write  1
#define SYS_mkdir 83
#define SYS_rmdir 84
#define SYS_rename 82
#define SYS_unlink 87
#define SYS_exit  60

static void say(const char *s) {
    u64 n = 0;
    while (s[n]) n++;
    sc3(SYS_write, 1, (i64)s, (i64)n);
}

void _start(void) {
    /* Idempotent: a re-run must not fail because the last one succeeded. */
    sc3(SYS_rmdir, (i64)"/probe/deep", 0, 0);
    sc3(SYS_rmdir, (i64)"/probe", 0, 0);
    /* /probe2/deep BEFORE /probe2 - the first run leaves a nested tree, and
     * rmdir correctly refuses a non-empty directory. Getting this order
     * wrong is what made an earlier run report "rename FAILED", which was
     * rename doing exactly the right thing. */
    sc3(SYS_rmdir, (i64)"/probe2/deep", 0, 0);
    sc3(SYS_rmdir, (i64)"/probe2", 0, 0);

    say(sc3(SYS_mkdir, (i64)"/probe", 0755, 0) == 0
        ? "mkprobe: mkdir /probe ok\n" : "mkprobe: mkdir /probe FAILED\n");
    say(sc3(SYS_mkdir, (i64)"/probe/deep", 0755, 0) == 0
        ? "mkprobe: mkdir /probe/deep ok\n" : "mkprobe: mkdir deep FAILED\n");
    say(sc3(SYS_rename, (i64)"/probe", (i64)"/probe2", 0) == 0
        ? "mkprobe: rename /probe -> /probe2 ok\n"
        : "mkprobe: rename FAILED\n");
    /* And a FILE, to exercise fat_free_chain - a 12KB file spans six
     * 2048-byte clusters, so a chain walk that stops early or frees the
     * wrong link shows up as a cluster count that does not drop by six. */
    say(sc3(SYS_unlink, (i64)"/victim.bin", 0, 0) == 0
        ? "mkprobe: unlink /victim.bin ok\n" : "mkprobe: unlink FAILED\n");
    say("mkprobe: left /probe2/deep on the volume for the host to check\n");
    sc3(SYS_exit, 0, 0, 0);
}
