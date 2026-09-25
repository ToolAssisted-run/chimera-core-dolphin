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
#include <utility>
#include <vector>
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
#include "Core/Config/SYSCONFSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/DSP.h"
#include "Core/HW/EXI/EXI_Device.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/SI/SI_Device.h"
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
#include "Common/NandPaths.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/Uids.h"
#include "zip-reader.h"

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
// The Wii's "Screen: Widescreen" system setting, which a Wii game reads from
// its SYSCONF to choose 16:9 or 4:3 (chimera#147). Part of the machine: the
// game draws differently, so a movie records it. A GameCube has no such
// setting and ignores it.
static bool s_widescreen = false;
// which console the project declares ("gamecube"/"wii"); empty = don't check
static char s_machine[16];
static PowerPC::CPUCore s_cpu_core = PowerPC::CPUCore::JIT64;
static bool s_renderer_opengl;
// The backend Init actually chose, not the setting: the bridge can be live
// while a project asked for the software renderer, and the GL hooks below must
// not run then (g_gfx is an SWGfx and the OGL helper casts it unchecked).
static bool s_gl_backend;
static bool s_port_present[4] = {true, false, false, false};
extern "C" int chimera_dolphin_gpu_bridge_present(void) __attribute__((weak));

static constexpr uint16_t kWireBit[12] = {
    PAD_BUTTON_A,    PAD_BUTTON_B,    PAD_BUTTON_X,    PAD_BUTTON_Y,
    PAD_TRIGGER_Z,   PAD_BUTTON_START, PAD_BUTTON_UP,  PAD_BUTTON_DOWN,
    PAD_BUTTON_LEFT, PAD_BUTTON_RIGHT, PAD_TRIGGER_L,  PAD_TRIGGER_R,
};

// ---- video out ------------------------------------------------------------
// The field hook converts the scanned-out XFB (YUYV in guest RAM) to BGRA.
// It fires AFTER the VI's progressive reassembly (ForceProgressive, dolphin's
// default): an interlaced game's two fields hand out the same full-height
// frame, so the picture holds still instead of bobbing a line at field rate.
static uint32_t s_video[720 * 576];
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

// A Wii keeps its saves in NAND, not on a card: every file under a title's
// data directory is exported too, named "nand/<NAND path>" (chimera#147).
// Taken as a snapshot by the count, as the export contract asks.
void Chimera_ListNandSaves(std::vector<std::pair<std::string, const std::vector<u8>*>>& out);
struct NandSave
{
  std::string name;
  const std::vector<u8>* data;
};
static std::vector<NandSave> s_nand_saves;

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

extern "C" uint64_t Chimera_SettingsSerialSeconds()
{
  // the same fixed machine epoch the RTC starts at: deterministic, and a
  // plausible moment for the console to have been set up
  return 946684800ull;
}

