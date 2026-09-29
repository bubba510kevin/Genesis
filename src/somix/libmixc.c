/* libmixc.so - a Linux shared object that USES libc, loaded into a Windows
 * (PE) process (ROADMAP item 19, the GNTlibc step).
 *
 * Unlike libmixa/libmixb, which are freestanding, this one is an ordinary
 * musl-linked .so: it calls malloc, snprintf, strtol, strlen, memcpy and the
 * math library, and it reads errno. None of that works in a PE process until
 * libc.so (GNTlibc) has installed a thread pointer - which its constructor
 * does, before this object's own constructors run. So the mere fact that
 * these functions return the right answers is the proof that GNTlibc works.
 *
 * Everything here returns a value the caller can check rather than printing,
 * so the test does not depend on where fd 1 is wired in a PE process.
 *
 * Built by src/somix/build.sh against GNTlibc's libc.so and headers.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <errno.h>

/* malloc + string: duplicate, measure, free. Returns the length, or -1. */
int c_dup_len(const char *s)
{
	size_t n = strlen(s);
	char *p = malloc(n + 1);
	int r;

	if (!p)
		return -1;
	memcpy(p, s, n + 1);
	r = (p[n] == '\0') ? (int)strlen(p) : -1;
	free(p);
	return r;
}

/* strtol over a comma list, summed. malloc'd scratch on the way. */
long c_csv_sum(const char *csv)
{
	size_t n = strlen(csv);
	char *buf = malloc(n + 1);
	long sum = 0;
	char *p;

	if (!buf)
		return -1;
	memcpy(buf, csv, n + 1);
	p = buf;
	while (*p) {
		char *end;
		long v = strtol(p, &end, 10);
		if (end == p)
			break;
		sum += v;
		p = end;
		if (*p == ',')
			p++;
	}
	free(buf);
	return sum;
}

/* stdio formatting into a caller buffer: returns the length snprintf reports. */
int c_format(char *out, int cap, int a, int b)
{
	return snprintf(out, (size_t)cap, "%d+%d=%d", a, b, a + b);
}

/* libm: a single double in, a double out - one leading float argument and a
 * float return, which the Win64<->SysV adapter carries in xmm0 either way. */
double c_hypot(double x, double y)
{
	return sqrt(x * x + y * y);
}

/* errno lives in the thread-pointer block. A failed libc call must set it,
 * which only works once the TCB exists. Returns 1 if errno behaves. */
int c_errno_ok(void)
{
	FILE *f;

	errno = 0;
	f = fopen("/genesis/definitely/no/such/path", "r");
	if (f) {
		fclose(f);
		return 0;
	}
	return errno == ENOENT ? 1 : 2;
}

/* A constructor that runs libc code: it must already have a thread pointer,
 * since libc.so's constructor ran first. Stores a malloc'd result c_ctor_val
 * reads back, proving allocation works from within .init_array. */
static int ctor_val;

__attribute__((constructor))
static void c_ctor(void)
{
	int *p = malloc(sizeof *p);
	if (p) {
		*p = 1234;
		ctor_val = *p;
		free(p);
	}
}

int c_ctor_val(void)
{
	return ctor_val;
}
