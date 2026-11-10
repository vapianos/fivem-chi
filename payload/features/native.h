#pragma once

namespace native {
bool init();
void shutdown();
bool ready();

void tick();
void render();
void renderMenu();

bool  esp();      void setEsp(bool v);
bool  box();      void setBox(bool v);
bool  aim();      void setAim(bool v);
bool  god();      void setGod(bool v);
bool  jump();     void setJump(bool v);
float fov();      void setFov(float v);

// true si el ESP esta proyectando por matriz en vez de por native.
bool usingMatrix();
}