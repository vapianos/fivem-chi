#pragma once
#include <windows.h>
#include <string>

namespace inj {
enum class Method { ManualMap, LoadLibrary };

DWORD FindProcess(const wchar_t* name);
DWORD FindGameProcess();

bool Inject(DWORD pid, const std::wstring& dll, Method m, std::string& err);
bool Eject(DWORD pid, std::string& err);
bool IsElevated();
}