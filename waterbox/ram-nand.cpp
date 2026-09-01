// The Wii NAND as an in-memory filesystem.
//
// Dolphin's IOS talks to its NAND through the FS::FileSystem interface, and
// the stock backend is a directory on the host - which a sandboxed machine
// does not have, and a deterministic one would not want. This backend keeps
// the whole tree in ordinary guest memory: every byte of it is machine state,
// captured and restored exactly by whole-machine savestates, identical across
// the native and sandboxed flavors by construction.
//
// The STORE is a process-wide singleton and the FileSystem objects handed to
// IOS are views of it: IOS recreates its filesystem object mid-boot (ES
// launches reset it), and the NAND's contents must survive that the way a
// real flash chip survives a reboot.
//
// Semantics follow the HostBackend where a game could tell the difference:
// permission checks (uid 0 is the supervisor), newest-first directory order
// (Nintendo's FST is a linked list with front insertion), and the real
// cluster/inode accounting for NAND stats.
// SPDX-License-Identifier: MIT

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "Common/ChunkFile.h"
#include "Core/IOS/FS/FileSystem.h"

namespace
{
using namespace IOS::HLE::FS;

struct Node
{
  bool is_file = false;
  FileAttribute attribute = 0;
  Modes modes{Mode::None, Mode::None, Mode::None};
  Uid uid = 0;
  Gid gid = 0;
  std::vector<u8> data;                // files
  std::vector<std::string> children;   // directories: newest FIRST
};

struct Store
{
  // path -> node; "/" always exists
  std::map<std::string, Node> nodes;

  Store() { Reset(); }

  void Reset()
  {
    nodes.clear();
    Node root;
    root.is_file = false;
    // Mode 0x16 in the FS sysmodule: directory, owner none, group/other read
    root.modes = {Mode::None, Mode::Read, Mode::Read};
    nodes.emplace("/", std::move(root));
  }
};

Store& TheStore()
{
  static Store store;
  return store;
}

bool AllowsMode(Mode allowed, Mode requested)
{
  return (u8(allowed) & u8(requested)) == u8(requested);
}

bool CheckPermission(const Node& node, Uid uid, Gid gid, Mode requested)
{
  if (uid == 0)
    return true;  // the supervisor
  Mode allowed = node.modes.other;
  if (node.gid == gid)
    allowed = Mode(u8(allowed) | u8(node.modes.group));
  if (node.uid == uid)
    allowed = Mode(u8(allowed) | u8(node.modes.owner));
  return AllowsMode(allowed, requested);
}

class RamFileSystem final : public FileSystem
{
public:
  void DoState(PointerWrap& p) override
  {
    // Arena snapshots make this redundant in the sandbox, but the interface
    // demands it and the native flavor may exercise it: serialize the store.
    auto& store = TheStore();
    u32 count = u32(store.nodes.size());
    p.Do(count);
    if (p.IsReadMode())
    {
      store.nodes.clear();
      for (u32 i = 0; i < count; i++)
      {
        std::string path;
        Node node;
        p.Do(path);
        p.Do(node.is_file);
        p.Do(node.attribute);
        p.Do(node.modes.owner);
        p.Do(node.modes.group);
        p.Do(node.modes.other);
        p.Do(node.uid);
        p.Do(node.gid);
        p.Do(node.data);
        p.Do(node.children);
        store.nodes.emplace(std::move(path), std::move(node));
      }
    }
    else
    {
      for (auto& [path, node] : store.nodes)
      {
        std::string key = path;
        p.Do(key);
        p.Do(node.is_file);
        p.Do(node.attribute);
        p.Do(node.modes.owner);
        p.Do(node.modes.group);
        p.Do(node.modes.other);
        p.Do(node.uid);
        p.Do(node.gid);
        p.Do(node.data);
        p.Do(node.children);
      }
    }
    for (auto& handle : m_handles)
    {
      p.Do(handle.open);
      p.Do(handle.path);
      p.Do(handle.mode);
      p.Do(handle.offset);
    }
  }

  ResultCode Format(Uid uid) override
  {
    if (uid != 0)
      return ResultCode::AccessDenied;
    TheStore().Reset();
    for (auto& handle : m_handles)
      handle.open = false;
    return ResultCode::Success;
  }

