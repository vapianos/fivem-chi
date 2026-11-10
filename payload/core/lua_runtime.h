#pragma once
#include <string>
#include <filesystem>

struct lua_State;

namespace lua {
bool init();
void shutdown();
bool ready();
lua_State* state();

void OnLoadBuffer(lua_State* L);
void RegisterApi(lua_State* L);

bool RunString(const std::string& code, const std::string& chunkName);
bool RunFile(const std::filesystem::path& file);

void PushPending(const std::string& name, std::string code);
void FlushPending();
size_t PendingCount();
}