// Adapted from MacNativeGames games/totk/runtime/cloud_save.m @ 6867ab2d (MIT, Copyright (c) 2026 MacNativeGames
// contributors): the folder layout, sync.plist, the save hash and the rules for which save wins, in C on platform.h in
// place of Foundation.
//
// Save sync through a folder the player chooses (totk.ini save_sync): one save shared by this port and MacNativeGames'
// on other computers. The folder is anything each computer sees as a local folder and some other program keeps in step,
// such as a Dropbox, OneDrive or iCloud Drive folder; this file only reads and writes files in it.
//
//   <save_sync>/Tears of the Kingdom/Users/<uid>/Saves/current/            a plain copy of the save
//   <save_sync>/Tears of the Kingdom/Users/<uid>/Saves/current/sync.plist  uid, gen, device, time, hash
//   <profile>/cloud-sync/<uid>.plist                 this computer's view: the generation and hash it last synced
//   <profile>/cloud-backups/<uid>/<time>-local-<id>  the local saves a download replaced, the newest 10
//
// The committed save (<profile>/save/<uid>, fs_host.c) stays the working copy. gen counts uploads: a computer uploads
// the generation after the highest it knows. At start, before the game reads a save (fs_host.c storage_init), the
// folder's copy replaces the local save when another computer uploaded since this one last synced; after a Commit, the
// save goes up once the game has written nothing for 3 s, and again at exit (fs_save_flush). A computer whose game is
// still running when another one uploads does not upload over that save: it keeps its saves local and its next start
// takes the other computer's save ("superseded" in the state file). A computer that never synced uploads only into a
// folder still empty at its first save, so it never replaces a save it has not seen. When both changed, the one saved
// last wins (sync.plist's time against the newest local file); the other one is kept in cloud-backups. Nothing inside a
// save is merged.
//
// The hash, as cloud_save.m save_hash: SHA-256 over the save's regular files sorted by relative path ('/' separators),
// leaving out storage/ (the game rewrites it while nobody plays), .DS_Store, iCloud placeholders (.<name>.icloud) and
// the top-level sync.plist. Each file adds its path and a NUL, its size as a little-endian uint64, then its bytes.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "fs.h"
#include "host.h"
#include "platform/platform.h"
#include "sha256.h"

#define GAME_NAME "Tears of the Kingdom"   // MacNativeGames game_version.h GAME_NAME: the folder both ports use
#define BACKUPS 10
#define PATHN (PLAT_PATH_MAX * 2)
#define LOG(...) rt_log("save sync: " __VA_ARGS__)

static int on;
static char g_local[PATHN], g_user[33], g_cloud[PATHN], g_cur[PATHN], g_state[PATHN], g_backups[PATHN], g_work[PATHN];
static int (*g_snapshot)(const char *dst);
static int g_superseded, g_identity_conflict;
static int64_t g_start_gen;   // the generation the start compared this computer's save with (0: the folder had none)
static PlatMutex push_lock = PLAT_MUTEX_INIT;   // one push at a time: the worker's and the exit's
static PlatMutex wake_lock = PLAT_MUTEX_INIT;
static PlatCond wake_cv = PLAT_COND_INIT;
static volatile int32_t pending;   // a Commit since the last push

static int exists(const char *p) { return plat_fs_kind(p) != PLAT_FS_MISSING; }

// ---- the save's files
typedef struct { char **v; int n, cap; } Names;

static void names_add(Names *l, const char *s) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 32;
        l->v = realloc(l->v, (size_t)l->cap * sizeof *l->v);
        if (!l->v) rt_fatal("save sync: out of memory");
    }
    if (!(l->v[l->n++] = strdup(s))) rt_fatal("save sync: out of memory");
}

static void names_free(Names *l) {
    for (int i = 0; i < l->n; i++) free(l->v[i]);
    free(l->v);
    *l = (Names){0};
}

typedef struct { Names files, dirs; } Entries;

static int entry_add(const char *name, int kind, uint64_t size, void *ctx) {
    (void)size;
    Entries *e = ctx;
    if (kind == PLAT_FS_FILE) names_add(&e->files, name);
    else if (kind == PLAT_FS_DIR) names_add(&e->dirs, name);   // links are left out, as copy_tree does
    return 1;
}

static int str_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static int skipped(const char *rel, const char *name) {
    size_t n = strlen(name);
    return !strcmp(name, ".DS_Store") || !strcmp(rel, "sync.plist") ||
           (name[0] == '.' && n > 7 && !strcmp(name + n - 7, ".icloud"));
}

