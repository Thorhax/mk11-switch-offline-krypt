#include "hydra_reply.hpp"
#include "log.hpp"

namespace mk11::hydra {

    namespace {
        // OnlineHydraSSC (OnlineHydraBase, 0x108 bytes): +8 reference count, +64 request state
        // (3 = complete), +0x48 OnlineError whose +8 code is negative on failure, +0xe8 the response
        // OnlineValueHandle.
        constexpr ptrdiff_t SscStateOffset = 64;
        constexpr ptrdiff_t SscErrorCodeOffset = 0x50;
        constexpr ptrdiff_t SscResponseOffset = 0xe8;
        constexpr s32 SscStateComplete = 3;
        constexpr int SscSize = 0x108;
        constexpr ptrdiff_t SscRefCountOffset = 8;
    }

    void CompletedReply(game::HydraHandle* out, const char* responseJson) {
        void* user = game::Fn<void* (*)(int, unsigned)>(off::GetActiveModeUser)(0, 1);
        if (!user) { Log("hydra reply: no user"); return; }
        const void* userHandle = game::Fn<const void* (*)(void*)>(off::FGUserEntity_GetUserHandle)(user);
        // Built like OnlineHydra::HydraObjectInit<OnlineHydraSSC>, which returns nothing while offline;
        // the request is not queued with OnlineHydra, so nothing ever sends it.
        auto ssc = static_cast<u8*>(game::Fn<void* (*)(int, int)>(off::IsolatedLongTermAllocator_Malloc)(SscSize, 8));
        if (!ssc) { Log("hydra reply: allocation failed"); return; }
        game::Fn<void (*)(void*, const void*)>(off::OnlineHydraSSC_Ctor)(ssc, userHandle);
        __atomic_add_fetch(reinterpret_cast<s32*>(ssc + SscRefCountOffset), 1, __ATOMIC_SEQ_CST);
        out->value = ssc;

        game::NRSString json(responseJson ? responseJson : "{}");
        game::OnlineValueHandle response = game::ParseFromJson(json);
        if (response.value) {
            auto slot = reinterpret_cast<game::OnlineValueHandle*>(ssc + SscResponseOffset);
            game::Fn<void (*)(game::OnlineValueHandle*)>(off::OnlineValueHandle_Dtor)(slot);
            slot->value = response.value;
            response.value = nullptr;
        }
        *reinterpret_cast<s32*>(ssc + SscErrorCodeOffset) = 0;
        *reinterpret_cast<s32*>(ssc + SscStateOffset) = SscStateComplete;
    }

    void Describe(const void* ssc, s32* state, u32* code) {
        *state = *reinterpret_cast<const s32*>(static_cast<const u8*>(ssc) + SscStateOffset);
        *code = *reinterpret_cast<const u32*>(static_cast<const u8*>(ssc) + SscErrorCodeOffset);
    }
}
