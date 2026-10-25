#pragma once

class KotoGbaUiService
{
public:
    /// Initializes the lower physical screen for kotoGba and loads the V0 home image.
    /// The GBA game is expected to run on the main engine / top physical screen with
    /// center-and-mask disabled.
    bool Initialize();
};

extern KotoGbaUiService gKotoGbaUiService;
