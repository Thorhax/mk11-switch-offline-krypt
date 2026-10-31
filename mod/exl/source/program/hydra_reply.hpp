#pragma once
// Completed Hydra replies made on the console, for requests the module answers itself.

#include "game.hpp"

namespace mk11::hydra {
    // A completed, successful OnlineHydraSSC whose response is the parsed `responseJson` ("{}" when
    // null). Nothing ever sends it. `out` stays empty when it cannot be built.
    void CompletedReply(game::HydraHandle* out, const char* responseJson);
    // The reply's state and error code, for logging.
    void Describe(const void* ssc, s32* state, u32* code);
}
