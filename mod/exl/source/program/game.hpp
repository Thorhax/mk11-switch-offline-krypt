#pragma once
// Thin wrappers over MK11 functions the module calls directly (offsets in mk11_offsets.hpp).
// Types the game returns through the hidden x8 pointer are declared non-trivial here, so the
// compiler uses the same indirect-return convention when we call those functions.

#include "lib.hpp"
#include "mk11_offsets.hpp"

namespace mk11::game {

    template<typename F>
    inline F Fn(ptrdiff_t offset) {
        return reinterpret_cast<F>(exl::util::modules::GetTargetStart() + offset);
    }

    // NRS::NRSBasicString<NRS::DefaultContainerAllocator>: a TArray<char> with the terminator counted.
    struct NRSString {
        char* data = nullptr;
        s32 num = 0;
        s32 max = 0;

        NRSString() = default;
        explicit NRSString(const char* s) { Fn<void (*)(NRSString*, const char*)>(off::NRSString_FromCStr)(this, s); }
        NRSString(const NRSString&) = delete;
        ~NRSString() {
            num = 0;
            Fn<void (*)(NRSString*)>(off::TArrayBase_Dtor)(this);
        }
        const char* c_str() const { return num ? data : ""; }
    };

    // OnlineValueHandle / HydraHandle<T>: an intrusive pointer (refcount at +8, release at vtable[1]).
    struct OnlineValueHandle {
        void* value = nullptr;

        OnlineValueHandle() = default;
        OnlineValueHandle(const OnlineValueHandle&) = delete;
        ~OnlineValueHandle() { Fn<void (*)(OnlineValueHandle*)>(off::OnlineValueHandle_Dtor)(this); }
    };

    struct HydraHandle {
        void* value = nullptr;

        HydraHandle() = default;
        HydraHandle(const HydraHandle&) = delete;
        HydraHandle(HydraHandle&& other) : value(other.value) { other.value = nullptr; }
        ~HydraHandle() {}
    };

    // A new empty OnlineValueMap held by `out`.
    inline void NewOnlineValueMap(OnlineValueHandle* out) {
        void* v = Fn<void* (*)(size_t)>(off::OnlineValue_New)(0x60);
        Fn<void (*)(void*)>(off::OnlineValueMap_Ctor)(v);
        Fn<void (*)(OnlineValueHandle*, void*)>(off::OnlineValueHandle_FromPtr)(out, v);
    }

    inline NRSString ToJsonString(const void* value) {
        return Fn<NRSString (*)(const void*, int)>(off::OnlineValue_ToJsonString)(value, 0);
    }

    inline OnlineValueHandle ParseFromJson(const NRSString& json) {
        return Fn<OnlineValueHandle (*)(const NRSString&)>(off::OnlineValueUtils_ParseFromJson)(json);
    }

    inline const NRSString& KryptBackgroundName() {
        return *Fn<const NRSString* (*)()>(off::GetKryptBackgroundName)();
    }

    inline void* Malloc(size_t n) { return Fn<void* (*)(size_t)>(off::Game_malloc)(n); }
    inline void Free(void* p) { Fn<void (*)(void*)>(off::Game_free)(p); }

    // NRS::Function<void (bool, HydraHandle<OnlineHydraSSC>)>: points at a block whose +0x20 holds
    // the callable; the call operator is vtable slot 6 taking (callable, bool&&, HydraHandle&&).
    inline void InvokeSaveCallback(const void* function, bool ok) {
        auto impl = *reinterpret_cast<u8* const*>(function);
        if (!impl) return;
        auto callable = *reinterpret_cast<void**>(impl + 0x20);
        if (!callable) return;
        using CallFn = void (*)(void*, bool*, void**);
        auto call = (*reinterpret_cast<CallFn**>(callable))[6];
        void* handle = nullptr;
        call(callable, &ok, &handle);
    }

    // FKryptSaveDataBase: vtable[2] writes the object into an OnlineValue, vtable[3] reads it back;
    // its save key string is at +8.
    struct KryptSaveData {
        void** vtable;
        NRSString key;

        void SaveTo(OnlineValueHandle* h) { reinterpret_cast<void (*)(KryptSaveData*, OnlineValueHandle*)>(vtable[2])(this, h); }
        void LoadFrom(const OnlineValueHandle* h) { reinterpret_cast<void (*)(KryptSaveData*, const OnlineValueHandle*)>(vtable[3])(this, h); }
    };

    // TArray<T*, N, DefaultContainerAllocator>
    template<typename T>
    struct PtrArray {
        T** data;
        s32 num;
    };
}
