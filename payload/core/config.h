#pragma once

// Sin constructores: agregado con inicializadores por defecto, se puede
// rellenar entero con memset si hiciera falta.
struct Config {
    int  menuKey        = 0x2D;   // VK_INSERT
    bool loadOnStart    = true;
    bool overlayEnabled = true;
    bool debugConsole   = false;
    int  logLevel       = 1;

    // Interruptores de biseccion: permiten dejar el payload sin hooks de
    // render (overlay=0) o sin subclase de ventana (wndproc=0).
    bool autoOpen       = true;
    bool hookWndProc    = true;
    bool nativesEnabled = true;
};

namespace config {
Config& Get();
void    Load();
void    Save();
}