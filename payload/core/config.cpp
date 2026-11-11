#include "core/config.h"
#include "utils/paths.h"
#include "utils/log.h"

#include <Windows.h>
#include <cstdlib>
#include <cstring>

namespace {

Config g_config;

// SRWLOCK_INIT es un inicializador constante: no hay constructor estatico ni
// funcion de init, y por tanto nada que pueda fallar al arrancar.
SRWLOCK g_lock = SRWLOCK_INIT;

struct Guard {
    Guard()  { AcquireSRWLockExclusive(&g_lock); }
    ~Guard() { ReleaseSRWLockExclusive(&g_lock); }
};

bool Same(const char* a, size_t n, const char* lit) {
    return strlen(lit) == n && !memcmp(a, lit, n);
}

void Apply(const char* key, size_t keyLen, const char* val, size_t valLen) {
    char tmp[32]{};
    const size_t c = valLen < 31 ? valLen : 31;
    memcpy(tmp, val, c);
    const long n = strtol(tmp, nullptr, 10);

    if (Same(key, keyLen, "menu_key"))            g_config.menuKey = (int)n;
    else if (Same(key, keyLen, "load_on_start"))  g_config.loadOnStart = n != 0;
    else if (Same(key, keyLen, "overlay"))        g_config.overlayEnabled = n != 0;
    else if (Same(key, keyLen, "debug_console"))  g_config.debugConsole = n != 0;
    else if (Same(key, keyLen, "log_level"))      g_config.logLevel = (int)n;
    else if (Same(key, keyLen, "auto_open"))      g_config.autoOpen = n != 0;
    else if (Same(key, keyLen, "wndproc"))        g_config.hookWndProc = n != 0;
}

// Se llama siempre con el lock tomado: nunca reentra en Save().
void SaveLocked() {
    const std::wstring dir = paths::DataDirW();
    paths::EnsureDirW(dir);
    const std::wstring file = dir + L"\\config.ini";

    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        LogE("no se pudo escribir la config");
        return;
    }

    char buf[512]{};
    const int n = wsprintfA(buf,
        "; fivem-cheat config\r\n"
        "menu_key=%d\r\n"
        "load_on_start=%d\r\n"
        "overlay=%d\r\n"
        "debug_console=%d\r\n"
        "log_level=%d\r\n"
        "auto_open=%d\r\n"
        "wndproc=%d\r\n",
        g_config.menuKey,
        g_config.loadOnStart ? 1 : 0,
        g_config.overlayEnabled ? 1 : 0,
        g_config.debugConsole ? 1 : 0,
        g_config.logLevel,
        g_config.autoOpen ? 1 : 0,
        g_config.hookWndProc ? 1 : 0);

    DWORD put = 0;
    WriteFile(h, buf, (DWORD)n, &put, nullptr);
    CloseHandle(h);
}
} // namespace

namespace config {

Config& Get() { return g_config; }

void Load() {
    Guard guard;

    const std::wstring dir = paths::DataDirW();
    paths::EnsureDirW(dir);
    const std::wstring file = dir + L"\\config.ini";

    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        SaveLocked();
        return;
    }

    char buf[8192]{};
    DWORD got = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(h);

    size_t i = 0;
    while (i < got) {
        size_t end = i;
        while (end < got && buf[end] != '\n') ++end;

        const char* line = buf + i;
        size_t len = end - i;

        // recorte
        while (len && (line[0] == ' ' || line[0] == '\t' || line[0] == '\r')) { ++line; --len; }
        while (len && (line[len - 1] == ' ' || line[len - 1] == '\t' || line[len - 1] == '\r')) --len;

        if (len && line[0] != ';' && line[0] != '#' && line[0] != '[') {
            const char* eq = static_cast<const char*>(memchr(line, '=', len));
            if (eq) {
                const char* key = line;
                size_t keyLen = static_cast<size_t>(eq - line);
                while (keyLen && (key[keyLen - 1] == ' ' || key[keyLen - 1] == '\t')) --keyLen;

                const char* val = eq + 1;
                // el valor arranca justo tras el '=', no tras la clave recortada
                size_t valLen = len - static_cast<size_t>(eq - line) - 1;
                while (valLen && (*val == ' ' || *val == '\t')) { ++val; --valLen; }

                if (keyLen) Apply(key, keyLen, val, valLen);
            }
        }

        i = end + 1;
    }

    Log("config cargada");
}

void Save() {
    Guard guard;
    SaveLocked();
}
} // namespace config