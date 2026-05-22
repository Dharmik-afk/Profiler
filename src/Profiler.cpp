#include "Profiler.h"
#include "ProfilerRuntime.h"
#include "detail/ProfilerBackend.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Profiler {
void Init(const Config &config) noexcept { ::ProfilerInit(&config); }

void Shutdown() noexcept { ::ProfilerShutdown(); }

void BeginFrame() noexcept { ::ProfilerBeginFrame(); }

void EndFrame() noexcept { ::ProfilerEndFrame(); }

void BeginScope(const char *name) noexcept { ::ProfilerBeginScope(name); }

void EndScope() noexcept { ::ProfilerEndScope(); }

ProfileFrameView GetLastFrame() noexcept { return ::ProfilerGetLastFrame(); }
} // namespace Profiler

namespace Profiler {
namespace {
using Clock = std::chrono::steady_clock;

static uint64_t NowNs() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             Clock::now().time_since_epoch())
      .count();
}

static uint32_t MaxU32(uint32_t a, uint32_t b) noexcept {
  return (a > b) ? a : b;
}

static uint32_t RemapIndex(uint32_t index, uint32_t offset) noexcept {
  return (index == INVALID_NODE) ? INVALID_NODE : (index + offset);
}

struct FrameStorage {
  std::vector<ProfileNode> Nodes;
  uint64_t FrameStart;
  uint64_t FrameEnd;

  FrameStorage() : FrameStart(0), FrameEnd(0) {}
};

struct ThreadBuffer {
  std::vector<ProfileNode> Nodes;
  std::vector<uint32_t> ScopeStack;

  uint32_t ThreadId;
  uint32_t Generation;

  std::mutex Mutex;

  ThreadBuffer() : ThreadId(0), Generation(0) {}
};

struct StringPool {
  std::mutex Mutex;
  std::deque<std::string> Strings;
  std::unordered_map<std::string, const char *> Lookup;

  const char *Intern(const char *name) noexcept {
    if (name == 0 || name[0] == '\0') {
      return "";
    }

    try {
      std::lock_guard<std::mutex> lock(Mutex);

      const std::string key(name);
      std::unordered_map<std::string, const char *>::const_iterator it =
          Lookup.find(key);
      if (it != Lookup.end()) {
        return it->second;
      }

      Strings.push_back(key);
      const char *stored = Strings.back().c_str();
      Lookup.emplace(Strings.back(), stored);
      return stored;
    } catch (...) {
      return "";
    }
  }
};

static Config g_Config;
static std::atomic<bool> g_Initialized(false);

static std::atomic<bool> g_FrameActive(false);
static std::atomic<bool> g_FrameClosing(false);

alignas(64) static std::atomic<uint32_t> g_CurrentGeneration(1);
alignas(64) static std::atomic<uint32_t> g_ActiveScopes(0);
alignas(64) static std::atomic<uint32_t> g_NextThreadId(1);

static uint64_t g_CurrentFrameStartNs = 0;

static std::mutex g_RegistryMutex;
static std::mutex g_FrameMutex;
static std::condition_variable g_ActiveScopesCv;
static std::mutex g_ActiveScopesCvMutex;

static std::vector<std::unique_ptr<ThreadBuffer>> g_ThreadBuffers;
static StringPool g_StringPool;

static FrameStorage g_Frames[3];
static std::atomic<uint32_t> g_ReadFrameIndex(0);
static uint32_t g_WriteFrameIndex = 1;

thread_local ThreadBuffer *g_LocalBuffer = 0;
thread_local bool g_InProfilerCall = false;

struct RecursionGuard {
  bool Active;

  RecursionGuard() noexcept : Active(false) {
    if (!g_InProfilerCall) {
      g_InProfilerCall = true;
      Active = true;
    }
  }

  ~RecursionGuard() noexcept {
    if (Active) {
      g_InProfilerCall = false;
    }
  }

  bool Ok() const noexcept { return Active; }
};

struct ActiveScopeToken {
  bool Counted;

  ActiveScopeToken() noexcept : Counted(true) {
    g_ActiveScopes.fetch_add(1, std::memory_order_relaxed);
  }

  ~ActiveScopeToken() noexcept {
    if (Counted) {
      const uint32_t previous =
          g_ActiveScopes.fetch_sub(1, std::memory_order_release);
      if (previous == 1) {
        std::lock_guard<std::mutex> lock(g_ActiveScopesCvMutex);
        g_ActiveScopesCv.notify_all();
      }
    }
  }

