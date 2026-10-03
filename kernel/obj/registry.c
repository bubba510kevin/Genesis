#include "kheap.h"
#include "ksmp.h"
#include "nt.h"
#include "object.h"
#include "pmm.h"
#include "registry.h"
#include "timer.h"
#include "typesk.h"

/* See registry.h. Everything runs under the big kernel lock. */

#define REG_NAME_MAX   255             /* characters in one key name, as NT */
#define REG_VNAME_MAX  16383           /* characters in a value name        */
#define REG_DATA_MAX   (1u << 20)      /* bytes in one value: generous      */

#define STATUS_BUFFER_OVERFLOW   0x80000005u
#define STATUS_NO_MORE_ENTRIES   0x8000001Au
#define STATUS_KEY_DELETED       0xC000017Cu
#define STATUS_CANNOT_DELETE     0xC0000121u

typedef struct reg_value {
    struct reg_value *next;
    uint16 *name;
    uint32  name_len;                  /* characters */
    uint32  type;
    uint32  size;
    uint8  *data;
} reg_value_t;

typedef struct reg_key {
    struct reg_key *parent;
    struct reg_key *child;             /* first subkey, sorted               */
    struct reg_key *next;              /* next sibling                       */
    uint16 *name;
    uint32  name_len;
    uint16 *klass;
    uint32  class_len;
    reg_value_t *values;               /* in the order first set             */
    uint64  last_write;                /* 100ns since 1601                   */
    uint32  refs;                      /* the tree's link, plus each handle  */
    uint8   deleted;
} reg_key_t;

static reg_key_t *root;                /* \Registry                          */

static uint64 now_100ns(void) {
    return timer_realtime_ns() / 100 + 116444736000000000ULL;
}

/* --- names ------------------------------------------------------------------ */

static uint16 fold(uint16 c) {
    return (c >= 'a' && c <= 'z') ? (uint16)(c - 'a' + 'A') : c;
}

/* <0, 0, >0 ignoring ASCII case - the order subkeys are kept in. */
static int name_cmp(const uint16 *a, uint32 al, const uint16 *b, uint32 bl) {
    uint32 i;

    for (i = 0; i < al && i < bl; i++) {
        uint16 x = fold(a[i]), y = fold(b[i]);

        if (x != y) {
            return x < y ? -1 : 1;
        }
    }
    return al == bl ? 0 : (al < bl ? -1 : 1);
}

static uint16 *wdup(const uint16 *s, uint32 n) {
    uint16 *d = kmalloc((uint64)(n + 1) * 2);
    uint32 i;

    if (d == NULL) {
        return NULL;
    }
    for (i = 0; i < n; i++) {
        d[i] = s[i];
    }
    d[n] = 0;
    return d;
}

/* --- keys ---------------------------------------------------------------------- */

static void key_free(reg_key_t *k) {
    reg_value_t *v = k->values;

    while (v != NULL) {
        reg_value_t *n = v->next;

        kfree(v->name);
        if (v->data != NULL) {
            kfree(v->data);
        }
        kfree(v);
        v = n;
    }
    kfree(k->name);
    if (k->klass != NULL) {
        kfree(k->klass);
    }
    kfree(k);
}

void registry_key_deref(reg_key_t *k) {
    if (k != NULL && k->refs > 0 && --k->refs == 0) {
        key_free(k);
    }
}

static reg_key_t *find_child(reg_key_t *k, const uint16 *name, uint32 n) {
    reg_key_t *c;

    for (c = k->child; c != NULL; c = c->next) {
        if (name_cmp(c->name, c->name_len, name, n) == 0) {
            return c;
        }
    }
    return NULL;
}

