#include "lib.hpp"
#include "game.hpp"
#include "batch_apply.hpp"
#include "hydra_reply.hpp"
#include "json.hpp"
#include "krypt_store.hpp"
#include "log.hpp"
#include "portals.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Towers (MK11 "portals") offline.
//
// Tower content ships with the game (PortalDefinitions.mko); Hydra only stores one record per tower
// set, {id, unique_key, expire_time, data: PortalSSCData}, that the client generates
// (MK11PortalObj::GenerateServerData) and then changes through update commands
// ({updateType: "set", path: "data.stages.N...", value}). Offline the records live in the SD store as
// "portal/<name>", are handed to the game with UpdatePortalGOSObject (as the Hydra callbacks do), and
// the update requests are applied here and answered with a completed reply.
//
// Every request is also appended to portal_requests.jsonl.

using namespace mk11;

namespace {
    constexpr const char* KlassicName = "KlassicTowers";
    constexpr int PortalTypeKlassic = 8;          // PortalObjType passed by MK11KlassicPortalMode::CreatePortals
    constexpr size_t KlassicPortalObjSize = 0x98;  // operator new size in CreatePortals

    // TArray<OnlineValueHandle>: each element is one OnlineValue*.
    struct HandleArray {
        void** data;
        s32 num;
    };

    // Records changed by update requests are handed to the game later, when the tower menu is opened
    // again: replacing a record inside the request (e.g. while a tower is being completed) frees the
    // PortalGOSData the active portal still points to.
    bool s_RecordsStale = false;

    void* ActiveUser() { return game::Fn<void* (*)(int, unsigned)>(off::GetActiveModeUser)(0, 1); }

    bool Offline(void* user) {
        return user && !game::Fn<bool (*)(void*)>(off::FGUserEntity_IsConnectedToHydra)(user);
    }

    void StoreKey(char* out, size_t size, const char* name) { snprintf(out, size, "portal/%s", name); }

    const void* FindRecord(void* user, const char* name) {
        game::NRSString key(name);
        return game::Fn<const void* (*)(void*, const game::NRSString&)>(off::MK11PortalInfo_GetPortalGOSData)(user, key);
    }

    bool InjectValue(void* user, const char* text) {
        game::NRSString json(text);
        game::OnlineValueHandle value = game::ParseFromJson(json);
        if (!value.value) return false;
        game::Fn<void (*)(void*, const game::OnlineValueHandle&)>(off::UpdatePortalGOSObject)(user, value);
        return true;
    }

    // Hands a stored record to the game the way the Hydra portal callbacks do. The best score of each
    // completed tower is kept in the record as offline.persistent.<key> = {score, completionRank}; the game
    // shows it from its persistent event objects ("P<portal>_<stage>_<event>"), which UpdatePortalGOSObject
    // takes from "persistentData", one per call, so the record is handed over once per entry.
    // Fills offline.persistent from the scores the events already store (data.stages.N.events.M.data.score),
    // for records completed before best scores were kept.
    void BackfillBestScores(json::Value* root) {
        json::Value* stages = json::Find(root, "data.stages");
        const char* ids[] = { json::String(json::Find(root, "id")), json::String(json::Find(root, "data.name")) };
        for (size_t st = 0; st < json::Count(stages); st++) {
            json::Value* events = json::Find(json::At(stages, st), "events");
            for (size_t ev = 0; ev < json::Count(events); ev++) {
                json::Value* event = json::At(events, ev);
                const char* name = json::String(json::Find(event, "name"));
                long long score;
                if (!name || !json::Integer(json::Find(event, "data.score"), &score) || score <= 0) continue;
                for (const char* id : ids) {
                    if (!id || !*id) continue;
                    char path[300];
                    snprintf(path, sizeof(path), "offline.persistent.P%s_%d_%s.score", id, (int)st, name);
                    long long best;
                    if (!json::Integer(json::Find(root, path), &best) || best < score) json::SetInteger(root, path, score);
                }
            }
        }
    }

    bool Inject(void* user, const char* record) {
        json::Value* root = json::Parse(record);
        if (root) BackfillBestScores(root);
        json::Value* persistent = json::Find(root, "offline.persistent");
        size_t count = json::Members(persistent);
        if (!root || !count) {
            json::Destroy(root);
            return InjectValue(user, record);
        }
        bool ok = true;
        for (size_t i = 0; i < count; i++) {
            const char* key = json::KeyAt(persistent, i);
            char* data = json::Serialize(json::MemberAt(persistent, i));
            size_t n = strlen(key) * 2 + (data ? strlen(data) : 2) + 64;
            char* text = static_cast<char*>(game::Malloc(n));
            snprintf(text, n, "{\"id\":\"offline-%s\",\"unique_key\":\"%s\",\"data\":%s}", key, key, data ? data : "{}");
            json::Value* object = json::Parse(text);
            if (object) json::Set(root, "persistentData", object);
            json::Destroy(object);
            game::Free(text);
            game::Free(data);
            char* full = json::Serialize(root);
            ok = full && InjectValue(user, full) && ok;
            game::Free(full);
        }
        json::Destroy(root);
        return ok;
    }