  void Release() noexcept { Counted = false; }
};

static ThreadBuffer *CreateThreadBuffer() noexcept {
  try {
    std::unique_ptr<ThreadBuffer> buffer(new ThreadBuffer());

    buffer->ThreadId = g_NextThreadId.fetch_add(1, std::memory_order_relaxed);
    buffer->Generation = g_CurrentGeneration.load(std::memory_order_acquire);

    uint32_t threadCount = g_Config.MaxThreads;
    if (threadCount == 0) {
      threadCount = 8;
    }

    uint32_t perThreadNodes = g_Config.MaxNodesPerFrame / threadCount;
    perThreadNodes = MaxU32(perThreadNodes, 64u);

    buffer->Nodes.reserve(perThreadNodes);
    buffer->ScopeStack.reserve(MaxU32(g_Config.MaxScopeDepth, 16u));

    ThreadBuffer *raw = buffer.get();
    g_ThreadBuffers.push_back(std::move(buffer));
    return raw;
  } catch (...) {
    return 0;
  }
}

static ThreadBuffer *GetThreadBuffer() noexcept {
  if (g_LocalBuffer != 0) {
    return g_LocalBuffer;
  }

  std::lock_guard<std::mutex> lock(g_RegistryMutex);

  if (g_LocalBuffer == 0) {
    g_LocalBuffer = CreateThreadBuffer();
  }

  return g_LocalBuffer;
}

static void SyncThreadGeneration(ThreadBuffer &buffer) noexcept {
  const uint32_t generation =
      g_CurrentGeneration.load(std::memory_order_acquire);

  if (buffer.Generation != generation) {
    buffer.Nodes.clear();
    buffer.ScopeStack.clear();
    buffer.Generation = generation;
  }
}

static void MergeThreadBufferIntoFrame(const ThreadBuffer &buffer,
                                       FrameStorage &frame) noexcept {
  if (buffer.Generation !=
      g_CurrentGeneration.load(std::memory_order_acquire)) {
    return;
  }

  const uint32_t addCount = static_cast<uint32_t>(buffer.Nodes.size());
  if (addCount == 0) {
    return;
  }

  const uint32_t oldSize = static_cast<uint32_t>(frame.Nodes.size());
  const uint32_t newSize = oldSize + addCount;

  try {
    if (frame.Nodes.capacity() < static_cast<std::size_t>(newSize)) {
      frame.Nodes.reserve(static_cast<std::size_t>(newSize));
    }

    frame.Nodes.insert(frame.Nodes.end(), buffer.Nodes.begin(),
                       buffer.Nodes.end());

    for (uint32_t i = oldSize; i < newSize; ++i) {
      ProfileNode &node = frame.Nodes[i];
      node.Parent = RemapIndex(node.Parent, oldSize);
      node.FirstChild = RemapIndex(node.FirstChild, oldSize);
      node.LastChild = RemapIndex(node.LastChild, oldSize);
      node.NextSibling = RemapIndex(node.NextSibling, oldSize);
    }
  } catch (...) {
    frame.Nodes.resize(oldSize);
  }
}

static void WaitForScopesToDrain() noexcept {
  std::unique_lock<std::mutex> lock(g_ActiveScopesCvMutex);
  g_ActiveScopesCv.wait(lock, []() noexcept {
    return g_ActiveScopes.load(std::memory_order_acquire) == 0;
  });
}

static void SetFrameInactive() noexcept {
  g_FrameClosing.store(true, std::memory_order_release);
  g_FrameActive.store(false, std::memory_order_release);
}

static void SetFrameActive() noexcept {
  g_FrameClosing.store(false, std::memory_order_release);
  g_FrameActive.store(true, std::memory_order_release);
}
} // namespace

