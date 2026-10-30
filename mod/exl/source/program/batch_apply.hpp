#pragma once
// Applies a Krypt inventory batch (the JSON MK11 would have posted to Hydra) on the console:
//  inc_consumable    -> wallet (Koins, Souls, Hearts only; never Time Krystals)
//  game_unlockables  -> MK11Unlockables::Unlock(page, bit) on the local profile
//  add_game_item     -> InventoryManager::AddItem (gear, level 1), only with apply_gear_rewards=1
// Other request kinds are logged; the whole batch is also kept in inventory_batches.jsonl.

namespace mk11::batch {
    void Apply(const char* json);
}

namespace mk11::wallet {
    // Wallet-managed consumables; anything else is left to the game.
    bool Manages(const char* slug);
    int Get(const char* slug);
    void Set(const char* slug, int value);
}