    // Keeps the best score of a completed tower event (extraData: score, completionRank) in the record,
    // under both forms of the game's key (portal id or name).
    void RecordBestScore(json::Value* record, const char* portalId, s32 stage, const char* eventName, const char* extraText) {
        json::Value* extra = json::Parse(extraText);
        long long score, rank = 0;
        if (json::Integer(json::Find(extra, "score"), &score)) {
            json::Integer(json::Find(extra, "completionRank"), &rank);
            const char* ids[] = { portalId, json::String(json::Find(record, "data.name")) };
            for (const char* id : ids) {
                if (!id || !*id) continue;
                char path[256];
                snprintf(path, sizeof(path), "offline.persistent.P%s_%d_%s", id, stage, eventName);
                char scorePath[300], rankPath[300];
                snprintf(scorePath, sizeof(scorePath), "%s.score", path);
                snprintf(rankPath, sizeof(rankPath), "%s.completionRank", path);
                long long best = 0;
                if (json::Integer(json::Find(record, scorePath), &best) && best >= score) continue;
                json::SetInteger(record, scorePath, score);
                json::SetInteger(record, rankPath, rank);
                Log("portals: best score %s = %lld (rank %lld)", path + 19, score, rank);
            }
        }
        json::Destroy(extra);
    }

    char* ValueJson(const void* value) {
        if (!value) return nullptr;
        game::NRSString text = game::ToJsonString(value);
        return game::StrDup(text.c_str());
    }

    // A first record for the Klassic Towers, generated by the game's own portal object.
    char* GenerateKlassicRecord(void* user) {
        void* portal = game::Fn<void* (*)(size_t)>(off::OperatorNew)(KlassicPortalObjSize);
        game::Fn<void (*)(void*, void*, void*)>(off::MK11KlassicPortalObj_Ctor)(portal, user, nullptr);
        char* record = nullptr;
        if (game::Fn<bool (*)(void*, const char*, int)>(off::MK11PortalObj_PortalObjInit)(portal, KlassicName, PortalTypeKlassic)) {
            // PortalSSCData is about 0x60 bytes; it is generated once and not torn down.
            alignas(16) static u8 data[0x200];
            game::Fn<void (*)(void*)>(off::PortalSSCData_Ctor)(data);
            game::Fn<void (*)(void*, void*)>(off::MK11PortalObj_GenerateServerData)(portal, data);
            game::NRSString json = game::Fn<game::NRSString (*)(const void*, int)>(off::PortalSSCData_ToJsonString)(data, 0);
            size_t n = strlen(json.c_str()) + 128;
            record = static_cast<char*>(game::Malloc(n));
            if (record) snprintf(record, n, "{\"id\":\"offline-%s\",\"unique_key\":\"%s\",\"data\":%s}", KlassicName, KlassicName, json.c_str());
        }
        auto vtable = *reinterpret_cast<void (***)(void*)>(portal);
        vtable[1](portal);  // deleting destructor
        return record;
    }

    // Makes sure the Klassic Towers record exists and is current: from the SD card, or generated the
    // first time.
    bool EnsureKlassicRecord(void* user) {
        store::Batch storeBatch;
        if (s_RecordsStale) {
            char key[128];
            StoreKey(key, sizeof(key), KlassicName);
            if (const char* stored = store::Get(key)) Log("portals: %s record refreshed: %s", KlassicName, Inject(user, stored) ? "ok" : "failed");
            s_RecordsStale = false;
        }
        if (FindRecord(user, KlassicName)) return true;
        char* record = GenerateKlassicRecord(user);
        if (!record) {
            Log("portals: %s record could not be generated", KlassicName);
            return false;
        }
        char key[128];
        StoreKey(key, sizeof(key), KlassicName);
        store::Put(key, record);
        store::Flush();
        bool loaded = Inject(user, record);
        Log("portals: %s record generated (%zu bytes): %s", KlassicName, strlen(record), loaded ? "loaded" : "NOT loaded");
        game::Free(record);
        return loaded && FindRecord(user, KlassicName);
    }

    // The stored record whose id or data.name is one of `names`; its store key goes to `key`.
    json::Value* LoadRecord(const char* const* names, int count, char* key, size_t keySize) {
        for (int i = 0; i < count; i++) {
            if (!names[i] || !*names[i]) continue;
            StoreKey(key, keySize, names[i]);
            if (json::Value* v = json::Parse(store::Get(key))) return v;
        }
        // Not stored under any of the names: look for a matching id.
        for (int i = 0; i < count; i++) {
            if (!names[i] || strncmp(names[i], "offline-", 8)) continue;
            StoreKey(key, keySize, names[i] + 8);
            if (json::Value* v = json::Parse(store::Get(key))) return v;
        }
        return nullptr;
    }