// The relative paths of dir's regular files, sorted; progress_only leaves out storage/. 0 on success.
static int walk(const char *root, const char *rel, Names *out, int progress_only) {
    char dir[PATHN], r[PATHN];
    snprintf(dir, sizeof dir, rel ? "%s/%s" : "%s", root, rel);
    Entries e = {0};
    if (plat_fs_list(dir, entry_add, &e)) return -1;
    int bad = 0;
    for (int i = 0; i < e.files.n; i++) {
        snprintf(r, sizeof r, rel ? "%s/%s" : "%s%s", rel ? rel : "", e.files.v[i]);
        if (skipped(r, e.files.v[i]) || (progress_only && !strncmp(r, "storage/", 8))) continue;
        names_add(out, r);
    }
    for (int i = 0; i < e.dirs.n && !bad; i++) {
        snprintf(r, sizeof r, rel ? "%s/%s" : "%s%s", rel ? rel : "", e.dirs.v[i]);
        bad = walk(root, r, out, progress_only);
    }
    names_free(&e.files);
    names_free(&e.dirs);
    if (!rel) qsort(out->v, (size_t)out->n, sizeof *out->v, str_cmp);
    return bad;
}

static int file_count(const char *dir) {
    Names l = {0};
    int n = walk(dir, NULL, &l, 0) ? 0 : l.n;
    names_free(&l);
    return n;
}

// The save hash of dir as 64 hex digits; 0 when a file cannot be read (still arriving from another computer).
static int save_hash(const char *dir, char out[65]) {
    Names l = {0};
    if (walk(dir, NULL, &l, 1)) {
        names_free(&l);
        return 0;
    }
    Sha256 s;
    sha256_init(&s);
    enum { CHUNK = 1 << 20 };
    uint8_t *buf = malloc(CHUNK);
    if (!buf) rt_fatal("save sync: out of memory");
    int ok = 1;
    for (int i = 0; i < l.n && ok; i++) {
        char p[PATHN];
        snprintf(p, sizeof p, "%s/%s", dir, l.v[i]);
        PlatFile *f = plat_fs_open(p, 0, NULL);
        PlatFileId id;
        if (!f || !plat_file_id(f, &id)) {
            if (f) plat_file_close(f);
            ok = 0;
            break;
        }
        uint8_t len[8];
        for (int k = 0; k < 8; k++) len[k] = (uint8_t)(id.size >> (8 * k));
        sha256_update(&s, l.v[i], strlen(l.v[i]) + 1);
        sha256_update(&s, len, sizeof len);
        uint64_t at = 0;
        for (;;) {
            int64_t got = plat_file_pread(f, buf, CHUNK, at);
            if (got <= 0) {
                ok = got == 0 && at == id.size;
                break;
            }
            sha256_update(&s, buf, (size_t)got);
            at += (uint64_t)got;
        }
        plat_file_close(f);
    }
    free(buf);
    names_free(&l);
    if (!ok) return 0;
    uint8_t h[32];
    sha256_final(&s, h);
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", h[i]);
    return 1;
}

// The newest last-write time of dir's progress files, seconds since 1970.
static double newest_mtime(const char *dir) {
    Names l = {0};
    double t = 0;
    if (!walk(dir, NULL, &l, 1))
        for (int i = 0; i < l.n; i++) {
            char p[PATHN];
            snprintf(p, sizeof p, "%s/%s", dir, l.v[i]);
            int64_t ns = plat_file_mtime(p);
            if (ns > 0 && ns / 1e9 > t) t = ns / 1e9;
        }
    names_free(&l);
    return t;
}

// ---- property lists: the XML form NSDictionary writeToFile:atomically: writes and reads, for flat dictionaries of
// strings, integers and reals.
typedef struct {
    int has_gen, has_superseded, has_time;
    int64_t gen, superseded;
    double time;
    char uid[40], device[256], hash[72];
} Plist;

