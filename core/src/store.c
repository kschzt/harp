#include "harp/store.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <direct.h>  /* _mkdir  */
#  include <io.h>      /* _access */
#else
#  include <dirent.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

/* Path buffer size. The on-disk ref name can be up to 3x the logical name on
 * Windows (every ':' -> "%3A"), so a store dir (<= sizeof harp_store::dir) plus
 * "refs/" plus a fully-escaped ref name must fit here with headroom for the
 * write_atomic ".tmp.<pid>" suffix. */
#define HARP_STORE_PATH_MAX 1024

/* ---- platform filesystem primitives ----
 *
 * The store's durability contract (store.h: "a crash leaves the old or the new
 * ref, never a hybrid") rests on three OS operations done exactly right:
 * directory creation, a durable flush to disk, and an atomic replace-rename.
 *
 * POSIX rename(2) atomically replaces an existing target; the Win32 CRT
 * rename() does NOT — it fails when the target exists — so the CAS would
 * silently break. We use MoveFileEx(REPLACE_EXISTING|WRITE_THROUGH) instead,
 * which gives an atomic same-volume replace whose metadata change is flushed.
 * fsync->_commit (FlushFileBuffers under the hood); mkdir->_mkdir. */

static int dir_make(const char *path) {
#ifdef _WIN32
    return _mkdir(path); /* sets errno==EEXIST on collision, like POSIX */
#else
    return mkdir(path, 0755);
#endif
}

static int path_readable(const char *path) {
#ifdef _WIN32
    return _access(path, 4 /* R_OK */);
#else
    return access(path, R_OK);
#endif
}

static int file_sync(FILE *f) {
#ifdef _WIN32
    return _commit(_fileno(f));
#else
    return fsync(fileno(f));
#endif
}

static int rename_atomic(const char *tmp, const char *path) {
#ifdef _WIN32
    return MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : -1;
#else
    return rename(tmp, path);
#endif
}

static int proc_id(void) {
#ifdef _WIN32
    return (int)GetCurrentProcessId();
#else
    return (int)getpid();
#endif
}

