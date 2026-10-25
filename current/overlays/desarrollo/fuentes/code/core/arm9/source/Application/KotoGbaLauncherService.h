#pragma once
#include <stddef.h>

struct GbaHeader;

struct KotoGbaPackageInfo
{
    char gameCode[5];
    unsigned char revision;
    bool validRom;
    bool installed;
    char packagePath[64];
};

class KotoGbaLauncherService
{
public:
    bool SelectRom(char* outputPath, size_t outputPathSize);
    bool ResolvePackageForRom(const char* romPath);
    bool ResolvePackageForHeader(const GbaHeader& header);
    const KotoGbaPackageInfo& GetPackageInfo() const;
};

extern KotoGbaLauncherService gKotoGbaLauncherService;
