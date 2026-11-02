#pragma once
// Select screen art for Cyrax, Sektor and Kronika.

namespace mk11::selectart {
    void Install();

    // For a versus image the game lacks ("...ui_ps_Versus_Jpegs.<Name>_P1.jpg") on a versus image element:
    // "" if the fighter's art is shown already, a stand-in image to decode in its place (its image request is
    // then answered with the fighter's art), or null if there is no art.
    const char* VersusArt(void* element, const char* name);

    // Any image decode on a versus image element ends a previous VersusArt.
    void ClearVersusArt(void* element);
}