    // Applies one Hydra update command ({updateType, path, value}, `text` for the log) to a record.
    void ApplyCommand(json::Value* record, json::Value* command, const char* text) {
        {
            const char* type = json::String(json::Find(command, "updateType"));
            const char* path = json::String(json::Find(command, "path"));
            json::Value* value = json::Find(command, "value");
            long long amount, current = 0;
            if (type && path && value && !strcmp(type, "set")) {
                Log("portals: set %s -> %s", path, json::Set(record, path, value) ? "ok" : "path not found");
            } else if (type && path && json::Integer(value, &amount) && (!strcmp(type, "inc") || !strcmp(type, "max"))) {
                // inc adds to the stored number, max keeps the larger one; a missing number counts as 0
                bool had = json::Integer(json::Find(record, path), &current);
                long long result = !strcmp(type, "inc") ? current + amount : (had && current > amount ? current : amount);
                Log("portals: %s %s %lld -> %lld %s", type, path, current, result,
                    json::SetInteger(record, path, result) ? "ok" : "path not found");
            } else
                Log("portals: command not applied: %.300s", text ? text : "(none)");
        }
    }

    // Applies Hydra update commands to a record.
    void ApplyCommands(json::Value* record, const HandleArray& commands) {
        for (s32 i = 0; i < commands.num; i++) {
            char* text = ValueJson(commands.data[i]);
            json::Value* command = json::Parse(text);
            ApplyCommand(record, command, text);
            json::Destroy(command);
            game::Free(text);
        }
    }

    // Saves the record and returns it as the reply. `rewardsUserData` (the
    // granted batch's user_data, if any) is added as "rewards" in the format the unlock pop-ups read.
    void Commit(void* user, json::Value* record, const char* key, game::HydraHandle* reply, const char* rewardsUserData = nullptr) {
        char* text = json::Serialize(record);
        if (!text) return;
        store::Put(key, text);
        store::Flush();
        s_RecordsStale = true;
        if (rewardsUserData) {
            size_t n = strlen(text) + strlen(rewardsUserData) + 64;
            char* withRewards = static_cast<char*>(game::Malloc(n));
            // the record is an object: insert "rewards" before its closing brace
            snprintf(withRewards, n, "%.*s,\"rewards\":{\"items\":[],\"user_data\":%s}}", (int)strlen(text) - 1, text, rewardsUserData);
            hydra::CompletedReply(reply, withRewards);
            game::Free(withRewards);
        } else {
            hydra::CompletedReply(reply, text);
        }
        game::Free(text);
    }

    // A completed tower carries its rewards as an inventory batch in extraData.unlocks (built by
    // MK11PortalEvent::ExecuteEventCompletedSSCEvent); the server would grant it. Applied here like a
    // Krypt batch. Returns the batch's user_data JSON for the reply (free with std::free), or nullptr.
    char* GrantUnlocks(const char* extraText) {
        Log("portals: reward data %zu bytes", strlen(extraText));
        json::Value* extra = json::Parse(extraText);
        json::Value* unlocks = json::Find(extra, "unlocks");
        Log("portals: reward data parsed: %s, unlocks %s", extra ? "yes" : "no", unlocks ? "found" : "none");
        char* userData = nullptr;
        if (unlocks) {
            char* batchText = json::Serialize(unlocks);
            if (batchText) {
                Log("portals: tower rewards %.300s", batchText);
                store::RecordBatch(batchText);
                batch::Apply(batchText);
                Log("portals: tower rewards granted");
                game::Free(batchText);
            }
            userData = json::Serialize(json::Find(unlocks, "user_data"));
        }
        json::Destroy(extra);
        return userData;
    }

    void RecordRequest(const char* kind, const char* a, const char* b, const char* c, u64 version,
                       const HandleArray& commands, const void* extra) {
        size_t size = 512;
        char** parts = static_cast<char**>(game::Calloc(commands.num + 1, sizeof(char*)));
        for (s32 i = 0; i < commands.num; i++) {
            parts[i] = ValueJson(commands.data[i]);
            size += parts[i] ? strlen(parts[i]) + 1 : 5;
        }
        char* extraText = ValueJson(extra);
        size += extraText ? strlen(extraText) : 4;
        char* line = static_cast<char*>(game::Malloc(size));
        int n = snprintf(line, size, "{\"request\":\"%s\",\"args\":[\"%s\",\"%s\",\"%s\"],\"version\":%llu,\"commands\":[",
                         kind, a ? a : "", b ? b : "", c ? c : "", (unsigned long long)version);
        for (s32 i = 0; i < commands.num; i++)
            n += snprintf(line + n, size - n, "%s%s", i ? "," : "", parts[i] ? parts[i] : "null");
        snprintf(line + n, size - n, "],\"extra\":%s}", extraText ? extraText : "null");
        store::AppendLine("portal_requests.jsonl", line);
        for (s32 i = 0; i < commands.num; i++) game::Free(parts[i]);
        game::Free(parts);
        game::Free(extraText);
        game::Free(line);
    }
}

// NRSDate::DateTime is 16 bytes and returned through x8 (it is not trivially copyable).
struct DateTimeValue {
    alignas(8) u8 bytes[0x10];
    ~DateTimeValue() {}
};

// Online values carry dates only as OnlineValueDate, which record JSON cannot express, so a record
// handed to UpdatePortalGOSObject has no data.endTime. Summoned tower records keep their end as
// "offlineEndMs" and it is written into the loaded record (PortalGOSData: data +0x38, endTime +0x38).
namespace {
    constexpr ptrdiff_t RecordEndTimeOffset = 0x70;

