#include "krypt_store.hpp"
#include "game.hpp"
#include "log.hpp"
#include "nn/fs.hpp"
#include <atomic>
#include <cstring>

namespace nn::fs {
    Result DeleteFile(char const* path);
    Result RenameFile(char const* from, char const* to);
    void SetEnabledAutoAbort(bool enabled);
}

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

    size_t Count() {
        Guard g;
        EnsureLoaded();
        return s_Count;
    }

    void RecordBatch(const char* json) {
        constexpr const char* Path = "mk11sd:/mk11offline/inventory_batches.jsonl";
        if (!MountSd()) return;
        size_t n = strlen(json);
        char* line = static_cast<char*>(game::Malloc(n + 1));
        for (size_t i = 0; i < n; i++) line[i] = (json[i] == '\n' || json[i] == '\r') ? ' ' : json[i];
        line[n] = '\n';

        Guard g;
        nn::fs::SetEnabledAutoAbort(false);
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
