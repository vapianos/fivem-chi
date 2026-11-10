#include "utils/paths.h"
#include <Windows.h>
#include <Psapi.h>

namespace {
std::wstring Env(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2]{};
    const DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH * 2);
    return (n && n < MAX_PATH * 2) ? std::wstring(buf, n) : std::wstring();
}

std::wstring StripFile(const std::wstring& p) {
    const auto pos = p.find_last_of(L"\\/");
    return pos == std::wstring::npos ? p : p.substr(0, pos);
}

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return StripFile(buf);
}

// \Device\HarddiskVolumeN\... -> X:\...
std::wstring DosPath(const std::wstring& nt) {
    wchar_t drives[512]{};
    const DWORD n = GetLogicalDriveStringsW(511, drives);
    for (DWORD i = 0; i < n; i += (DWORD)wcslen(drives + i) + 1) {
        wchar_t dev[MAX_PATH]{};
        if (!QueryDosDeviceW(drives + i, dev, MAX_PATH)) continue;
        const size_t dl = wcslen(dev);
        if (dl && _wcsnicmp(nt.c_str(), dev, dl) == 0) {
            std::wstring out = drives + i;
            if (!out.empty()) out.pop_back();
            out += nt.substr(dl);
            return out;
        }
    }
    return L"";
}

// Bajo manual map el DLL no esta en la lista de modulos, pero la vista si esta
// respaldada por un fichero: GetMappedFileNameW lo devuelve.
std::wstring SelfDir() {
    HMODULE self = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&SelfDir), &self) && self) {
        wchar_t buf[MAX_PATH]{};
        if (GetModuleFileNameW(self, buf, MAX_PATH)) return StripFile(buf);
    }

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(&SelfDir), &mbi, sizeof(mbi)) && mbi.Type == MEM_IMAGE) {
        wchar_t buf[MAX_PATH]{};
        if (GetMappedFileNameW(GetCurrentProcess(), mbi.AllocationBase, buf, MAX_PATH)) {
            const std::wstring dos = DosPath(buf);
            if (!dos.empty()) return StripFile(dos);
        }
    }
    return L"";
}

std::wstring ReadText(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"";

    char raw[1024]{};
    DWORD got = 0;
    ReadFile(h, raw, sizeof(raw) - 1, &got, nullptr);
    CloseHandle(h);
    if (!got) return L"";

    const int n = MultiByteToWideChar(CP_UTF8, 0, raw, (int)got, nullptr, 0);
    if (n <= 0) return L"";
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, raw, (int)got, &w[0], n);
    while (!w.empty() && (w.back() == L'\\' || w.back() == L'\r' || w.back() == L'\n')) w.pop_back();
    return w;
}

std::wstring MarkerDir() {
    const std::wstring appdata = Env(L"LOCALAPPDATA");
    if (appdata.empty()) return L"";
    std::wstring d = ReadText(appdata + L"\\FiveMCheat\\dir.txt");
    if (d.empty()) d = ReadText(appdata + L"\\VeniceFiveM\\dir.txt");
    return d;
}
} // namespace

namespace paths {

std::wstring DataDirW() {
    static std::wstring cached = [] {
        std::wstring up = Env(L"USERPROFILE");
        if (!up.empty()) return up + L"\\Documents\\VeniceFiveM";
        std::wstring tmp = Env(L"TEMP");
        if (tmp.empty()) tmp = L".";
        return tmp + L"\\VeniceFiveM";
    }();
    return cached;
}

std::wstring ConfigFileW() { return DataDirW() + L"\\config.ini"; }
std::wstring LogFileW()    { return DataDirW() + L"\\payload.log"; }

bool EnsureDirW(const std::wstring& dir) {
    if (CreateDirectoryW(dir.c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

fs::path GameDir() {
    static fs::path cached = [] {
        std::wstring marker = MarkerDir();
        if (!marker.empty()) return fs::path(marker);
        std::wstring self = SelfDir();
        if (!self.empty()) return fs::path(self);
        return fs::path(ExeDir());
    }();
    return cached;
}

fs::path DataDir() { return fs::path(DataDirW()); }

fs::path LuaDir() {
    static fs::path cached = [] {
        std::error_code ec;
        fs::path local = GameDir() / L"lua";
        if (fs::exists(local, ec)) return local;
        return fs::path(DataDirW() + L"\\lua");
    }();
    return cached;
}

fs::path ConfigFile() { return fs::path(ConfigFileW()); }
fs::path LogFile()    { return fs::path(LogFileW()); }

bool EnsureDir(const fs::path& dir) { return EnsureDirW(dir.wstring()); }

std::string ToUtf8(const fs::path& p) {
    const std::wstring w = p.wstring();
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &out[0], n, nullptr, nullptr);
    return out;
}
} // namespace paths