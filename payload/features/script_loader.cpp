#include "features/script_loader.h"
#include "core/lua_runtime.h"
#include "utils/log.h"
#include "utils/paths.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#include <algorithm>

namespace fs = std::filesystem;

namespace scripts {

void load_all() {
    std::error_code ec;
    const fs::path dir = paths::LuaDir();
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) {
        LogE("carpeta de scripts inexistente: %s", paths::ToUtf8(dir).c_str());
        return;
    }

    std::vector<fs::path> files;
    for (auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        if (entry.path().extension() != L".lua") continue;
        files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());

    if (files.empty()) {
        Log("sin scripts .lua en %s", paths::ToUtf8(dir).c_str());
        return;
    }

    for (const fs::path& file : files) {
        std::ifstream in(file, std::ios::binary);
        if (!in.is_open()) {
            LogE("no se pudo abrir %s", paths::ToUtf8(file).c_str());
            continue;
        }
        std::stringstream ss;
        ss << in.rdbuf();
        const std::string name = file.filename().string();

        // Solo se encola. La ejecucion la hace el hook de luaL_loadbufferx, que
        // es el unico punto que corre en el hilo del juego: aqui (menu, boton)
        // se esta en el hilo del render.
        lua::PushPending(name, ss.str());
    }

    Log("%zu scripts encolados desde %s", files.size(), paths::ToUtf8(dir).c_str());
    if (!lua::ready()) Log("el runtime Lua aun no esta: se ejecutaran al capturar el estado");
}
} // namespace scripts