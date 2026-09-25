// A read-only view of a zip archive, copied from chimera-core-rpcs3's
// waterbox/archive.h (the same author; MIT). Dolphin uses it for one thing: the
// .zip a Wii project's Save data slot carries, which is exactly what Export
// Save Data wrote - entries named nand/<NAND path> (chimera#147).
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

namespace chimera
{
  struct zip_entry
  {
    std::string path;      // as stored, '/' separated, never with a leading '/'
    u64 size = 0;          // uncompressed
    u64 csize = 0;         // compressed
    u64 header_offset = 0; // local file header, from the start of the archive
    u16 method = 0;        // 0 stored, 8 deflated
  };

  // The central directory, read once. Immutable afterwards, so any number of
  // open files may share one index.
  struct zip_index
  {
    std::string host_path;
    std::vector<zip_entry> entries;
  };

  // Reads the central directory. False when the file is not a zip at all;
  // `error` then says why, for a message a person can act on.
  bool zip_open(const std::string& host_path, zip_index& out, std::string& error);

  // One open file inside the archive. Each holds its own handle and its own
  // decompression cursor, so reads of different entries never disturb each
  // other and nothing needs a lock: a green thread that yields mid-read
  // resumes on its own cursor.
  class zip_stream
  {
  public:
    zip_stream(std::shared_ptr<const zip_index> index, size_t entry);
    ~zip_stream();

    zip_stream(const zip_stream&) = delete;
    zip_stream& operator=(const zip_stream&) = delete;

    // Bytes of the UNCOMPRESSED entry at `offset`. Short reads mean the end.
    u64 read_at(u64 offset, void* buffer, u64 size);

  private:
    bool ensure_data_offset();
    bool restart();
    bool skip_to(u64 offset);

    std::shared_ptr<const zip_index> m_index;
    size_t m_entry = 0;
    std::FILE* m_file = nullptr;
    u64 m_data_offset = 0;   // where the entry's bytes begin, 0 until resolved
    bool m_resolved = false;

    // the deflate cursor: where the next decompressed byte would land
    void* m_stream = nullptr;   // z_stream, owned
    u64 m_pos = 0;              // uncompressed position of the cursor
    u64 m_in_left = 0;          // compressed bytes not yet fed
    std::vector<u8> m_in;       // compressed input window
    std::vector<u8> m_skip;     // scratch for seeking forward
  };
}  // namespace chimera
