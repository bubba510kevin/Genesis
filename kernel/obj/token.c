/* Access tokens - see token.h. */

#include "token.h"
#include "acl.h"
#include "kheap.h"
#include "nt.h"
#include "ntsec.h"
#include "object.h"
#include "process.h"

#define TOKEN_MAX_GROUPS  (CRED_NGROUPS + 8)
#define TOKEN_MAX_PRIVS   24
#define SID_MAX           28

#define SE_GROUP_MANDATORY          0x00000001u
#define SE_GROUP_ENABLED_BY_DEFAULT 0x00000002u
#define SE_GROUP_ENABLED            0x00000004u
#define SE_GROUP_OWNER              0x00000008u
#define SE_GROUP_INTEGRITY          0x00000020u
#define SE_GROUP_INTEGRITY_ENABLED  0x00000040u
#define SE_GROUP_LOGON_ID           0xC0000000u
#define GROUP_STD (SE_GROUP_MANDATORY | SE_GROUP_ENABLED_BY_DEFAULT | \
                   SE_GROUP_ENABLED)

#define SE_PRIVILEGE_ENABLED_BY_DEFAULT 0x00000001u
#define SE_PRIVILEGE_ENABLED            0x00000002u
#define SE_PRIVILEGE_REMOVED            0x00000004u

#define STATUS_NOT_ALL_ASSIGNED_  0x00000106u

typedef struct {
    uint8  sid[SID_MAX];
    uint32 sid_len;
    uint32 attr;
} tgroup_t;

typedef struct {
    uint32 luid;
    uint32 attr;
} tpriv_t;

typedef struct {
    uint8    user[SID_MAX];
    uint32   user_len;
    uint8    primary[SID_MAX];
    uint32   primary_len;
    tgroup_t groups[TOKEN_MAX_GROUPS];
    uint32   ngroups;
    tgroup_t label;                   /* the integrity level */
    tpriv_t  privs[TOKEN_MAX_PRIVS];
    uint32   nprivs;
    int      elevated;
    uint64   id;
    uint64   modified;
} token_t;

static uint64 next_luid = 0x10000;

/* S-1-<auth>-<subs...>, as ntsec.c writes them. */
static uint32 sid_make(uint8 *p, uint64 auth, const uint32 *subs, int n) {
    int i;

    p[0] = 1;
    p[1] = (uint8)n;
    for (i = 0; i < 6; i++) {
        p[2 + i] = (uint8)(auth >> (8 * (5 - i)));
    }
    for (i = 0; i < n; i++) {
        p[8 + i * 4 + 0] = (uint8)subs[i];
        p[8 + i * 4 + 1] = (uint8)(subs[i] >> 8);
        p[8 + i * 4 + 2] = (uint8)(subs[i] >> 16);
        p[8 + i * 4 + 3] = (uint8)(subs[i] >> 24);
    }
    return 8 + 4 * (uint32)n;
}

static void add_group(token_t *t, uint64 auth, const uint32 *subs, int n,
                      uint32 attr) {
    tgroup_t *g;

    if (t->ngroups >= TOKEN_MAX_GROUPS) {
        return;
    }
    g = &t->groups[t->ngroups++];
    g->sid_len = sid_make(g->sid, auth, subs, n);
    g->attr = attr;
}

static void add_priv(token_t *t, uint32 luid, int enabled) {
    if (t->nprivs < TOKEN_MAX_PRIVS) {
        t->privs[t->nprivs].luid = luid;
        t->privs[t->nprivs].attr = enabled
            ? (SE_PRIVILEGE_ENABLED | SE_PRIVILEGE_ENABLED_BY_DEFAULT) : 0;
        t->nprivs++;
    }
}

static void token_destroy(object_t *obj) {
    if (obj->body != NULL) {
        kfree(obj->body);
        obj->body = NULL;
    }
}

static const object_type_t token_type = {
    .name    = "Token",
    .klass   = OBJ_TOKEN,
    .destroy = token_destroy
};

