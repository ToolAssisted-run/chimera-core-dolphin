// Guest-only overrides for libc calls whose syscalls the miniBox surface
// rejects (by design: no host filesystem). One static link means defining
// these here shadows musl's versions. Everything reports a read-only
// filesystem; dolphin logs and carries on.
// SPDX-License-Identifier: MIT
#include <cerrno>
#include <cstring>
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