extern "C" bool Chimera_GetPadStatus(int chan, GCPadStatus* status)
{
  if (chan < 0 || chan >= 4 || !s_port_present[chan])
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
  const uint32_t h = fb_height > 576 ? 576 : fb_height;
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
    s_gl_backend = gl;
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
    // what is plugged into each controller port is part of the machine
    for (int ch = 0; ch < 4; ch++)
    {
      layer->Set(Config::GetInfoForSIDevice(ch),
                 s_port_present[ch] ? SerialInterface::SIDEVICE_GC_CONTROLLER :
                                      SerialInterface::SIDEVICE_NONE);
    }
    // the machine's clock belongs to the machine: a fixed epoch, never the host
    layer->Set(Config::MAIN_CUSTOM_RTC_ENABLE, true);
    layer->Set(Config::MAIN_CUSTOM_RTC_VALUE, u32(946684800));
    // BootManager writes the SYSCONF settings into the NAND's SYSCONF file
    // before the game starts, so the game reads this as the console's own
    layer->Set(Config::SYSCONF_WIDESCREEN, s_widescreen);
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

static bool WaitForState(Core::State want);

// The machine is up by the time a load error is known, and a refusal that
// leaves it running is not a refusal: the emu threads outlive main, and the
// first static destructor to notice - the async shader compiler, which asserts
// on live workers - hangs the process instead of returning the load error. Put
// the machine away first. Returns Init's failure value.
static int RefuseRunningMachine()
{
  Core::Stop(Sys());
  WaitForState(Core::State::Uninitialized);
  Core::Shutdown(Sys());
  UICommon::ShutdownControllers();
  UICommon::Shutdown();
  return 0;
}

// ---- a Wii's saves, taken back in (chimera#147) ------------------------------
// A Wii keeps a game's saves in NAND under /title/<id>/data, and Export Save
// Data hands them out as nand/<NAND path>. A Wii project's Save data slot takes
// that .zip back: its files go into the booted game's data directory once the
// boot has made it (ES_DIVerify creates it and gives it to the game's uid and
// gid), before the game runs a single instruction and before the machine is
// sealed - so the saves are baseline, and a savestate carries only what the
// game writes afterwards. Each file is the game's own: created as its uid and
// gid, with the modes the data directory has. Anything that is not this
// game's save is refused rather than ignored - a project that carries someone's
// progress and silently starts from nothing is worse than one that will not
// load (chimera docs/save-data.md).
static std::string s_wii_savedata_zip;

static bool SeedWiiSaves()
{
  if (s_wii_savedata_zip.empty())
    return true;
  if (!Sys().IsWii())
  {
    s_error = "the Save data slot holds a .zip, which is a Wii's saves; a GameCube project "
              "takes the memory card image (.raw) Export Save Data writes";
    return false;
  }
  auto index = std::make_shared<chimera::zip_index>();
  std::string error;
  if (!chimera::zip_open(s_wii_savedata_zip, *index, error))
  {
    s_error = "the save data could not be read as a zip: " + error;
    return false;
  }

  using namespace IOS::HLE;
  using IOS::PID_KERNEL;
  const u64 title = SConfig::GetInstance().GetTitleID();
  const std::string data_dir = Common::GetTitleDataPath(title);
  const std::string prefix = "nand" + data_dir + "/";
  const auto fs = Sys().GetIOS()->GetFS();
  const auto owner = fs->GetMetadata(PID_KERNEL, PID_KERNEL, data_dir);
  if (!owner || owner->is_file)
  {
    s_error = "the game has no data directory in NAND to put its saves in (" + data_dir + ")";
    return false;
  }

  size_t files = 0;
  for (size_t i = 0; i < index->entries.size(); i++)
  {
    const auto& e = index->entries[i];
    if (e.path.empty() || e.path.back() == '/')
      continue;  // a directory entry: the files under it make it
    if (e.path.compare(0, prefix.size(), prefix) != 0 || e.path.find("..") != std::string::npos)
    {
      s_error = "the save data holds '" + e.path + "', which is not a save of this game - every "
                "entry is " + prefix + "<file>, as Export Save Data writes them for this game";
      return false;
    }
    std::vector<u8> bytes(e.size);
    chimera::zip_stream stream(index, i);
    if (stream.read_at(0, bytes.data(), e.size) != e.size)
    {
      s_error = "the save data's '" + e.path + "' could not be unpacked";
      return false;
    }
    const std::string path = e.path.substr(4);  // "/title/.../data/..."
    // the folders between the data directory and the file are the game's too
    for (size_t slash = data_dir.size() + 1; (slash = path.find('/', slash)) != std::string::npos; slash++)
    {
      const std::string dir = path.substr(0, slash);
      fs->CreateDirectory(owner->uid, owner->gid, dir, 0, owner->modes);  // may exist
    }
    const auto file = fs->CreateAndOpenFile(owner->uid, owner->gid, path, owner->modes);
    if (!file || !file->Write(bytes.data(), bytes.size()))
    {
      s_error = "the save data's '" + e.path + "' could not be written to NAND";
      return false;
    }
    files++;
  }
  if (files == 0)
  {
    s_error = "the save data zip holds no files";
    return false;
  }
  fprintf(stderr, "[driver] seeded %zu Wii save file(s) into %s\n", files, data_dir.c_str());
  return true;
}

void chimera_dolphin_set_wii_savedata(const char* zip_path)
{
  s_wii_savedata_zip = zip_path ? zip_path : "";
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
    // it is down, but not put away: its threads outlive Init and a joinable
    // one met by a destructor at exit is std::terminate - a failed .wad
    // install ended in a core dump instead of this load error (chimera#148)
    return RefuseRunningMachine();
  }
  // The project declares which machine it is; the image does not get a vote.
  // A mismatch is a load error, not a silent boot of the other console.
  if (s_machine[0])
  {
    const bool wants_wii = strcmp(s_machine, "wii") == 0;
    if (wants_wii != Sys().IsWii())
    {
      s_error = wants_wii
                    ? "the project says Wii, but this image boots a GameCube - pick "
                      "GameCube in the New Project wizard's System box"
                    : "the project says GameCube, but this image boots a Wii - pick "
                      "Wii in the New Project wizard's System box";
      return RefuseRunningMachine();
    }
  }
  // the saves the project starts from, before the machine is sealed
  if (!SeedWiiSaves())
    return RefuseRunningMachine();
  fprintf(stderr, "[driver] video backend: %s\n",
          g_video_backend ? g_video_backend->GetConfigName().c_str() : "(none)");
  // NullSound zeroes the mixer's output rate so nothing consumes samples;
  // the harness IS the consumer, at dolphin's canonical output rate.
  if (Sys().GetSoundStream())
    Sys().GetSoundStream()->GetMixer()->SetSampleRate(48000);
  return 1;
}

extern "C" void chimera_dolphin_gl_frame_start(void);
extern "C" void chimera_dolphin_gl_state_loaded(void);

/* Told after every load of the machine - a savestate, a branch file, a
 * greenzone restore - with the machine stopped and before it runs again. The
 * only thing this core keeps that a load invalidates is the OGL backend's claim
 * about which GL context its objects came from, and the claim a state made
 * BEFORE anyone looked is the one that was believed and should not have been
 * (chimera issue 126; see OGLGfx.cpp). Set after the load, so the load cannot
 * wipe it. */