int token_is_token(const object_t *obj) {
    return obj != NULL && obj->type == &token_type;
}

static object_t *token_build(process_t *p) {
    token_t *t = (token_t *)kcalloc(1, sizeof(*t));
    object_t *obj;
    cred_t c;
    uint32 s[2], i;
    int admin;

    if (t == NULL) {
        return NULL;
    }
    proc_cred(p, &c);
    admin = cred_is_root(&c) || c.supreme;
    t->user_len = (uint32)ntsec_sid_for_uid(c.euid, t->user);
    t->primary_len = (uint32)ntsec_sid_for_gid(c.egid, t->primary);

    s[0] = 2;
    s[1] = c.egid;
    add_group(t, 22, s, 2, GROUP_STD);
    for (i = 0; i < c.ngroups && i < CRED_NGROUPS; i++) {
        if (c.groups[i] != c.egid) {
            s[1] = c.groups[i];
            add_group(t, 22, s, 2, GROUP_STD);
        }
    }
    s[0] = 0;
    add_group(t, 1, s, 1, GROUP_STD);                   /* Everyone        */
    add_group(t, 2, s, 1, GROUP_STD);                   /* LOCAL           */
    s[0] = 4;
    add_group(t, 5, s, 1, GROUP_STD);                   /* INTERACTIVE     */
    s[0] = 11;
    add_group(t, 5, s, 1, GROUP_STD);                   /* Authenticated   */
    if (admin) {
        s[0] = 32;
        s[1] = 544;                                     /* Administrators  */
        add_group(t, 5, s, 2, GROUP_STD | SE_GROUP_OWNER);
    }
    s[0] = admin ? 0x3000u : 0x2000u;                   /* High / Medium   */
    t->label.sid_len = sid_make(t->label.sid, 16, s, 1);
    t->label.attr = SE_GROUP_INTEGRITY | SE_GROUP_INTEGRITY_ENABLED;

    /* The LUIDs are NT's well-known privilege values (SE_*_PRIVILEGE). */
    add_priv(t, 19, 0);                                 /* Shutdown        */
    add_priv(t, 23, 1);                                 /* ChangeNotify    */
    add_priv(t, 25, 0);                                 /* Undock          */
    add_priv(t, 33, 0);                                 /* IncreaseWorkingSet */
    add_priv(t, 34, 0);                                 /* TimeZone        */
    if (admin) {
        add_priv(t, 5, 0);                              /* IncreaseQuota   */
        add_priv(t, 8, 0);                              /* Security        */
        add_priv(t, 9, 0);                              /* TakeOwnership   */
        add_priv(t, 10, 0);                             /* LoadDriver      */
        add_priv(t, 12, 0);                             /* Systemtime      */
        add_priv(t, 17, 0);                             /* Backup          */
        add_priv(t, 18, 0);                             /* Restore         */
        add_priv(t, 20, 0);                             /* Debug           */
        add_priv(t, 29, 1);                             /* Impersonate     */
        add_priv(t, 30, 1);                             /* CreateGlobal    */
        add_priv(t, 35, 0);                             /* CreateSymbolicLink */
    }
    t->elevated = admin;
    t->id = next_luid++;
    t->modified = next_luid++;
    obj = ob_create(&token_type, t);
    if (obj == NULL) {
        kfree(t);
    }
    return obj;
}

object_t *token_of_process(process_t *leader) {
    if (leader == NULL) {
        return NULL;
    }
    if (leader->nt_token == NULL) {
        leader->nt_token = token_build(leader);
        if (leader->nt_token == NULL) {
            return NULL;
        }
    }
    ob_ref(leader->nt_token);
    return leader->nt_token;
}

/* --- queries -------------------------------------------------------------- */

static void put32(uint8 *b, uint32 off, uint32 v) {
    *(uint32 *)(b + off) = v;
}

static void put64(uint8 *b, uint32 off, uint64 v) {
    *(uint64 *)(b + off) = v;
}

