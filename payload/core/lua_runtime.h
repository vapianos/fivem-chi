#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

struct lua_State;

namespace lua {

bool init();
void shutdown();
bool ready();
lua_State* state();

void RegisterApi(lua_State* L);
void OnLoadBuffer(lua_State* L);

bool RunString(const std::string& code, const std::string& chunkName);
bool RunFile(const std::filesystem::path& file);

void PushPending(const std::string& name, std::string code);
void FlushPending();
std::size_t PendingCount();

// Anade una funcion C al campo `name` de la tabla que este en la cima.
void AddField(lua_State* L, const char* name, int (*fn)(lua_State*));
const char* GetStringArg(lua_State* L, int idx, std::size_t& len);
void PushInteger(lua_State* L, long long v);

} // namespace lua