void chimera_dolphin_state_loaded(void)
{
  if (s_gl_backend)
    chimera_dolphin_gl_state_loaded();
}

void chimera_dolphin_frame(void)
{
  // chimera: before the machine steps, give the OGL backend the chance to
  // notice its GL context has moved (a savestate loaded into this session) and
  // rebuild - here nothing is mid-draw. Only when the OGL BACKEND is the one
  // running: the software renderer has no GL objects to lose, and a project can
  // ask for it while the bridge is live, in which case g_gfx is an SWGfx and the
  // helper's cast would be a lie.
  if (s_gl_backend)
    chimera_dolphin_gl_frame_start();
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

// Domains: main RAM always; a Wii adds MEM2 and loses the ARAM a GameCube
// has (index 1 keeps a stable meaning per machine, and the count says which
// machine this is).
int chimera_dolphin_domain_count(void)
{
  return Sys().IsWii() ? 3 : 3;
}

uint8_t* chimera_dolphin_domain_ptr(int i)
{
  auto& memory = Sys().GetMemory();
  if (Sys().IsWii())
  {
    switch (i)
    {
    case 0:
      return memory.GetRAM();
    case 1:
      return memory.GetEXRAM();
    case 2:
      return memory.GetL1Cache();
    }
    return nullptr;
  }
  switch (i)
  {
  case 0:
    return memory.GetRAM();
  case 1:
    return Sys().GetDSP().GetARAMPtr();
  case 2:
    return memory.GetL1Cache();
  }
  return nullptr;
}

int64_t chimera_dolphin_domain_size(int i)
{
  auto& memory = Sys().GetMemory();
  if (Sys().IsWii())
  {
    switch (i)
    {
    case 0:
      return memory.GetRamSizeReal();
    case 1:
      return memory.GetExRamSizeReal();
    case 2:
      return memory.GetL1CacheSize();
    }
    return 0;
  }
  switch (i)
  {
  case 0:
    return memory.GetRamSizeReal();
  case 1:
    return 16 * 1024 * 1024;
  case 2:
    return memory.GetL1CacheSize();
  }
  return 0;
}

const char* chimera_dolphin_domain_name(int i)
{
  if (Sys().IsWii())
  {
    switch (i)
    {
    case 0:
      return "System RAM";
    case 1:
      return "MEM2";
    case 2:
      return "L1 Cache";
    }
    return nullptr;
  }
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

void chimera_dolphin_set_machine(const char* name)
{
  snprintf(s_machine, sizeof s_machine, "%s", name ? name : "");
}

void chimera_dolphin_set_widescreen(int on)
{
  s_widescreen = on != 0;
}

void chimera_dolphin_set_memcard_a(int present)
{
  s_memcard_a = present != 0;
}

void chimera_dolphin_set_port(int port, int present)
{
  if (port >= 0 && port < 4)
    s_port_present[port] = present != 0;
}

int chimera_dolphin_port_present(int port)
{
  return port >= 0 && port < 4 && s_port_present[port];
}

void chimera_dolphin_set_renderer(const char* name)
{
  // "opengl-hw" is the package's declared value (-hw = draws on a real GPU,
  // the convention every core shares); plain "opengl" stays as the harness alias
  s_renderer_opengl = name && (strcmp(name, "opengl-hw") == 0 || strcmp(name, "opengl") == 0);
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

// A Wii's saves are its NAND's; the GameCube card slot a Wii also has is not
// where a Wii game saves, and exporting its blank image only confused (#147).
static int CardCount()
{
  if (Sys().IsWii())
    return 0;
  return (s_memcard[0].data ? 1 : 0) + (s_memcard[1].data ? 1 : 0);
}

int chimera_dolphin_savedata_count(void)
{
  s_nand_saves.clear();
  if (Sys().IsWii())
  {
    std::vector<std::pair<std::string, const std::vector<u8>*>> files;
    Chimera_ListNandSaves(files);
    for (auto& [path, data] : files)
      s_nand_saves.push_back({"nand/" + path, data});
  }
  return CardCount() + static_cast<int>(s_nand_saves.size());
}

static const NandSave* NandSaveAt(int i)
{
  i -= CardCount();
  return (i >= 0 && i < static_cast<int>(s_nand_saves.size())) ? &s_nand_saves[i] : nullptr;
}

static const MemcardReg* SavedataAt(int i)
{
  if (Sys().IsWii())
    return nullptr;
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
  if (const MemcardReg* r = SavedataAt(i))
    return r->name.c_str();
  const NandSave* n = NandSaveAt(i);
  return n ? n->name.c_str() : nullptr;
}

int64_t chimera_dolphin_savedata_size(int i)
{
  if (const MemcardReg* r = SavedataAt(i))
    return r->size;
  const NandSave* n = NandSaveAt(i);
  return n ? static_cast<int64_t>(n->data->size()) : 0;
}

const uint8_t* chimera_dolphin_savedata_buffer(int i)
{
  if (const MemcardReg* r = SavedataAt(i))
    return r->data;
  const NandSave* n = NandSaveAt(i);
  return n ? n->data->data() : nullptr;
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
