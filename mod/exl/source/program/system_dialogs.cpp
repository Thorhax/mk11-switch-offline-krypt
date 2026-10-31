#include "lib.hpp"
#include "log.hpp"
#include "system_dialogs.hpp"

// System error dialogs the game can raise while offline. Each call is logged with its caller so
// the boot-time 2123-0011 dialog can be traced to the call that shows it.
extern "C" {
    void SdkShowErrorResult(u32 result) asm("_ZN2nn3err9ShowErrorENS_6ResultE");
    void SdkShowErrorCode(u64 code) asm("_ZN2nn3err9ShowErrorENS0_9ErrorCodeE");
    void SdkShowApplicationError(const void* arg) asm("_ZN2nn3err20ShowApplicationErrorERKNS0_19ApplicationErrorArgE");
    u32 SdkHandleNetworkRequestErrorResult() asm("_ZN2nn4nifm31HandleNetworkRequestErrorResultEv");
    u32 SdkEnsureNetworkServiceAccountAvailable(const void* user) asm("_ZN2nn7account36EnsureNetworkServiceAccountAvailableERKNS0_10UserHandleE");
    u32 SdkShowLicenseRequirementsForNetworkService(const void* user) asm("_ZN2nn7account40ShowLicenseRequirementsForNetworkServiceERKNS0_10UserHandleE");
}

namespace {
    uintptr_t Caller(void* lr) {
        return reinterpret_cast<uintptr_t>(lr) - exl::util::modules::GetTargetStart();
    }
}

HOOK_DEFINE_TRAMPOLINE(ShowErrorResult) {
    static void Callback(u32 result) {
        mk11::Log("system: nn::err::ShowError(Result %#x = %04d-%04d) from main+%#lx", result,
                  2000 + (result & 0x1ff), (result >> 9) & 0x1fff, Caller(__builtin_return_address(0)));
        Orig(result);
    }
};

HOOK_DEFINE_TRAMPOLINE(ShowErrorCode) {
    static void Callback(u64 code) {
        mk11::Log("system: nn::err::ShowError(ErrorCode %04u-%04u) from main+%#lx", (u32)code, (u32)(code >> 32),
                  Caller(__builtin_return_address(0)));
        Orig(code);
    }
};

HOOK_DEFINE_TRAMPOLINE(ShowApplicationError) {
    static void Callback(const void* arg) {
        mk11::Log("system: nn::err::ShowApplicationError from main+%#lx", Caller(__builtin_return_address(0)));
        Orig(arg);
    }
};

HOOK_DEFINE_TRAMPOLINE(HandleNetworkRequestErrorResult) {
    static u32 Callback() {
        u32 r = Orig();
        mk11::Log("system: nn::nifm::HandleNetworkRequestErrorResult -> %#x from main+%#lx", r, Caller(__builtin_return_address(0)));
        return r;
    }
};

HOOK_DEFINE_TRAMPOLINE(EnsureNetworkServiceAccountAvailable) {
    static u32 Callback(const void* user) {
        u32 r = Orig(user);
        mk11::Log("system: nn::account::EnsureNetworkServiceAccountAvailable -> %#x from main+%#lx", r, Caller(__builtin_return_address(0)));
        return r;
    }
};

HOOK_DEFINE_TRAMPOLINE(ShowLicenseRequirements) {
    static u32 Callback(const void* user) {
        u32 r = Orig(user);
        mk11::Log("system: nn::account::ShowLicenseRequirementsForNetworkService -> %#x from main+%#lx", r, Caller(__builtin_return_address(0)));
        return r;
    }
};

namespace mk11::system_dialogs {
    void Install() {
        ShowErrorResult::InstallAtFuncPtr(SdkShowErrorResult);
        ShowErrorCode::InstallAtFuncPtr(SdkShowErrorCode);
        ShowApplicationError::InstallAtFuncPtr(SdkShowApplicationError);
        HandleNetworkRequestErrorResult::InstallAtFuncPtr(SdkHandleNetworkRequestErrorResult);
        EnsureNetworkServiceAccountAvailable::InstallAtFuncPtr(SdkEnsureNetworkServiceAccountAvailable);
        ShowLicenseRequirements::InstallAtFuncPtr(SdkShowLicenseRequirementsForNetworkService);
    }
}
