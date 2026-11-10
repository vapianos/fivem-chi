#pragma once
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace paths {
// Sin std::filesystem: seguras para usar en el arranque.
std::wstring DataDirW();
std::wstring ConfigFileW();
std::wstring LogFileW();
bool         EnsureDirW(const std::wstring& dir);

fs::path    GameDir();
fs::path    DataDir();
fs::path    LuaDir();
fs::path    ConfigFile();
fs::path    LogFile();
bool        EnsureDir(const fs::path& dir);
std::string ToUtf8(const fs::path& p);
}