// First-light runner for the native reference: boot, step N fields, print a
// running FNV-1a digest of main RAM. Two invocations printing the same lines
// is the determinism check; the guest printing them too is M1.
// SPDX-License-Identifier: MIT

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "dolphin-driver.h"

#ifdef CHIMERA_GL_BRIDGE
extern "C" {
int chimera_gl_host_init(char* err, int errlen);
const char* chimera_gl_host_description(void);
uintptr_t chimera_gl_host_dispatch(uintptr_t op, uintptr_t a, uintptr_t b, uintptr_t c,
                                   uintptr_t d, uintptr_t e);
void chimera_dolphin_install_gpu_bridge(uint64_t addr);
int chimera_dolphin_gpu_bridge_present(void);
}
#endif

#include <execinfo.h>
#include <pthread.h>
#include <csignal>
#include <thread>
#include <sys/time.h>
#include <unistd.h>

static void DumpHandler(int)
{
  void* frames[32];
  int n = backtrace(frames, 32);
  backtrace_symbols_fd(frames, n, 2);
  _exit(9);
}

static uint64_t fnv(const uint8_t* p, int64_t n)
{
  uint64_t h = 1469598103934665603ULL;
  for (int64_t i = 0; i < n; i++)
  {
    h ^= p[i];
    h *= 1099511628211ULL;
  }
  return h;
}

