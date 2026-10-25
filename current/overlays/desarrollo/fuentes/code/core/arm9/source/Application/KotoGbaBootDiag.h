#pragma once
#include "common.h"
#ifdef __cplusplus
extern "C" {
#endif
void kotogba_bootStep(u32 stage, u32 value);
extern volatile u32 gKotoGbaBootMode; // 0: normal, 1: emergency, 2/3: content failure
#ifdef __cplusplus
}
#endif
