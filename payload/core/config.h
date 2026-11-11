#pragma once

struct Config {
    int  menuKey        = 0x2D;   // VK_INSERT
    bool loadOnStart    = true;
    bool overlayEnabled = true;
    bool debugConsole   = false;
    int  logLevel       = 1;

    bool autoOpen       = true;
    bool hookWndProc    = true;   // sin esto el menu no recibe clics
    bool nativesEnabled = true;
};

namespace config {
Config& Get();
void    Load();
void    Save();
}