  Result<FileHandle> OpenFile(Uid uid, Gid gid, const std::string& path, Mode mode) override
  {
    if (!IsValidPath(path))
      return std::unexpected{ResultCode::Invalid};
    Node* node = Find(path);
    if (!node)
      return std::unexpected{ResultCode::NotFound};
    if (!node->is_file)
      return std::unexpected{ResultCode::Invalid};
    if (!CheckPermission(*node, uid, gid, mode))
      return std::unexpected{ResultCode::AccessDenied};
    for (u32 i = 0; i < u32(m_handles.size()); i++)
    {
      if (m_handles[i].open)
        continue;
      m_handles[i] = {true, path, mode, 0};
      return FileHandle{this, i};
    }
    return std::unexpected{ResultCode::NoFreeHandle};
  }

  ResultCode Close(Fd fd) override
  {
    if (fd >= m_handles.size() || !m_handles[fd].open)
      return ResultCode::Invalid;
    m_handles[fd].open = false;
    return ResultCode::Success;
  }

  Result<u32> ReadBytesFromFile(Fd fd, u8* ptr, u32 size) override
  {
    Handle* h = HandleOf(fd);
    if (!h)
      return std::unexpected{ResultCode::Invalid};
    if (!AllowsMode(h->mode, Mode::Read))
      return std::unexpected{ResultCode::AccessDenied};
    Node* node = Find(h->path);
    if (!node)
      return std::unexpected{ResultCode::NotFound};
    const u32 available = h->offset < node->data.size() ? u32(node->data.size() - h->offset) : 0;
    const u32 take = std::min(size, available);
    std::memcpy(ptr, node->data.data() + h->offset, take);
    h->offset += take;
    return take;
  }

  Result<u32> WriteBytesToFile(Fd fd, const u8* ptr, u32 size) override
  {
    Handle* h = HandleOf(fd);
    if (!h)
      return std::unexpected{ResultCode::Invalid};
    if (!AllowsMode(h->mode, Mode::Write))
      return std::unexpected{ResultCode::AccessDenied};
    Node* node = Find(h->path);
    if (!node)
      return std::unexpected{ResultCode::NotFound};
    if (node->data.size() < size_t(h->offset) + size)
      node->data.resize(size_t(h->offset) + size);
    std::memcpy(node->data.data() + h->offset, ptr, size);
    h->offset += size;
    return size;
  }

  Result<u32> SeekFile(Fd fd, u32 offset, SeekMode mode) override
  {
    Handle* h = HandleOf(fd);
    if (!h)
      return std::unexpected{ResultCode::Invalid};
    Node* node = Find(h->path);
    if (!node)
      return std::unexpected{ResultCode::NotFound};
    u64 base = 0;
    switch (mode)
    {
    case SeekMode::Set:
      base = 0;
      break;
    case SeekMode::Current:
      base = h->offset;
      break;
    case SeekMode::End:
      base = node->data.size();
      break;
    default:
      return std::unexpected{ResultCode::Invalid};
    }
    const u64 target = base + offset;
    // the FS sysmodule refuses seeks past the end
    if (target > node->data.size())
      return std::unexpected{ResultCode::Invalid};
    h->offset = u32(target);
    return h->offset;
  }

  Result<FileStatus> GetFileStatus(Fd fd) override
  {
    Handle* h = HandleOf(fd);
    if (!h)
      return std::unexpected{ResultCode::Invalid};
    Node* node = Find(h->path);
    if (!node)
      return std::unexpected{ResultCode::NotFound};
    return FileStatus{h->offset, u32(node->data.size())};
  }

  ResultCode CreateFile(Uid caller_uid, Gid caller_gid, const std::string& path,
                        FileAttribute attribute, Modes modes) override
  {
    return Create(caller_uid, caller_gid, path, attribute, modes, /*is_file=*/true);
  }

  ResultCode CreateDirectory(Uid caller_uid, Gid caller_gid, const std::string& path,
                             FileAttribute attribute, Modes modes) override
  {
    return Create(caller_uid, caller_gid, path, attribute, modes, /*is_file=*/false);
  }

