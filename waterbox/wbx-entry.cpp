// The chimera guest ABI layer: wraps dolphin-driver into the miniBox core
// ABI (the same export surface as the other chimera cores). Compiled ONLY
// for the guest; run-native.cpp is the native twin.
//
// M1 scope: boot + frame advance + the RAM domain. Video, audio, input,
// savedata and settings arrive with their milestones.
// SPDX-License-Identifier: MIT

#include <cstdint>
#include <cstdio>
#include <cstring>

#include <emulibc.h>
#include <waterbox_slots.h>

#include "dolphin-driver.h"

static char g_loadError[512];

extern "C" {

ECL_EXPORT const char* GetLoadError(void)
{
  return g_loadError;
}

ECL_EXPORT int Init(void)
{
  g_loadError[0] = '\0';

  // The game to boot: the project mounts "slots" ({"game":["name"]}, the
  // file itself under that canonical name); a rom opened directly arrives
  // as "rom" with its real name in "rom.name" so extension detection still
  // works. See file_slots.json once it exists.
  char romName[256] = "rom";
  if (!wbx_slot_first("game", romName, sizeof romName))
  {
    FILE* f = fopen("rom.name", "rb");
    if (f)
    {
      size_t n = fread(romName, 1, sizeof romName - 1, f);
      while (n && (romName[n - 1] == '\n' || romName[n - 1] == '\r'))
        n--;
      romName[n] = '\0';
      fclose(f);
    }
  }

  if (!chimera_dolphin_init("/user", "/sys", romName))
  {
    snprintf(g_loadError, sizeof g_loadError, "%s", chimera_dolphin_error());
    return 0;
  }
  return 1;
}

ECL_EXPORT void FrameAdvance(uint64_t /*input*/)
{
  chimera_dolphin_frame();
}

ECL_EXPORT int GetMemoryDomainCount(void)
{
  return 1;
}

ECL_EXPORT const char* GetMemoryDomainName(int i)
{
  return i == 0 ? "System RAM" : nullptr;
}

ECL_EXPORT uint8_t* GetMemoryDomainPtr(int i)
{
  return i == 0 ? chimera_dolphin_ram_ptr() : nullptr;
}

ECL_EXPORT int64_t GetMemoryDomainSize(int i)
{
  return i == 0 ? chimera_dolphin_ram_size() : 0;
}

ECL_EXPORT int GetMemoryDomainWritable(int i)
{
  return i == 0;
}

}  // extern "C"
