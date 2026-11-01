#include "krypt_store.hpp"
#include "game.hpp"
#include "log.hpp"
#include "nn/fs.hpp"
#include <atomic>
#include <cstdio>
#include <cstring>

namespace nn::fs {
    Result DeleteFile(char const* path);
    Result RenameFile(char const* from, char const* to);
    Result CreateDirectory(char const* path);
    Result OpenDirectory(DirectoryHandle* outHandle, char const* path, s32 openMode);
    Result ReadDirectory(s64* outEntryCount, DirectoryEntry* outEntries, DirectoryHandle handle, s64 entryBufferLength);
    void CloseDirectory(DirectoryHandle handle);
    void SetEnabledAutoAbort(bool enabled);
}

extern "C" u32 SdkUserClockGetCurrentTime(s64* posixTime) asm("_ZN2nn4time23StandardUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE");

namespace mk11::store {

    namespace {
        constexpr const char* DatPath = "mk11sd:/mk11offline/krypt.dat";
        constexpr const char* TmpPath = "mk11sd:/mk11offline/krypt.tmp";
        constexpr const char* BakPath = "mk11sd:/mk11offline/krypt.bak";
        constexpr const char* Magic = "MK11KRYPT1\n";

        // File format after the magic line: "<key>\t<json>\n" per entry. Keys never contain tabs or
        // newlines; newlines inside the JSON are replaced with spaces when stored.

        struct Entry { char* key; char* json; size_t keyLen; size_t jsonLen; };

        constexpr size_t Capacity = 1 << 15;  // open addressing; the Krypt has a few thousand keys
        Entry s_Table[Capacity];
        size_t s_Count = 0;
        bool s_Loaded = false;
        bool s_Dirty = false;
        std::atomic<int> s_BatchDepth{0};
        constexpr s64 LogRotateSize = 1 << 20;  // jsonl files are rotated to .old above 1 MB

        std::atomic_flag s_Lock = ATOMIC_FLAG_INIT;
        struct Guard {
            Guard()  { while (s_Lock.test_and_set(std::memory_order_acquire)) {} }
            ~Guard() { s_Lock.clear(std::memory_order_release); }
        };

        size_t Hash(const char* s, size_t n) {
            size_t h = 1469598103934665603ull;
            for (size_t i = 0; i < n; i++) h = (h ^ (u8)s[i]) * 1099511628211ull;
            return h;
        }

        Entry* Find(const char* key, size_t n, bool create) {
            for (size_t i = Hash(key, n) & (Capacity - 1), probes = 0; probes < Capacity; i = (i + 1) & (Capacity - 1), probes++) {
                Entry& e = s_Table[i];
                if (!e.key) {
                    if (!create || s_Count >= Capacity * 3 / 4) return nullptr;
                    e.key = static_cast<char*>(game::Malloc(n + 1));
                    memcpy(e.key, key, n);
                    e.key[n] = 0;
                    e.keyLen = n;
                    s_Count++;
                    return &e;
                }
                if (e.keyLen == n && memcmp(e.key, key, n) == 0) return &e;
            }
            return nullptr;
        }

        void SetJson(Entry* e, const char* json, size_t n) {
            if (e->json) game::Free(e->json);
            e->json = static_cast<char*>(game::Malloc(n + 1));
            for (size_t i = 0; i < n; i++) e->json[i] = (json[i] == '\n' || json[i] == '\r') ? ' ' : json[i];
            e->json[n] = 0;
            e->jsonLen = n;
        }

        // Backups of krypt.dat as it was when the game started: backups/krypt-<date>-<time>.dat for each start
        // (newest KeepStarts kept) and backups/daily-<date>.dat for the first start of each day (newest
        // KeepDays kept). Dates come from the console clock.
        constexpr const char* BackupDir = "mk11sd:/mk11offline/backups";
        constexpr int KeepStarts = 10;
        constexpr int KeepDays = 14;