/* 0 if path exists (*is_dir set), -1 otherwise. */
static int path_kind(const char *path, int *is_dir) {
#ifdef _WIN32
    DWORD a = GetFileAttributesA(path);
    if (a == INVALID_FILE_ATTRIBUTES) return -1;
    *is_dir = (a & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
    return 0;
#else
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    *is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
    return 0;
#endif
}

/* ---- ref name <-> on-disk name ----
 *
 * Ref names are logical identifiers ([A-Za-z0-9._:/-], '/' = hierarchy). On
 * POSIX they map 1:1 to filesystem paths. On Windows ':' is illegal in a file
 * name (NTFS reserves it for alternate data streams), and archive refs are
 * ISO-8601 timestamps full of colons (archive/2026-06-10T12:00:00Z). We
 * percent-escape ':' as "%3A" in the ON-DISK name only — logical names (the
 * API, the wire, cross-platform store interchange) keep the colon. '%' cannot
 * occur in a valid ref name, so the escape is unambiguous and reversible. '/'
 * is preserved as the path separator. On POSIX both maps are the identity, so
 * on-disk layout is byte-identical to before. */
#ifdef _WIN32
static bool fs_from_ref(const char *ref, char *out, size_t outsz) {
    size_t o = 0;
    for (const char *p = ref; *p; p++) {
        if (*p == ':') {
            if (o + 3 >= outsz) return false;
            out[o++] = '%';
            out[o++] = '3';
            out[o++] = 'A';
        } else {
            if (o + 1 >= outsz) return false;
            out[o++] = *p;
        }
    }
    if (o >= outsz) return false;
    out[o] = 0;
    return true;
}
static bool ref_from_fs(const char *fs, char *out, size_t outsz) {
    size_t o = 0;
    for (const char *p = fs; *p;) {
        if (p[0] == '%' && p[1] == '3' && (p[2] == 'A' || p[2] == 'a')) {
            if (o + 1 >= outsz) return false;
            out[o++] = ':';
            p += 3;
        } else {
            if (o + 1 >= outsz) return false;
            out[o++] = *p++;
        }
    }
    if (o >= outsz) return false;
    out[o] = 0;
    return true;
}
#else
static bool fs_from_ref(const char *ref, char *out, size_t outsz) {
    size_t n = strlen(ref);
    if (n + 1 > outsz) return false;
    memcpy(out, ref, n + 1);
    return true;
}
static bool ref_from_fs(const char *fs, char *out, size_t outsz) {
    return fs_from_ref(fs, out, outsz); /* identity on POSIX */
}
#endif

/* mkdir -p */
static int mkdirs(const char *path) {
    char tmp[HARP_STORE_PATH_MAX];
    size_t len = strlen(path);
    if (len >= sizeof tmp) return -1;
    memcpy(tmp, path, len + 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (dir_make(tmp) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (dir_make(tmp) != 0 && errno != EEXIST) return -1;
    return 0;
}

int harp_store_open(harp_store *s, const char *dir) {
    if (strlen(dir) >= sizeof s->dir - 16) return -1;
    snprintf(s->dir, sizeof s->dir, "%s", dir);
    char path[HARP_STORE_PATH_MAX];
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
    char path[HARP_STORE_PATH_MAX];
    obj_path(s, h, path, sizeof path);
    return path_readable(path) == 0;
}

/* Test seam (§T10 crash-atomicity): when set, write_atomic stops AFTER the tmp is
 * written + fsync'd but BEFORE the atomic rename — leaving the complete tmp orphaned
 * (the store ignores .tmp.* on read) and the target file UNCHANGED, exactly like a
 * crash in the torn window. Default false; ONLY a test sets it, to prove that a crash
 * leaves the ref old-or-new, never a hybrid. */
bool harp_store_fault_skip_rename = false;

/* Write a file atomically: tmp in the same directory, fsync, rename. */
static int write_atomic(const char *path, const uint8_t *data, size_t len) {
    char tmp[HARP_STORE_PATH_MAX + 64];
    snprintf(tmp, sizeof tmp, "%s.tmp.%d", path, proc_id());
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    if (len && fwrite(data, 1, len, f) != len) {
        fclose(f);
        remove(tmp);
        return -1;
    }
    fflush(f);
    if (file_sync(f) != 0) { /* durability: data must hit disk before the rename */
        fclose(f);
        remove(tmp);
        return -1;
    }
    fclose(f);
    if (harp_store_fault_skip_rename) return -1; /* simulated crash: tmp synced, rename never happens */
    if (rename_atomic(tmp, path) != 0) {
        remove(tmp);
        return -1;
    }
    return 0;
}

int harp_store_put(harp_store *s, const uint8_t *enc, size_t len, harp_hash *out) {
    harp_hash h = harp_hash_compute(enc, len);
    if (out) *out = h;
    char path[HARP_STORE_PATH_MAX];
    obj_path(s, &h, path, sizeof path);
    if (path_readable(path) == 0) return 0; /* content-addressed: already present */
    return write_atomic(path, enc, len);
}

int harp_store_get(const harp_store *s, const harp_hash *h, harp_cbuf *out) {
    char path[HARP_STORE_PATH_MAX];
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
    char fsname[3 * HARP_REF_NAME_MAX];
    if (!fs_from_ref(name, fsname, sizeof fsname)) return -1;
    int n = snprintf(out, outsz, "%s/refs/%s", s->dir, fsname);
    return (n < 0 || (size_t)n >= outsz) ? -1 : 0; /* refuse a truncated path */
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
                    if (!harp_hash_read(d, &r->hash)) return false;
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
    char path[HARP_STORE_PATH_MAX];
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
    char path[HARP_STORE_PATH_MAX];
    if (ref_path(s, r->name, path, sizeof path) != 0) return -1;
    /* ensure parent dirs exist for hierarchical names (archive/..., live/...) */
    char dir[HARP_STORE_PATH_MAX];
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

static int walk_refs(const harp_store *s, const char *rel, harp_ref_cb cb, void *ud);

/* Process one directory entry (on-disk name) during the ref walk. rel is the
 * LOGICAL relative path so far; dname is the raw on-disk component (escaped). */
static void walk_one(const harp_store *s, const char *rel, const char *dname, harp_ref_cb cb,
                     void *ud) {
    if (dname[0] == '.') return;            /* ".", "..", and dotfiles */
    if (strstr(dname, ".tmp.")) return;     /* in-flight write_atomic temporaries */
    char dlog[HARP_REF_NAME_MAX];
    if (!ref_from_fs(dname, dlog, sizeof dlog)) return; /* on-disk -> logical component */
    char crel[HARP_REF_NAME_MAX];
    if ((size_t)snprintf(crel, sizeof crel, "%s%s%s", rel, *rel ? "/" : "", dlog) >= sizeof crel)
        return;
    char cpath[HARP_STORE_PATH_MAX];
    if (ref_path(s, crel, cpath, sizeof cpath) != 0) return; /* re-escapes to on-disk path */
    int is_dir;
    if (path_kind(cpath, &is_dir) != 0) return;
    if (is_dir) {
        walk_refs(s, crel, cb, ud);
    } else {
        harp_ref r;
        if (harp_store_ref_read(s, crel, &r) == 0) cb(&r, ud);
    }
}

static int walk_refs(const harp_store *s, const char *rel, harp_ref_cb cb, void *ud) {
    char path[HARP_STORE_PATH_MAX];
    if (*rel) {
        char fsrel[3 * HARP_REF_NAME_MAX];
        if (!fs_from_ref(rel, fsrel, sizeof fsrel)) return -1;
        if ((size_t)snprintf(path, sizeof path, "%s/refs/%s", s->dir, fsrel) >= sizeof path)
            return -1;
    } else {
        snprintf(path, sizeof path, "%s/refs", s->dir);
    }
#ifdef _WIN32
    char glob[HARP_STORE_PATH_MAX + 64];
    snprintf(glob, sizeof glob, "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(glob, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    do {
        walk_one(s, rel, fd.cFileName, cb, ud);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d))) walk_one(s, rel, e->d_name, cb, ud);
    closedir(d);
#endif
    return 0;
}

int harp_store_ref_list(const harp_store *s, harp_ref_cb cb, void *ud) {
    return walk_refs(s, "", cb, ud);
}
