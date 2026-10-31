#pragma once
// Optional settings from sdmc:/mk11offline/config.ini ("key=value" lines, '#' comments).
// Settings default to off when the file or key is absent, except skip_tower_credits (on).

namespace mk11::config {
    // Koins/Souls/Hearts are topped up to this amount whenever the Krypt starts (0 = off).
    int KryptCurrencyTopUp();
    // Gear rewards (add_game_item) are granted locally only when apply_gear_rewards=1; otherwise
    // they stay recorded in inventory_batches.jsonl.
    bool ApplyGearRewards();
    // The studio credits after a tower's character ending are skipped unless skip_tower_credits=0.
    bool SkipTowerCredits();
    // The Store (ZR) opens with the offline catalogue only when offline_store=1 (experimental: showing the
    // catalogue can close the game).
    bool OfflineStore();
}
