// mk11dump: finds the installed Mortal Kombat 11, copies its exefs (with the update applied)
// and the NACP to sdmc:/mk11-dump/, and writes a listing of its romfs. The console's own
// fs service decrypts everything; no keys are read or used.
#define _GNU_SOURCE
#include <switch.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

#define OUT "sdmc:/mk11-dump"

// FileSystemProxyType values libnx leaves unnamed.
#define FS_TYPE_CODE ((FsFileSystemType)0)
#define FS_TYPE_ROM  ((FsFileSystemType)1)

static FILE* g_log;

static void logf_(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt); vprintf(fmt, a); va_end(a);
    if (g_log) { va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a); fflush(g_log); }
    consoleUpdate(NULL);
}
#define LOG(...) logf_(__VA_ARGS__)

static int copy_file(const char* src, const char* dst) {
    FILE* in = fopen(src, "rb");
    if (!in) { LOG("  open %s failed (%d)\n", src, errno); return -1; }
    FILE* out = fopen(dst, "wb");
    if (!out) { fclose(in); LOG("  create %s failed (%d)\n", dst, errno); return -1; }
    static u8 buf[0x100000];
    size_t n, total = 0;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) { fwrite(buf, 1, n, out); total += n; }
    fclose(in); fclose(out);
    LOG("  %s -> %zu bytes\n", src, total);
    return 0;
}

static void copy_tree(const char* src, const char* dst) {
    mkdir(dst, 0777);
    DIR* d = opendir(src);
    if (!d) { LOG("  opendir %s failed (%d)\n", src, errno); return; }
    struct dirent* e;
    char s[1024], t[1024];
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(s, sizeof(s), "%s/%s", src, e->d_name);
        snprintf(t, sizeof(t), "%s/%s", dst, e->d_name);
        if (e->d_type == DT_DIR) copy_tree(s, t); else copy_file(s, t);
    }
    closedir(d);
}

static u64 g_files, g_bytes;

static void list_tree(FILE* out, const char* path, const char* rel) {
    DIR* d = opendir(path);
    if (!d) { fprintf(out, "!! opendir %s failed (%d)\n", path, errno); return; }
    struct dirent* e;
    char p[1024], r[1024];
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(p, sizeof(p), "%s/%s", path, e->d_name);
        snprintf(r, sizeof(r), "%s/%s", rel, e->d_name);
        if (e->d_type == DT_DIR) { fprintf(out, "%s/\n", r); list_tree(out, p, r); }
        else {
            struct stat st; long long sz = stat(p, &st) == 0 ? (long long)st.st_size : -1;
            fprintf(out, "%12lld %s\n", sz, r);
            g_files++; if (sz > 0) g_bytes += sz;
        }
    }
    closedir(d);
}

static u64 find_mk11(void) {
    NsApplicationRecord recs[64];
    s32 count = 0, off = 0;
    static NsApplicationControlData ctrl;
    for (;;) {
        if (R_FAILED(nsListApplicationRecord(recs, 64, off, &count)) || count <= 0) break;
        for (s32 i = 0; i < count; i++) {
            u64 sz = 0;
            if (R_FAILED(nsGetApplicationControlData(NsApplicationControlSource_Storage, recs[i].application_id, &ctrl, sizeof(ctrl), &sz))) continue;
            NacpLanguageEntry* le = NULL;
            if (R_FAILED(nacpGetLanguageEntry(&ctrl.nacp, &le)) || !le) continue;
            if (strcasestr(le->name, "Mortal Kombat 11")) {
                LOG("Found: %s (%s) id %016lX, version %s\n", le->name, le->author, recs[i].application_id, ctrl.nacp.display_version);
                FILE* f = fopen(OUT "/control.nacp", "wb");
                if (f) { fwrite(&ctrl.nacp, 1, sizeof(ctrl.nacp), f); fclose(f); }
                return recs[i].application_id;
            }
        }
        off += count;
    }
    return 0;
}

static void log_meta(u64 tid) {
    NsApplicationContentMetaStatus st[16];
    s32 n = 0;
    if (R_SUCCEEDED(nsListApplicationContentMetaStatus(tid, 0, st, 16, &n)))
        for (s32 i = 0; i < n; i++)
            LOG("  meta type %u id %016lX version %u storage %u\n", st[i].meta_type, st[i].application_id, st[i].version, st[i].storageID);
}

