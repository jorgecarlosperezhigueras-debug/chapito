#pragma once
#include "common.h"

class KotoGbaRuntimeService
{
public:
    void ShowBiosError();
    void Initialize(u32 gameCode, u8 revision, bool packageInstalled);
};

extern KotoGbaRuntimeService gKotoGbaRuntimeService;

extern "C" {
extern volatile u32 gKotoGbaRuntimeEnabled;
extern volatile u32 gKotoGbaPendingCardId;
void kotogba_vblankUpdate();
}
