#pragma once

class Application {
public:
  Application();

  void Run();

private:
  void Update();
  void Physics();
  void Render();
  void UI();

  bool m_Running;
};