  ResultCode Delete(Uid caller_uid, Gid caller_gid, const std::string& path) override
  {
    if (!IsValidNonRootPath(path))
      return ResultCode::Invalid;
    auto& store = TheStore();
    Node* node = Find(path);
    if (!node)
      return ResultCode::NotFound;
    const auto split = SplitPathAndBasename(path);
    Node* parent = Find(split.parent);
    if (!parent || !CheckPermission(*parent, caller_uid, caller_gid, Mode::Write))
      return ResultCode::AccessDenied;
    // a directory goes with everything under it, like the FS sysmodule
    const std::string prefix = path + "/";
    for (auto it = store.nodes.begin(); it != store.nodes.end();)
    {
      if (it->first == path || it->first.rfind(prefix, 0) == 0)
        it = store.nodes.erase(it);
      else
        ++it;
    }
    std::erase(parent->children, split.file_name);
    return ResultCode::Success;
  }

  ResultCode Rename(Uid caller_uid, Gid caller_gid, const std::string& old_path,
                    const std::string& new_path) override
  {
    if (!IsValidNonRootPath(old_path) || !IsValidNonRootPath(new_path))
      return ResultCode::Invalid;
    auto& store = TheStore();
    Node* node = Find(old_path);
    if (!node)
      return ResultCode::NotFound;
    const auto old_split = SplitPathAndBasename(old_path);
    const auto new_split = SplitPathAndBasename(new_path);
    Node* old_parent = Find(old_split.parent);
    Node* new_parent = Find(new_split.parent);
    if (!old_parent || !new_parent)
      return ResultCode::NotFound;
    if (!CheckPermission(*old_parent, caller_uid, caller_gid, Mode::Write) ||
        !CheckPermission(*new_parent, caller_uid, caller_gid, Mode::Write))
      return ResultCode::AccessDenied;
    // an existing destination file is replaced, like the FS sysmodule
    if (Node* existing = Find(new_path))
    {
      if (!existing->is_file || !node->is_file)
        return ResultCode::Invalid;
      store.nodes.erase(new_path);
      std::erase(new_parent->children, new_split.file_name);
    }
    // move the node and, for directories, the whole subtree
    std::vector<std::pair<std::string, Node>> moved;
    const std::string prefix = old_path + "/";
    for (auto it = store.nodes.begin(); it != store.nodes.end();)
    {
      if (it->first == old_path || it->first.rfind(prefix, 0) == 0)
      {
        std::string renamed = new_path + it->first.substr(old_path.size());
        moved.emplace_back(std::move(renamed), std::move(it->second));
        it = store.nodes.erase(it);
      }
      else
      {
        ++it;
      }
    }
    for (auto& [p, n] : moved)
      store.nodes.emplace(std::move(p), std::move(n));
    std::erase(old_parent->children, old_split.file_name);
    // renamed nodes are the newest: front insertion
    new_parent = Find(new_split.parent);  // re-find: the map changed
    new_parent->children.insert(new_parent->children.begin(), new_split.file_name);
    return ResultCode::Success;
  }

  Result<std::vector<std::string>> ReadDirectory(Uid caller_uid, Gid caller_gid,
                                                 const std::string& path) override
  {
    Node* node = Find(path);
    if (!node)
      return std::unexpected{ResultCode::NotFound};
    if (node->is_file)
      return std::unexpected{ResultCode::Invalid};
    if (!CheckPermission(*node, caller_uid, caller_gid, Mode::Read))
      return std::unexpected{ResultCode::AccessDenied};
    return node->children;
  }

  Result<Metadata> GetMetadata(Uid caller_uid, Gid caller_gid, const std::string& path) override
  {
    Node* node = Find(path);
    if (!node)
      return std::unexpected{ResultCode::NotFound};
    Metadata meta{};
    meta.uid = node->uid;
    meta.gid = node->gid;
    meta.attribute = node->attribute;
    meta.modes = node->modes;
    meta.is_file = node->is_file;
    meta.size = u32(node->data.size());
    meta.fst_index = 0;
    return meta;
  }

  ResultCode SetMetadata(Uid caller_uid, const std::string& path, Uid uid, Gid gid,
                         FileAttribute attribute, Modes modes) override
  {
    Node* node = Find(path);
    if (!node)
      return ResultCode::NotFound;
    if (caller_uid != 0 && caller_uid != node->uid)
      return ResultCode::AccessDenied;
    if (caller_uid != 0 && node->uid != uid)
      return ResultCode::AccessDenied;
    node->uid = uid;
    node->gid = gid;
    node->attribute = attribute;
    node->modes = modes;
    return ResultCode::Success;
  }