    void ApplyStoredEndTime(const void* record, const char* json) {
        const char* p = record && json ? strstr(json, "\"offlineEndMs\":") : nullptr;
        if (!p) return;
        s64* end = reinterpret_cast<s64*>(static_cast<u8*>(const_cast<void*>(record)) + RecordEndTimeOffset);
        end[0] = strtoll(p + 15, nullptr, 10);
        end[1] = 0;
    }
}

// Towers of Time: the calendar towers are chosen from the date of the last Hydra portal request
// (FGUserEntity::GetLastPortalRequestTime), and each one is only created when its record exists. Offline
// the date is set to now, and a missing record is generated by the game's own TimedPortalObj.
namespace {
    constexpr size_t TimedPortalObjSize = 0x98;  // operator new size in CreateCalendarPortals
    constexpr int MaxCalendarPortals = 6;        // CreateCalendarPortals uses at most 6
    // PortalDefinition: +0x00 start DateTime, +0x10 end DateTime, +0x20 name, +0x30 PortalObjType.
    constexpr ptrdiff_t DefinitionEndOffset = 0x10;
    constexpr ptrdiff_t DefinitionNameOffset = 0x20;
    constexpr ptrdiff_t DefinitionTypeOffset = 0x30;
    constexpr size_t DateTimeSize = 0x10;

    // Set while a Towers of Time record is generated: the online portal then builds its stages and events
    // from the portal definition script ("<name>_PortalInit", as local portals do) instead of the record.
    bool s_GeneratingTimed = false;

    char* GenerateTimedRecord(void* user, const u8* definition) {
        const char* name = reinterpret_cast<const game::NRSString*>(definition + DefinitionNameOffset)->c_str();
        s32 type = *reinterpret_cast<const s32*>(definition + DefinitionTypeOffset);
        void* portal = game::Fn<void* (*)(size_t)>(off::OperatorNew)(TimedPortalObjSize);
        game::Fn<void (*)(void*, void*, void*)>(off::TimedPortalObj_Ctor)(portal, user, nullptr);
        char* record = nullptr;
        s_GeneratingTimed = true;
        bool ok = game::Fn<bool (*)(void*, const char*, s32, const void*, const void*)>(off::TimedPortalObj_PortalObjInit)(
            portal, name, type, definition, definition + DefinitionEndOffset);
        s_GeneratingTimed = false;
        if (ok) {
            alignas(16) static u8 data[0x200];
            game::Fn<void (*)(void*)>(off::PortalSSCData_Ctor)(data);
            game::Fn<void (*)(void*, void*)>(off::MK11PortalObj_GenerateServerData)(portal, data);
            game::NRSString json = game::Fn<game::NRSString (*)(const void*, int)>(off::PortalSSCData_ToJsonString)(data, 0);
            size_t n = strlen(json.c_str()) + strlen(name) * 2 + 128;
            record = static_cast<char*>(game::Malloc(n));
            if (record) snprintf(record, n, "{\"id\":\"offline-%s\",\"unique_key\":\"%s\",\"data\":%s}", name, name, json.c_str());
        }
        auto vtable = *reinterpret_cast<void (***)(void*)>(portal);
        vtable[1](portal);
        return record;
    }

    // A DateTime (milliseconds since 1970 at +0) as a JSON string "YYYY-MM-DDTHH:MM:SS+0000".
    void FormatDateTime(const u8* dateTime, char* out, size_t size) {
        s64 seconds = *reinterpret_cast<const s64*>(dateTime) / 1000;
        s64 days = seconds / 86400, rest = seconds % 86400;
        if (rest < 0) { rest += 86400; days--; }
        // civil date from days since 1970-01-01 (Howard Hinnant's algorithm)
        days += 719468;
        s64 era = (days >= 0 ? days : days - 146096) / 146097;
        s64 doe = days - era * 146097;
        s64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        s64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        s64 mp = (5 * doy + 2) / 153;
        s64 day = doy - (153 * mp + 2) / 5 + 1;
        s64 month = mp < 10 ? mp + 3 : mp - 9;
        s64 year = yoe + era * 400 + (month <= 2);
        snprintf(out, size, "\"%04d-%02d-%02dT%02d:%02d:%02d+0000\"", (int)(year % 10000), (int)month, (int)day,
                 (int)(rest / 3600), (int)(rest / 60 % 60), (int)(rest % 60));
    }

    // Makes sure every current calendar tower has a record (from the SD card, or generated) whose end
    // time is the tower's end.
    // Removes stored Towers of Time records whose tower has ended (Klassic Towers never ends).
    void RemoveExpiredRecords(const char* now) {
        struct Context { const char* now; int removed; } context{ now, 0 };
        store::ForEach("portal/", [](const char* key, const char* json, void* raw) {
            auto context = static_cast<Context*>(raw);
            if (!strcmp(key, "portal/KlassicTowers")) return;
            json::Value* record = json::Parse(json);
            const char* end = json::String(json::Find(record, "data.endTime"));
            // ISO times compare as text ("2021-09-30T03:00:00+0000"); the stored value has no quotes
            if (end && strncmp(end, context->now + 1, 24) < 0) {
                store::Remove(key);
                context->removed++;
            }
            json::Destroy(record);
        }, &context);
        if (context.removed) Log("portals: %d expired Towers of Time record(s) removed", context.removed);
    }

