#pragma once
// Daily Challenges offline (Kombat Kard, Quests page).

#include "game.hpp"

namespace mk11::daily {
    void Install();
    // Answers the daily challenge requests to Hydra ("daily_challenge_update", "daily_challenge_reward") while
    // offline; false for any other request.
    bool AnswerPost(const char* endpoint, const game::OnlineValueHandle* body, const void* done, game::HydraHandle* reply);
}