        bool WriteNewFile(const char* path, const char* data, s64 size) {
            if (R_FAILED(nn::fs::CreateFile(path, size))) return false;  // also when it exists
            nn::fs::FileHandle h;
            if (R_FAILED(nn::fs::OpenFile(&h, path, nn::fs::OpenMode_Write))) return false;
            bool ok = R_SUCCEEDED(nn::fs::WriteFile(h, 0, data, size, nn::fs::WriteOption::CreateOption(nn::fs::WriteOptionFlag_Flush)));
            nn::fs::CloseFile(h);
            return ok;
        }

        // Deletes the oldest backups whose names start with `prefix` until `keep` remain (names sort by date).
        void Prune(const char* prefix, int keep) {
            constexpr int Max = 64;
            static char names[Max][48];
            static nn::fs::DirectoryEntry entry;
            int count = 0;
            nn::fs::DirectoryHandle dir;
            if (R_FAILED(nn::fs::OpenDirectory(&dir, BackupDir, nn::fs::OpenDirectoryMode_File))) return;
            s64 read = 0;
            size_t pl = strlen(prefix);
            while (count < Max && R_SUCCEEDED(nn::fs::ReadDirectory(&read, &entry, dir, 1)) && read == 1)
                if (strncmp(entry.m_Name, prefix, pl) == 0 && strlen(entry.m_Name) < sizeof(names[0]))
                    strcpy(names[count++], entry.m_Name);
            nn::fs::CloseDirectory(dir);
            while (count > keep) {
                int oldest = 0;
                for (int i = 1; i < count; i++)
                    if (strcmp(names[i], names[oldest]) < 0) oldest = i;
                char path[96];
                snprintf(path, sizeof(path), "%s/%s", BackupDir, names[oldest]);
                nn::fs::DeleteFile(path);
                strcpy(names[oldest], names[--count]);
            }
        }

        void Backup(const char* data, s64 size) {
            s64 seconds = 0;
            if (SdkUserClockGetCurrentTime(&seconds) != 0 || seconds <= 0) seconds = 0;
            s64 days = seconds / 86400 + 719468, rest = seconds % 86400;
            s64 era = days / 146097, doe = days - era * 146097;
            s64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
            s64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
            int day = doy - (153 * mp + 2) / 5 + 1, month = mp < 10 ? mp + 3 : mp - 9;
            int year = yoe + era * 400 + (month <= 2);
            char date[16], path[96];
            snprintf(date, sizeof(date), "%04d%02d%02d", year, month, day);

            nn::fs::SetEnabledAutoAbort(false);
            nn::fs::CreateDirectory(BackupDir);
            snprintf(path, sizeof(path), "%s/krypt-%s-%02d%02d%02d.dat", BackupDir, date, (int)(rest / 3600), (int)(rest / 60 % 60), (int)(rest % 60));
            bool start = WriteNewFile(path, data, size);
            snprintf(path, sizeof(path), "%s/daily-%s.dat", BackupDir, date);
            bool daily = WriteNewFile(path, data, size);
            Prune("krypt-", KeepStarts);
            Prune("daily-", KeepDays);
            nn::fs::SetEnabledAutoAbort(true);
            Log("store: backup %s-%02d%02d%02d %s%s", date, (int)(rest / 3600), (int)(rest / 60 % 60), (int)(rest % 60),
                start ? "written" : "FAILED", daily ? ", first of the day" : "");
        }