int main(int argc, char** argv)
{
  const char* game = nullptr;
  const char* user = "work/user";
  const char* sys = nullptr;
  long frames = 60;
  long report = 10;
  const char* ram_out = nullptr;
  const char* savedata_out = nullptr;
  struct { long first, count; int index; } press[32];
  int presses = 0;
  for (int i = 1; i < argc; i++)
  {
    if (!strcmp(argv[i], "--frames") && i + 1 < argc)
      frames = atol(argv[++i]);
    else if (!strcmp(argv[i], "--report") && i + 1 < argc)
      report = atol(argv[++i]);
    else if (!strcmp(argv[i], "--user") && i + 1 < argc)
      user = argv[++i];
    else if (!strcmp(argv[i], "--sys") && i + 1 < argc)
      sys = argv[++i];
    else if (!strcmp(argv[i], "--ram-out") && i + 1 < argc)
      ram_out = argv[++i];
    else if (!strcmp(argv[i], "--savedata-out") && i + 1 < argc)
      savedata_out = argv[++i];
    else if (!strcmp(argv[i], "--cpu-core") && i + 1 < argc)
      chimera_dolphin_set_cpu_core(argv[++i]);
    else if (!strcmp(argv[i], "--machine") && i + 1 < argc)
      chimera_dolphin_set_machine(argv[++i]);
    else if (!strcmp(argv[i], "--ports") && i + 1 < argc)
    {
      const char* mask = argv[++i]; // e.g. "1010": port1 and port3
      for (int pi = 0; pi < 4 && mask[pi]; pi++)
        chimera_dolphin_set_port(pi, mask[pi] == '1');
    }
    else if (!strcmp(argv[i], "--renderer") && i + 1 < argc)
    {
      const char* r = argv[++i];
      chimera_dolphin_set_renderer(r);
#ifdef CHIMERA_GL_BRIDGE
      if (!strcmp(r, "opengl"))
      {
        // Install BEFORE the host context loads its entry points: in this
        // single binary the guest install writes wrappers into the shared
        // glad table, and gladLoadGL afterwards puts the REAL driver
        // functions back - the host dispatch must call those, or every call
        // recurses through its own wrapper forever.
        chimera_dolphin_install_gpu_bridge(
            (uint64_t)(uintptr_t)&chimera_gl_host_dispatch);
        char glerr[256] = "";
        if (chimera_gl_host_init(glerr, sizeof glerr) != 0)
        {
          fprintf(stderr, "gpu bridge: no context (%s)\n", glerr);
        }
        else
        {
          fprintf(stderr, "gpu bridge: %s\n", chimera_gl_host_description());
          fprintf(stderr, "gpu bridge: installed=%d\n", chimera_dolphin_gpu_bridge_present());
        }
      }
#endif
    }
    else if (!strcmp(argv[i], "--press") && i + 1 < argc && presses < 32)
    {
      long a, b;
      int c;
      if (sscanf(argv[++i], "%ld:%ld:%d", &a, &b, &c) == 3)
      {
        press[presses].first = a;
        press[presses].count = b;
        press[presses].index = c;
        presses++;
      }
    }
    else
      game = argv[i];
  }
  if (!game || !sys)
  {
    fprintf(stderr, "usage: run-native --sys <Data/Sys> [--user D] [--frames N] [--report N] <game.dol|iso>\n");
    return 2;
  }

  if (getenv("CHIMERA_WATCHDOG"))
  {
    signal(SIGPROF, DumpHandler);
    std::thread([] {
      sleep(15);
      // a profiling timer fires on whichever thread is burning CPU - the one
      // whose stack we want
      struct itimerval it = {{0, 0}, {0, 10000}};
      setitimer(ITIMER_PROF, &it, nullptr);
    }).detach();
  }
  if (!chimera_dolphin_init(user, sys, game))
  {
    fprintf(stderr, "init failed: %s\n", chimera_dolphin_error());
    return 1;
  }
  printf("booted paused; ram %" PRId64 " bytes\n", chimera_dolphin_ram_size());
  fflush(stdout);

  long lag = 0;
  for (long f = 1; f <= frames; f++)
  {
    for (int pi = 0; pi < presses; pi++)
      chimera_dolphin_set_button(0, press[pi].index,
                                 f >= press[pi].first && f < press[pi].first + press[pi].count);
    chimera_dolphin_frame();
    if (!chimera_dolphin_input_was_read())
      lag++;
    if (f % report == 0 || f == frames)
    {
      int vw, vh, an;
      const uint32_t* vid = chimera_dolphin_video(&vw, &vh);
      const int16_t* aud = chimera_dolphin_audio(&an);
      printf("frame %5ld ram %016" PRIx64 " vid %dx%d %016" PRIx64 " aud %d %016" PRIx64
             " lag %ld\n",
             f, fnv(chimera_dolphin_ram_ptr(), chimera_dolphin_ram_size()), vw, vh,
             fnv((const uint8_t*)vid, (int64_t)vw * vh * 4), an,
             fnv((const uint8_t*)aud, (int64_t)an * 4), lag);
      fflush(stdout);
    }
    const char* snapf = getenv("CHIMERA_SNAP_FRAME");
    if (snapf && atol(snapf) == f)
    {
      FILE* sf = fopen(getenv("CHIMERA_SNAP_OUT"), "wb");
      fwrite(chimera_dolphin_ram_ptr(), 1, chimera_dolphin_ram_size(), sf);
      fclose(sf);
    }
  }
  if (ram_out)
  {
    FILE* f = fopen(ram_out, "wb");
    fwrite(chimera_dolphin_ram_ptr(), 1, chimera_dolphin_ram_size(), f);
    fclose(f);
  }
  if (savedata_out)
  {
    for (int i = 0; i < chimera_dolphin_savedata_count(); i++)
    {
      char path[1024];
      snprintf(path, sizeof path, "%s/%s", savedata_out, chimera_dolphin_savedata_name(i));
      FILE* f = fopen(path, "wb");
      if (!f)
      {
        fprintf(stderr, "cannot write %s\n", path);
        continue;
      }
      fwrite(chimera_dolphin_savedata_buffer(i), 1, (size_t)chimera_dolphin_savedata_size(i), f);
      fclose(f);
      printf("savedata %s %lld bytes\n", chimera_dolphin_savedata_name(i),
             (long long)chimera_dolphin_savedata_size(i));
    }
  }
  chimera_dolphin_shutdown();
  printf("done\n");
  return 0;
}