    void EnsureCalendarRecords(void* user) {
        store::Batch storeBatch;
        u8* requestTime = game::Fn<u8* (*)(void*)>(off::FGUserEntity_GetLastPortalRequestTime)(user);
        DateTimeValue now = game::Fn<DateTimeValue (*)()>(off::DateTime_CurrentGameTime)();
        memcpy(requestTime, now.bytes, DateTimeSize);
        Log("portals: Towers of Time date set to now (year %d)", game::Fn<s32 (*)(const void*)>(off::DateTime_GetYear)(requestTime));
        char nowText[64];
        FormatDateTime(now.bytes, nowText, sizeof(nowText));
        RemoveExpiredRecords(nowText);

        alignas(16) u8 info[0x20] = {};
        game::Fn<void (*)(const void*, void*)>(off::MK11PortalInfo_GetCalendarPortalInfos)(requestTime, info);
        game::Fn<void (*)(void*)>(off::PortalEventInfo_Sort)(info);
        s32 count = game::Fn<s32 (*)(void*)>(off::PortalEventInfo_GetNumOfPortals)(info);
        if (count > MaxCalendarPortals) count = MaxCalendarPortals;
        for (s32 i = 0; i < count; i++) {
            const u8* definition = game::Fn<const u8* (*)(void*, s32)>(off::PortalEventInfo_GetPortal)(info, i);
            if (!definition) continue;
            const char* name = reinterpret_cast<const game::NRSString*>(definition + DefinitionNameOffset)->c_str();
            if (!name[0]) continue;
            char key[160];
            StoreKey(key, sizeof(key), name);
            char* generated = nullptr;
            const char* text = store::Get(key);
            if (!text) {
                generated = GenerateTimedRecord(user, definition);
                if (!generated) {
                    Log("portals: %s record could not be generated", name);
                    continue;
                }
                Log("portals: %s record generated (%zu bytes)", name, strlen(generated));
                text = generated;
            }
            // Hydra sets the record's endTime (the tower expires then); it is the definition's end.
            json::Value* record = json::Parse(text);
            game::Free(generated);
            if (!record) continue;
            char endTime[64];
            FormatDateTime(definition + DefinitionEndOffset, endTime, sizeof(endTime));
            json::Value* end = json::Parse(endTime);
            json::Set(record, "data.endTime", end);
            json::Destroy(end);
            char* updated = json::Serialize(record);
            json::Destroy(record);
            if (!updated) continue;
            store::Put(key, updated);
            Log("portals: %s ends %s: %s", name, endTime, Inject(user, updated) ? "loaded" : "NOT loaded");
            game::Free(updated);
        }
        store::Flush();
        game::Fn<void (*)(void*)>(off::PortalDefinitionArray_Dtor)(info);
    }
}

HOOK_DEFINE_TRAMPOLINE(OnlinePortalCreateEvents) {
    static bool Callback(void* portal) {
        if (s_GeneratingTimed && !*reinterpret_cast<void**>(static_cast<u8*>(portal) + 48))
            return game::Fn<bool (*)(void*)>(off::MK11PortalObj_CreateEventsAndStages)(portal);
        return Orig(portal);
    }
};

extern "C" u32 SdkUserClockGetCurrentTime(s64* posixTime) asm("_ZN2nn4time23StandardUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE");

// The game's clock (time() + gmtime) reads 1970 on this offline console; it falls back to the console's
// user clock then (tower calendars, expiry times). A DateTime holds milliseconds since 1970 at +0.
HOOK_DEFINE_TRAMPOLINE(NetworkTime) {
    static DateTimeValue Callback() {
        DateTimeValue now = Orig();
        if (game::Fn<s32 (*)(const void*)>(off::DateTime_GetYear)(now.bytes) >= 2000) return now;
        s64 seconds = 0;
        u32 result = SdkUserClockGetCurrentTime(&seconds);
        static int s_Logged = 0;
        if (s_Logged++ < 3) Log("clock: game time %lld ms; user clock result %#x, %lld s", (long long)*reinterpret_cast<s64*>(now.bytes), result, (long long)seconds);
        if (result == 0 && seconds > 0) {
            *reinterpret_cast<s64*>(now.bytes) = seconds * 1000;
            *reinterpret_cast<s64*>(now.bytes + 8) = 0;
        }
        return now;
    }
};

// After a tower the game goes back to the tower screen without loading the tower records again, so
// objectives completed offline (and the towers they unlock) only showed after re-entering Towers of Time.
// Changed records are handed to the game whenever the tower screen starts.
HOOK_DEFINE_TRAMPOLINE(TowersScreenStart) {
    static void Callback(void* screen) {
        void* user = game::Fn<void* (*)(int, unsigned)>(off::GetActiveModeUser)(0, 1);
        if (s_RecordsStale && Offline(user)) {
            s_RecordsStale = false;
            int count = 0;
            struct Context { void* user; int* count; } context{ user, &count };
            store::ForEach("portal/", [](const char* key, const char* json, void* c) {
                auto* ctx = static_cast<Context*>(c);
                if (Inject(ctx->user, json)) (*ctx->count)++;
                else Log("portals: %s not refreshed", key);
            }, &context);
            Log("portals: %d tower record(s) refreshed for the tower screen", count);
        }
        Orig(screen);
    }
};