static void copy(uint8 *dst, const uint8 *src, uint32 n) {
    uint32 i;

    for (i = 0; i < n; i++) {
        dst[i] = src[i];
    }
}

/* One SID_AND_ATTRIBUTES at `at` pointing at a copy of the SID at `sid_off`. */
static void sid_and_attr(uint8 *b, uint32 at, uint32 sid_off, uint64 base,
                         const uint8 *sid, uint32 sid_len, uint32 attr) {
    put64(b, at, base + sid_off);
    put32(b, at + 8, attr);
    put32(b, at + 12, 0);
    copy(b + sid_off, sid, sid_len);
}

uint32 token_query(object_t *obj, uint32 klass, void *out, uint32 len,
                   uint64 base, uint32 *needed) {
    token_t *t = (token_t *)obj->body;
    uint8 *b = (uint8 *)out;
    uint32 need = 0, i, off;

    switch (klass) {
    case 1:                                    /* TokenUser */
        need = 16 + t->user_len;
        break;
    case 2:                                    /* TokenGroups */
        need = 8 + 16 * t->ngroups;
        for (i = 0; i < t->ngroups; i++) {
            need += t->groups[i].sid_len;
        }
        break;
    case 3:                                    /* TokenPrivileges */
        need = 4 + 12 * t->nprivs;
        break;
    case 4:                                    /* TokenOwner */
        need = 8 + t->user_len;
        break;
    case 5:                                    /* TokenPrimaryGroup */
        need = 8 + t->primary_len;
        break;
    case 8:                                    /* TokenType */
    case 12:                                   /* TokenSessionId */
    case 18:                                   /* TokenElevationType */
    case 20:                                   /* TokenElevation */
    case 29:                                   /* TokenIsAppContainer */
        need = 4;
        break;
    case 10:                                   /* TokenStatistics */
        need = 56;
        break;
    case 25:                                   /* TokenIntegrityLevel */
        need = 16 + t->label.sid_len;
        break;
    default:
        *needed = 0;
        return STATUS_INVALID_INFO_CLASS;
    }
    *needed = need;
    if (len < need) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    switch (klass) {
    case 1:
        sid_and_attr(b, 0, 16, base, t->user, t->user_len, 0);
        break;
    case 2:
        put32(b, 0, t->ngroups);
        put32(b, 4, 0);
        off = 8 + 16 * t->ngroups;
        for (i = 0; i < t->ngroups; i++) {
            sid_and_attr(b, 8 + 16 * i, off, base, t->groups[i].sid,
                         t->groups[i].sid_len, t->groups[i].attr);
            off += t->groups[i].sid_len;
        }
        break;
    case 3:
        put32(b, 0, t->nprivs);
        for (i = 0; i < t->nprivs; i++) {
            put32(b, 4 + 12 * i, t->privs[i].luid);       /* LowPart  */
            put32(b, 8 + 12 * i, 0);                      /* HighPart */
            put32(b, 12 + 12 * i, t->privs[i].attr);
        }
        break;
    case 4:
        put64(b, 0, base + 8);
        copy(b + 8, t->user, t->user_len);
        break;
    case 5:
        put64(b, 0, base + 8);
        copy(b + 8, t->primary, t->primary_len);
        break;
    case 8:
        put32(b, 0, 1);                                   /* TokenPrimary */
        break;
    case 10:
        for (i = 0; i < 56; i++) {
            b[i] = 0;
        }
        put64(b, 0, t->id);                               /* TokenId      */
        put64(b, 8, 0x3E7);                               /* the logon    */
        put64(b, 16, 0x7FFFFFFFFFFFFFFFULL);              /* never expires*/
        put32(b, 24, 1);                                  /* TokenPrimary */
        put32(b, 40, t->ngroups);
        put32(b, 44, t->nprivs);
        put64(b, 48, t->modified);
        break;
    case 12:
        put32(b, 0, 1);                                   /* session 1    */
        break;
    case 18:
        put32(b, 0, 1);                       /* TokenElevationTypeDefault */
        break;
    case 20:
        put32(b, 0, (uint32)t->elevated);
        break;
    case 25:
        sid_and_attr(b, 0, 16, base, t->label.sid, t->label.sid_len,
                     t->label.attr);
        break;
    case 29:
        put32(b, 0, 0);
        break;
    }
    return STATUS_SUCCESS;
}