static reg_key_t *add_child(reg_key_t *parent, const uint16 *name, uint32 n,
                            const uint16 *klass, uint32 class_len) {
    reg_key_t *k = kmalloc(sizeof(*k)), **at;

    if (k == NULL) {
        return NULL;
    }
    k->name = wdup(name, n);
    k->klass = (klass != NULL && class_len > 0) ? wdup(klass, class_len) : NULL;
    if (k->name == NULL) {
        kfree(k);
        return NULL;
    }
    k->name_len = n;
    k->class_len = k->klass != NULL ? class_len : 0;
    k->parent = parent;
    k->child = NULL;
    k->values = NULL;
    k->last_write = now_100ns();
    k->refs = 1;                         /* the tree's */
    k->deleted = 0;
    /* Sorted, so enumeration comes out in NT's order. */
    for (at = &parent->child; *at != NULL &&
         name_cmp((*at)->name, (*at)->name_len, name, n) < 0;
         at = &(*at)->next) {
    }
    k->next = *at;
    *at = k;
    parent->last_write = k->last_write;
    return k;
}

uint32 registry_lookup(reg_key_t *base, const uint16 *name, uint32 chars,
                       int create, const uint16 *klass, uint32 class_chars,
                       reg_key_t **out, int *created) {
    reg_key_t *k;
    uint32 i = 0;

    *created = 0;
    if (base == NULL) {
        /* Absolute: must start \Registry, case aside. */
        static const uint16 reg[] = { '\\', 'R', 'E', 'G', 'I', 'S', 'T',
                                      'R', 'Y' };

        if (chars < 9 || name_cmp(name, 9, reg, 9) != 0 ||
            (chars > 9 && name[9] != '\\')) {
            return STATUS_OBJECT_PATH_NOT_FOUND;
        }
        k = root;
        i = 9;
    } else {
        if (base->deleted) {
            return STATUS_KEY_DELETED;
        }
        k = base;
        if (chars > 0 && name[0] == '\\') {
            return STATUS_OBJECT_PATH_SYNTAX_BAD;   /* relative, but rooted */
        }
    }

    while (i < chars) {
        uint32 start, len;
        reg_key_t *c;
        int last;

        while (i < chars && name[i] == '\\') {
            i++;
        }
        start = i;
        while (i < chars && name[i] != '\\') {
            i++;
        }
        len = i - start;
        if (len == 0) {
            break;
        }
        if (len > REG_NAME_MAX) {
            return STATUS_INVALID_PARAMETER;
        }
        last = 1;
        {
            uint32 j = i;

            while (j < chars && name[j] == '\\') {
                j++;
            }
            last = j >= chars;
        }
        c = find_child(k, &name[start], len);
        if (c == NULL) {
            if (!create) {
                return last ? STATUS_OBJECT_NAME_NOT_FOUND
                            : STATUS_OBJECT_PATH_NOT_FOUND;
            }
            /* NT creates one level at a time: a missing PARENT is an error
             * even for NtCreateKey (RegCreateKeyEx makes the chain itself,
             * a level per call). */
            if (!last) {
                return STATUS_OBJECT_NAME_NOT_FOUND;
            }
            c = add_child(k, &name[start], len, klass, class_chars);
            if (c == NULL) {
                return STATUS_INSUFFICIENT_RESOURCES;
            }
            *created = 1;
        }
        k = c;
    }
    k->refs++;
    *out = k;
    return STATUS_SUCCESS;
}

uint32 registry_delete_key(reg_key_t *k) {
    reg_key_t **at;

    if (k->deleted) {
        return STATUS_KEY_DELETED;
    }
    /* \Registry itself, \Registry\Machine and \Registry\User, and the
     * hive roots under them (SOFTWARE, SYSTEM, .DEFAULT...), are not the
     * caller's to remove - whether or not they have subkeys. */
    if (k->parent == NULL || k->parent == root || k->parent->parent == root) {
        return STATUS_ACCESS_DENIED;
    }
    if (k->child != NULL) {
        return STATUS_CANNOT_DELETE;     /* subkeys first */
    }
    for (at = &k->parent->child; *at != NULL; at = &(*at)->next) {
        if (*at == k) {
            *at = k->next;
            break;
        }
    }
    k->deleted = 1;
    k->parent->last_write = now_100ns();
    k->parent = NULL;
    registry_key_deref(k);               /* the tree's reference */
    return STATUS_SUCCESS;
}

/* --- values -------------------------------------------------------------------- */

