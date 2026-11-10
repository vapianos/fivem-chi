#include "features/script_loader.h"
#include "core/lua_runtime.h"
#include "utils/log.h"
#include "utils/paths.h"

#include <fstream>
#include <sstream>
#include <filesystem>

namespace fs = std::filesystem;

namespace scripts {

std::vector<std::string> list() {
    std::vector<std::string> out;
    std::error_code ec;

    if (!fs::exists(paths::LuaDir(), ec)) {
        paths::EnsureDir(paths::LuaDir());
        return out;
    }

    for (const auto& entry : fs::directory_iterator(paths::LuaDir(), ec)) {
        if (!entry.is_regular_file(ec)) continue;
        if (entry.path().extension() != L".lua") continue;
        out.push_back(entry.path().filename().string());
    }

    std::sort(out.begin(), out.end());
    return out;
}

bool run(const std::string& fileName) {
    const fs::path full = paths::LuaDir() / fileName;
    if (lua::ready())
        return lua::RunFile(full);

    std::ifstream in(full, std::ios::binary);
    if (!in.is_open()) {
        LogE("no se pudo abrir %s", fileName.c_str());
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    lua::PushPending(fileName, ss.str());
    Log("script en cola: %s", fileName.c_str());
    return true;
}

void load_all() {
    paths::EnsureDir(paths::LuaDir());

    const auto files = list();
    if (files.empty()) {
        Log("no hay scripts en %s", paths::ToUtf8(paths::LuaDir()).c_str());
        return;
    }

    for (const auto& file : files)
        run(file);

    if (lua::ready())
        lua::FlushPending();

    Log("%zu scripts procesados", files.size());
}

size_t pending() { return lua::PendingCount(); }
} // namespace scripts