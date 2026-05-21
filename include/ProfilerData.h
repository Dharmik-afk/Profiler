#pragma once

#include <stdint.h>

#if defined(_WIN32)
#if defined(PROFILER_BUILD)
#define PROFILER_API __declspec(dllexport)
#else
#define PROFILER_API __declspec(dllimport)
#endif
#else
#define PROFILER_API
#endif

namespace Profiler {
static const uint32_t INVALID_NODE = 0xffffffffu;

struct ProfileNode {
  uint32_t Parent;
  uint32_t FirstChild;
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

PROFILER_API ProfileFrameView GetLastFrame() noexcept;
} // namespace Profiler
