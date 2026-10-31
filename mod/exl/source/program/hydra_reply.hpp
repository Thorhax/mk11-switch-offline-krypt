#pragma once
// Completed Hydra replies made on the console, for requests the module answers itself.

#include "game.hpp"

namespace mk11::hydra {
    // A completed, successful OnlineHydraSSC whose response is the parsed `responseJson` ("{}" when
    // null). Nothing ever sends it. `out` stays empty when it cannot be built.
    void CompletedReply(game::HydraHandle* out, const char* responseJson);
    // The reply's response value (the OnlineValueHandle a completion callback receives).
    const game::OnlineValueHandle* Response(const void* ssc);
    // Calls an NRS::Function<void (HydraHandle<OnlineHydraSSC>, OnlineError const&, OnlineValueHandle const&)>
    // with `reply`, a success OnlineError and the reply's response.
    void InvokeCompletion(const void* function, game::HydraHandle* reply);
    // The reply's state and error code, for logging.
    void Describe(const void* ssc, s32* state, u32* code);
}
