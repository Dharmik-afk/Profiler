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
struct Config {
  uint32_t MaxNodesPerFrame;
  uint32_t MaxScopeDepth;
  uint32_t MaxThreads;
};

PROFILER_API void Init(const Config &config) noexcept;
PROFILER_API void Shutdown() noexcept;

PROFILER_API void BeginFrame() noexcept;
PROFILER_API void EndFrame() noexcept;

PROFILER_API void BeginScope(const char *name) noexcept;
PROFILER_API void EndScope() noexcept;

class Scope {
public:
  explicit Scope(const char *name) noexcept { BeginScope(name); }

  ~Scope() noexcept { EndScope(); }

  Scope(const Scope &) = delete;
  Scope &operator=(const Scope &) = delete;
};
} // namespace Profiler

#define PROFILER_CONCAT_INTERNAL(x, y) x##y
#define PROFILER_CONCAT(x, y) PROFILER_CONCAT_INTERNAL(x, y)

#define PROFILE_SCOPE()                                                        \
  Profiler::Scope PROFILER_CONCAT(_profilerScope_, __LINE__)(__func__)

#define PROFILE_SCOPE_NAMED(name)                                              \
  Profiler::Scope PROFILER_CONCAT(_profilerScope_, __LINE__)(name)

#define PROFILE_BEGIN_FRAME() Profiler::BeginFrame()
#define PROFILE_END_FRAME() Profiler::EndFrame()
