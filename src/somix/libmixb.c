/* libmixb.so - the dependency of libmixa.so (ROADMAP item 19, stage 3).
 *
 * Freestanding: no libc, so nothing here needs a thread pointer in %fs or
 * musl's startup - the two things a Windows process does not give a .so.
 * What it has is what a real library has: exported data another object
 * reaches through its GOT, and an exported function reached through a PLT. */

int b_counter;

int b_add(int a, int b) {
    return a + b + b_counter;
}