        void LoadLocked() {
            s_Loaded = true;
            if (!MountSd()) return;
            nn::fs::SetEnabledAutoAbort(false);
            nn::fs::FileHandle h;
            if (R_FAILED(nn::fs::OpenFile(&h, DatPath, nn::fs::OpenMode_Read))) {
                nn::fs::SetEnabledAutoAbort(true);
                Log("store: no krypt.dat, starting empty");
                return;
            }
            s64 size = 0;
            nn::fs::GetFileSize(&size, h);
            char* buf = static_cast<char*>(game::Malloc(size + 1));
            nn::fs::ReadFile(h, 0, buf, size);
            nn::fs::CloseFile(h);
            nn::fs::SetEnabledAutoAbort(true);
            buf[size] = 0;

            size_t ml = strlen(Magic);
            if (size < (s64)ml || memcmp(buf, Magic, ml) != 0) {
                Log("store: krypt.dat has a bad header, ignoring it");
                game::Free(buf);
                return;
            }
            char* p = buf + ml;
            char* end = buf + size;
            while (p < end) {
                char* tab = static_cast<char*>(memchr(p, '\t', end - p));
                if (!tab) break;
                char* nl = static_cast<char*>(memchr(tab + 1, '\n', end - tab - 1));
                if (!nl) break;
                if (Entry* e = Find(p, tab - p, true)) SetJson(e, tab + 1, nl - tab - 1);
                p = nl + 1;
            }
            if (s_Count) Backup(buf, size);
            game::Free(buf);
            Log("store: loaded %zu entries (%lld bytes)", s_Count, (long long)size);
        }

        void EnsureLoaded() { if (!s_Loaded) LoadLocked(); }
    }

    const char* Get(const char* key) {
        Guard g;
        EnsureLoaded();
        Entry* e = Find(key, strlen(key), false);
        return e && e->json ? e->json : nullptr;
    }

    void Put(const char* key, const char* json) {
        Guard g;
        EnsureLoaded();
        if (Entry* e = Find(key, strlen(key), true)) {
            size_t n = strlen(json);
            if (e->json && e->jsonLen == n && memcmp(e->json, json, n) == 0) return;
            SetJson(e, json, n);
            s_Dirty = true;
        } else {
            Log("store: table full, dropping %s", key);
        }
    }

    void Remove(const char* key) {
        Guard g;
        EnsureLoaded();
        // Open addressing: the key stays as a tombstone; entries without JSON are neither found by Get
        // nor written.
        if (Entry* e = Find(key, strlen(key), false); e && e->json) {
            game::Free(e->json);
            e->json = nullptr;
            e->jsonLen = 0;
            s_Dirty = true;
        }
    }

    void ForEach(const char* prefix, void (*fn)(const char* key, const char* json, void* context), void* context) {
        // Keys are collected first so `fn` may call Get/Put/Remove.
        size_t n = strlen(prefix), count = 0;
        char** keys;
        {
            Guard g;
            EnsureLoaded();
            keys = static_cast<char**>(game::Malloc(sizeof(char*) * (s_Count + 1)));
            for (auto& e : s_Table)
                if (e.key && e.json && e.keyLen >= n && memcmp(e.key, prefix, n) == 0) keys[count++] = e.key;
        }
        for (size_t i = 0; i < count; i++)
            if (const char* json = Get(keys[i])) fn(keys[i], json, context);
        game::Free(keys);
    }

    Batch::Batch() { s_BatchDepth++; }
    Batch::~Batch() {
        if (--s_BatchDepth == 0) Flush();
    }

    size_t Count() {
        Guard g;
        EnsureLoaded();
        return s_Count;
    }

    void RecordBatch(const char* json) { AppendLine("inventory_batches.jsonl", json); }

