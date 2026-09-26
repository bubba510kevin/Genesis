#ifndef LINUX_LIST_H
#define LINUX_LIST_H

#include "linux/kernel.h"

/* <linux/list.h> - Linux's intrusive circular doubly-linked list.
 *
 * Vendored in spirit rather than adapted: this is the same algorithm and the
 * same names, because driver source uses list_for_each_entry and
 * list_add_tail constantly and any deviation is a compile error in somebody
 * else's file. It is header-only and allocates nothing, so there is no
 * Genesis primitive underneath it to shim - the list IS the implementation.
 *
 * The circular-with-sentinel design is worth understanding before editing:
 * an empty list is a head whose next and prev both point AT the head, so
 * insertion and removal need no special case for the empty or single-element
 * list. That is the whole trick, and it is why there are no NULL checks here.
 */

struct list_head {
    struct list_head *next, *prev;
};

#define LIST_HEAD_INIT(name) { &(name), &(name) }
#define LIST_HEAD(name)      struct list_head name = LIST_HEAD_INIT(name)

static inline void INIT_LIST_HEAD(struct list_head *list) {
    list->next = list;
    list->prev = list;
}

static inline void __list_add(struct list_head *nw, struct list_head *prev,
                              struct list_head *next) {
    next->prev = nw;
    nw->next   = next;
    nw->prev   = prev;
    prev->next = nw;
}

static inline void list_add(struct list_head *nw, struct list_head *head) {
    __list_add(nw, head, head->next);
}

static inline void list_add_tail(struct list_head *nw,
                                 struct list_head *head) {
    __list_add(nw, head->prev, head);
}

static inline void __list_del(struct list_head *prev, struct list_head *next) {
    next->prev = prev;
    prev->next = next;
}

static inline void list_del(struct list_head *entry) {
    __list_del(entry->prev, entry->next);
    /* Linux poisons these with LIST_POISON1/2 so a use-after-remove faults
     * on a recognisable address instead of walking a stale list. Same idea,
     * a non-canonical address so the fault is immediate and obvious. */
    entry->next = (struct list_head *)0xDEAD000000000100ULL;
    entry->prev = (struct list_head *)0xDEAD000000000122ULL;
}

static inline void list_del_init(struct list_head *entry) {
    __list_del(entry->prev, entry->next);
    INIT_LIST_HEAD(entry);
}

static inline int list_empty(const struct list_head *head) {
    return head->next == head;
}

static inline void list_move(struct list_head *list, struct list_head *head) {
    __list_del(list->prev, list->next);
    list_add(list, head);
}

static inline void list_move_tail(struct list_head *list,
                                  struct list_head *head) {
    __list_del(list->prev, list->next);
    list_add_tail(list, head);
}

static inline void list_replace(struct list_head *old,
                                struct list_head *nw) {
    nw->next       = old->next;
    nw->next->prev = nw;
    nw->prev       = old->prev;
    nw->prev->next = nw;
}

#define list_entry(ptr, type, member) container_of(ptr, type, member)

#define list_first_entry(ptr, type, member) \
    list_entry((ptr)->next, type, member)
#define list_last_entry(ptr, type, member) \
    list_entry((ptr)->prev, type, member)
#define list_next_entry(pos, member) \
    list_entry((pos)->member.next, __typeof__(*(pos)), member)

#define list_for_each(pos, head) \
    for (pos = (head)->next; pos != (head); pos = pos->next)

/* The _safe form caches the next pointer BEFORE the body runs, so the body
 * may free or remove the current entry. Using the plain form and freeing
 * inside it reads the freed entry's next pointer - the single most common
 * way to misuse this API. */
#define list_for_each_safe(pos, n, head) \
    for (pos = (head)->next, n = pos->next; pos != (head); \
         pos = n, n = pos->next)

#define list_for_each_entry(pos, head, member)                       \
    for (pos = list_entry((head)->next, __typeof__(*pos), member);   \
         &pos->member != (head);                                     \
         pos = list_entry(pos->member.next, __typeof__(*pos), member))

#define list_for_each_entry_safe(pos, n, head, member)               \
    for (pos = list_entry((head)->next, __typeof__(*pos), member),   \
         n = list_entry(pos->member.next, __typeof__(*pos), member); \
         &pos->member != (head);                                     \
         pos = n, n = list_entry(n->member.next, __typeof__(*n), member))

#endif