static reg_value_t *find_value(reg_key_t *k, const uint16 *name, uint32 n,
                               reg_value_t ***link) {
    reg_value_t **at;

    for (at = &k->values; *at != NULL; at = &(*at)->next) {
        if (name_cmp((*at)->name, (*at)->name_len, name, n) == 0) {
            if (link != NULL) {
                *link = at;
            }
            return *at;
        }
    }
    if (link != NULL) {
        *link = at;                      /* the end: where a new one goes */
    }
    return NULL;
}

uint32 registry_set_value(reg_key_t *k, const uint16 *name, uint32 chars,
                          uint32 type, const void *data, uint32 size) {
    reg_value_t *v, **link;
    uint8 *copy = NULL;
    uint32 i;

    if (k->deleted) {
        return STATUS_KEY_DELETED;
    }
    if (chars > REG_VNAME_MAX || size > REG_DATA_MAX) {
        return STATUS_INVALID_PARAMETER;
    }
    if (size > 0) {
        copy = kmalloc(size);
        if (copy == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        for (i = 0; i < size; i++) {
            copy[i] = ((const uint8 *)data)[i];
        }
    }
    v = find_value(k, name, chars, &link);
    if (v == NULL) {
        v = kmalloc(sizeof(*v));
        if (v == NULL || (v->name = wdup(name, chars)) == NULL) {
            if (v != NULL) {
                kfree(v);
            }
            if (copy != NULL) {
                kfree(copy);
            }
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        v->name_len = chars;
        v->next = NULL;
        *link = v;
    } else if (v->data != NULL) {
        kfree(v->data);
    }
    v->type = type;
    v->size = size;
    v->data = copy;
    k->last_write = now_100ns();
    return STATUS_SUCCESS;
}

uint32 registry_delete_value(reg_key_t *k, const uint16 *name, uint32 chars) {
    reg_value_t *v, **link;

    if (k->deleted) {
        return STATUS_KEY_DELETED;
    }
    v = find_value(k, name, chars, &link);
    if (v == NULL) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    *link = v->next;
    kfree(v->name);
    if (v->data != NULL) {
        kfree(v->data);
    }
    kfree(v);
    k->last_write = now_100ns();
    return STATUS_SUCCESS;
}

/* --- the information structures ------------------------------------------------
 *
 * Built into `buf` field by field. A fixed part that does not fit is
 * BUFFER_TOO_SMALL and writes nothing; a fixed part that fits with the
 * variable part (name, data) cut short is BUFFER_OVERFLOW, a WARNING, and
 * the fixed part is there - which is how a caller learns the size and the
 * type in one call. *result is always the full size. */

static void put32(void *buf, uint32 off, uint32 v) {
    *(uint32 *)((uint8 *)buf + off) = v;
}

static void put64(void *buf, uint32 off, uint64 v) {
    *(uint64 *)((uint8 *)buf + off) = v;
}

static void putbytes(void *buf, uint32 off, const void *src, uint32 n,
                     uint32 len) {
    uint32 i;

    for (i = 0; i < n && off + i < len; i++) {
        ((uint8 *)buf)[off + i] = ((const uint8 *)src)[i];
    }
}

static uint32 finish(uint32 fixed, uint32 full, uint32 len, uint32 *result) {
    *result = full;
    if (len < fixed) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    return len < full ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
}

#define KeyValueBasicInformation    0
#define KeyValueFullInformation     1
#define KeyValuePartialInformation  2

static uint32 value_info(const reg_value_t *v, uint32 klass, void *buf,
                         uint32 len, uint32 *result) {
    uint32 nb = v->name_len * 2, fixed, full, doff;

    switch (klass) {
    case KeyValueBasicInformation:      /* TitleIndex, Type, NameLength, Name */
        fixed = 12;
        full = fixed + nb;
        if (len >= fixed) {
            put32(buf, 0, 0);
            put32(buf, 4, v->type);
            put32(buf, 8, nb);
            putbytes(buf, 12, v->name, nb, len);
        }
        return finish(fixed, full, len, result);
    case KeyValueFullInformation:       /* + DataOffset, DataLength */
        fixed = 20;
        doff = (fixed + nb + 7) & ~7u;
        full = doff + v->size;
        if (len >= fixed) {
            put32(buf, 0, 0);
            put32(buf, 4, v->type);
            put32(buf, 8, doff);
            put32(buf, 12, v->size);
            put32(buf, 16, nb);
            putbytes(buf, 20, v->name, nb, len);
            putbytes(buf, doff, v->data, v->size, len);
        }
        return finish(fixed, full, len, result);
    case KeyValuePartialInformation:    /* TitleIndex, Type, DataLength, Data */
        fixed = 12;
        full = fixed + v->size;
        if (len >= fixed) {
            put32(buf, 0, 0);
            put32(buf, 4, v->type);
            put32(buf, 8, v->size);
            putbytes(buf, 12, v->data, v->size, len);
        }
        return finish(fixed, full, len, result);
    default:
        return STATUS_INVALID_INFO_CLASS;
    }
}

uint32 registry_query_value(reg_key_t *k, const uint16 *name, uint32 chars,
                            uint32 klass, void *buf, uint32 len,
                            uint32 *result) {
    reg_value_t *v;

    if (k->deleted) {
        return STATUS_KEY_DELETED;
    }
    v = find_value(k, name, chars, NULL);
    if (v == NULL) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    return value_info(v, klass, buf, len, result);
}

uint32 registry_enumerate_value(reg_key_t *k, uint32 index, uint32 klass,
                                void *buf, uint32 len, uint32 *result) {
    reg_value_t *v = k->values;

    if (k->deleted) {
        return STATUS_KEY_DELETED;
    }
    while (v != NULL && index > 0) {
        v = v->next;
        index--;
    }
    if (v == NULL) {
        return STATUS_NO_MORE_ENTRIES;
    }
    return value_info(v, klass, buf, len, result);
}

#define KeyBasicInformation  0
#define KeyNodeInformation   1
#define KeyFullInformation   2
#define KeyNameInformation   3

static uint32 key_info(const reg_key_t *k, uint32 klass, void *buf, uint32 len,
                       uint32 *result) {
    uint32 nb = k->name_len * 2, cb = k->class_len * 2, fixed, full;

    switch (klass) {
    case KeyBasicInformation:    /* LastWriteTime, TitleIndex, NameLength, Name */
        fixed = 16;
        full = fixed + nb;
        if (len >= fixed) {
            put64(buf, 0, k->last_write);
            put32(buf, 8, 0);
            put32(buf, 12, nb);
            putbytes(buf, 16, k->name, nb, len);
        }
        return finish(fixed, full, len, result);
    case KeyNodeInformation:     /* + ClassOffset, ClassLength; class after */
        fixed = 24;
        full = fixed + nb + cb;
        if (len >= fixed) {
            put64(buf, 0, k->last_write);
            put32(buf, 8, 0);
            put32(buf, 12, cb != 0 ? fixed + nb : 0xFFFFFFFFu);
            put32(buf, 16, cb);
            put32(buf, 20, nb);
            putbytes(buf, 24, k->name, nb, len);
            putbytes(buf, 24 + nb, k->klass, cb, len);
        }
        return finish(fixed, full, len, result);
    case KeyFullInformation: {   /* counts and maxima, then the class */
        const reg_key_t *c;
        const reg_value_t *v;
        uint32 subkeys = 0, maxname = 0, maxclass = 0;
        uint32 values = 0, maxvname = 0, maxdata = 0;

        for (c = k->child; c != NULL; c = c->next) {
            subkeys++;
            maxname = c->name_len * 2 > maxname ? c->name_len * 2 : maxname;
            maxclass = c->class_len * 2 > maxclass ? c->class_len * 2 : maxclass;
        }
        for (v = k->values; v != NULL; v = v->next) {
            values++;
            maxvname = v->name_len * 2 > maxvname ? v->name_len * 2 : maxvname;
            maxdata = v->size > maxdata ? v->size : maxdata;
        }
        fixed = 44;
        full = fixed + cb;
        if (len >= fixed) {
            put64(buf, 0, k->last_write);
            put32(buf, 8, 0);
            put32(buf, 12, cb != 0 ? fixed : 0xFFFFFFFFu);
            put32(buf, 16, cb);
            put32(buf, 20, subkeys);
            put32(buf, 24, maxname);
            put32(buf, 28, maxclass);
            put32(buf, 32, values);
            put32(buf, 36, maxvname);
            put32(buf, 40, maxdata);
            putbytes(buf, 44, k->klass, cb, len);
        }
        return finish(fixed, full, len, result);
    }
    case KeyNameInformation: {   /* NameLength, then the full path */
        const reg_key_t *p;
        uint32 total = 0, at;

        for (p = k; p != NULL; p = p->parent) {
            total += (p->name_len + 1) * 2;          /* "\" + name */
        }
        fixed = 4;
        full = fixed + total;
        if (len >= fixed) {
            put32(buf, 0, total);
            at = fixed + total;
            for (p = k; p != NULL; p = p->parent) {
                at -= p->name_len * 2;
                putbytes(buf, at, p->name, p->name_len * 2, len);
                at -= 2;
                if (at + 2 <= len) {
                    *(uint16 *)((uint8 *)buf + at) = '\\';
                }
            }
        }
        return finish(fixed, full, len, result);
    }
    default:
        return STATUS_INVALID_INFO_CLASS;
    }
}

uint32 registry_enumerate_key(reg_key_t *k, uint32 index, uint32 klass,
                              void *buf, uint32 len, uint32 *result) {
    reg_key_t *c = k->child;

    if (k->deleted) {
        return STATUS_KEY_DELETED;
    }
    if (klass == KeyNameInformation) {
        return STATUS_INVALID_PARAMETER;
    }
    while (c != NULL && index > 0) {
        c = c->next;
        index--;
    }
    if (c == NULL) {
        return STATUS_NO_MORE_ENTRIES;
    }
    return key_info(c, klass, buf, len, result);
}

uint32 registry_query_key(reg_key_t *k, uint32 klass, void *buf, uint32 len,
                          uint32 *result) {
    if (k->deleted) {
        return STATUS_KEY_DELETED;
    }
    return key_info(k, klass, buf, len, result);
}

/* --- handles ---------------------------------------------------------------- */

static void key_object_destroy(object_t *obj) {
    registry_key_deref(obj->body);
}

static const object_type_t key_type = {
    .name    = "Key",
    .klass   = OBJ_KEY,
    .destroy = key_object_destroy,
};

object_t *registry_key_object(reg_key_t *k) {
    object_t *obj = ob_create(&key_type, k);

    if (obj == NULL) {
        registry_key_deref(k);           /* the reference it was to hold */
    }
    return obj;
}

reg_key_t *registry_key_of(object_t *obj) {
    return (obj != NULL && obj->type == &key_type) ? obj->body : NULL;
}

/* --- the tree at boot --------------------------------------------------------- */

static uint32 alen(const char *s) {
    uint32 n = 0;

    while (s[n] != '\0') {
        n++;
    }
    return n;
}

/* An ASCII path under \Registry, made level by level. */
static reg_key_t *seed_key(const char *path) {
    reg_key_t *k = root;
    uint32 i = 0, n = alen(path);

    while (i < n) {
        uint16 w[REG_NAME_MAX];
        uint32 len = 0;
        reg_key_t *c;

        while (i < n && path[i] == '\\') {
            i++;
        }
        while (i < n && path[i] != '\\' && len < REG_NAME_MAX) {
            w[len++] = (uint8)path[i++];
        }
        if (len == 0) {
            break;
        }
        c = find_child(k, w, len);
        if (c == NULL) {
            c = add_child(k, w, len, NULL, 0);
            if (c == NULL) {
                return NULL;
            }
        }
        k = c;
    }
    return k;
}

static void seed_sz(reg_key_t *k, const char *name, uint32 type,
                    const char *value) {
    uint16 wn[64], wv[128];
    uint32 n = alen(name), v = alen(value), i;

    if (k == NULL || n > 63 || v > 127) {
        return;
    }
    for (i = 0; i < n; i++) {
        wn[i] = (uint8)name[i];
    }
    for (i = 0; i < v; i++) {
        wv[i] = (uint8)value[i];
    }
    wv[v] = 0;
    (void)registry_set_value(k, wn, n, type, wv, (v + 1) * 2);
}

static void seed_dword(reg_key_t *k, const char *name, uint32 value) {
    uint16 wn[64];
    uint32 n = alen(name), i;

    if (k == NULL || n > 63) {
        return;
    }
    for (i = 0; i < n; i++) {
        wn[i] = (uint8)name[i];
    }
    (void)registry_set_value(k, wn, n, REG_DWORD, &value, 4);
}

static void decimal(char *out, uint32 v) {
    char t[11];
    int n = 0, i;

    do {
        t[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v != 0);
    for (i = 0; i < n; i++) {
        out[i] = t[n - 1 - i];
    }
    out[n] = '\0';
}

void registry_init(void) {
    static const uint16 reg[] = { 'R', 'E', 'G', 'I', 'S', 'T', 'R', 'Y' };
    reg_key_t *k;
    char num[12];

    if (root != NULL) {
        return;
    }
    root = kmalloc(sizeof(*root));
    if (root == NULL) {
        return;
    }
    root->name = wdup(reg, 8);
    root->name_len = 8;
    root->klass = NULL;
    root->class_len = 0;
    root->parent = root->child = root->next = NULL;
    root->values = NULL;
    root->last_write = now_100ns();
    root->refs = 1;
    root->deleted = 0;

    (void)seed_key("MACHINE\\SAM");
    (void)seed_key("MACHINE\\SECURITY");
    (void)seed_key("MACHINE\\SYSTEM\\CurrentControlSet\\Services");
    (void)seed_key("USER\\.DEFAULT\\Software");
    (void)seed_key("USER\\.DEFAULT\\Environment");

    /* What a program reads to learn what it runs on - the same NT 10.0
     * build 19045 the shared page and the PEB report (kusd.h). */
    k = seed_key("MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion");
    seed_sz(k, "ProductName", REG_SZ, "Windows 10 Pro");
    seed_sz(k, "EditionID", REG_SZ, "Professional");
    seed_sz(k, "CurrentVersion", REG_SZ, "6.3");
    seed_sz(k, "CurrentBuild", REG_SZ, "19045");
    seed_sz(k, "CurrentBuildNumber", REG_SZ, "19045");
    seed_sz(k, "DisplayVersion", REG_SZ, "22H2");
    seed_sz(k, "ReleaseId", REG_SZ, "2009");
    seed_dword(k, "CurrentMajorVersionNumber", 10);
    seed_dword(k, "CurrentMinorVersionNumber", 0);
    seed_sz(k, "SystemRoot", REG_SZ, "C:\\Windows");
    seed_sz(k, "RegisteredOwner", REG_SZ, "Genesis");
    seed_sz(k, "InstallationType", REG_SZ, "Client");

    k = seed_key("MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion");
    seed_sz(k, "ProgramFilesDir", REG_SZ, "C:\\Program Files");
    seed_sz(k, "CommonFilesDir", REG_SZ, "C:\\Program Files\\Common Files");
    seed_sz(k, "ProgramFilesDir (x86)", REG_SZ, "C:\\Program Files (x86)");

    k = seed_key("MACHINE\\SYSTEM\\CurrentControlSet\\Control\\"
                 "Session Manager\\Environment");
    seed_sz(k, "OS", REG_SZ, "Windows_NT");
    seed_sz(k, "ComSpec", REG_EXPAND_SZ, "%SystemRoot%\\system32\\cmd.exe");
    seed_sz(k, "Path", REG_EXPAND_SZ,
            "%SystemRoot%\\system32;%SystemRoot%;C:\\bin");
    seed_sz(k, "PROCESSOR_ARCHITECTURE", REG_SZ, "AMD64");
    decimal(num, (uint32)smp_cpu_count());
    seed_sz(k, "NUMBER_OF_PROCESSORS", REG_SZ, num);
    seed_sz(k, "TEMP", REG_EXPAND_SZ, "%SystemRoot%\\TEMP");

    k = seed_key("MACHINE\\SYSTEM\\CurrentControlSet\\Control\\"
                 "ComputerName\\ComputerName");
    seed_sz(k, "ComputerName", REG_SZ, "GENESIS");

    k = seed_key("MACHINE\\HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0");
    seed_sz(k, "Identifier", REG_SZ, "Intel64 Family 6");
    {
        uint64 hz = timer_tsc_hz();

        seed_dword(k, "~MHz", (uint32)(hz / 1000000ULL));
    }
}