HOOK_DEFINE_TRAMPOLINE(CalendarCreatePortals) {
    static void Callback(void* user, void* portals) {
        if (Offline(user)) EnsureCalendarRecords(user);
        Orig(user, portals);
    }
};

// Stored records are handed to the game the first time it looks one up while offline.
HOOK_DEFINE_TRAMPOLINE(PortalGetRecord) {
    static inline bool s_Injecting = false;  // UpdatePortalGOSObject may look records up itself

    static const void* Callback(void* user, const game::NRSString& name) {
        const void* record = Orig(user, name);
        if (record || s_Injecting || !Offline(user) || !name.c_str()[0]) return record;
        char key[128];
        StoreKey(key, sizeof(key), name.c_str());
        const char* stored = store::Get(key);
        if (!stored) return record;
        s_Injecting = true;
        bool ok = Inject(user, stored);
        s_Injecting = false;
        if (!ok) return record;
        Log("portals: %s record loaded from the SD card", name.c_str());
        record = Orig(user, name);
        ApplyStoredEndTime(record, stored);
        return record;
    }
};

HOOK_DEFINE_TRAMPOLINE(KlassicCreatePortals) {
    static void Callback(void* mode) {
        void* user = ActiveUser();
        if (Offline(user)) EnsureKlassicRecord(user);
        Orig(mode);
    }
};

// Offline, the full Klassic Towers experience only needs the tower record (normally also a Hydra
// connection).
HOOK_DEFINE_TRAMPOLINE(KlassicEligible) {
    static bool Callback(void* user) {
        if (!Offline(user)) return Orig(user);
        bool ok = EnsureKlassicRecord(user);
        Log("portals: Klassic Towers full experience offline: %s", ok ? "yes" : "no record");
        return ok;
    }
};

HOOK_DEFINE_TRAMPOLINE(PortalUpdate) {
    static game::HydraHandle Callback(void* user, const game::NRSString* a, const game::NRSString* b, const game::NRSString* c,
                                      u64 version, HandleArray* commands, const game::OnlineValueHandle* data) {
        if (!Offline(user)) return Orig(user, a, b, c, version, commands, data);
        store::Batch storeBatch;
        RecordRequest("UpdatePortal", a->c_str(), b->c_str(), c->c_str(), version, *commands, data ? data->value : nullptr);
        game::HydraHandle reply;
        const char* names[] = { b->c_str(), a->c_str(), c->c_str() };
        char key[128];
        json::Value* record = LoadRecord(names, 3, key, sizeof(key));
        if (!record) {
            Log("portals: UpdatePortal for unknown tower (%s / %s / %s)", a->c_str(), b->c_str(), c->c_str());
            hydra::CompletedReply(&reply, nullptr);
            return reply;
        }
        ApplyCommands(record, *commands);
        // A completed Premier objective set ("portal_quest_completed", MK11PortalObj::CompletePortalQuestObjective)
        // carries its rewards in the extra data's unlocks, like a completed tower.
        char* extraText = ValueJson(data ? data->value : nullptr);
        char* rewards = extraText && strstr(extraText, "\"unlocks\"") ? GrantUnlocks(extraText) : nullptr;
        Commit(user, record, key, &reply, rewards);
        game::Free(rewards);
        game::Free(extraText);
        json::Destroy(record);
        Log("portals: UpdatePortal %s: %d command(s) applied%s", key, commands->num, rewards ? ", rewards granted" : "");
        return reply;
    }
};

// Unlike UpdatePortal, UpdatePortalEvent returns the request's OnlineHydra object id (the caller looks
// it up with OnlineHydra::GetObjectById and keeps it as the event's completion request).
HOOK_DEFINE_TRAMPOLINE(PortalEventUpdate) {
    static u64 Callback(void* user, const game::NRSString* a, const game::NRSString* b, const game::NRSString* c,
                                      u64 version, s32 stage, const game::NRSString* eventName, s32 eventIndex,
                                      HandleArray* commands, const game::OnlineValueHandle* extra) {
        if (!Offline(user)) return Orig(user, a, b, c, version, stage, eventName, eventIndex, commands, extra);
        store::Batch storeBatch;
        RecordRequest("UpdatePortalEvent", a->c_str(), b->c_str(), eventName->c_str(), version, *commands,
                      extra ? extra->value : nullptr);
        Log("portals: UpdatePortalEvent %s / %s stage %d event %d (%s)", a->c_str(), b->c_str(), stage, eventIndex, eventName->c_str());
        game::HydraHandle reply;
        const char* names[] = { b->c_str(), a->c_str(), c->c_str() };
        char key[128];
        json::Value* record = LoadRecord(names, 3, key, sizeof(key));
        if (!record) {
            hydra::CompletedReply(&reply, nullptr);
            return hydra::ObjectId(reply.value);
        }
        ApplyCommands(record, *commands);
        char* extraText = ValueJson(extra ? extra->value : nullptr);
        Log("portals: event extra data %s", extraText ? "read" : "none");
        if (extraText) RecordBestScore(record, b->c_str(), stage, eventName->c_str(), extraText);
        char* rewards = extraText ? GrantUnlocks(extraText) : nullptr;
        Commit(user, record, key, &reply, rewards);
        game::Free(rewards);
        game::Free(extraText);
        json::Destroy(record);
        // The reply's reference is kept (HydraHandle does not release it), so the id stays valid.
        Log("portals: UpdatePortalEvent answered (request id %#llx)", (unsigned long long)hydra::ObjectId(reply.value));
        return hydra::ObjectId(reply.value);
    }
};

