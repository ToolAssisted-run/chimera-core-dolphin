// The Chimera adapter around Dolphin: boot a machine into a paused state,
// advance it exactly one VI field at a time, and hand out the machine's
// memory. Compiled for both flavors; the native reference drives it via
// run-native.cpp, the guest via the waterbox ABI shim (M1).
// SPDX-License-Identifier: MIT

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <cstring>
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
#include "Core/HW/DSP.h"
#include "Core/HW/EXI/EXI_Device.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "AudioCommon/AudioCommon.h"
#include "AudioCommon/Mixer.h"
#include "AudioCommon/SoundStream.h"
#include "Core/HW/VideoInterface.h"
#include "InputCommon/GCPadStatus.h"
#include "Core/Config/GraphicsSettings.h"
#include "VideoCommon/VideoBackendBase.h"
#include "VideoCommon/VideoConfig.h"
#include "UICommon/UICommon.h"

#include "dolphin-driver.h"

static std::string s_error;

// ---- the input wire -------------------------------------------------------
// Wire order (waterbox.config to declare the same): buttons
// 0 A, 1 B, 2 X, 3 Y, 4 Z, 5 Start, 6 Up, 7 Down, 8 Left, 9 Right, 10 L, 11 R;
// axes 0 MainX, 1 MainY, 2 CX, 3 CY, 4 TriggerL, 5 TriggerR (0..255, 128 center).
struct PadWire
{
  uint16_t buttons = 0;
  uint8_t axis[6] = {0x80, 0x80, 0x80, 0x80, 0, 0};
};
static PadWire s_pad[4];
static bool s_input_read;
static bool s_memcard_a = true;
static PowerPC::CPUCore s_cpu_core = PowerPC::CPUCore::JIT64;
static bool s_renderer_opengl;
extern "C" int chimera_dolphin_gpu_bridge_present(void) __attribute__((weak));

static constexpr uint16_t kWireBit[12] = {
    PAD_BUTTON_A,    PAD_BUTTON_B,    PAD_BUTTON_X,    PAD_BUTTON_Y,
    PAD_TRIGGER_Z,   PAD_BUTTON_START, PAD_BUTTON_UP,  PAD_BUTTON_DOWN,
    PAD_BUTTON_LEFT, PAD_BUTTON_RIGHT, PAD_TRIGGER_L,  PAD_TRIGGER_R,
};

// ---- video out ------------------------------------------------------------
// The field hook converts the scanned-out XFB (YUYV in guest RAM) to BGRA.
// Interlaced content arrives one field at a time; for now the latest field IS
// the picture (the PS2 deinterlacing lessons apply later).
static uint32_t s_video[720 * 574];
static int s_video_w = 640, s_video_h = 480;

// ---- save data ------------------------------------------------------------
// The memory cards report in through patch 0015's hook; the frontend reads
// them out through the savedata exports and mounts prior saves back at
// "savedata/<name>", which is exactly the path the machine opens.
struct MemcardReg
{
  std::string name;
  uint8_t* data = nullptr;
  uint32_t size = 0;
};
static MemcardReg s_memcard[2];

extern "C" void Chimera_RegisterMemcard(int slot, const char* filename, uint8_t* data,
                                        uint32_t size)
{
  if (slot < 0 || slot > 1)
    return;
  if (!filename)
  {
    s_memcard[slot] = {};
    return;
  }
  const char* base = strrchr(filename, '/');
  s_memcard[slot].name = base ? base + 1 : filename;
  s_memcard[slot].data = data;
  s_memcard[slot].size = size;
}

// ---- audio out ------------------------------------------------------------
static int16_t s_audio[16384 * 2];
static int s_audio_frames;
static uint64_t s_audio_acc;

