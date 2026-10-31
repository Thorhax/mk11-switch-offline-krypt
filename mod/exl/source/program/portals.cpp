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

    // Applies Hydra update commands ({updateType, path, value}) to a record.
    void ApplyCommands(json::Value* record, const HandleArray& commands) {
        for (s32 i = 0; i < commands.num; i++) {
            char* text = ValueJson(commands.data[i]);
            json::Value* command = json::Parse(text);
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

    char* GenerateTimedRecord(void* user, const u8* definition) {
        const char* name = reinterpret_cast<const game::NRSString*>(definition + DefinitionNameOffset)->c_str();
        s32 type = *reinterpret_cast<const s32*>(definition + DefinitionTypeOffset);
        void* portal = game::Fn<void* (*)(size_t)>(off::OperatorNew)(TimedPortalObjSize);
        game::Fn<void (*)(void*, void*, void*)>(off::TimedPortalObj_Ctor)(portal, user, nullptr);
        char* record = nullptr;
        if (game::Fn<bool (*)(void*, const char*, s32, const void*, const void*)>(off::TimedPortalObj_PortalObjInit)(
                portal, name, type, definition, definition + DefinitionEndOffset)) {
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

    // Makes sure every current calendar tower has a record (from the SD card, or generated).
    void EnsureCalendarRecords(void* user) {
        u8* requestTime = game::Fn<u8* (*)(void*)>(off::FGUserEntity_GetLastPortalRequestTime)(user);
        alignas(16) u8 now[DateTimeSize] = {};
        game::Fn<void (*)(u8*)>(off::DateTime_CurrentGameTime)(now);  // DateTime returned through x8
        memcpy(requestTime, now, DateTimeSize);
        Log("portals: Towers of Time date set to now (year %d)", game::Fn<s32 (*)(const void*)>(off::DateTime_GetYear)(requestTime));

        alignas(16) u8 info[0x20] = {};
        game::Fn<void (*)(const void*, void*)>(off::MK11PortalInfo_GetCalendarPortalInfos)(requestTime, info);
        game::Fn<void (*)(void*)>(off::PortalEventInfo_Sort)(info);
        s32 count = game::Fn<s32 (*)(void*)>(off::PortalEventInfo_GetNumOfPortals)(info);
        if (count > MaxCalendarPortals) count = MaxCalendarPortals;
        for (s32 i = 0; i < count; i++) {
            const u8* definition = game::Fn<const u8* (*)(void*, s32)>(off::PortalEventInfo_GetPortal)(info, i);
            if (!definition) continue;
            const char* name = reinterpret_cast<const game::NRSString*>(definition + DefinitionNameOffset)->c_str();
            if (!name[0] || FindRecord(user, name)) continue;  // FindRecord also loads stored records
            char* record = GenerateTimedRecord(user, definition);
            if (!record) {
                Log("portals: %s record could not be generated", name);
                continue;
            }
            char key[160];
            StoreKey(key, sizeof(key), name);
            store::Put(key, record);
            store::Flush();
            Log("portals: %s record generated (%zu bytes): %s", name, strlen(record), Inject(user, record) ? "loaded" : "NOT loaded");
            game::Free(record);
        }
        game::Fn<void (*)(void*)>(off::PortalDefinitionArray_Dtor)(info);
    }
}

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
        return Orig(user, name);
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
        Commit(user, record, key, &reply);
        json::Destroy(record);
        Log("portals: UpdatePortal %s: %d command(s) applied", key, commands->num);
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
        RecordRequest("UpdateMultiplePortals", nullptr, nullptr, nullptr, 0, none, data ? data->value : nullptr);
        Log("portals: UpdateMultiplePortals recorded, not applied yet");
        game::HydraHandle reply;
        hydra::CompletedReply(&reply, nullptr);
        return reply;
    }
};

namespace mk11::portals {
    void Install() {
        PortalGetRecord::InstallAtOffset(off::MK11PortalInfo_GetPortalGOSData);
        KlassicCreatePortals::InstallAtOffset(off::MK11KlassicPortalMode_CreatePortals);
        KlassicEligible::InstallAtOffset(off::MK11KlassicPortalMode_IsEligibleForFullExperience);
        PortalUpdate::InstallAtOffset(off::MK11PortalSSC_UpdatePortal);
        PortalEventUpdate::InstallAtOffset(off::MK11PortalSSC_UpdatePortalEvent);
        PortalUpdateMultiple::InstallAtOffset(off::MK11PortalSSC_UpdateMultiplePortals);
        CalendarCreatePortals::InstallAtOffset(off::MK11PortalInfo_CreateCalendarPortals);
        // Towers of Time menu: the portal feed download, its success, the Hydra connection and the feed
        // freshness are all treated as satisfied (the records come from the SD card).
        for (ptrdiff_t call : { off::GoingToPortals_WaitForFeedCall, off::GoingToPortals_FeedSuccessfulCall,
                                off::GoingToPortals_IsConnectedCall, off::GoingToPortals_FeedUpToDateCall })
            exl::patch::CodePatcher(call).Write<u32>(0x52800020);  // mov w0, #1
    }
}