HOOK_DEFINE_TRAMPOLINE(PortalUpdateMultiple) {
    static game::HydraHandle Callback(void* user, const game::OnlineValueHandle* data) {
        if (!Offline(user)) return Orig(user, data);
        HandleArray none{ nullptr, 0 };
        store::Batch storeBatch;
        RecordRequest("UpdateMultiplePortals", nullptr, nullptr, nullptr, 0, none, data ? data->value : nullptr);
        // Global requirements (Premier tower objectives counted across towers, MK11PortalInfo::
        // GetPendingGlobalRequirementUpdates): {<tower>: [commands]} for each tower with queued commands.
        char* text = ValueJson(data ? data->value : nullptr);
        json::Value* towers = text ? json::Parse(text) : nullptr;
        for (size_t t = 0; t < json::Members(towers); t++) {
            const char* names[] = { json::KeyAt(towers, t) };
            json::Value* commands = json::MemberAt(towers, t);
            char key[128];
            json::Value* record = LoadRecord(names, 1, key, sizeof(key));
            if (!record) {
                Log("portals: UpdateMultiplePortals: no record for %s", names[0]);
                continue;
            }
            for (size_t i = 0; i < json::Count(commands); i++) {
                json::Value* command = json::At(commands, i);
                char* commandText = json::Serialize(command);
                ApplyCommand(record, command, commandText);
                std::free(commandText);
            }
            if (char* saved = json::Serialize(record)) {
                store::Put(key, saved);
                std::free(saved);
                s_RecordsStale = true;
            }
            Log("portals: UpdateMultiplePortals %s: %zu command(s) applied", key, json::Count(commands));
            json::Destroy(record);
        }
        json::Destroy(towers);
        game::Free(text);
        game::HydraHandle reply;
        hydra::CompletedReply(&reply, nullptr);
        return reply;
    }
};

// Towers of Time summonable towers: MK11PortalSSC::SummonKeyPortal asks Hydra ("portal_key_activate") to
// activate the tower a key summons (activatePortalName), take the key (cost) and drop the tower summoned
// before (deactivatePortalId). Offline the record is generated here and MK11PortalInfo::CreateKeyPortal
// then finds it: data.type "key", available until data.endTime.
namespace {
    constexpr size_t KeyPortalObjSize = 0x98;     // KeyPortalObj / TimedPortalObj operator new size
    constexpr int PortalTypeKey = 2;              // PortalObjType passed by CreateKeyPortal
    constexpr s64 KeyTowerDurationMs = 24LL * 3600 * 1000;

    // Generated like a Towers of Time record (TimedPortalObj, events and stages from the tower's
    // "<name>_PortalInit" script): a KeyPortalObj without a record only gets an empty stage.
    char* GenerateKeyRecord(void* user, const char* name, const u8* start, const u8* end) {
        void* portal = game::Fn<void* (*)(size_t)>(off::OperatorNew)(KeyPortalObjSize);
        game::Fn<void (*)(void*, void*, void*)>(off::TimedPortalObj_Ctor)(portal, user, nullptr);
        char* record = nullptr;
        s_GeneratingTimed = true;
        bool ok = game::Fn<bool (*)(void*, const char*, s32, const void*, const void*)>(off::TimedPortalObj_PortalObjInit)(
            portal, name, PortalTypeKey, start, end);
        s_GeneratingTimed = false;
        if (ok) {
            alignas(16) static u8 data[0x200];
            game::Fn<void (*)(void*)>(off::PortalSSCData_Ctor)(data);
            game::Fn<void (*)(void*, void*)>(off::MK11PortalObj_GenerateServerData)(portal, data);
            game::NRSString json = game::Fn<game::NRSString (*)(const void*, int)>(off::PortalSSCData_ToJsonString)(data, 0);
            size_t n = strlen(json.c_str()) + strlen(name) + 128;
            record = static_cast<char*>(game::Malloc(n));
            if (record) snprintf(record, n, "{\"id\":\"offline-key-%s\",\"unique_key\":\"%s\",\"data\":%s}", name, name, json.c_str());
        }
        auto vtable = *reinterpret_cast<void (***)(void*)>(portal);
        vtable[1](portal);  // deleting destructor
        return record;
    }

    // Removes stored records of summoned towers (data.type "key") other than `keep`.
    void RemoveKeyRecords(const char* keep) {
        struct Context { const char* keep; int removed; } context{ keep, 0 };
        store::ForEach("portal/", [](const char* key, const char* json, void* raw) {
            auto context = static_cast<Context*>(raw);
            if (!strcmp(key, context->keep)) return;
            json::Value* record = json::Parse(json);
            const char* type = json::String(json::Find(record, "data.type"));
            if (type && !strcmp(type, "key")) {
                store::Remove(key);
                context->removed++;
            }
            json::Destroy(record);
        }, &context);
        if (context.removed) Log("portals: %d previously summoned tower record(s) removed", context.removed);
    }

