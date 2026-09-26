#ifndef _SYS_RANDOM_H_
#define _SYS_RANDOM_H_

/* Entropy harvesting, stubbed.
 *
 * FreeBSD feeds the arrival timing of network packets, disk interrupts and
 * keystrokes into its entropy pool. Genesis has no entropy pool, so there is
 * nothing to feed and these compile away.
 *
 * Worth being explicit rather than quiet about it, because this is a
 * SECURITY-relevant absence and not merely a missing feature: anything here
 * that ever needs unpredictable numbers - a TCP initial sequence number, an
 * IP fragment identifier - currently has no good source for them. The
 * network code vendored so far does not, but TCP would. */
#define RANDOM_NET_ETHER  4
#define RANDOM_UMA        5
#define RANDOM_PURE_OCTEON 6
#define random_harvest_queue(e, s, o)        do { } while (0)
#define random_harvest_queue_ether(e, s)     do { } while (0)
#define random_harvest_fast(e, s)            do { } while (0)
#define random_harvest_fast_uma(e, s, o)     do { } while (0)
#define random_harvest_direct(e, s, o)       do { } while (0)

#endif
