#pragma once

#include "ProfilerData.h"

namespace Profiler {

void Init(const Config &config) noexcept;
void Shutdown() noexcept;

void BeginFrame() noexcept;
void EndFrame() noexcept;

ProfileFrameView GetLastFrame() noexcept;

} // namespace Profiler

#define PROFILE_BEGIN_FRAME() ::Profiler::BeginFrame()
#define PROFILE_END_FRAME() ::Profiler::EndFrame()
