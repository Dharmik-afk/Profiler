# Profiler

A small, built-in C++ profiler for timing nested scopes across a frame.  
**Ideal for game engines, real‑time simulations, or any frame‑based application where you need per‑frame timing trees without a full tracing suite.**

It records execution as a tree of scopes and exposes the completed frame data to the main application.

## Goals

- Simple API  
- Low runtime overhead  
- Nested scopes  
- Frame-based output  
- Multi-thread friendly  
- No GUI dependency  
- Portable C++ only  
- Safe to call across dynamic libraries when the profiler implementation lives in one module  

## Overview

The profiler works like this:

1. The application calls `Profiler::Init()` once at startup.  
2. Each frame begins with `Profiler::BeginFrame()`.  
3. Code paths are instrumented with `PROFILE_SCOPE()` or `PROFILE_SCOPE_NAMED("Name")`.  
4. Each scope records start and end time.  
5. Worker threads use their own thread-local buffers.  
6. At `Profiler::EndFrame()`, the profiler merges all thread buffers into one immutable frame snapshot.  
7. The main system reads the finished data with `Profiler::GetLastFrame()`.

## Public Files

- **Profiler.h** – Public instrumentation API. Include this anywhere you want to place profiling scopes.  
- **ProfilerData.h** – Read-only data access API. Use this in the main system or debugging code to inspect the last completed frame.  
- **Profiler.cpp** – Internal implementation. This should be compiled into exactly one module.

## Core API

```cpp
Profiler::Init(config);
Profiler::Shutdown();
Profiler::BeginFrame();
Profiler::EndFrame();
Profiler::BeginScope("Name");
Profiler::EndScope();
```

Most users should prefer the macro-based scope API:

```cpp
PROFILE_SCOPE();                     // uses __func__ as name
PROFILE_SCOPE_NAMED("Physics");
```

Data Model

The profiler stores execution as a tree of nodes.
Each node contains:

· parent index
· first child index
· next sibling index
· start time
· end time
· scope name
· thread id

The final frame is returned as a read-only view:

```cpp
struct ProfileFrameView
{
    const ProfileNode* Nodes;
    uint32_t NodeCount;
    uint64_t FrameStart;
    uint64_t FrameEnd;
};
```

Threading Model

The profiler avoids races by using thread-local recording buffers:

· Each thread records into its own local buffer.
· No thread writes directly into another thread’s profiling memory.
· Shared work is limited to thread registration and frame merge.
· EndFrame() collects the finished per-thread data into one snapshot.

This keeps the hot path simple and avoids shared vector writes during scope recording.

Important Rules

· Call Profiler::Init() once before profiling.
· Call Profiler::BeginFrame() and Profiler::EndFrame() around the frame boundary.
· Keep worker jobs for the frame finished before EndFrame().
· Scope names must remain valid for the lifetime of the recorded frame.
· Compile the profiler implementation into one owning module only.
· Do not expose STL-backed profiler data across dynamic library boundaries.

Build Integration

Add these files to your project:

· Profiler.h
· ProfilerData.h
· Profiler.cpp

Compile Profiler.cpp into the main executable or into one shared module that owns the profiler state.
Define PROFILER_BUILD only when building the module that exports the profiler functions.

Example Usage

Application

```cpp
#include "Profiler.h"
#include "ProfilerData.h"

class Application
{
public:
    void Run();

private:
    void Update();
    void Physics();
    void Render();
    void UI();
};

void Application::Run()
{
    Profiler::Config config;
    config.MaxNodesPerFrame = 4096;
    config.MaxScopeDepth = 128;
    config.MaxThreads = 8;

    Profiler::Init(config);

    while (true)
    {
        PROFILE_BEGIN_FRAME();

        {
            PROFILE_SCOPE_NAMED("Main Loop");
            Update();
            Render();
        }

        PROFILE_END_FRAME();

        Profiler::ProfileFrameView frame = Profiler::GetLastFrame();
        // Read frame.Nodes[0..frame.NodeCount-1]
    }

    Profiler::Shutdown();
}
```

Worker Job

```cpp
void DoWork()
{
    PROFILE_SCOPE_NAMED("Worker Job");
    // work
}
```

Example Output

A finished frame may contain data like this:

```
Main Loop
 ├── Update
 │    └── Physics
 └── Render
      ├── Shadow Pass
      └── UI
```

Notes on Portability

This profiler avoids compiler-specific function-name extensions and keeps the public ABI simple.
It uses:

· __func__ for automatic function names
· fixed-width integer types
· standard C++ containers internally
· std::chrono::steady_clock for timing
· thread_local for thread-local state

Current Limitations

· Frame-based, not a continuous trace system.
· Not a GUI profiler.
· Scope names are stored as pointers, so the caller must ensure valid lifetime.
· The merge step assumes jobs for the frame have finished before EndFrame().

Future Improvements

Possible next steps:

· string interning
· per-frame statistics
· min/max/average scope durations
· JSON export
· optional nested frame history
· better visualization support

License

MIT License
