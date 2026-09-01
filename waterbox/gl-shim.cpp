// The guest's end of the GPU bridge: the context Dolphin's OGL backend thinks
// it is drawing into, when the drawing happens on a real GPU outside the
// sandbox.
//
// Dolphin loads every GL entry point through GLContext::GetFuncAddress, which
// is exactly the seam the bridge wants: each name resolves to a generated
// wrapper that packs the call's arguments and hands them to the host through
// the sandbox's one callback. The renderer is unmodified, and what it believes
// about the GPU - version, extensions - is what the driver actually said,
// because glGetString crosses the bridge like everything else.
//
// WHAT THIS COSTS: the GPU is outside the sandbox. It is outside the
// savestate, outside this core's determinism, and different on every machine.
// A run recorded this way replays only on the same driver.
// SPDX-License-Identifier: MIT

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "Common/GL/GLContext.h"

#include "gl-bridge.h" /* miniBox source/gl: the shared contract */

/* generated-gl/gl-bridge-guest.cpp; install refuses a host whose list is
 * shorter than this core was built against */
bool chimera_gl_install(chimera_gl_bridge_fn bridge);
void* chimera_gl_lookup(const char* name);

static chimera_gl_bridge_fn g_bridge;

// Native single-binary builds link the host half too; the guest resolves
// this to null and correctly does nothing (one host thread, no contention).
extern "C" void chimera_gl_host_release_current(void) __attribute__((weak));

extern "C" void chimera_dolphin_install_gpu_bridge(uint64_t addr)
{
  const auto fn = reinterpret_cast<chimera_gl_bridge_fn>(static_cast<uintptr_t>(addr));
  if (fn && chimera_gl_install(fn))
    g_bridge = fn;
}

extern "C" int chimera_dolphin_gpu_bridge_present(void)
{
  return g_bridge != nullptr;
}

namespace
{
[[noreturn]] void UnbridgedCallTrap()
{
  fprintf(stderr, "chimera gl: an entry point with no bridge wrapper was CALLED - the\n"
                  "core-profile renderer was assumed never to reach it. Rebuild with the\n"
                  "name added to waterbox/gl-entry-points.txt (and the master list).\n");
  abort();
}

class ChimeraGLContext final : public GLContext
{
public:
  bool IsHeadless() const override { return true; }
  bool MakeCurrent() override { return true; }
  bool ClearCurrent() override
  {
    if (chimera_gl_host_release_current)
      chimera_gl_host_release_current();
    return true;
  }
  ~ChimeraGLContext() override
  {
    if (chimera_gl_host_release_current)
      chimera_gl_host_release_current();
  }
  void Swap() override {}
  void SwapInterval(int) override {}
  void* GetFuncAddress(const std::string& name) override
  {
    void* p = chimera_gl_lookup(name.c_str());
    if (p)
      return p;
    // Dolphin's loader asks for the whole desktop-GL name set and treats a
    // null as "driver too old" - but a core-profile renderer never CALLS the
    // legacy names, the same vacuous satisfaction every real driver's
    // GetProcAddress provides. Answer with a trap that names the caller if
    // that assumption ever breaks.
    if (getenv("CHIMERA_TRACE_GL"))
      fprintf(stderr, "[gl] no wrapper for %s (trap stub handed out)\n", name.c_str());
    return reinterpret_cast<void*>(&UnbridgedCallTrap);
  }
  // One real context exists; shader compilation is pinned synchronous, so
  // nobody should ask - and an asker gets "no" rather than a second context
  // the bridge cannot provide.
  std::unique_ptr<GLContext> CreateSharedContext() override { return nullptr; }

protected:
  bool Initialize(const WindowSystemInfo&, bool /*stereo*/, bool /*core*/) override
  {
    m_opengl_mode = Mode::OpenGL;
    m_backbuffer_width = 640;
    m_backbuffer_height = 528;
    return true;
  }
};
}  // namespace

extern "C" GLContext* Chimera_CreateGLContext()
{
  // null when no bridge was handed over: the factory falls through to the
  // platform paths, of which the sandbox has none - but the driver never
  // selects the OGL backend without a bridge, so this is belt and braces.
  return g_bridge ? new ChimeraGLContext() : nullptr;
}
