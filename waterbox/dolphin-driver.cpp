// The Chimera adapter around Dolphin: boot a machine into a paused state,
// advance it exactly one VI field at a time, and hand out the machine's
// memory. Compiled for both flavors; the native reference drives it via
// run-native.cpp, the guest via the waterbox ABI shim (M1).
// SPDX-License-Identifier: MIT

#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>

#include "Common/FileUtil.h"
#include "Common/Config/Config.h"
#include "Common/Config/Layer.h"
#include "Common/MsgHandler.h"
#include "Common/Logging/LogManager.h"
#include "Common/WindowSystemInfo.h"
#include "Core/Boot/Boot.h"
#include "Core/BootManager.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "AudioCommon/AudioCommon.h"
#include "VideoCommon/VideoBackendBase.h"
#include "UICommon/UICommon.h"

#include "dolphin-driver.h"

static std::string s_error;

// Every alert is answered "yes" and logged; a machine has nobody to ask.
static bool AlertHandler(const char* caption, const char* text, bool /*yes_no*/,
                         Common::MsgType /*style*/)
{
  fprintf(stderr, "[alert] %s: %s\n", caption, text);
  return true;
}

// The forced machine configuration rides in a layer ABOVE every file-backed
// layer, so nothing a config reload does can undo it. Same mechanism movies
// and netplay use for their own pins.
class ChimeraConfigLayer final : public Config::ConfigLayerLoader
{
public:
  ChimeraConfigLayer() : ConfigLayerLoader(Config::LayerType::CurrentRun) {}
  void Load(Config::Layer* layer) override
  {
    layer->Set(Config::MAIN_CPU_CORE, PowerPC::CPUCore::Interpreter);
    layer->Set(Config::MAIN_CPU_THREAD, false);
    layer->Set(Config::MAIN_GFX_BACKEND, std::string("Software Renderer"));
    layer->Set(Config::MAIN_DSP_HLE, true);
    layer->Set(Config::MAIN_DSP_JIT, false);
    layer->Set(Config::MAIN_AUDIO_BACKEND, std::string(BACKEND_NULLSOUND));
    layer->Set(Config::MAIN_EMULATION_SPEED, 0.0f);
    layer->Set(Config::MAIN_WIIMOTE_CONTINUOUS_SCANNING, false);
    // the machine's clock belongs to the machine: a fixed epoch, never the host
    layer->Set(Config::MAIN_CUSTOM_RTC_ENABLE, true);
    layer->Set(Config::MAIN_CUSTOM_RTC_VALUE, u32(946684800));
    layer->Set(Common::Log::LOGGER_VERBOSITY, Common::Log::LogLevel::LINFO);
  }
  void Save(Config::Layer*) override {}
};

static Core::System& Sys()
{
  return Core::System::GetInstance();
}

// Pump host-side jobs until the machine reports the wanted state. Under
// miniBox green threads the yield is what lets the EmuThread run at all.
// Returns false if the machine instead lands in a terminal state - a boot
// that failed tears down to Uninitialized, and spinning on it helps nobody.
static bool WaitForState(Core::State want)
{
  for (;;)
  {
    const Core::State got = Core::GetState(Sys());
    if (got == want)
      return true;
    if (got == Core::State::Uninitialized && want != Core::State::Uninitialized)
      return false;
    Core::HostDispatchJobs(Sys());
    std::this_thread::yield();
  }
}

extern "C" {

const char* chimera_dolphin_error(void)
{
  return s_error.c_str();
}

int chimera_dolphin_init(const char* user_dir, const char* sys_dir, const char* game_path)
{
  // Sys first: boot reads GameSettings and the free DSP roms from there.
  Common::RegisterMsgAlertHandler(AlertHandler);
  File::SetSysDirectory(sys_dir);
  UICommon::SetUserDirectory(user_dir);
  UICommon::Init();

  // The deterministic machine: interpreter, one thread, software renderer,
  // HLE audio mixed into a buffer nobody plays, no throttle - the harness is
  // the clock.
  Config::AddLayer(std::make_unique<ChimeraConfigLayer>());
  // land paused: zero instructions run before the first frame advance
  SConfig::GetInstance().bBootToPause = true;

  WindowSystemInfo wsi;
  wsi.type = WindowSystemType::Headless;
  UICommon::InitControllers(wsi);

  auto boot = BootParameters::GenerateFromFile(game_path);
  if (!boot)
  {
    s_error = "could not make boot parameters from the given path";
    return 0;
  }
  // BootManager owns the whole boot ritual (config game layers, SYSCONF
  // control transfer, determinism update) - bypassing it for Core::Init left
  // the shutdown transfer asserting.
  if (!BootManager::BootCore(Sys(), std::move(boot), wsi))
  {
    s_error = "BootManager::BootCore refused";
    return 0;
  }
  if (!WaitForState(Core::State::Paused))
  {
    s_error = "the machine tore down during boot (see log)";
    return 0;
  }
  fprintf(stderr, "[driver] video backend: %s\n",
          g_video_backend ? g_video_backend->GetConfigName().c_str() : "(none)");
  return 1;
}

void chimera_dolphin_frame(void)
{
  // DoFrameStep stores Running before it returns and the machine stores
  // Paused at the end of the next VI field, so waiting for Paused after the
  // call cannot race the step.
  Core::DoFrameStep(Sys());
  WaitForState(Core::State::Paused);
  // The state flag flips to Paused before the CPU thread has fully settled;
  // natively it can still be mid-slice while the harness reads memory. A
  // CPUThreadGuard blocks until the machine is genuinely quiet - the same
  // point the frozen-threads sandbox observes for free.
  {
    const Core::CPUThreadGuard guard(Sys());
  }
}

uint8_t* chimera_dolphin_ram_ptr(void)
{
  return Sys().GetMemory().GetRAM();
}

int64_t chimera_dolphin_ram_size(void)
{
  return static_cast<int64_t>(Sys().GetMemory().GetRamSizeReal());
}

void chimera_dolphin_shutdown(void)
{
  Core::Stop(Sys());
  WaitForState(Core::State::Uninitialized);
  Core::Shutdown(Sys());
  UICommon::ShutdownControllers();
  UICommon::Shutdown();
}

}  // extern "C"
