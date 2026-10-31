#pragma once
// Towers (Klassic Towers, Towers of Time) without the server: tower records kept on the SD card.

namespace mk11::portals {
    void Install();
    // True once after a tower was summoned offline (its placeholder's "has expired" notice is skipped).
    bool TakeSummonNotice();
}
