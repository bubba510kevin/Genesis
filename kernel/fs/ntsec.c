/* Building a Windows SECURITY_DESCRIPTOR out of the ACL that is already
 * there. See kernel/include/ntsec.h for what actually differs between the
 * two representations - it is less than the names suggest, and the parts that
 * do differ are the parts a memcpy would silently get wrong.
 */

#include "ntsec.h"
#include "acl.h"
#include "typesk.h"

#define ENOSPC 28
#define EINVAL 22

/* --- SIDs ------------------------------------------------------------------
 *
 * S-1-<authority>-<sub>... laid out as: revision byte, sub-authority count
 * byte, six bytes of identifier authority BIG-ENDIAN, then the sub-authorities
 * as 32-bit LITTLE-endian words.
 *
 * The mixed byte order is not a mistake in this file, it is the format: the
 * authority field was defined as a 48-bit big-endian quantity and the
 * sub-authorities as native little-endian words, and every implementation
 * carries the same wart. Writing the authority with a normal little-endian
 * store produces S-1-1441151880758558720-0 instead of S-1-1-0, which is a
 * valid-looking SID that matches nothing. */
static int sid_write(void *buf, uint64 authority, const uint32 *subs,
                     int nsubs) {
    uint8 *p = (uint8 *)buf;
    int i;

    p[0] = 1;                       /* SID_REVISION */
    p[1] = (uint8)nsubs;
    /* Big-endian, most significant byte first. */
    p[2] = (uint8)((authority >> 40) & 0xFF);
    p[3] = (uint8)((authority >> 32) & 0xFF);
    p[4] = (uint8)((authority >> 24) & 0xFF);
    p[5] = (uint8)((authority >> 16) & 0xFF);
    p[6] = (uint8)((authority >> 8) & 0xFF);
    p[7] = (uint8)(authority & 0xFF);
    for (i = 0; i < nsubs; i++) {
        p[8 + i * 4 + 0] = (uint8)(subs[i] & 0xFF);
        p[8 + i * 4 + 1] = (uint8)((subs[i] >> 8) & 0xFF);
        p[8 + i * 4 + 2] = (uint8)((subs[i] >> 16) & 0xFF);
        p[8 + i * 4 + 3] = (uint8)((subs[i] >> 24) & 0xFF);
    }
    return 8 + nsubs * 4;
}

int ntsec_sid_for_uid(uint32 uid, void *buf) {
    uint32 subs[2];

    subs[0] = 1;                    /* Samba: 1 = user under authority 22 */
    subs[1] = uid;
    return sid_write(buf, 22, subs, 2);
}

int ntsec_sid_for_gid(uint32 gid, void *buf) {
    uint32 subs[2];

    subs[0] = 2;                    /* 2 = group */
    subs[1] = gid;
    return sid_write(buf, 22, subs, 2);
}

/* S-1-1-0, Everyone. */
static int sid_everyone(void *buf) {
    uint32 sub = 0;

    return sid_write(buf, 1, &sub, 1);
}

/* S-1-3-0 CREATOR_OWNER and S-1-3-1 CREATOR_GROUP. Only ever emitted for an
 * inherit-only ACE - see the header. */
static int sid_creator_owner(void *buf) {
    uint32 sub = 0;

    return sid_write(buf, 3, &sub, 1);
}

static int sid_creator_group(void *buf) {
    uint32 sub = 1;

    return sid_write(buf, 3, &sub, 1);
}

/* --- flags -----------------------------------------------------------------
 *
 * Bit by bit, deliberately. The first four line up and the rest do not, and
 * writing it as four assignments plus three renames makes the divergence
 * visible at the point where it matters instead of hiding it behind a mask
 * constant that looks like it agrees. */
uint8 ntsec_flags_to_nt(uint32 f) {
    uint8 out = 0;

    if (f & ACE_FILE_INHERIT_ACE)          { out |= NT_OBJECT_INHERIT_ACE; }
    if (f & ACE_DIRECTORY_INHERIT_ACE)     { out |= NT_CONTAINER_INHERIT_ACE; }
    if (f & ACE_NO_PROPAGATE_INHERIT_ACE)  { out |= NT_NO_PROPAGATE_INHERIT_ACE; }
    if (f & ACE_INHERIT_ONLY_ACE)          { out |= NT_INHERIT_ONLY_ACE; }
    /* 0x80 over there, 0x10 over here. */
    if (f & ACE_INHERITED_ACE)             { out |= NT_INHERITED_ACE; }
    /* 0x10/0x20 over there, 0x40/0x80 over here. */
    if (f & ACE_SUCCESSFUL_ACCESS_ACE_FLAG) { out |= NT_SUCCESSFUL_ACCESS_FLAG; }
    if (f & ACE_FAILED_ACCESS_ACE_FLAG)     { out |= NT_FAILED_ACCESS_FLAG; }
    /* ACE_IDENTIFIER_GROUP has no NT flag at all. It is not dropped - it
     * decides which SID the ACE gets, in ace_sid below. */
    return out;
}

static void put16(uint8 *p, uint16 v) {
    p[0] = (uint8)(v & 0xFF);
    p[1] = (uint8)((v >> 8) & 0xFF);
}

static void put32(uint8 *p, uint32 v) {
    p[0] = (uint8)(v & 0xFF);
    p[1] = (uint8)((v >> 8) & 0xFF);
    p[2] = (uint8)((v >> 16) & 0xFF);
    p[3] = (uint8)((v >> 24) & 0xFF);
}

