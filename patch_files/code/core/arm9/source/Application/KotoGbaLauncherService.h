#pragma once
#include <stddef.h>

class KotoGbaLauncherService
{
public:
    bool SelectRom(char* outputPath, size_t outputPathSize);
};

extern KotoGbaLauncherService gKotoGbaLauncherService;
