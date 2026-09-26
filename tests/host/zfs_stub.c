/* The two kernel facilities the ZFS path reaches that the main harness does
 * not already provide.
 *
 * The harness already has print_string, print_hex, print_hex64 and
 * clear_screen (vmm_test.c), print_char (kbd_test.c), and kmalloc/kfree
 * (fat_test.c). What it has never needed is the two heap calls the vendored
 * reader uses and FAT never did.
 *
 * Only what is MISSING, rather than a second set: two definitions of kmalloc
 * in one link is a link error, and the version that would win if it were not
 * is not the one anyone chose. */

#include <stdio.h>
#include <stdlib.h>

#include "typesk.h"
#include "kheap.h"

void *kcalloc(kh_size count, kh_size size) {
    return calloc((size_t)count, (size_t)size);
}

void *krealloc(void *ptr, kh_size size) {
    return realloc(ptr, (size_t)size);
}
