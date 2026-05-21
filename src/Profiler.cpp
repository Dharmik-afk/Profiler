#include "Profiler.h"
#include "ProfilerData.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

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

static uint32_t MinU32(uint32_t a, uint32_t b) noexcept {
  return (a < b) ? a : b;
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

static Config g_Config;
static bool g_Initialized = false;

static std::atomic<bool> g_FrameActive(false);
static std::atomic<bool> g_FrameClosing(false);
static std::atomic<uint32_t> g_CurrentGeneration(1);
static std::atomic<uint32_t> g_ActiveScopes(0);
static std::atomic<uint32_t> g_NextThreadId(1);

static uint64_t g_CurrentFrameStartNs = 0;

static std::mutex g_RegistryMutex;
static std::vector<std::unique_ptr<ThreadBuffer>> g_ThreadBuffers;

static FrameStorage g_Frames[2];
static uint32_t g_ReadFrameIndex = 0;
static uint32_t g_WriteFrameIndex = 1;

thread_local ThreadBuffer *g_LocalBuffer = 0;

static ThreadBuffer *CreateThreadBuffer() noexcept {
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
  uint32_t generation = g_CurrentGeneration.load(std::memory_order_acquire);

  if (buffer.Generation != generation) {
    buffer.Nodes.clear();
    buffer.ScopeStack.clear();
    buffer.Generation = generation;
  }
}

static void MergeThreadBufferIntoFrame(const ThreadBuffer &buffer,
                                       FrameStorage &frame) {
  if (buffer.Generation !=
      g_CurrentGeneration.load(std::memory_order_acquire)) {
    return;
  }

  const uint32_t oldSize = static_cast<uint32_t>(frame.Nodes.size());
  const uint32_t addCount = static_cast<uint32_t>(buffer.Nodes.size());

  if (addCount == 0) {
    return;
  }

  frame.Nodes.insert(frame.Nodes.end(), buffer.Nodes.begin(),
                     buffer.Nodes.end());

  const uint32_t newSize = oldSize + addCount;

  for (uint32_t i = oldSize; i < newSize; ++i) {
    ProfileNode &node = frame.Nodes[i];

    node.Parent = RemapIndex(node.Parent, oldSize);
    node.FirstChild = RemapIndex(node.FirstChild, oldSize);
    node.NextSibling = RemapIndex(node.NextSibling, oldSize);
  }
}

static void WaitForScopesToDrain() noexcept {
  while (g_ActiveScopes.load(std::memory_order_acquire) != 0) {
    std::this_thread::yield();
  }
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

void Init(const Config &config) noexcept {
  Shutdown();

  g_Config = config;

  if (g_Config.MaxNodesPerFrame == 0) {
    g_Config.MaxNodesPerFrame = 4096;
  }

  if (g_Config.MaxScopeDepth == 0) {
    g_Config.MaxScopeDepth = 128;
  }

  if (g_Config.MaxThreads == 0) {
    g_Config.MaxThreads = 8;
  }

  g_ThreadBuffers.reserve(g_Config.MaxThreads);

  g_Frames[0].Nodes.reserve(g_Config.MaxNodesPerFrame);
  g_Frames[1].Nodes.reserve(g_Config.MaxNodesPerFrame);

  g_CurrentGeneration.store(1, std::memory_order_release);
  g_CurrentFrameStartNs = 0;

  g_FrameActive.store(false, std::memory_order_release);
  g_FrameClosing.store(false, std::memory_order_release);
  g_ActiveScopes.store(0, std::memory_order_release);

  g_ReadFrameIndex = 0;
  g_WriteFrameIndex = 1;

  g_Initialized = true;
}

void Shutdown() noexcept {
  g_FrameActive.store(false, std::memory_order_release);
  g_FrameClosing.store(false, std::memory_order_release);
  g_ActiveScopes.store(0, std::memory_order_release);

  {
    std::lock_guard<std::mutex> lock(g_RegistryMutex);
    g_ThreadBuffers.clear();
  }

  g_Frames[0].Nodes.clear();
  g_Frames[1].Nodes.clear();
  g_Frames[0].FrameStart = 0;
  g_Frames[0].FrameEnd = 0;
  g_Frames[1].FrameStart = 0;
  g_Frames[1].FrameEnd = 0;

  g_LocalBuffer = 0;
  g_CurrentGeneration.store(1, std::memory_order_release);
  g_CurrentFrameStartNs = 0;
  g_ReadFrameIndex = 0;
  g_WriteFrameIndex = 1;

  g_Initialized = false;
}

void BeginFrame() noexcept {
  if (!g_Initialized) {
    return;
  }

  SetFrameActive();
  g_CurrentFrameStartNs = NowNs();
}

void EndFrame() noexcept {
  if (!g_Initialized) {
    return;
  }

  if (!g_FrameActive.load(std::memory_order_acquire)) {
    return;
  }

  SetFrameInactive();
  WaitForScopesToDrain();

  FrameStorage &writeFrame = g_Frames[g_WriteFrameIndex];
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

  std::swap(g_ReadFrameIndex, g_WriteFrameIndex);
  g_CurrentGeneration.fetch_add(1, std::memory_order_release);

  FrameStorage &readFrame = g_Frames[g_ReadFrameIndex];
  readFrame.FrameStart = writeFrame.FrameStart;
  readFrame.FrameEnd = writeFrame.FrameEnd;
}

void BeginScope(const char *name) noexcept {
  if (!g_Initialized) {
    return;
  }

  if (!g_FrameActive.load(std::memory_order_acquire) ||
      g_FrameClosing.load(std::memory_order_acquire)) {
    return;
  }

  ThreadBuffer *buffer = GetThreadBuffer();
  if (buffer == 0) {
    return;
  }

  std::lock_guard<std::mutex> lock(buffer->Mutex);

  if (!g_FrameActive.load(std::memory_order_acquire) ||
      g_FrameClosing.load(std::memory_order_acquire)) {
    return;
  }

  SyncThreadGeneration(*buffer);

  if (buffer->Nodes.size() >= buffer->Nodes.capacity()) {
    return;
  }

  ProfileNode node;
  node.Parent = INVALID_NODE;
  node.FirstChild = INVALID_NODE;
  node.NextSibling = INVALID_NODE;
  node.StartTime = NowNs();
  node.EndTime = 0;
  node.Name = (name != 0) ? name : "";
  node.ThreadId = buffer->ThreadId;

  if (!buffer->ScopeStack.empty()) {
    node.Parent = buffer->ScopeStack.back();
  }

  const uint32_t nodeIndex = static_cast<uint32_t>(buffer->Nodes.size());
  buffer->Nodes.push_back(node);

  if (node.Parent != INVALID_NODE) {
    ProfileNode &parent = buffer->Nodes[node.Parent];

    if (parent.FirstChild == INVALID_NODE) {
      parent.FirstChild = nodeIndex;
    } else {
      uint32_t sibling = parent.FirstChild;
      while (buffer->Nodes[sibling].NextSibling != INVALID_NODE) {
        sibling = buffer->Nodes[sibling].NextSibling;
      }

      buffer->Nodes[sibling].NextSibling = nodeIndex;
    }
  }

  buffer->ScopeStack.push_back(nodeIndex);
  g_ActiveScopes.fetch_add(1, std::memory_order_acq_rel);
}

void EndScope() noexcept {
  if (!g_Initialized) {
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

  g_ActiveScopes.fetch_sub(1, std::memory_order_acq_rel);
}

ProfileFrameView GetLastFrame() noexcept {
  ProfileFrameView view;
  view.Nodes = 0;
  view.NodeCount = 0;
  view.FrameStart = 0;
  view.FrameEnd = 0;

  if (!g_Initialized) {
    return view;
  }

  const FrameStorage &readFrame = g_Frames[g_ReadFrameIndex];

  if (!readFrame.Nodes.empty()) {
    view.Nodes = &readFrame.Nodes[0];
    view.NodeCount = static_cast<uint32_t>(readFrame.Nodes.size());
    view.FrameStart = readFrame.FrameStart;
    view.FrameEnd = readFrame.FrameEnd;
  }

  return view;
}
} // namespace Profiler
