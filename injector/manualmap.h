#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace manualmap {
// Mapea el DLL en `proc` como imagen respaldada por seccion (MEM_IMAGE), igual
// que haria el loader, y arranca el CRT + DllMain con los argumentos correctos.
// Devuelve true si el mapeo y la inicializacion fueron bien.
bool Map(HANDLE proc, const std::vector<uint8_t>& dll, std::string& err);

// Igual, pero devuelve tambien la base mapeada (para logs del inyector).
bool MapEx(HANDLE proc, const std::vector<uint8_t>& dll, std::string& err, uintptr_t* baseOut);
}