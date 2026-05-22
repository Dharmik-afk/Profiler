#pragma once

#include <stdint.h>

namespace Profiler {

static const uint32_t INVALID_NODE = UINT32_MAX;

struct Config {
  uint32_t MaxNodesPerFrame;
  uint32_t MaxScopeDepth;
  uint32_t MaxThreads;
};

struct ProfileNode {
  uint32_t Parent;
  uint32_t FirstChild;
  uint32_t LastChild;
  uint32_t NextSibling;

  uint64_t StartTime;
  uint64_t EndTime;

  const char *Name;
  uint32_t ThreadId;
};

struct ProfileFrameView {
  const ProfileNode *Nodes;
  uint32_t NodeCount;

  uint64_t FrameStart;
  uint64_t FrameEnd;
};

} // namespace Profiler