/* The SID an ACE is about. */
static int ace_sid(const acl_ace_t *e, const acl_t *a, void *buf) {
    uint32 entry = e->flags & ACE_TYPE_FLAGS;
    int inherit_only = (e->flags & ACE_INHERIT_ONLY_ACE) != 0;

    if (entry == ACE_OWNER) {
        return inherit_only ? sid_creator_owner(buf)
                            : ntsec_sid_for_uid(a->owner, buf);
    }
    if (entry == ACE_OWNING_GROUP) {
        return inherit_only ? sid_creator_group(buf)
                            : ntsec_sid_for_gid(a->group, buf);
    }
    if (entry == ACE_EVERYONE) {
        return sid_everyone(buf);
    }
    if (entry == ACE_IDENTIFIER_GROUP) {
        return ntsec_sid_for_gid((uint32)e->who, buf);
    }
    return ntsec_sid_for_uid((uint32)e->who, buf);
}

int64 ntsec_build(const acl_t *a, void *buf, uint64 cap) {
    uint8 scratch[24];
    uint8 *p = (uint8 *)buf;
    uint64 need;
    uint64 off;
    uint32 owner_off, group_off, dacl_off;
    uint32 i;
    uint32 emitted = 0;

    if (a == NULL) {
        return -EINVAL;
    }

    /* --- size it first, in one pass, without writing anything -------------
     *
     * Two passes rather than one-and-a-realloc because a security descriptor
     * is self-relative: the DACL's offset has to be written into the header
     * before the DACL is laid down, and that offset depends on how long the
     * owner and group SIDs turned out to be. Measuring first is simpler than
     * back-patching, and it is also what makes the size query (buf NULL,
     * cap 0) free rather than a special case. */
    need = 20;                                  /* the header */
    need += (uint64)ntsec_sid_for_uid(a->owner, scratch);
    need += (uint64)ntsec_sid_for_gid(a->group, scratch);
    need += 8;                                  /* the ACL header */
    for (i = 0; i < a->count && i < ACL_ACE_MAX; i++) {
        const acl_ace_t *e = &a->ace[i];

        /* Only allow and deny are emitted. Audit and alarm belong in a SACL,
         * which is a different field and requires SE_SACL_PRESENT - putting
         * them in the DACL would turn "log this" into "permit this". */
        if (e->type != ACE_ACCESS_ALLOWED_ACE_TYPE &&
            e->type != ACE_ACCESS_DENIED_ACE_TYPE) {
            continue;
        }
        need += 8 + (uint64)ace_sid(e, a, scratch);
    }

    if (buf == NULL || cap == 0) {
        return (int64)need;
    }
    if (cap < need) {
        return -ENOSPC;
    }

    /* --- header ---------------------------------------------------------- */
    p[0] = 1;                                   /* Revision */
    p[1] = 0;                                   /* Sbz1 */
    put16(p + 2, (uint16)(SE_SELF_RELATIVE | SE_DACL_PRESENT));

    off = 20;
    owner_off = (uint32)off;
    off += (uint64)ntsec_sid_for_uid(a->owner, p + off);
    group_off = (uint32)off;
    off += (uint64)ntsec_sid_for_gid(a->group, p + off);

    put32(p + 4, owner_off);
    put32(p + 8, group_off);
    put32(p + 12, 0);                           /* Sacl: none */
    dacl_off = (uint32)off;
    put32(p + 16, dacl_off);

    /* --- the DACL --------------------------------------------------------
     *
     * In the SAME ORDER as the source ACL, which is the single most important
     * property of this whole function. NFSv4 and NT both evaluate a list in
     * order and let the first entry to mention a bit decide it, so reordering
     * - to put denies first, say, which is the "canonical" Windows form -
     * would change what the descriptor MEANS. The fixture's ACL is a live
     * example: its DENY everyone@ sits after two allows precisely so that the
     * owner and uid 1001 keep their access, and hoisting it would take that
     * away from both. */
    off += 8;                                   /* leave room for the header */
    for (i = 0; i < a->count && i < ACL_ACE_MAX; i++) {
        const acl_ace_t *e = &a->ace[i];
        uint64 ace_start = off;
        int sidlen;

        if (e->type != ACE_ACCESS_ALLOWED_ACE_TYPE &&
            e->type != ACE_ACCESS_DENIED_ACE_TYPE) {
            continue;
        }

        p[off + 0] = (uint8)(e->type == ACE_ACCESS_DENIED_ACE_TYPE ? 1 : 0);
        p[off + 1] = ntsec_flags_to_nt(e->flags);
        /* The access mask crosses unchanged. Not a shortcut - NFSv4 took
         * these bits from NT and they have never differed. */
        put32(p + off + 4, e->mask);
        sidlen = ace_sid(e, a, p + off + 8);
        put16(p + off + 2, (uint16)(8 + sidlen));
        off = ace_start + 8 + (uint64)sidlen;
        emitted++;
    }

    /* The ACL header, now that the count and the length are known. */
    p[dacl_off + 0] = NT_ACL_REVISION;
    p[dacl_off + 1] = 0;
    put16(p + dacl_off + 2, (uint16)(off - dacl_off));
    put16(p + dacl_off + 4, (uint16)emitted);
    put16(p + dacl_off + 6, 0);

    return (int64)off;
}
