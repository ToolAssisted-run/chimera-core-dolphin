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
#include <waterbox_settings.h>
#include <waterbox_slots.h>

#include "dolphin-driver.h"

static char g_loadError[512];

#ifdef CHIMERA_GL_BRIDGE
extern "C" void chimera_dolphin_install_gpu_bridge(uint64_t addr);
#endif

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
  char romName[256] = "game";
  if (!wbx_slot_first("game", romName, sizeof romName))
  {
    // No slot map: a directly-opened rom sits under the fixed mount "game"
    // with its true name in rom.name. Alias an extension-bearing path onto
    // the mount so dolphin's type detection has something to read.
    char realName[256] = "";
    FILE* f = fopen("rom.name", "rb");
    if (f)
    {
      size_t n = fread(realName, 1, sizeof realName - 1, f);
      while (n && (realName[n - 1] == '\n' || realName[n - 1] == '\r'))
        n--;
      realName[n] = '\0';
      fclose(f);
    }
    // The harness and the frontend both mount the file under its real
    // basename (with a leading slash) alongside the fixed "game" mount;
    // boot whichever answers, preferring the name that carries the
    // extension dolphin's type detection wants.
    char candidate[300];
    snprintf(candidate, sizeof candidate, "%s%s", realName[0] == '/' ? "" : "/", realName);
    FILE* probe = realName[0] ? fopen(candidate, "rb") : nullptr;
    if (!probe && realName[0])
      probe = fopen(realName, "rb"), snprintf(candidate, sizeof candidate, "%s", realName);
    if (probe)
    {
      fclose(probe);
      snprintf(romName, sizeof romName, "%s", candidate);
    }
  }

  chimera_dolphin_set_memcard_a(wbx_setting_bool("memcard_a", 1));
  char cpuCore[32] = "jit";
  wbx_setting_str("cpu_core", cpuCore, sizeof cpuCore);
  chimera_dolphin_set_cpu_core(cpuCore);
  char renderer[32] = "software";
  wbx_setting_str("renderer", renderer, sizeof renderer);
  chimera_dolphin_set_renderer(renderer);

  if (!chimera_dolphin_init("/user", "/sys", romName))
  {
    snprintf(g_loadError, sizeof g_loadError, "%s", chimera_dolphin_error());
    return 0;
  }
  return 1;
}

ECL_EXPORT void SetButton(int32_t index, int32_t state)
{
  chimera_dolphin_set_button(index / 12, index % 12, state);
}

ECL_EXPORT void SetAxis(int32_t index, int32_t value)
{
  // the frontend's signed axis (-128..127) onto the pad's biased byte
  chimera_dolphin_set_axis(index / 6, index % 6, value + 128);
}

#ifdef CHIMERA_GL_BRIDGE
ECL_EXPORT void SetGpuBridge(uint64_t addr)
{
  chimera_dolphin_install_gpu_bridge(addr);
}
#endif

ECL_EXPORT void FrameAdvance(uint64_t /*input*/)
{
  chimera_dolphin_frame();
}

ECL_EXPORT int InputWasRead(void)
{
  return chimera_dolphin_input_was_read();
}

static int g_vw, g_vh, g_an;

ECL_EXPORT uint32_t* GetVideoBgra(void)
{
  return const_cast<uint32_t*>(chimera_dolphin_video(&g_vw, &g_vh));
}

ECL_EXPORT int GetVideoWidth(void)
{
  chimera_dolphin_video(&g_vw, &g_vh);
  return g_vw;
}

ECL_EXPORT int GetVideoHeight(void)
{
  chimera_dolphin_video(&g_vw, &g_vh);
  return g_vh;
}

ECL_EXPORT int16_t* GetAudio(void)
{
  return const_cast<int16_t*>(chimera_dolphin_audio(&g_an));
}

ECL_EXPORT int GetAudioSampleCount(void)
{
  chimera_dolphin_audio(&g_an);
  return g_an;
}

ECL_EXPORT int GetVsyncNumerator(void)
{
  return chimera_dolphin_vsync_numerator();
}

ECL_EXPORT int GetVsyncDenominator(void)
{
  return chimera_dolphin_vsync_denominator();
}

ECL_EXPORT int32_t GetSaveDataFileCount(void)
{
  return chimera_dolphin_savedata_count();
}

ECL_EXPORT const char* GetSaveDataFileName(int32_t i)
{
  return chimera_dolphin_savedata_name(i);
}

ECL_EXPORT int64_t GetSaveDataFileSize(int32_t i)
{
  return chimera_dolphin_savedata_size(i);
}

ECL_EXPORT const uint8_t* GetSaveDataFileBuffer(int32_t i)
{
  return chimera_dolphin_savedata_buffer(i);
}

ECL_EXPORT int GetMemoryDomainCount(void)
{
  return chimera_dolphin_domain_count();
}

ECL_EXPORT const char* GetMemoryDomainName(int i)
{
  return chimera_dolphin_domain_name(i);
}

ECL_EXPORT uint8_t* GetMemoryDomainPtr(int i)
{
  return chimera_dolphin_domain_ptr(i);
}

ECL_EXPORT int64_t GetMemoryDomainSize(int i)
{
  return chimera_dolphin_domain_size(i);
}

ECL_EXPORT int GetMemoryDomainWritable(int i)
{
  return i >= 0 && i < 3;
}

}  // extern "C"
