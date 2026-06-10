#include "harp/store.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* mkdir -p */
static int mkdirs(const char *path) {
    char tmp[600];
    size_t len = strlen(path);
    if (len >= sizeof tmp) return -1;
    memcpy(tmp, path, len + 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

int harp_store_open(harp_store *s, const char *dir) {
    if (strlen(dir) >= sizeof s->dir - 16) return -1;
    snprintf(s->dir, sizeof s->dir, "%s", dir);
    char path[600];
    snprintf(path, sizeof path, "%s/objects", dir);
    if (mkdirs(path) != 0) return -1;
    snprintf(path, sizeof path, "%s/refs", dir);
    if (mkdirs(path) != 0) return -1;
    return 0;
}

static void obj_path(const harp_store *s, const harp_hash *h, char *out, size_t outsz) {
    char hex[2 * HARP_HASH_LEN + 1];
    harp_hash_hex(h, hex);
    snprintf(out, outsz, "%s/objects/%s", s->dir, hex);
}

bool harp_store_have(const harp_store *s, const harp_hash *h) {
    char path[700];
    obj_path(s, h, path, sizeof path);
    return access(path, R_OK) == 0;
}

/* Write a file atomically: tmp in the same directory, fsync, rename. */
static int write_atomic(const char *path, const uint8_t *data, size_t len) {
    char tmp[760];
    snprintf(tmp, sizeof tmp, "%s.tmp.%d", path, (int)getpid());
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    if (len && fwrite(data, 1, len, f) != len) {
        fclose(f);
        unlink(tmp);
        return -1;
    }
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

int harp_store_put(harp_store *s, const uint8_t *enc, size_t len, harp_hash *out) {
    harp_hash h = harp_hash_compute(enc, len);
    if (out) *out = h;
    char path[700];
    obj_path(s, &h, path, sizeof path);
    if (access(path, R_OK) == 0) return 0; /* content-addressed: already present */
    return write_atomic(path, enc, len);
}

int harp_store_get(const harp_store *s, const harp_hash *h, harp_cbuf *out) {
    char path[700];
    obj_path(s, h, path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) harp_cbuf_put(out, buf, n);
    int err = ferror(f);
    fclose(f);
    return (err || out->oom) ? -1 : 0;
}

/* ---- refs ---- */

/* Ref names: [A-Za-z0-9._:-] plus '/' as hierarchy. No "..", no leading '/'. */
static bool ref_name_ok(const char *name) {
    size_t len = strlen(name);
    if (len == 0 || len >= HARP_REF_NAME_MAX) return false;
    if (name[0] == '/' || name[len - 1] == '/') return false;
    if (strstr(name, "..")) return false;
    for (const char *p = name; *p; p++) {
        char c = *p;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == ':' || c == '-' || c == '/'))
            return false;
    }
    return true;
}

static int ref_path(const harp_store *s, const char *name, char *out, size_t outsz) {
    if (!ref_name_ok(name)) return -1;
    snprintf(out, outsz, "%s/refs/%s", s->dir, name);
    return 0;
}

void harp_ref_encode(harp_cbuf *out, const harp_ref *r) {
    harp_cbor_map(out, 4);
    harp_cbor_uint(out, 0);
    harp_cbor_text(out, r->name);
    harp_cbor_uint(out, 1);
    if (r->unborn)
        harp_cbor_null(out);
    else
        harp_cbor_bytes(out, r->hash.b, HARP_HASH_LEN);
    harp_cbor_uint(out, 2);
    harp_cbor_uint(out, r->generation);
    harp_cbor_uint(out, 3);
    harp_cbor_bool(out, r->dirty);
}

bool harp_ref_decode(harp_cdec *d, harp_ref *r) {
    memset(r, 0, sizeof *r);
    r->unborn = true;
    uint64_t n;
    if (!harp_cdec_map(d, &n)) return false;
    bool have_name = false;
    for (uint64_t i = 0; i < n; i++) {
        uint64_t key;
        if (!harp_cdec_uint(d, &key)) return false;
        switch (key) {
            case 0: {
                const char *s;
                size_t sl;
                if (!harp_cdec_text(d, &s, &sl) || sl >= HARP_REF_NAME_MAX) return false;
                memcpy(r->name, s, sl);
                r->name[sl] = 0;
                have_name = true;
                break;
            }
            case 1:
                if (harp_cdec_peek_null(d)) {
                    if (!harp_cdec_null(d)) return false;
                    r->unborn = true;
                } else {
                    const uint8_t *p;
                    size_t pl;
                    if (!harp_cdec_bytes(d, &p, &pl) || pl != HARP_HASH_LEN) return false;
                    memcpy(r->hash.b, p, HARP_HASH_LEN);
                    r->unborn = false;
                }
                break;
            case 2:
                if (!harp_cdec_uint(d, &r->generation)) return false;
                break;
            case 3:
                if (!harp_cdec_bool(d, &r->dirty)) return false;
                break;
            default:
                if (!harp_cdec_skip(d)) return false;
        }
    }
    return have_name;
}

int harp_store_ref_read(const harp_store *s, const char *name, harp_ref *r) {
    char path[700];
    if (ref_path(s, name, path, sizeof path) != 0) return -1;
    memset(r, 0, sizeof *r);
    snprintf(r->name, sizeof r->name, "%s", name);
    r->unborn = true;
    FILE *f = fopen(path, "rb");
    if (!f) return 0; /* unborn */
    uint8_t buf[1024];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    harp_cdec d;
    harp_cdec_init(&d, buf, n);
    harp_ref tmp;
    if (!harp_ref_decode(&d, &tmp)) return -1;
    *r = tmp;
    return 0;
}

int harp_store_ref_write(harp_store *s, const harp_ref *r) {
    char path[700];
    if (ref_path(s, r->name, path, sizeof path) != 0) return -1;
    /* ensure parent dirs exist for hierarchical names (archive/..., live/...) */
    char dir[700];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = 0;
        if (mkdirs(dir) != 0) return -1;
    }
    harp_cbuf b;
    harp_cbuf_init(&b);
    harp_ref_encode(&b, r);
    int rc = b.oom ? -1 : write_atomic(path, b.buf, b.len);
    harp_cbuf_free(&b);
    return rc;
}

static int walk_refs(const harp_store *s, const char *rel, harp_ref_cb cb, void *ud) {
    char path[700];
    snprintf(path, sizeof path, "%s/refs%s%s", s->dir, *rel ? "/" : "", rel);
    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (strstr(e->d_name, ".tmp.")) continue;
        char crel[HARP_REF_NAME_MAX];
        if ((size_t)snprintf(crel, sizeof crel, "%s%s%s", rel, *rel ? "/" : "", e->d_name) >=
            sizeof crel)
            continue;
        char cpath[700];
        snprintf(cpath, sizeof cpath, "%s/refs/%s", s->dir, crel);
        struct stat st;
        if (stat(cpath, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            walk_refs(s, crel, cb, ud);
        } else {
            harp_ref r;
            if (harp_store_ref_read(s, crel, &r) == 0) cb(&r, ud);
        }
    }
    closedir(d);
    return 0;
}

int harp_store_ref_list(const harp_store *s, harp_ref_cb cb, void *ud) {
    return walk_refs(s, "", cb, ud);
}
