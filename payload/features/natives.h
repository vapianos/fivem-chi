#pragma once

#include <cstdint>

namespace natives {

// Resuelve la tabla de registro de nativas. Idempotente y barato: solo mira
// gta-core-five.dll y el ejecutable, nunca el proceso entero.
bool init();
bool ready();
const char* statusText();

// Ejecuta una nativa. ret/args en bytes crudos, 8 por argumento, ret alineado
// a 16. Devuelve false si el hash no esta en la tabla.
bool call(uint64_t hash, void* ret, int argCount, const uint64_t* args);

// Helpers tipados (todos comprueban que la nativa exista).
bool    has(uint64_t hash);
void    v_void(uint64_t hash, int argc, const uint64_t* args);
int64_t v_int(uint64_t hash, int argc, const uint64_t* args);

} // namespace natives