    void AppendLine(const char* file, const char* json) {
        char path[128];
        snprintf(path, sizeof(path), "mk11sd:/mk11offline/%s", file);
        const char* Path = path;
        if (!MountSd()) return;
        size_t n = strlen(json);
        char* line = static_cast<char*>(game::Malloc(n + 1));
        for (size_t i = 0; i < n; i++) line[i] = (json[i] == '\n' || json[i] == '\r') ? ' ' : json[i];
        line[n] = '\n';

        Guard g;
        nn::fs::SetEnabledAutoAbort(false);
        // Keep the file small: above LogRotateSize it becomes <file>.old (replacing the previous one).
        {
            nn::fs::FileHandle sizeHandle;
            s64 current = 0;
            if (R_SUCCEEDED(nn::fs::OpenFile(&sizeHandle, Path, nn::fs::OpenMode_Read))) {
                nn::fs::GetFileSize(&current, sizeHandle);
                nn::fs::CloseFile(sizeHandle);
            }
            if (current > LogRotateSize) {
                char old[140];
                snprintf(old, sizeof(old), "%s.old", path);
                nn::fs::DeleteFile(old);
                nn::fs::RenameFile(Path, old);
            }
        }
        nn::fs::CreateFile(Path, 0);
        nn::fs::FileHandle h;
        if (R_SUCCEEDED(nn::fs::OpenFile(&h, Path, nn::fs::OpenMode_Write | nn::fs::OpenMode_Append))) {
            s64 size = 0;
            nn::fs::GetFileSize(&size, h);
            nn::fs::WriteFile(h, size, line, n + 1, nn::fs::WriteOption::CreateOption(nn::fs::WriteOptionFlag_Flush));
            nn::fs::CloseFile(h);
        }
        nn::fs::SetEnabledAutoAbort(true);
        game::Free(line);
    }

    char* TakeReplayBatch() {
        constexpr const char* Path = "mk11sd:/mk11offline/replay_batch.json";
        if (!MountSd()) return nullptr;
        char* buf = nullptr;
        nn::fs::SetEnabledAutoAbort(false);
        nn::fs::FileHandle h;
        if (R_SUCCEEDED(nn::fs::OpenFile(&h, Path, nn::fs::OpenMode_Read))) {
            s64 size = 0;
            nn::fs::GetFileSize(&size, h);
            buf = static_cast<char*>(game::Malloc(size + 1));
            nn::fs::ReadFile(h, 0, buf, size);
            buf[size] = 0;
            nn::fs::CloseFile(h);
            nn::fs::DeleteFile(Path);
        }
        nn::fs::SetEnabledAutoAbort(true);
        return buf;
    }

    void Flush() {
        if (s_BatchDepth > 0) return;  // written when the outermost Batch ends
        Guard g;
        if (!s_Dirty || !MountSd()) return;

        size_t total = strlen(Magic);
        for (auto& e : s_Table) if (e.key && e.json) total += e.keyLen + e.jsonLen + 2;
        char* buf = static_cast<char*>(game::Malloc(total));
        char* p = buf;
        memcpy(p, Magic, strlen(Magic)); p += strlen(Magic);
        for (auto& e : s_Table) {
            if (!e.key || !e.json) continue;
            memcpy(p, e.key, e.keyLen); p += e.keyLen; *p++ = '\t';
            memcpy(p, e.json, e.jsonLen); p += e.jsonLen; *p++ = '\n';
        }

        // Write the new file beside the old one, then swap, keeping the previous copy as .bak.
        nn::fs::SetEnabledAutoAbort(false);
        nn::fs::DeleteFile(TmpPath);
        bool ok = false;
        if (R_SUCCEEDED(nn::fs::CreateFile(TmpPath, total))) {
            nn::fs::FileHandle h;
            if (R_SUCCEEDED(nn::fs::OpenFile(&h, TmpPath, nn::fs::OpenMode_Write))) {
                ok = R_SUCCEEDED(nn::fs::WriteFile(h, 0, buf, total, nn::fs::WriteOption::CreateOption(nn::fs::WriteOptionFlag_Flush)));
                nn::fs::CloseFile(h);
            }
        }
        if (ok) {
            nn::fs::DeleteFile(BakPath);
            nn::fs::RenameFile(DatPath, BakPath);
            ok = R_SUCCEEDED(nn::fs::RenameFile(TmpPath, DatPath));
        }
        nn::fs::SetEnabledAutoAbort(true);
        game::Free(buf);

        if (ok) s_Dirty = false;
        Log("store: flush %zu entries, %zu bytes -> %s", s_Count, total, ok ? "ok" : "FAILED");
    }
}
