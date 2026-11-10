@echo off
where cmake >nul 2>nul || (echo Instala CMake y marca "Add to PATH" & pause & exit /b)
where git   >nul 2>nul || (echo Instala Git & pause & exit /b)
cmake -B build -A x64
cmake --build build --config Release
echo.
echo Listo. Ejecutable en build\Release\launcher.exe
pause