static void xml_text(char *out, size_t cap, const char *s, size_t n) {
    static const struct { const char *ent; char c; } ents[] = {
        {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < cap; i++) {
        char c = s[i];
        if (c == '&')
            for (size_t k = 0; k < sizeof ents / sizeof *ents; k++)
                if (!strncmp(s + i, ents[k].ent, strlen(ents[k].ent))) {
                    c = ents[k].c, i += strlen(ents[k].ent) - 1;
                    break;
                }
        out[o++] = c;
    }
    out[o] = 0;
}

// 1 when path holds an XML property list with a dictionary; the keys this file uses go into *p.
static int plist_read(const char *path, Plist *p) {
    *p = (Plist){0};
    uint64_t size = 0;
    char *s = (char *)plat_file_read(path, &size);
    if (!s) return 0;
    int ok = strstr(s, "<plist") && strstr(s, "<dict>");
    for (const char *k = s; ok && (k = strstr(k, "<key>"));) {
        k += 5;
        const char *ke = strstr(k, "</key>");
        if (!ke) break;
        char key[32];
        xml_text(key, sizeof key, k, (size_t)(ke - k));
        const char *v = ke + 6;
        while (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n') v++;
        char val[256] = "";
        const char *tags[] = {"string", "integer", "real"};
        int type = -1;
        for (int t = 0; t < 3 && type < 0; t++) {
            char open[16], close[16], empty[16];
            snprintf(open, sizeof open, "<%s>", tags[t]);
            snprintf(close, sizeof close, "</%s>", tags[t]);
            snprintf(empty, sizeof empty, "<%s/>", tags[t]);
            if (!strncmp(v, empty, strlen(empty))) type = t;
            else if (!strncmp(v, open, strlen(open))) {
                const char *ve = strstr(v, close);
                if (!ve) break;
                xml_text(val, sizeof val, v + strlen(open), (size_t)(ve - v - strlen(open)));
                type = t;
            }
        }
        k = v;
        if (type < 0) continue;
        if (!strcmp(key, "uid") && type == 0) snprintf(p->uid, sizeof p->uid, "%s", val);
        else if (!strcmp(key, "device") && type == 0) snprintf(p->device, sizeof p->device, "%s", val);
        else if (!strcmp(key, "hash") && type == 0) snprintf(p->hash, sizeof p->hash, "%s", val);
        else if (!strcmp(key, "gen") && type == 1) p->gen = strtoll(val, NULL, 10), p->has_gen = 1;
        else if (!strcmp(key, "superseded") && type == 1) p->superseded = strtoll(val, NULL, 10), p->has_superseded = 1;
        else if (!strcmp(key, "time") && type != 0) p->time = strtod(val, NULL), p->has_time = 1;
    }
    free(s);
    return ok;
}

static size_t xml_escape(char *out, size_t cap, const char *s) {
    size_t o = 0;
    for (; *s && o + 7 < cap; s++) {
        const char *e = *s == '&' ? "&amp;" : *s == '<' ? "&lt;" : *s == '>' ? "&gt;" : NULL;
        if (e) o += (size_t)snprintf(out + o, cap - o, "%s", e);
        else out[o++] = *s;
    }
    out[o] = 0;
    return o;
}

// Keys in sorted order, as CoreFoundation writes them. full: a sync.plist (uid, device, time); else a state file.
static int plist_write(const char *path, const Plist *p, int full) {
    char buf[2048], dev[512];
    int n = snprintf(buf, sizeof buf,
                     "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
                     "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\">\n<dict>\n");
    if (full) {
        xml_escape(dev, sizeof dev, p->device);
        n += snprintf(buf + n, sizeof buf - n, "\t<key>device</key>\n\t<string>%s</string>\n", dev);
    }
    n += snprintf(buf + n, sizeof buf - n, "\t<key>gen</key>\n\t<integer>%lld</integer>\n\t<key>hash</key>\n\t<string>%s</string>\n",
                  (long long)p->gen, p->hash);
    if (!full && p->has_superseded)
        n += snprintf(buf + n, sizeof buf - n, "\t<key>superseded</key>\n\t<integer>%lld</integer>\n", (long long)p->superseded);
    if (full)
        n += snprintf(buf + n, sizeof buf - n, "\t<key>time</key>\n\t<real>%.17g</real>\n\t<key>uid</key>\n\t<string>%s</string>\n",
                      p->time, p->uid);
    n += snprintf(buf + n, sizeof buf - n, "</dict>\n</plist>\n");
    return n < (int)sizeof buf && plat_file_write_atomic(path, buf, (size_t)n);
}

// The folder's sync.plist (dir: current or a copy of it). 1 when it is there and belongs to this user; a file of
// another user, or one without a generation or hash, marks g_identity_conflict: a copied or misconfigured folder must
// not become another account's save, and this run leaves it alone.
static int read_cloud_meta(const char *dir, Plist *m) {
    char p[PATHN];
    snprintf(p, sizeof p, "%s/sync.plist", dir);
    if (!plist_read(p, m)) return 0;
    if (strcmp(m->uid, g_user) || !m->has_gen || m->gen < 1 || strlen(m->hash) != 64) {
        g_identity_conflict = 1;
        LOG("the folder's save belongs to another or no user; left untouched: %s", p);
        return 0;
    }
    return 1;
}

static int read_state(Plist *s) { return plist_read(g_state, s); }

static void write_state(int64_t gen, const char *hash) {
    Plist s = {.gen = gen};
    snprintf(s.hash, sizeof s.hash, "%s", hash);
    if (!plist_write(g_state, &s, 0)) LOG("cannot write %s", g_state);
}

static void write_superseded(const Plist *state, int64_t gen) {
    Plist s = state ? *state : (Plist){0};
    s.superseded = gen, s.has_superseded = 1;
    if (!plist_write(g_state, &s, 0)) LOG("cannot write %s", g_state);
}

// ---- folders
static void rand_id(char out[9]) {
    uint8_t b[4] = {0};
    if (!plat_random(b, sizeof b)) {
        uint64_t t = plat_now_ns();
        memcpy(b, &t, sizeof b);
    }
    snprintf(out, 9, "%02x%02x%02x%02x", b[0], b[1], b[2], b[3]);
}

static int fetch(const char *incoming, int64_t want_gen, Plist *m);

static int backup_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

// Keep the newest BACKUPS of cloud-backups/<uid> (the names start with the time).
static void prune_backups(void) {
    Entries e = {0};
    plat_fs_list(g_backups, entry_add, &e);
    qsort(e.dirs.v, (size_t)e.dirs.n, sizeof *e.dirs.v, backup_cmp);
    for (int i = 0; i + BACKUPS < e.dirs.n; i++) {
        char p[PATHN];
        snprintf(p, sizeof p, "%s/%s", g_backups, e.dirs.v[i]);
        fs_tree_remove(p);
    }
    names_free(&e.files);
    names_free(&e.dirs);
}

// A new folder name in cloud-backups/<uid>: <UTC time>-<what>-<random id>, so the names sort by time.
static void backup_name(char *out, size_t cap, const char *what) {
    uint64_t ns = plat_unix_time_ns();
    struct tm t;
    char id[9];
    plat_gmtime((int64_t)(ns / 1000000000u), &t);
    rand_id(id);
    plat_file_mkdir_p(g_backups);
    snprintf(out, cap, "%s/%04d%02d%02d-%02d%02d%02d-%s-%s", g_backups, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec, what, id);
}

// Replace the local save with the verified copy in incoming. The local save, if it has files, moves to cloud-backups
// (a rename in the profile: nothing is copied); a failure leaves it where it was.
static int install(const char *incoming, int keep_local) {
    char old[PATHN] = "";
    if (keep_local) {
        backup_name(old, sizeof old, "local");
        if (plat_fs_rename(g_local, old, 1)) {
            LOG("cannot move the local save to %s; it stays in use", old);
            return 0;
        }
        LOG("kept the local save in %s", old);
    } else if (fs_tree_remove(g_local)) {
        LOG("cannot clear the empty local save folder; it stays in use");
        return 0;
    }
    if (plat_fs_rename(incoming, g_local, 1)) {
        LOG("cannot move the folder's save into place; the local save stays in use");
        if (*old) plat_fs_rename(old, g_local, 1);
        else plat_file_mkdir_p(g_local);
        return 0;
    }
    if (*old) prune_backups();
    return 1;
}

// ---- upload
// Write the local save to the folder as the next generation, push_lock held.
static void push(void) {
    if (!on || g_superseded || g_identity_conflict) return;
    char snap[PATHN], tmp[PATHN], old[PATHN], plist[PATHN], hash[65], id[9];
    int64_t gen = 0;
    snprintf(snap, sizeof snap, "%s/upload", g_work);
    fs_tree_remove(snap);
    if (g_snapshot(snap)) {
        LOG("cannot take a snapshot of the save; the upload waits for the next save");
        fs_tree_remove(snap);
        return;
    }
    Plist st, m, latest, meta = {0};
    int has_state = read_state(&st), has_meta, has_latest;
    if (!save_hash(snap, hash) || !file_count(snap)) goto done;
    has_meta = read_cloud_meta(g_cur, &m);
    if (g_identity_conflict) goto done;
    if (!has_meta && exists(g_cur)) {
        LOG("the folder's save has no complete sync.plist; the upload waits rather than replacing it");
        goto done;
    }
    if (!has_meta && has_state) {
        LOG("the folder's save is not visible right now; this one waits for the next save");
        goto done;
    }
    if (!has_state) {
        // Never synced: upload only into a folder still empty now. A save that appeared since the start is not
        // overwritten; the next start takes it and keeps this computer's save in cloud-backups.
        if (has_meta && m.gen != g_start_gen) {
            g_superseded = 1;
            write_superseded(NULL, m.gen);
            LOG("a save from %s (generation %lld) is in the folder now: this computer's saves stay local, and its next "
                "start takes that one (keeping this one in cloud-backups)", m.device, (long long)m.gen);
            goto done;
        }
    }
    if (has_meta && has_state && m.gen > st.gen) {   // another computer uploaded while this game ran
        g_superseded = 1;
        write_superseded(&st, m.gen);
        LOG("%s uploaded a newer save (generation %lld): this computer's saves stay local, and its next start takes "
            "that one", m.device, (long long)m.gen);
        goto done;
    }
    if (has_state && has_meta && !strcmp(hash, st.hash) && m.gen == st.gen) goto done;   // nothing new on either side
    if (has_meta) gen = m.gen;
    if (has_state && st.gen > gen) gen = st.gen;
    gen++;
    plat_file_mkdir_p(g_cloud);
    rand_id(id);
    snprintf(tmp, sizeof tmp, "%s.incoming-%s", g_cur, id);
    snprintf(old, sizeof old, "%s.old-%s", g_cur, id);
    // The copy and its sync.plist are complete before current is swapped, so a reader never finds current without one.
    meta = (Plist){.gen = gen, .time = plat_unix_time_ns() / 1e9, .has_gen = 1, .has_time = 1};
    snprintf(meta.uid, sizeof meta.uid, "%s", g_user);
    snprintf(meta.hash, sizeof meta.hash, "%s", hash);
    if (!plat_host_name(meta.device, sizeof meta.device)) snprintf(meta.device, sizeof meta.device, "?");
    snprintf(plist, sizeof plist, "%s/sync.plist", tmp);
    if (fs_tree_copy(snap, tmp) || !plist_write(plist, &meta, 1)) {
        LOG("cannot write to %s; this save stays local", g_cloud);
        fs_tree_remove(tmp);
        goto done;
    }
    // Look again just before the swap: another computer can upload while this one copies.
    has_latest = read_cloud_meta(g_cur, &latest);
    if (g_identity_conflict || has_latest != has_meta ||
        (has_latest && (latest.gen != m.gen || strcmp(latest.hash, m.hash)))) {
        LOG("the folder's generation changed while preparing the upload; this save stays local");
        fs_tree_remove(tmp);
        goto done;
    }
    if (exists(g_cur) && plat_fs_rename(g_cur, old, 1)) {
        LOG("cannot replace %s (a file in it is in use?); this save stays local", g_cur);
        fs_tree_remove(tmp);
        goto done;
    }
    if (plat_fs_rename(tmp, g_cur, 1)) {
        LOG("cannot move the upload into place; this save stays local");
        if (exists(old)) plat_fs_rename(old, g_cur, 1);
        fs_tree_remove(tmp);
        goto done;
    }
    fs_tree_remove(old);
    write_state(gen, hash);
    LOG("saved to the sync folder (generation %lld)", (long long)gen);
done:
    fs_tree_remove(snap);
}

// The upload thread: after a Commit, once the game has written no save data for 3 s.
static void worker(void *arg) {
    (void)arg;
    for (;;) {
        plat_mutex_lock(&wake_lock);
        while (!plat_cas32(&pending, 0, 0)) plat_cond_wait(&wake_cv, &wake_lock);
        plat_mutex_unlock(&wake_lock);
        do plat_sleep_ms(3000);
        while (fs_save_busy());
        if (!plat_xchg32(&pending, 0)) continue;   // the exit took it
        plat_mutex_lock(&push_lock);
        push();
        plat_mutex_unlock(&push_lock);
    }
}

void save_sync_changed(void) {
    if (!on || plat_xchg32(&pending, 1)) return;
    plat_mutex_lock(&wake_lock);
    plat_cond_signal(&wake_cv);
    plat_mutex_unlock(&wake_lock);
}

// At exit: an upload still waiting, and a save written since this computer's last upload with none waiting (push
// does nothing when neither side changed). The game's last Commit is done (fs_save_flush waited for it).
void save_sync_quit(void) {
    if (!on) return;
    int had = plat_xchg32(&pending, 0);
    Plist st;
    if (!had && !read_state(&st)) return;
    for (int i = 0; i < 50 && fs_save_busy(); i++) plat_sleep_ms(100);
    if (fs_save_busy()) {
        LOG("a save is still being written at exit; no incomplete save uploaded");
        return;
    }
    plat_mutex_lock(&push_lock);
    push();
    plat_mutex_unlock(&push_lock);
}

// ---- start
// Paths and identity for save_dir; 0 when the setting or the save folder cannot be used.
static int setup(const char *save_dir, const char *profile, const char *root) {
    const char *base = save_dir + strlen(save_dir);
    while (base > save_dir && base[-1] != '/' && base[-1] != '\\') base--;
    if (strlen(base) != 32 || strspn(base, "0123456789abcdefABCDEF") != 32) {
        LOG("off: the save folder %s does not name a user", save_dir);
        return 0;
    }
    for (int i = 0; i < 32; i++) g_user[i] = (char)(base[i] | 0x20);
    g_user[32] = 0;
    size_t nr = strlen(root), nl = strlen(save_dir);
    if ((nl >= nr && !strncmp(save_dir, root, nr) && (save_dir[nr] == '/' || save_dir[nr] == '\\' || !save_dir[nr])) ||
        (nr >= nl && !strncmp(root, save_dir, nl) && (root[nl] == '/' || root[nl] == '\\'))) {
        LOG("off: the save folder %s is inside the sync folder %s (keep the profile out of it)", save_dir, root);
        return 0;
    }
    int bad = snprintf(g_local, sizeof g_local, "%s", save_dir) >= (int)sizeof g_local;
    bad |= snprintf(g_cloud, sizeof g_cloud, "%s/Users/%s/Saves", root, g_user) >= (int)sizeof g_cloud;
    bad |= snprintf(g_cur, sizeof g_cur, "%s/current", g_cloud) >= (int)sizeof g_cur;
    bad |= snprintf(g_work, sizeof g_work, "%s/cloud-sync", profile) >= (int)sizeof g_work;
    bad |= snprintf(g_state, sizeof g_state, "%s/%s.plist", g_work, g_user) >= (int)sizeof g_state;
    bad |= snprintf(g_backups, sizeof g_backups, "%s/cloud-backups/%s", profile, g_user) >= (int)sizeof g_backups;
    if (bad) {
        LOG("off: a path is too long");
        return 0;
    }
    plat_file_mkdir_p(g_work);
    if (!plat_dir_exists(g_work)) {
        LOG("off: cannot create %s", g_work);
        return 0;
    }
    g_superseded = g_identity_conflict = 0, g_start_gen = 0;
    return 1;
}

// Copy current into the profile and check it against its sync.plist; a sync client (Dropbox, OneDrive) brings the
// files one by one, so a copy that does not match yet is tried again for up to 20 s. 1 with *m the copy's sync.plist.
static int fetch(const char *incoming, int64_t want_gen, Plist *m) {
    char got[65], p[PATHN];
    for (int tries = 0; tries < 20; tries++) {
        if (tries) plat_sleep_ms(1000);
        fs_tree_remove(incoming);
        if (fs_tree_copy(g_cur, incoming)) continue;
        if (!read_cloud_meta(incoming, m)) {
            if (g_identity_conflict) break;
            continue;
        }
        snprintf(p, sizeof p, "%s/sync.plist", incoming);
        plat_fs_unlink(p);
        if (m->gen >= want_gen && save_hash(incoming, got) && !strcmp(got, m->hash)) return 1;
    }
    fs_tree_remove(incoming);
    return 0;
}

// The start's comparison of the folder's save with the local one (cloud_save.m runtime_cloud_save_start).
static void start_sync(void) {
    Plist st, m;
    int has_state = read_state(&st), has_meta = read_cloud_meta(g_cur, &m);
    if (g_identity_conflict) return;
    g_start_gen = has_meta ? m.gen : 0;
    char local[65] = "";
    int local_saves = file_count(g_local) > 0;
    if (local_saves && !save_hash(g_local, local)) {
        LOG("the local save cannot be read; nothing synced this run");
        g_identity_conflict = 1;
        return;
    }
    if (!has_meta) {
        // A computer that synced before but sees nothing has not received the folder's files yet: it uploads at its
        // next save. One that never synced uploads at its first save if the folder is still empty then.
        if (has_state) LOG("no save visible in the folder yet; this computer's save is used");
        else LOG("no save in the folder yet%s", local_saves ? "; this computer's save goes up at its first save in the "
                                                             "game, if the folder is still empty then" : "");
        return;
    }
    if (!strcmp(m.hash, local)) {
        write_state(m.gen, local);
        LOG("in sync (generation %lld)", (long long)m.gen);
        return;
    }
    int local_changed = local_saves && !(has_state && !strcmp(st.hash, local));
    if (has_state && m.gen == st.gen) {   // new progress goes up with the next save
        LOG("the folder has this computer's last upload (generation %lld)", (long long)m.gen);
        if (local_changed) save_sync_changed();
        return;
    }
    if (has_state && m.gen < st.gen) {
        // An older generation than this computer synced: another one wrote from an out-of-date view of the folder.
        LOG("the folder has generation %lld, older than this computer's %lld: uploading this computer's save",
            (long long)m.gen, (long long)st.gen);
        if (local_saves) save_sync_changed();
        return;
    }
    // Another computer uploaded since. Its save wins unless this computer also saved since its last sync, and later
    // (when the last run saw that upload and stopped, the upload wins whatever this one wrote after it).
    int superseded = has_state && st.has_superseded && m.gen >= st.superseded;
    double local_time = local_saves ? newest_mtime(g_local) : 0;
    char incoming[PATHN], kept[PATHN];
    snprintf(incoming, sizeof incoming, "%s/incoming", g_work);
    Plist got;
    if (local_changed && !superseded && local_time > m.time) {
        // The upload replaces the folder's save, so a copy of that goes to cloud-backups first. The state then names
        // the folder's generation as seen (cloud_save.m keeps the old one, and its push takes the newer generation for
        // another computer's upload during this run: it marks itself superseded and the next start drops this save).
        if (!fetch(incoming, m.gen, &got)) {
            // Not replaced unseen: no upload this run (g_superseded in memory only), and the next start decides again.
            if (!g_identity_conflict)
                LOG("both saved and this computer's save is newer, but the folder's save is incomplete (still "
                    "syncing?); this computer's save is used and goes up at a later start");
            g_superseded = 1;
            return;
        }
        backup_name(kept, sizeof kept, "folder");
        if (plat_fs_rename(incoming, kept, 1)) {
            LOG("cannot keep the folder's save in %s; this computer's save is used and goes up at a later start", kept);
            fs_tree_remove(incoming);
            g_superseded = 1;
            return;
        }
        LOG("both saved; this computer's save is newer and goes up. Kept the folder's save (generation %lld from %s) "
            "in %s", (long long)got.gen, got.device, kept);
        prune_backups();
        write_state(m.gen, has_state ? st.hash : "");
        save_sync_changed();
        return;
    }
    if (!fetch(incoming, m.gen, &got)) {
        if (!g_identity_conflict) LOG("the folder's save is incomplete (still syncing?); this computer's save is used");
        return;
    }
    if (install(incoming, local_saves)) {
        write_state(got.gen, got.hash);
        LOG("loaded the save from %s (generation %lld)", got.device, (long long)got.gen);
    }
}

// The setting's folder with a leading ~ for the home folder, plus GAME_NAME.
static int sync_root(char *out, size_t cap) {
    const char *v = cfg_get("TOTK_SAVE_SYNC");
    if (!v || !*v || !strcmp(v, "0")) return 0;
    const char *home = getenv("HOME");
    if (!home || !*home) home = getenv("USERPROFILE");
    int n = v[0] == '~' && (v[1] == '/' || v[1] == '\\' || !v[1]) && home
                ? snprintf(out, cap, "%s%s/%s", home, v + 1, GAME_NAME)
                : snprintf(out, cap, "%s/%s", v, GAME_NAME);
    return n > 0 && (size_t)n < cap;
}

void save_sync_start(const char *save_dir, const char *profile, int (*snapshot)(const char *dst), int local_waits) {
    char root[PATHN];
    if (!sync_root(root, sizeof root)) return;
    char parent[PATHN];
    snprintf(parent, sizeof parent, "%s", root);
    *strrchr(parent, '/') = 0;
    if (!plat_dir_exists(parent)) {
        LOG("off: the folder %s is not there (is the sync program running?)", parent);
        return;
    }
    if (!setup(save_dir, profile, root)) return;
    g_snapshot = snapshot;
    on = 1;
    if (!plat_thread_spawn(worker, NULL, "save sync", 0)) on = 0;   // 1 on success
    if (!on) {
        LOG("off: cannot start the upload thread");
        return;
    }
    LOG("folder %s", g_cloud);
    if (local_waits) {   // a commit fs_host.c could not swap in is the newest save: this run uses it and uploads it
        LOG("a commit waits in save-tx; this start keeps the local save");
        Plist m;
        g_start_gen = read_cloud_meta(g_cur, &m) ? m.gen : 0;
        return;
    }
    start_sync();
}

// ---- --probe-save-sync: two computers sharing one folder, in a temporary directory. Each "computer" is a profile
// with a save folder; a run is setup and start_sync, a save is a file write, and the upload runs at once.
static int checks, failures;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(c)) {                                                                                                    \
            failures++;                                                                                                \
            printf("FAIL: ");                                                                                          \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

static char probe_root[PATHN];

static int probe_snapshot(const char *dst) { return fs_tree_copy(g_local, dst); }

static void probe_file(const char *dev, const char *rel, const char *text) {
    char p[PATHN];
    snprintf(p, sizeof p, "%s/%s/save/%s/%s", probe_root, dev, "3e69ca33dea6dc10cb00165c15cd5098", rel);
    char d[PATHN];
    snprintf(d, sizeof d, "%s", p);
    *strrchr(d, '/') = 0;
    plat_file_mkdir_p(d);
    plat_file_write(p, text, strlen(text));
}

// One run of computer dev: its start (start 0: only the paths, a game that is still running).
static void probe_run(const char *dev, int start) {
    char save[PATHN], prof[PATHN], root[PATHN];
    snprintf(prof, sizeof prof, "%s/%s", probe_root, dev);
    snprintf(save, sizeof save, "%s/save/3e69ca33dea6dc10cb00165c15cd5098", prof);
    snprintf(root, sizeof root, "%s/sync/%s", probe_root, GAME_NAME);
    plat_file_mkdir_p(save);
    CHECK(setup(save, prof, root), "setup of %s", dev);
    g_snapshot = probe_snapshot, on = 1;
    if (start) start_sync();
}

static void probe_save(void) {   // the game's Commit and the upload after it
    plat_xchg32(&pending, 1);
    save_sync_quit();
}

static int64_t probe_gen(void) {
    Plist m;
    return read_cloud_meta(g_cur, &m) ? m.gen : -1;
}

static int probe_read(const char *dev, const char *rel, const char *want) {
    char p[PATHN];
    snprintf(p, sizeof p, "%s/%s/save/3e69ca33dea6dc10cb00165c15cd5098/%s", probe_root, dev, rel);
    uint64_t n = 0;
    char *s = (char *)plat_file_read(p, &n);
    int ok = s && !strcmp(s, want);
    free(s);
    return ok;
}

static int probe_backups(const char *dev) {
    char p[PATHN];
    snprintf(p, sizeof p, "%s/%s/cloud-backups/3e69ca33dea6dc10cb00165c15cd5098", probe_root, dev);
    Entries e = {0};
    plat_fs_list(p, entry_add, &e);
    int n = e.dirs.n;
    names_free(&e.files);
    names_free(&e.dirs);
    return n;
}

int probe_save_sync(void) {
    char tmp[PATHN];
    const char *keep = getenv("TOTK_SAVE_SYNC_PROBE_DIR");   // tests/save_sync_check.py reads the folder afterwards
    if (keep && *keep) snprintf(probe_root, sizeof probe_root, "%s", keep);
    else {
        if (!plat_temp_dir(tmp, sizeof tmp)) rt_fatal("probe-save-sync: no temporary directory");
        snprintf(probe_root, sizeof probe_root, "%s/totk-probe-sync-%u", tmp, plat_process_id());
    }
    fs_tree_remove(probe_root);
    plat_file_mkdir_p(probe_root);

    // A uploads its first save into the empty folder
    probe_file("a", "slot_00/progress.sav", "a1");
    probe_file("a", "storage/cache.dat", "x");
    probe_file("a", "option.sav", "opt");
    probe_run("a", 1);
    CHECK(probe_gen() == -1, "the folder is empty before the first save");
    probe_save();
    CHECK(probe_gen() == 1, "A's first save is generation 1 (got %lld)", (long long)probe_gen());

    // B, which never ran, takes it at its start
    probe_run("b", 1);
    CHECK(probe_read("b", "slot_00/progress.sav", "a1") && probe_read("b", "option.sav", "opt"), "B took A's save");
    CHECK(probe_backups("b") == 0, "B had no save to keep");
    // a change only under storage/ is no new progress
    probe_file("b", "storage/cache.dat", "y");
    probe_save();
    CHECK(probe_gen() == 1, "a change under storage/ alone made generation %lld", (long long)probe_gen());
    probe_file("b", "slot_00/progress.sav", "b2");
    probe_save();
    CHECK(probe_gen() == 2, "B's save is generation 2 (got %lld)", (long long)probe_gen());

    // A starts again: B's save replaces its own, which goes to cloud-backups
    probe_run("a", 1);
    CHECK(probe_read("a", "slot_00/progress.sav", "b2"), "A took B's generation 2");
    CHECK(probe_backups("a") == 1, "A kept its own save in cloud-backups (%d backups)", probe_backups("a"));

    // A's game keeps running; B starts, saves (generation 3); then A saves: A stays local, and its next start takes
    // generation 3 although A's own save is newer
    probe_run("b", 1);
    probe_file("b", "slot_00/progress.sav", "b3");
    probe_save();
    CHECK(probe_gen() == 3, "B's second save is generation 3");
    probe_run("a", 0);
    probe_file("a", "slot_00/progress.sav", "a4");
    probe_save();
    CHECK(probe_gen() == 3 && g_superseded, "A uploaded over B's newer save");
    probe_run("a", 1);
    CHECK(probe_read("a", "slot_00/progress.sav", "b3"), "A's next start did not take B's save");
    CHECK(probe_backups("a") == 2, "A kept its superseded save (%d backups)", probe_backups("a"));

    // A saves (generation 4); B had saved after that without uploading (a crash): B's newer save wins at its start
    probe_file("a", "slot_00/progress.sav", "a5");
    probe_save();
    CHECK(probe_gen() == 4, "A's save is generation 4");
    plat_sleep_ms(20);
    probe_file("b", "slot_00/progress.sav", "b6");
    probe_run("b", 1);
    plat_xchg32(&pending, 0);   // start_sync queued the upload; the probe runs it now
    probe_save();
    CHECK(probe_gen() == 5 && probe_read("b", "slot_00/progress.sav", "b6"), "B's newer save is not generation 5 (%lld)",
          (long long)probe_gen());
    CHECK(probe_backups("b") == 1, "B kept the folder's generation 4 it replaced (%d backups)", probe_backups("b"));

    // the plist the start reads, as NSDictionary writes it (keys sorted, tabs) with an escaped device name
    char cur[PATHN], p[PATHN];
    snprintf(cur, sizeof cur, "%s", g_cur);
    snprintf(p, sizeof p, "%s/state.plist", probe_root);
    const char *apple =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\">\n<dict>\n\t<key>device</key>\n"
        "\t<string>Mac &amp; &lt;mini&gt;</string>\n\t<key>gen</key>\n\t<integer>12</integer>\n\t<key>hash</key>\n"
        "\t<string>0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef</string>\n\t<key>time</key>\n"
        "\t<real>1762650000.5</real>\n\t<key>uid</key>\n\t<string>3e69ca33dea6dc10cb00165c15cd5098</string>\n</dict>\n"
        "</plist>\n";
    Plist ap;
    CHECK(plat_file_write(p, apple, strlen(apple)) && plist_read(p, &ap) && ap.gen == 12 && ap.time == 1762650000.5 &&
              !strcmp(ap.device, "Mac & <mini>") && !strcmp(ap.uid, g_user) && strlen(ap.hash) == 64,
          "an Apple-written sync.plist");

    // another user's sync.plist: left alone, nothing uploaded
    snprintf(p, sizeof p, "%s/sync.plist", cur);
    uint64_t n = 0;
    char *orig = (char *)plat_file_read(p, &n), *other = orig ? strdup(orig) : NULL;
    char *u = other ? strstr(other, g_user) : NULL;
    if (u) u[0] = u[0] == '0' ? '1' : '0';
    CHECK(u && plat_file_write(p, other, strlen(other)), "writing another user's sync.plist");
    probe_run("a", 1);
    probe_file("a", "slot_00/progress.sav", "a7");
    probe_save();
    CHECK(g_identity_conflict && probe_read("a", "slot_00/progress.sav", "a7"), "another user's save was used");
    if (orig) plat_file_write(p, orig, strlen(orig));
    free(orig);
    free(other);

    on = 0;
    if (!keep || !*keep) fs_tree_remove(probe_root);
    if (failures) {
        printf("probe-save-sync: FAILED (%d of %d checks)\n", failures, checks);
        return 1;
    }
    printf("probe-save-sync: passed (%d checks)\n", checks);
    return 0;
}