static uint32_t YuyvToBgra(int y, int u, int v)
{
  const int c = y - 16, d = u - 128, e = v - 128;
  auto clamp = [](int x) { return x < 0 ? 0 : (x > 255 ? 255 : x); };
  const int r = clamp((298 * c + 409 * e + 128) >> 8);
  const int g = clamp((298 * c - 100 * d - 208 * e + 128) >> 8);
  const int b = clamp((298 * c + 516 * d + 128) >> 8);
  return 0xFF000000u | (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
}

extern "C" bool Chimera_GetPadStatus(int chan, GCPadStatus* status)
{
  if (chan < 0 || chan >= 4)
    return false;
  const PadWire& w = s_pad[chan];
  status->button = w.buttons;
  status->stickX = w.axis[0];
  status->stickY = w.axis[1];
  status->substickX = w.axis[2];
  status->substickY = w.axis[3];
  status->triggerLeft = uint8_t(w.buttons & PAD_TRIGGER_L ? 255 : w.axis[4]);
  status->triggerRight = uint8_t(w.buttons & PAD_TRIGGER_R ? 255 : w.axis[5]);
  status->isConnected = true;
  if (chan == 0)
    s_input_read = true;
  return true;
}

extern "C" void Chimera_OutputField(int /*field*/, uint32_t xfb_addr, uint32_t fb_width,
                                    uint32_t fb_stride, uint32_t fb_height)
{
  static int logged;
  if (logged < 3 && getenv("CHIMERA_TRACE_FIELD"))
  {
    fprintf(stderr, "[field] xfb %08x w %u stride %u h %u\n", xfb_addr, fb_width, fb_stride,
            fb_height);
    logged++;
  }
  if (!xfb_addr || !fb_width || !fb_height)
    return;
  auto& memory = Core::System::GetInstance().GetMemory();
  const uint32_t w = fb_width > 720 ? 720 : fb_width;
  const uint32_t h = fb_height > 574 ? 574 : fb_height;
  const uint8_t* src = memory.GetPointerForRange(xfb_addr, fb_stride * h);
  if (!src)
    return;
  for (uint32_t line = 0; line < h; line++)
  {
    const uint8_t* p = src + line * fb_stride;
    uint32_t* out = s_video + line * w;
    for (uint32_t x = 0; x + 1 < w; x += 2)
    {
      const int y0 = p[0], u = p[1], y1 = p[2], v = p[3];
      out[x] = YuyvToBgra(y0, u, v);
      out[x + 1] = YuyvToBgra(y1, u, v);
      p += 4;
    }
  }
  s_video_w = int(w);
  s_video_h = int(h);
}

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
    layer->Set(Config::MAIN_CPU_CORE, s_cpu_core);
    layer->Set(Config::MAIN_CPU_THREAD, false);
    // nothing may fault on purpose: no fastmem, no arena mirrors, in either
    // flavor (build option ENABLE_FAULT_OPTIMIZATIONS=OFF removes the
    // handler; these remove the askers)
    layer->Set(Config::MAIN_FASTMEM, false);
    layer->Set(Config::MAIN_FASTMEM_ARENA, false);
    const bool gl = s_renderer_opengl && chimera_dolphin_gpu_bridge_present &&
                    chimera_dolphin_gpu_bridge_present();
    layer->Set(Config::MAIN_GFX_BACKEND, std::string(gl ? "OGL" : "Software Renderer"));
    if (gl)
    {
      // the machine's video memory stays machine state: EFB and XFB copies
      // land in RAM (GPU readbacks), which is what the gate hashes and what
      // games that read their own picture depend on
      layer->Set(Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM, false);
      layer->Set(Config::GFX_HACK_SKIP_XFB_COPY_TO_RAM, false);
      // one real context: no worker-thread compilers, no disk shader cache
      layer->Set(Config::GFX_SHADER_COMPILATION_MODE, ShaderCompilationMode::Synchronous);
      layer->Set(Config::GFX_SHADER_CACHE, false);
    }
    layer->Set(Config::MAIN_DSP_HLE, true);
    layer->Set(Config::MAIN_DSP_JIT, false);
    layer->Set(Config::MAIN_AUDIO_BACKEND, std::string(BACKEND_NULLSOUND));
    layer->Set(Config::MAIN_EMULATION_SPEED, 0.0f);
    layer->Set(Config::MAIN_WIIMOTE_CONTINUOUS_SCANNING, false);
    // the machine's clock belongs to the machine: a fixed epoch, never the host
    layer->Set(Config::MAIN_CUSTOM_RTC_ENABLE, true);
    layer->Set(Config::MAIN_CUSTOM_RTC_VALUE, u32(946684800));
    // the cards live at a fixed relative path: the frontend mounts prior
    // saves there, the export names match, and no host user dir leaks in
    layer->Set(Config::MAIN_MEMCARD_A_PATH, std::string("savedata/MemoryCardA.raw"));
    layer->Set(Config::MAIN_MEMCARD_B_PATH, std::string("savedata/MemoryCardB.raw"));
    // raw cards, not GCI folders: one buffer is one save-data file
    layer->Set(Config::MAIN_SLOT_A, s_memcard_a ? ExpansionInterface::EXIDeviceType::MemoryCard :
                                                  ExpansionInterface::EXIDeviceType::None);
    layer->Set(Config::MAIN_SLOT_B, ExpansionInterface::EXIDeviceType::None);
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
  long spins = 0;
  for (;;)
  {
    const Core::State got = Core::GetState(Sys());
    spins++;
    if (getenv("CHIMERA_TRACE_STATE") && spins % 100000 == 0)
      fprintf(stderr, "[state] want %d got %d\n", int(want), int(got));
#ifndef CHIMERA_GUEST
    // Natively the pump would otherwise spin a whole core on sched_yield
    // against real threads; a short sleep after a polite start costs at most
    // 100us of latency per frame. The guest's yield IS its scheduler and
    // must stay untouched.
    if (spins > 200)
    {
      timespec ts{0, 100000};
      nanosleep(&ts, nullptr);
    }
#endif
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
  // NullSound zeroes the mixer's output rate so nothing consumes samples;
  // the harness IS the consumer, at dolphin's canonical output rate.
  if (Sys().GetSoundStream())
    Sys().GetSoundStream()->GetMixer()->SetSampleRate(48000);
  return 1;
}

void chimera_dolphin_frame(void)
{
  // DoFrameStep stores Running before it returns and the machine stores
  // Paused at the end of the next VI field, so waiting for Paused after the
  // call cannot race the step.
  s_input_read = false;
  Core::DoFrameStep(Sys());
  WaitForState(Core::State::Paused);
  // The state flag flips to Paused before the CPU thread has fully settled;
  // natively it can still be mid-slice while the harness reads memory. A
  // CPUThreadGuard blocks until the machine is genuinely quiet - the same
  // point the frozen-threads sandbox observes for free.
  {
    const Core::CPUThreadGuard guard(Sys());
  }

  // Drain the mixer: the machine made this much time pass, so this many
  // samples exist. The remainder accumulates so no fraction is ever lost.
  Mixer* mixer = Sys().GetSoundStream() ? Sys().GetSoundStream()->GetMixer() : nullptr;
  static int audlog;
  if (audlog < 2 && getenv("CHIMERA_TRACE_FIELD"))
  {
    auto& vi = Sys().GetVideoInterface();
    fprintf(stderr, "[aud] stream %p mixer %p rate %u num %u den %u\n",
            (void*)Sys().GetSoundStream(), (void*)mixer, mixer ? mixer->GetSampleRate() : 0,
            vi.GetTargetRefreshRateNumerator(), vi.GetTargetRefreshRateDenominator());
    audlog++;
  }
  s_audio_frames = 0;
  if (mixer)
  {
    auto& vi = Sys().GetVideoInterface();
    const uint64_t num = vi.GetTargetRefreshRateNumerator();
    const uint64_t den = vi.GetTargetRefreshRateDenominator();
    if (num)
    {
      s_audio_acc += uint64_t(mixer->GetSampleRate()) * den;
      uint64_t want = s_audio_acc / num;
      s_audio_acc %= num;
      if (want > 16384)
        want = 16384;
      s_audio_frames = int(mixer->Mix(s_audio, size_t(want)));
    }
  }
}

void chimera_dolphin_set_button(int pad, int index, int state)
{
  if (pad < 0 || pad >= 4 || index < 0 || index >= 12)
    return;
  if (state)
    s_pad[pad].buttons |= kWireBit[index];
  else
    s_pad[pad].buttons &= uint16_t(~kWireBit[index]);
}

void chimera_dolphin_set_axis(int pad, int index, int value)
{
  if (pad < 0 || pad >= 4 || index < 0 || index >= 6)
    return;
  s_pad[pad].axis[index] = uint8_t(value < 0 ? 0 : (value > 255 ? 255 : value));
}

int chimera_dolphin_input_was_read(void)
{
  return s_input_read ? 1 : 0;
}

const uint32_t* chimera_dolphin_video(int* w, int* h)
{
  *w = s_video_w;
  *h = s_video_h;
  return s_video;
}

const int16_t* chimera_dolphin_audio(int* frames)
{
  *frames = s_audio_frames;
  return s_audio;
}

int chimera_dolphin_vsync_numerator(void)
{
  return int(Sys().GetVideoInterface().GetTargetRefreshRateNumerator());
}

int chimera_dolphin_vsync_denominator(void)
{
  return int(Sys().GetVideoInterface().GetTargetRefreshRateDenominator());
}

// domains beyond main RAM: index 1 = ARAM (16MB audio memory), 2 = L1 cache
uint8_t* chimera_dolphin_domain_ptr(int i)
{
  switch (i)
  {
  case 0:
    return Sys().GetMemory().GetRAM();
  case 1:
    return Sys().GetDSP().GetARAMPtr();
  case 2:
    return Sys().GetMemory().GetL1Cache();
  }
  return nullptr;
}

int64_t chimera_dolphin_domain_size(int i)
{
  switch (i)
  {
  case 0:
    return Sys().GetMemory().GetRamSizeReal();
  case 1:
    return 16 * 1024 * 1024;
  case 2:
    return Sys().GetMemory().GetL1CacheSize();
  }
  return 0;
}

const char* chimera_dolphin_domain_name(int i)
{
  switch (i)
  {
  case 0:
    return "System RAM";
  case 1:
    return "ARAM";
  case 2:
    return "L1 Cache";
  }
  return nullptr;
}

void chimera_dolphin_set_memcard_a(int present)
{
  s_memcard_a = present != 0;
}

void chimera_dolphin_set_renderer(const char* name)
{
  s_renderer_opengl = name && strcmp(name, "opengl") == 0;
}

void chimera_dolphin_set_cpu_core(const char* name)
{
  if (!name)
    return;
  if (strcmp(name, "cached-interpreter") == 0)
    s_cpu_core = PowerPC::CPUCore::CachedInterpreter;
  else if (strcmp(name, "jit") == 0)
    s_cpu_core = PowerPC::CPUCore::JIT64;
  else
    s_cpu_core = PowerPC::CPUCore::Interpreter;
}

int chimera_dolphin_savedata_count(void)
{
  return (s_memcard[0].data ? 1 : 0) + (s_memcard[1].data ? 1 : 0);
}

static const MemcardReg* SavedataAt(int i)
{
  for (int slot = 0; slot < 2; slot++)
  {
    if (!s_memcard[slot].data)
      continue;
    if (i == 0)
      return &s_memcard[slot];
    i--;
  }
  return nullptr;
}

const char* chimera_dolphin_savedata_name(int i)
{
  const MemcardReg* r = SavedataAt(i);
  return r ? r->name.c_str() : nullptr;
}

int64_t chimera_dolphin_savedata_size(int i)
{
  const MemcardReg* r = SavedataAt(i);
  return r ? r->size : 0;
}

const uint8_t* chimera_dolphin_savedata_buffer(int i)
{
  const MemcardReg* r = SavedataAt(i);
  return r ? r->data : nullptr;
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
