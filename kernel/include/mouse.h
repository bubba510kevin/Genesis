#ifndef MOUSE_H
#define MOUSE_H

#include "typesk.h"

/* Pointer input - the device-independent half. ROADMAP item 14(j).
 *
 * A hardware driver (kernel/dev/psm.c today; a USB HID driver later, item
 * 14(r)) decodes whatever its hardware sends into ONE call per report:
 *
 *     mouse_report(dx, dy, wheel, buttons)
 *
 * and this file turns that into events, queues them, and serves them. Two
 * consumers:
 *
 *   user space   /dev/mouse0. read(2) returns whole Linux `struct
 *                input_event` records (24 bytes: a timeval, type, code,
 *                value) - evdev's format, so the reader needs no new
 *                vocabulary: EV_REL/REL_X,REL_Y,REL_WHEEL for motion,
 *                EV_KEY/BTN_LEFT,BTN_RIGHT,BTN_MIDDLE for buttons (1 down,
 *                0 up), and EV_SYN/SYN_REPORT closing each report. read
 *                blocks until there is at least one event; poll(2) says when
 *                there is.
 *   the kernel   mouse_take(), for the boot selftest now and for whatever
 *                window system turns motion into WM_MOUSEMOVE later.
 *
 * Conventions, fixed here once so every driver agrees: dx is positive to the
 * RIGHT, dy positive DOWN (screen coordinates, as evdev has it - PS/2 sends
 * up-positive and psm.c flips it), wheel positive AWAY from the user (a
 * scroll up). Buttons are a bitmask of MOUSE_BUTTON_*; only CHANGES become
 * events, so a report that moves nothing and presses nothing new produces
 * none.
 *
 * One queue, shared by every open of /dev/mouse0. A second reader steals
 * events from the first. That is a real limitation, and the right fix is a
 * per-open queue once there are two readers to serve - the window system
 * will be the only one.
 */

#define MOUSE_BUTTON_LEFT    0x1
#define MOUSE_BUTTON_RIGHT   0x2
#define MOUSE_BUTTON_MIDDLE  0x4

/* Linux <linux/input-event-codes.h> values. */
#define EV_SYN      0x00
#define EV_KEY      0x01
#define EV_REL      0x02
#define SYN_REPORT  0
#define REL_X       0x00
#define REL_Y       0x01
#define REL_WHEEL   0x08
#define BTN_LEFT    0x110
#define BTN_RIGHT   0x111
#define BTN_MIDDLE  0x112

/* struct input_event on x86-64. */
typedef struct mouse_event {
    uint64 tv_sec;
    uint64 tv_usec;
    uint16 type;
    uint16 code;
    int32  value;
} mouse_event_t;

_Static_assert(sizeof(mouse_event_t) == 24, "struct input_event is 24 bytes");

/* Empty the queue and forget the button state. Before sti. */
void mouse_init(void);

/* \Device\mouse0, reachable as /dev/mouse0. After namespace_init. */
void mouse_register_device(void);

/* One report from a driver - safe from an interrupt handler. Queues the
 * events it implies and wakes readers and pollers. A report that does not fit
 * whole is dropped whole (and counted): half a report would leave a reader
 * with a button event and no SYN, or motion on one axis only. */
void mouse_report(int32 dx, int32 dy, int32 wheel, uint32 buttons);

/* Kernel-side consumer: take the oldest event. 1 if one was taken, 0 if the
 * queue is empty. */
int mouse_take(mouse_event_t *ev);

/* Events queued right now. */
int mouse_pending(void);

/* Throw away everything queued and reset the button state to "all up" -
 * the selftest's clean slate, so nothing it injected reaches a reader. */
void mouse_flush(void);

/* Reports dropped for want of room, since boot. */
uint64 mouse_dropped(void);

#endif
