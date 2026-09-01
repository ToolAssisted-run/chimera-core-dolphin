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
    else
      game = argv[i];
  }
  if (!game || !sys)
  {
    fprintf(stderr, "usage: run-native --sys <Data/Sys> [--user D] [--frames N] [--report N] <game.dol|iso>\n");
    return 2;
  }

  if (!chimera_dolphin_init(user, sys, game))
  {
    fprintf(stderr, "init failed: %s\n", chimera_dolphin_error());
    return 1;
  }
  printf("booted paused; ram %" PRId64 " bytes\n", chimera_dolphin_ram_size());
  fflush(stdout);

  for (long f = 1; f <= frames; f++)
  {
    chimera_dolphin_frame();
    if (f % report == 0 || f == frames)
    {
      printf("frame %5ld ram %016" PRIx64 "\n", f,
             fnv(chimera_dolphin_ram_ptr(), chimera_dolphin_ram_size()));
      fflush(stdout);
    }
  }
  chimera_dolphin_shutdown();
  printf("done\n");
  return 0;
}
