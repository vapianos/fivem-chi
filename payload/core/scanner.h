#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include "core/module.h"

namespace scanner {
uintptr_t Find(const char* pattern, uintptr_t start, size_t size);
uintptr_t FindIn(const ModuleInfo& module, const char* pattern);
std::vector<uintptr_t> FindAll(const ModuleInfo& module, const char* pattern, size_t maxHits = 32);

// Devuelve el resultado solo si el patrón es único. Sigue thunks antes de devolver.
uintptr_t FindUnique(const ModuleInfo& module, const char* pattern, size_t* hitCount);
// Resuelve saltos E9 / EB / FF25 / 48FF25 encadenados hasta el cuerpo real.
uintptr_t FollowJumps(uintptr_t addr);
// Comprueba que la dirección caiga en memoria comprometida y ejecutable.
bool      InExecutableSection(uintptr_t addr);

// Resuelve un rel32 (call/jmp) situado en addr+offsetPos con longitud total instrLen.
uintptr_t ResolveRel32(uintptr_t addr, int offsetPos, int instrLen);
// Lee un puntero absoluto de 8 bytes.
uintptr_t ReadPtr(uintptr_t addr);
}