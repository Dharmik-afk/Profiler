#include "Application.h"

#include "Profiler.h"
#include "ProfilerData.h"

#include <chrono>
#include <stdio.h>
#include <thread>

Application::Application() : m_Running(true) {}

void Application::Run() {
  Profiler::Config config;

  config.MaxNodesPerFrame = 4096;
  config.MaxScopeDepth = 128;
  config.MaxThreads = 8;

  Profiler::Init(config);

  while (m_Running) {
    PROFILE_BEGIN_FRAME();

    {
      PROFILE_SCOPE_NAMED("Main Loop");

      Update();
      Render();
    }

    PROFILE_END_FRAME();

    // --------------------------------------------------------
    // Read Frame Data
    // --------------------------------------------------------

    Profiler::ProfileFrameView frame = Profiler::GetLastFrame();

    printf("\n========== FRAME ==========\n");

    printf("Node Count: %u\n", frame.NodeCount);

    uint64_t frameDuration = frame.FrameEnd - frame.FrameStart;

    printf("Frame Time: %.3f ms\n", (double)frameDuration / 1000000.0);

    for (uint32_t i = 0; i < frame.NodeCount; ++i) {
      const Profiler::ProfileNode &node = frame.Nodes[i];

      uint64_t duration = node.EndTime - node.StartTime;

      printf("[Thread %u] %s -> %.3f ms\n", node.ThreadId, node.Name,
             (double)duration / 1000000.0);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(16));

    static int frameCounter = 0;

    ++frameCounter;

    if (frameCounter > 5) {
      m_Running = false;
    }
  }

  Profiler::Shutdown();
}

void Application::Update() {
  PROFILE_SCOPE();

  Physics();

  std::this_thread::sleep_for(std::chrono::milliseconds(2));
}

void Application::Physics() {
  PROFILE_SCOPE();

  std::thread workerA([]() {
    PROFILE_SCOPE_NAMED("Worker A");

    std::this_thread::sleep_for(std::chrono::milliseconds(3));
  });

  std::thread workerB([]() {
    PROFILE_SCOPE_NAMED("Worker B");

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  });

  workerA.join();
  workerB.join();
}

void Application::Render() {
  PROFILE_SCOPE();

  {
    PROFILE_SCOPE_NAMED("Shadow Pass");

    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  UI();
}

void Application::UI() {
  PROFILE_SCOPE();

  std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
