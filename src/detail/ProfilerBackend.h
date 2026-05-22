#pragma once

#include "ProfilerData.h"

extern "C" {
void ProfilerInit(const Profiler::Config *config) noexcept;
void ProfilerShutdown() noexcept;

void ProfilerBeginFrame() noexcept;
void ProfilerEndFrame() noexcept;

void ProfilerBeginScope(const char *name) noexcept;
void ProfilerEndScope() noexcept;

Profiler::ProfileFrameView ProfilerGetLastFrame() noexcept;
}
