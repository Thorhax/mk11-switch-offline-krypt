#pragma once
// Writes every item definition the game has loaded to sdmc:/mk11offline/items.txt, so items can
// be looked up by internal name, GUID or display name.

namespace mk11::items {
    // Dumps the list once per boot when items.txt does not exist yet (delete it to dump again).
    void DumpIfMissing();

    // Writes the user's current inventory (one line per saved item) to inventory.txt.
    void DumpInventory(void* user);
}
