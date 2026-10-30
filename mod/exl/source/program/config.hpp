#pragma once
// Optional settings from sdmc:/mk11offline/config.ini ("key=value" lines, '#' comments).
// Every setting defaults to off when the file or key is absent.

namespace mk11::config {
    // Koins/Souls/Hearts are topped up to this amount whenever the Krypt starts (0 = off).
    int KryptCurrencyTopUp();
    // Gear rewards (add_game_item) are granted locally only when apply_gear_rewards=1; otherwise
    // they stay recorded in inventory_batches.jsonl.
    bool ApplyGearRewards();
}
