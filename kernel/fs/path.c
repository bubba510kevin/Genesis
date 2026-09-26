#include "path.h"
#include "typesk.h"

/* Build the result in place, one component at a time.
 *
 * `out` holds the answer so far, always in its final form: leading slash, no
 * trailing slash, no empty components. ".." is applied by truncating back to
 * the previous slash, which is why the buffer is written forwards and never
 * needs a second pass or a component array to unwind. */

static uint64 str_len(const char *s) {
    uint64 n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

/* Drop the last component. "/a/b" -> "/a", "/a" -> "/", "/" -> "/". */
static void pop_component(char *out, uint64 *len) {
    uint64 n = *len;

    while (n > 1 && out[n - 1] != '/') {
        n--;
    }
    if (n > 1) {
        n--;            /* remove the slash itself, unless it is the root's */
    }
    out[n] = '\0';
    *len = n;
}

static int push_component(char *out, uint64 *len, const char *comp,
                          uint64 comp_len, uint64 outsz) {
    uint64 n = *len;
    uint64 need = comp_len + (n > 1 ? 1u : 0u);   /* a separator unless at "/" */
    uint64 i;

    if (n + need + 1 > outsz) {
        return PATH_ERR_TOOLONG;
    }
    if (n > 1) {
        out[n++] = '/';
    }
    for (i = 0; i < comp_len; i++) {
        out[n++] = comp[i];
    }
    out[n] = '\0';
    *len = n;
    return PATH_OK;
}

int path_normalize(const char *cwd, const char *in, char *out, uint64 outsz) {
    uint64 len = 0;
    uint64 i = 0;
    const char *src;

    if (cwd == NULL || in == NULL || out == NULL || outsz < 2) {
        return PATH_ERR_INVAL;
    }

    /* An absolute input discards the cwd entirely - that is the whole point of
     * the leading slash, and getting it backwards makes "cd /bin" from /usr
     * land in /usr/bin. */
    if (in[0] == '/') {
        out[0] = '/';
        out[1] = '\0';
        len = 1;
        src = in;
    } else {
        /* Seed with the cwd. A caller that passes a relative cwd has a bug
         * upstream; treat it as the root rather than producing a path that is
         * relative to nothing. */
        if (cwd[0] != '/') {
            out[0] = '/';
            out[1] = '\0';
            len = 1;
        } else {
            uint64 n = str_len(cwd);
            if (n + 1 > outsz) {
                return PATH_ERR_TOOLONG;
            }
            for (i = 0; i < n; i++) {
                out[i] = cwd[i];
            }
            out[n] = '\0';
            len = n;
            /* Tolerate a cwd with a trailing slash rather than trusting the
             * caller to have normalized it. */
            while (len > 1 && out[len - 1] == '/') {
                out[--len] = '\0';
            }
        }
        src = in;
    }

    i = 0;
    for (;;) {
        uint64 start;
        uint64 clen;

        while (src[i] == '/') {
            i++;                    /* fold repeated and leading slashes */
        }
        if (src[i] == '\0') {
            break;
        }

        start = i;
        while (src[i] != '\0' && src[i] != '/') {
            i++;
        }
        clen = i - start;

        if (clen == 1 && src[start] == '.') {
            continue;               /* "." is a no-op */
        }
        if (clen == 2 && src[start] == '.' && src[start + 1] == '.') {
            /* Climbing above the root stays at the root. Not an error: "/.."
             * is "/" on every system, and a shell that says `cd ../../..` from
             * two levels down expects to arrive somewhere, not to fail. */
            pop_component(out, &len);
            continue;
        }

        {
            int rc = push_component(out, &len, src + start, clen, outsz);
            if (rc != PATH_OK) {
                return rc;
            }
        }
    }

    return PATH_OK;
}
