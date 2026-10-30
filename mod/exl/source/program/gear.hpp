#pragma once
// Adds gear and augments to the local inventory the way MK11Unlockables::Unlock does, and applies
// the give list in sdmc:/mk11offline/give.txt.

namespace mk11::gear {
    // `id` is an item definition GUID ("%08X%08X%08X%08X", as in items.txt and Krypt batches) or an
    // internal item name such as AUG_MIL_SquareWave.
    bool Add(void* user, const char* id);

    // Adds every item listed in give.txt (one id per line, '#' comments), then deletes the file.
    void ApplyGiveList(void* user);
}
