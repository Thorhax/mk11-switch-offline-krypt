#pragma once
// Optional settings from sdmc:/mk11offline/config.ini ("key=value" lines, '#' comments).
// Settings default to off when the file or key is absent, except skip_tower_credits and offline_store (on) and
// krypt_chest_krystals (10).

namespace mk11::config {
    // Koins/Souls/Hearts are topped up to this amount whenever the Krypt starts (0 = off).
    int KryptCurrencyTopUp();
    // Gear rewards (add_game_item) are granted locally only when apply_gear_rewards=1; otherwise
    // they stay recorded in inventory_batches.jsonl.
    bool ApplyGearRewards();
    // The studio credits after a tower's character ending are skipped unless skip_tower_credits=0.
    bool SkipTowerCredits();
    // The Store (ZR) opens with the offline catalogue unless offline_store=0.
    bool OfflineStore();
    // Time Krystals given for each Krypt chest opened (krypt_chest_krystals, default 10; 0 = off).
    int KryptChestKrystals();
    // Player 1 is fought by the CPU in every fight (p1_cpu=1): CPU vs CPU matches, and testing.
    bool P1Cpu();
    // Offline Daily Challenges on the Kombat Kard (daily_challenges=1; experimental, progress is not counted yet).
    bool DailyChallenges();
}
