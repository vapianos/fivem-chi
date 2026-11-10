#pragma once

namespace native {

bool ready();
bool usingMatrix();

bool esp();
bool box();
bool aim();
bool god();
bool jump();
bool skel();
float fov();

void setEsp(bool v);
void setBox(bool v);
void setAim(bool v);
void setGod(bool v);
void setJump(bool v);
void setSkel(bool v);
void setFov(float v);

bool init();
void shutdown();
void tick();
void render();
void renderMenu();

} // namespace native