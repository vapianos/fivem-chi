#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <filesystem>

struct lua_State;

namespace lua {

bool  init();
void  shutdown();
bool  ready();
lua_State* state();

// Direccion de luaL_loadbufferx ya resuelta (nullptr si no). La usa el hook:
// la resolucion de firmas y el enganche van separados a proposito.
void* target();

// Lee lua_gettop sobre un L recien llegado: sirve para validar que la firma
// apunta a la funcion correcta antes de fiarse de ella.
int probeTop(lua_State* L);

void AddField(lua_State* L, const char* name, int (*fn)(lua_State*));
const char* GetStringArg(lua_State* L, int idx, size_t& len);
void PushInteger(lua_State* L, long long v);

void RegisterApi(lua_State* L);
void OnLoadBuffer(lua_State* L);

bool RunString(const std::string& code, const std::string& chunkName);
bool RunFile(const std::filesystem::path& file);

void PushPending(const std::string& name, std::string code);
void FlushPending();
size_t PendingCount();

} // namespace lua