// Path of the newest Program NCA for `id` (the update if installed, else the base).
static bool program_path(u64 id, char* out, size_t outsz) {
    const NcmStorageId ids[] = { NcmStorageId_SdCard, NcmStorageId_BuiltInUser };
    for (int i = 0; i < 2; i++) {
        NcmContentMetaDatabase db; NcmContentStorage cs; NcmContentMetaKey key; NcmContentId cid;
        if (R_FAILED(ncmOpenContentMetaDatabase(&db, ids[i]))) continue;
        bool ok = R_SUCCEEDED(ncmContentMetaDatabaseGetLatestContentMetaKey(&db, &key, id)) &&
                  R_SUCCEEDED(ncmContentMetaDatabaseGetContentIdByType(&db, &cid, &key, NcmContentType_Program));
        ncmContentMetaDatabaseClose(&db);
        if (!ok || R_FAILED(ncmOpenContentStorage(&cs, ids[i]))) continue;
        ok = R_SUCCEEDED(ncmContentStorageGetPath(&cs, out, outsz, &cid));
        ncmContentStorageClose(&cs);
        if (ok) { LOG("  program %016lX v%u on storage %d: %s\n", key.id, key.version, ids[i], out); return true; }
    }
    return false;
}

int main(int argc, char** argv) {
    consoleInit(NULL);
    socketInitializeDefault();
    nxlinkStdio();
    mkdir(OUT, 0777);
    g_log = fopen(OUT "/dump.log", "w");
    LOG("mk11dump\n");

    Result rc;
    if (R_FAILED(rc = nsInitialize())) { LOG("nsInitialize %x\n", rc); goto done; }
    if (R_FAILED(rc = ncmInitialize())) { LOG("ncmInitialize %x\n", rc); goto done; }

    u64 tid = find_mk11();
    if (!tid) { LOG("Mortal Kombat 11 not found\n"); goto done; }
    log_meta(tid);

    // exefs: the update's Program NCA carries the complete exefs.
    FsFileSystem code;
    char path[FS_MAX_PATH] = {0};
    rc = fsOpenFileSystemWithPatch(&code, tid, FS_TYPE_CODE);
    LOG("OpenWithPatch(code): %x\n", rc);
    if (R_FAILED(rc) && program_path(tid + 0x800, path, sizeof(path))) {
        rc = fsOpenFileSystemWithId(&code, tid, FS_TYPE_CODE, path, FsContentAttributes_All);
        LOG("OpenWithId(update code): %x\n", rc);
    }
    if (R_FAILED(rc) && program_path(tid, path, sizeof(path))) {
        rc = fsOpenFileSystemWithId(&code, tid, FS_TYPE_CODE, path, FsContentAttributes_All);
        LOG("OpenWithId(base code): %x\n", rc);
    }
    if (R_SUCCEEDED(rc)) {
        fsdevMountDevice("code", code);
        LOG("Copying exefs...\n");
        copy_tree("code:", OUT "/exefs");
        fsdevUnmountDevice("code");
    }

    // romfs listing (patched if the fs service allows it, else the base romfs).
    FsFileSystem data;
    const char* which = "patched";
    rc = fsOpenFileSystemWithPatch(&data, tid, FS_TYPE_ROM);
    LOG("OpenWithPatch(data): %x\n", rc);
    if (R_FAILED(rc) && program_path(tid, path, sizeof(path))) {
        rc = fsOpenFileSystemWithId(&data, tid, FS_TYPE_ROM, path, FsContentAttributes_All);
        which = "base";
        LOG("OpenWithId(base data): %x\n", rc);
    }
    if (R_SUCCEEDED(rc)) {
        fsdevMountDevice("data", data);
        FILE* f = fopen(OUT "/romfs_listing.txt", "w");
        if (f) {
            fprintf(f, "# romfs (%s)\n", which);
            list_tree(f, "data:", "");
            fclose(f);
        }
        LOG("romfs (%s): %lu files, %lu bytes\n", which, g_files, g_bytes);
        fsdevUnmountDevice("data");
    }

done:
    LOG("DONE\n");
    if (g_log) fclose(g_log);
    ncmExit();
    nsExit();
    socketExit();
    consoleExit(NULL);
    return 0;
}
