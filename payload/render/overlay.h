#pragma once

namespace overlay {
bool init();
void shutdown();
bool visible();
void setVisible(bool state);
void toggle();
bool armed();
}