    void SetJson(json::Value* record, const char* path, const char* text) {
        json::Value* value = json::Parse(text);
        json::Set(record, path, value);
        json::Destroy(value);
    }
}

// Set by an offline summon: the Towers of Time menu then finds its "Summonable Towers" placeholder
// without a record, says it has expired and rebuilds the key tower (MK11PortalMode::ReplacePortal)
// after a portal feed refresh; that notice is skipped once (main.cpp, OkMessageBox).
bool s_SummonedOffline = false;

HOOK_DEFINE_TRAMPOLINE(SummonKey) {
    static game::HydraHandle Callback(void* user, const char* name, const game::NRSString& keySlug, const void* previous) {
        if (!Offline(user) || !name || !name[0]) return Orig(user, name, keySlug, previous);
        const char* slug = keySlug.c_str();
        int keys = wallet::Get(slug);
        DateTimeValue start = game::Fn<DateTimeValue (*)()>(off::DateTime_CurrentGameTime)();
        DateTimeValue end = start;
        *reinterpret_cast<s64*>(end.bytes) += KeyTowerDurationMs;
        char* generated = keys > 0 ? GenerateKeyRecord(user, name, start.bytes, end.bytes) : nullptr;
        json::Value* record = generated ? json::Parse(generated) : nullptr;
        game::Free(generated);
        game::HydraHandle reply;
        if (!record) {
            Log("portals: summon %s with %s (%d owned) failed", name, slug, keys);
            return reply;
        }
        char endTime[64];
        FormatDateTime(end.bytes, endTime, sizeof(endTime));
        SetJson(record, "data.endTime", endTime);
        SetJson(record, "data.type", "\"key\"");
        char endMs[24];
        snprintf(endMs, sizeof(endMs), "%lld", (long long)*reinterpret_cast<s64*>(end.bytes));
        SetJson(record, "offlineEndMs", endMs);
        char* text = json::Serialize(record);
        json::Destroy(record);
        if (!text) return reply;

        store::Batch storeBatch;
        char key[160];
        StoreKey(key, sizeof(key), name);
        RemoveKeyRecords(key);
        store::Put(key, text);
        wallet::Set(slug, keys - 1);
        bool loaded = Inject(user, text);
        if (loaded) {
            game::NRSString recordName(name);
            ApplyStoredEndTime(game::Fn<const void* (*)(void*, const game::NRSString&)>(off::MK11PortalInfo_GetPortalGOSData)(user, recordName), text);
        }
        Log("portals: %s summoned with %s until %s: %s (%zu bytes)", name, slug, endTime, loaded ? "loaded" : "NOT loaded", strlen(text));
        game::Free(text);
        s_SummonedOffline = loaded;
        hydra::CompletedReply(&reply, nullptr);
        return reply;
    }
};

namespace mk11::portals {
    bool TakeSummonNotice() {
        bool taken = s_SummonedOffline;
        s_SummonedOffline = false;
        return taken;
    }

    void Install() {
        PortalGetRecord::InstallAtOffset(off::MK11PortalInfo_GetPortalGOSData);
        KlassicCreatePortals::InstallAtOffset(off::MK11KlassicPortalMode_CreatePortals);
        KlassicEligible::InstallAtOffset(off::MK11KlassicPortalMode_IsEligibleForFullExperience);
        PortalUpdate::InstallAtOffset(off::MK11PortalSSC_UpdatePortal);
        PortalEventUpdate::InstallAtOffset(off::MK11PortalSSC_UpdatePortalEvent);
        PortalUpdateMultiple::InstallAtOffset(off::MK11PortalSSC_UpdateMultiplePortals);
        TowersScreenStart::InstallAtOffset(off::UITowersMainScreen_StartScreen);
        CalendarCreatePortals::InstallAtOffset(off::MK11PortalInfo_CreateCalendarPortals);
        OnlinePortalCreateEvents::InstallAtOffset(off::MK11OnlinePortalObj_CreateEventsAndStages);
        NetworkTime::InstallAtOffset(off::DateTime_PlatformGetNetworkUTC);
        SummonKey::InstallAtOffset(off::MK11PortalSSC_SummonKeyPortal);
        // Towers of Time menu: the reconnect attempt succeeds and its follow-up error check finds nothing;
        // the portal feed download, its success, the Hydra connection and the feed freshness are all
        // treated as satisfied (the records come from the SD card).
        exl::patch::CodePatcher(off::GoingToPortals_ReconnectCall).Write<u32>(0x52800020);       // mov w0, #1
        exl::patch::CodePatcher(off::GoingToPortals_ScriptErrorCheckCall).Write<u32>(0x52800000); // mov w0, #0
        for (ptrdiff_t call : { off::GoingToPortals_WaitForFeedCall, off::GoingToPortals_FeedSuccessfulCall,
                                off::GoingToPortals_IsConnectedCall, off::GoingToPortals_FeedUpToDateCall })
            exl::patch::CodePatcher(call).Write<u32>(0x52800020);  // mov w0, #1
    }
}