extern "C" {

void ProfilerInit(const Profiler::Config *config) noexcept {
  ProfilerShutdown();

  if (config != 0) {
    g_Config = *config;
  } else {
    g_Config.MaxNodesPerFrame = 0;
    g_Config.MaxScopeDepth = 0;
    g_Config.MaxThreads = 0;
  }

  if (g_Config.MaxNodesPerFrame == 0) {
    g_Config.MaxNodesPerFrame = 4096;
  }
  if (g_Config.MaxScopeDepth == 0) {
    g_Config.MaxScopeDepth = 128;
  }
  if (g_Config.MaxThreads == 0) {
    g_Config.MaxThreads = 8;
  }

  {
    std::lock_guard<std::mutex> lock(g_RegistryMutex);
    g_ThreadBuffers.reserve(g_Config.MaxThreads);
  }

  g_Frames[0].Nodes.reserve(g_Config.MaxNodesPerFrame);
  g_Frames[1].Nodes.reserve(g_Config.MaxNodesPerFrame);
  g_Frames[2].Nodes.reserve(g_Config.MaxNodesPerFrame);

  g_CurrentGeneration.store(1, std::memory_order_release);
  g_CurrentFrameStartNs = 0;

  g_FrameActive.store(false, std::memory_order_release);
  g_FrameClosing.store(false, std::memory_order_release);
  g_ActiveScopes.store(0, std::memory_order_release);

  g_ReadFrameIndex.store(0, std::memory_order_release);
  g_WriteFrameIndex = 1;

  g_Initialized.store(true, std::memory_order_release);
}

void ProfilerShutdown() noexcept {
  if (!g_Initialized.load(std::memory_order_acquire)) {
    return;
  }

  SetFrameInactive();
  WaitForScopesToDrain();

  g_FrameActive.store(false, std::memory_order_release);
  g_FrameClosing.store(false, std::memory_order_release);
  g_ActiveScopes.store(0, std::memory_order_release);

  {
    std::lock_guard<std::mutex> registryLock(g_RegistryMutex);
    for (std::size_t i = 0; i < g_ThreadBuffers.size(); ++i) {
      ThreadBuffer *buffer = g_ThreadBuffers[i].get();
      if (buffer == 0) {
        continue;
      }

      std::lock_guard<std::mutex> bufferLock(buffer->Mutex);
      buffer->Nodes.clear();
      buffer->ScopeStack.clear();
      buffer->Generation = g_CurrentGeneration.load(std::memory_order_relaxed);
    }
  }

  g_Frames[0].Nodes.clear();
  g_Frames[1].Nodes.clear();
  g_Frames[2].Nodes.clear();

  g_Frames[0].FrameStart = 0;
  g_Frames[0].FrameEnd = 0;
  g_Frames[1].FrameStart = 0;
  g_Frames[1].FrameEnd = 0;
  g_Frames[2].FrameStart = 0;
  g_Frames[2].FrameEnd = 0;

  g_LocalBuffer = 0;
  g_CurrentGeneration.store(1, std::memory_order_release);
  g_CurrentFrameStartNs = 0;
  g_ReadFrameIndex.store(0, std::memory_order_release);
  g_WriteFrameIndex = 1;

  g_Initialized.store(false, std::memory_order_release);
}

void ProfilerBeginFrame() noexcept {
  if (!g_Initialized.load(std::memory_order_acquire)) {
    return;
  }

  std::lock_guard<std::mutex> frameLock(g_FrameMutex);

  if (g_FrameActive.load(std::memory_order_acquire)) {
    return;
  }

  SetFrameActive();
  g_CurrentFrameStartNs = NowNs();
}

void ProfilerEndFrame() noexcept {
  if (!g_Initialized.load(std::memory_order_acquire)) {
    return;
  }

  std::lock_guard<std::mutex> frameLock(g_FrameMutex);

  if (!g_FrameActive.load(std::memory_order_acquire)) {
    return;
  }

  SetFrameInactive();
  WaitForScopesToDrain();

  const uint32_t readIndex = g_ReadFrameIndex.load(std::memory_order_acquire);
  const uint32_t writeIndex = g_WriteFrameIndex;

  FrameStorage &writeFrame = g_Frames[writeIndex];

  try {
    writeFrame.Nodes.clear();
    writeFrame.FrameStart = g_CurrentFrameStartNs;
    writeFrame.FrameEnd = NowNs();

    std::vector<ThreadBuffer *> buffers;

    {
      std::lock_guard<std::mutex> lock(g_RegistryMutex);
      buffers.reserve(g_ThreadBuffers.size());

      for (std::size_t i = 0; i < g_ThreadBuffers.size(); ++i) {
        buffers.push_back(g_ThreadBuffers[i].get());
      }
    }

    for (std::size_t i = 0; i < buffers.size(); ++i) {
      ThreadBuffer *buffer = buffers[i];
      if (buffer == 0) {
        continue;
      }

      std::lock_guard<std::mutex> lock(buffer->Mutex);
      MergeThreadBufferIntoFrame(*buffer, writeFrame);
    }

    g_CurrentGeneration.fetch_add(1, std::memory_order_release);
    g_ReadFrameIndex.store(writeIndex, std::memory_order_release);
    g_WriteFrameIndex = readIndex;
  } catch (...) {
    writeFrame.Nodes.clear();
  }
}

void ProfilerBeginScope(const char *name) noexcept {
  if (!g_Initialized.load(std::memory_order_acquire)) {
    PROFILER_UNLIKELY return;
  }

  RecursionGuard recursionGuard;
  if (!recursionGuard.Ok()) {
    PROFILER_UNLIKELY return;
  }

  ActiveScopeToken activeToken;

  if (!g_FrameActive.load(std::memory_order_acquire) ||
      g_FrameClosing.load(std::memory_order_acquire)) {
    PROFILER_UNLIKELY return;
  }

  ThreadBuffer *buffer = GetThreadBuffer();
  if (buffer == 0) {
    PROFILER_UNLIKELY return;
  }

  std::lock_guard<std::mutex> lock(buffer->Mutex);

  if (!g_FrameActive.load(std::memory_order_acquire) ||
      g_FrameClosing.load(std::memory_order_acquire)) {
    PROFILER_UNLIKELY return;
  }

  SyncThreadGeneration(*buffer);

  if (buffer->Nodes.size() >= buffer->Nodes.capacity()) {
    PROFILER_UNLIKELY return;
  }

  if (buffer->ScopeStack.size() >= buffer->ScopeStack.capacity()) {
    PROFILER_UNLIKELY return;
  }

  const uint32_t nodeIndex = static_cast<uint32_t>(buffer->Nodes.size());
  const uint32_t parentIndex =
      buffer->ScopeStack.empty() ? INVALID_NODE : buffer->ScopeStack.back();

  ProfileNode node;
  node.Parent = parentIndex;
  node.FirstChild = INVALID_NODE;
  node.LastChild = INVALID_NODE;
  node.NextSibling = INVALID_NODE;
  node.StartTime = NowNs();
  node.EndTime = 0;
  node.Name = g_StringPool.Intern(name);
  node.ThreadId = buffer->ThreadId;

  buffer->Nodes.push_back(node);

  if (parentIndex != INVALID_NODE) {
    ProfileNode &parent = buffer->Nodes[parentIndex];

    if (parent.FirstChild == INVALID_NODE) {
      parent.FirstChild = nodeIndex;
      parent.LastChild = nodeIndex;
    } else {
      buffer->Nodes[parent.LastChild].NextSibling = nodeIndex;
      parent.LastChild = nodeIndex;
    }
  }

  buffer->ScopeStack.push_back(nodeIndex);
  activeToken.Release();
}

void ProfilerEndScope() noexcept {
  if (!g_Initialized.load(std::memory_order_acquire)) {
    return;
  }

  ThreadBuffer *buffer = GetThreadBuffer();
  if (buffer == 0) {
    return;
  }

  std::lock_guard<std::mutex> lock(buffer->Mutex);

  if (buffer->ScopeStack.empty()) {
    return;
  }

  const uint32_t nodeIndex = buffer->ScopeStack.back();
  buffer->ScopeStack.pop_back();

  if (nodeIndex < buffer->Nodes.size()) {
    buffer->Nodes[nodeIndex].EndTime = NowNs();
  }

  const uint32_t previous =
      g_ActiveScopes.fetch_sub(1, std::memory_order_release);
  if (previous == 1) {
    std::lock_guard<std::mutex> cvLock(g_ActiveScopesCvMutex);
    g_ActiveScopesCv.notify_all();
  }
}

Profiler::ProfileFrameView ProfilerGetLastFrame() noexcept {
  Profiler::ProfileFrameView view{};
  if (!g_Initialized.load(std::memory_order_acquire)) {
    return view;
  }

  const uint32_t readIndex = g_ReadFrameIndex.load(std::memory_order_acquire);
  const FrameStorage &readFrame = g_Frames[readIndex];

  if (readFrame.Nodes.empty()) {
    return view;
  }

  view.Nodes = readFrame.Nodes.data();
  view.NodeCount = static_cast<uint32_t>(readFrame.Nodes.size());
  view.FrameStart = readFrame.FrameStart;
  view.FrameEnd = readFrame.FrameEnd;
  return view;
}

} // extern "C"
} // namespace Profiler