/* --- AdjustTokenPrivileges ----------------------------------------------- */

uint32 token_adjust(object_t *obj, int disable_all, const void *new_state,
                    uint32 new_len, void *prev, uint32 prev_len,
                    uint32 *prev_needed) {
    token_t *t = (token_t *)obj->body;
    const uint8 *ns = (const uint8 *)new_state;
    uint8 *pv = (uint8 *)prev;
    uint32 count = 0, i, j, changed = 0, missing = 0;
    uint32 old[TOKEN_MAX_PRIVS];

    for (i = 0; i < t->nprivs; i++) {
        old[i] = t->privs[i].attr;
    }
    if (!disable_all) {
        if (new_state == NULL || new_len < 4) {
            return STATUS_INVALID_PARAMETER;
        }
        count = *(const uint32 *)ns;
        if (new_len < 4 + 12 * count) {
            return STATUS_INVALID_PARAMETER;
        }
    }
    /* What would change, for the size of PreviousState, before changing
     * anything: a PreviousState too small fails the whole call. */
    for (i = 0; i < t->nprivs; i++) {
        uint32 want = t->privs[i].attr;

        if (disable_all) {
            want &= ~SE_PRIVILEGE_ENABLED;
        } else {
            for (j = 0; j < count; j++) {
                uint32 luid = *(const uint32 *)(ns + 4 + 12 * j);
                uint32 hi = *(const uint32 *)(ns + 8 + 12 * j);
                uint32 a = *(const uint32 *)(ns + 12 + 12 * j);

                if (hi == 0 && luid == t->privs[i].luid) {
                    want = (a & SE_PRIVILEGE_ENABLED)
                               ? (want | SE_PRIVILEGE_ENABLED)
                               : (want & ~SE_PRIVILEGE_ENABLED);
                }
            }
        }
        if (want != t->privs[i].attr) {
            changed++;
        }
    }
    *prev_needed = 4 + 12 * changed;
    if (prev != NULL && prev_len < *prev_needed) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    if (!disable_all) {
        for (j = 0; j < count; j++) {
            uint32 luid = *(const uint32 *)(ns + 4 + 12 * j);
            uint32 hi = *(const uint32 *)(ns + 8 + 12 * j);
            uint32 a = *(const uint32 *)(ns + 12 + 12 * j);
            int found = 0;

            for (i = 0; i < t->nprivs; i++) {
                if (hi == 0 && t->privs[i].luid == luid) {
                    found = 1;
                    if (a & SE_PRIVILEGE_ENABLED) {
                        t->privs[i].attr |= SE_PRIVILEGE_ENABLED;
                    } else {
                        t->privs[i].attr &= ~SE_PRIVILEGE_ENABLED;
                    }
                }
            }
            if (!found) {
                missing = 1;
            }
        }
    } else {
        for (i = 0; i < t->nprivs; i++) {
            t->privs[i].attr &= ~SE_PRIVILEGE_ENABLED;
        }
    }
    if (prev != NULL) {
        uint32 n = 0;

        for (i = 0; i < t->nprivs; i++) {
            if (old[i] != t->privs[i].attr) {
                put32(pv, 4 + 12 * n, t->privs[i].luid);
                put32(pv, 8 + 12 * n, 0);
                put32(pv, 12 + 12 * n, old[i]);
                n++;
            }
        }
        put32(pv, 0, n);
    }
    if (changed != 0) {
        t->modified = next_luid++;
    }
    return missing ? STATUS_NOT_ALL_ASSIGNED_ : STATUS_SUCCESS;
}
