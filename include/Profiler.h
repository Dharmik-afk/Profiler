#pragma once

namespace Profiler {

void BeginScope(const char *name) noexcept;
void EndScope() noexcept;

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
