# fivem-cheat

Cheat interno para FiveM basado en payload DLL + injector manual-map.

## Estructura
- `injector/` — inyector con GUI (manual map).
- `payload/` — DLL que se inyecta en el proceso del juego.
  - `core/`     hooks (MinHook), runtime Lua, scanner de patrones, config, módulos
  - `render/`   overlay D3D11 + ImGui y menú
  - `features/` cargador de scripts `.lua`
  - `utils/`    log y rutas
- `lua/` — scripts de usuario (se cargan desde disco).

## Build