  Result<NandStats> GetNandStats() override
  {
    u32 clusters = 0, inodes = 0;
    for (const auto& [path, node] : TheStore().nodes)
    {
      inodes++;
      if (node.is_file)
        clusters += u32((node.data.size() + CLUSTER_SIZE - 1) / CLUSTER_SIZE);
    }
    NandStats stats{};
    stats.cluster_size = CLUSTER_SIZE;
    stats.used_clusters = clusters;
    stats.free_clusters = USABLE_CLUSTERS > clusters ? USABLE_CLUSTERS - clusters : 0;
    stats.bad_clusters = 0;
    stats.reserved_clusters = RESERVED_CLUSTERS;
    stats.used_inodes = inodes;
    stats.free_inodes = TOTAL_INODES > inodes ? TOTAL_INODES - inodes : 0;
    return stats;
  }

  Result<DirectoryStats> GetDirectoryStats(const std::string& path) override
  {
    const auto extended = GetExtendedDirectoryStats(path);
    if (!extended)
      return std::unexpected{extended.error()};
    DirectoryStats stats{};
    stats.used_clusters = u32(std::min<u64>(extended->used_clusters, 0xffffffff));
    stats.used_inodes = u32(std::min<u64>(extended->used_inodes, 0xffffffff));
    return stats;
  }

  Result<ExtendedDirectoryStats> GetExtendedDirectoryStats(const std::string& path) override
  {
    Node* node = Find(path);
    if (!node)
      return std::unexpected{ResultCode::NotFound};
    ExtendedDirectoryStats stats{};
    const std::string prefix = path == "/" ? "/" : path + "/";
    for (const auto& [p, n] : TheStore().nodes)
    {
      if (p != path && p.rfind(prefix, 0) != 0)
        continue;
      stats.used_inodes++;
      if (n.is_file)
        stats.used_clusters += (n.data.size() + CLUSTER_SIZE - 1) / CLUSTER_SIZE;
    }
    return stats;
  }

  void SetNandRedirects(std::vector<NandRedirect>) override
  {
    // Redirects point Wii paths at host directories; there is no host here.
  }

private:
  struct Handle
  {
    bool open = false;
    std::string path;
    Mode mode = Mode::None;
    u32 offset = 0;
  };

  Node* Find(const std::string& path)
  {
    auto& nodes = TheStore().nodes;
    auto it = nodes.find(path);
    return it == nodes.end() ? nullptr : &it->second;
  }

  Handle* HandleOf(Fd fd)
  {
    if (fd >= m_handles.size() || !m_handles[fd].open)
      return nullptr;
    return &m_handles[fd];
  }

  ResultCode Create(Uid caller_uid, Gid caller_gid, const std::string& path,
                    FileAttribute attribute, Modes modes, bool is_file)
  {
    if (!IsValidNonRootPath(path))
      return ResultCode::Invalid;
    auto& store = TheStore();
    if (store.nodes.count(path))
      return ResultCode::AlreadyExists;
    const auto split = SplitPathAndBasename(path);
    Node* parent = Find(split.parent);
    if (!parent)
      return ResultCode::NotFound;
    if (parent->is_file)
      return ResultCode::Invalid;
    if (!CheckPermission(*parent, caller_uid, caller_gid, Mode::Write))
      return ResultCode::AccessDenied;
    if (split.file_name.size() > MaxFilenameLength)
      return ResultCode::Invalid;
    Node node;
    node.is_file = is_file;
    node.attribute = attribute;
    node.modes = modes;
    node.uid = caller_uid;
    node.gid = caller_gid;
    store.nodes.emplace(path, std::move(node));
    // the FST is a linked list with front insertion: newest first
    parent = Find(split.parent);
    parent->children.insert(parent->children.begin(), split.file_name);
    return ResultCode::Success;
  }

  std::array<Handle, 16> m_handles{};
};
}  // namespace

extern "C" IOS::HLE::FS::FileSystem* Chimera_MakeNandFilesystem()
{
  return new RamFileSystem();
}
