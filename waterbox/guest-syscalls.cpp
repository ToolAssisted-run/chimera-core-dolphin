// Guest-only overrides for libc calls whose syscalls the miniBox surface
// rejects (by design: no host filesystem). One static link means defining
// these here shadows musl's versions. Everything reports a read-only
// filesystem; dolphin logs and carries on.
// SPDX-License-Identifier: MIT
#include <cerrno>
#include <cstring>
#include <pthread.h>
#include <sched.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

extern "C" {

int mkdir(const char*, mode_t)
{
  errno = EROFS;
  return -1;
}

int rmdir(const char*)
{
  errno = EROFS;
  return -1;
}

int unlink(const char*)
{
  errno = EROFS;
  return -1;
}

int rename(const char*, const char*)
{
  errno = EROFS;
  return -1;
}

int chmod(const char*, mode_t)
{
  errno = EROFS;
  return -1;
}

// std::thread::hardware_concurrency goes through here; the box is one CPU
// and saying so keeps every pool deterministic.
int sched_getaffinity(pid_t, size_t cpusetsize, cpu_set_t* mask)
{
  if (!mask || cpusetsize < sizeof(unsigned long))
  {
    errno = EINVAL;
    return -1;
  }
  memset(mask, 0, cpusetsize);
  CPU_SET(0, mask);
  return 0;
}

// musl defines all four affinity functions in one object; shadowing one
// means providing all of them.
int sched_setaffinity(pid_t, size_t, const cpu_set_t*)
{
  return 0;
}

int pthread_setaffinity_np(pthread_t, size_t, const cpu_set_t*)
{
  return 0;
}

int pthread_getaffinity_np(pthread_t, size_t cpusetsize, cpu_set_t* mask)
{
  return sched_getaffinity(0, cpusetsize, mask);
}

char* getcwd(char* buf, size_t size)
{
  // one flat namespace; "/" is as true as anything
  if (!buf || size < 2)
  {
    errno = ERANGE;
    return nullptr;
  }
  buf[0] = '/';
  buf[1] = '\0';
  return buf;
}

}  // extern "C"
