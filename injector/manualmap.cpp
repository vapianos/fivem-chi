#include "manualmap.h"
#include <winternl.h>
#include <tlhelp32.h>
#include <cstdio>

#pragma comment(lib, "ntdll.lib")

namespace {

using NtAlloc_t     = NTSTATUS(NTAPI*)(HANDLE, PVOID*, ULONG_PTR, PSIZE_T, ULONG, ULONG);
using NtFree_t      = NTSTATUS(NTAPI*)(HANDLE, PVOID*, PSIZE_T, ULONG);
using NtRead_t      = NTSTATUS(NTAPI*)(HANDLE, PVOID, PVOID, SIZE_T, PSIZE_T);
using NtWrite_t     = NTSTATUS(NTAPI*)(HANDLE, PVOID, PVOID, SIZE_T, PSIZE_T);
using NtProt_t      = NTSTATUS(NTAPI*)(HANDLE, PVOID*, PSIZE_T, ULONG, PULONG);
using NtThread_t    = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, PVOID, HANDLE, PVOID, PVOID,
                                       ULONG, SIZE_T, SIZE_T, SIZE_T, PVOID);
using NtCreateSec_t = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PLARGE_INTEGER,
                                       ULONG, ULONG, HANDLE);
using NtMapView_t   = NTSTATUS(NTAPI*)(HANDLE, HANDLE, PVOID*, ULONG_PTR, SIZE_T, PLARGE_INTEGER,
                                       PSIZE_T, ULONG, ULONG, ULONG);

NtAlloc_t     pAlloc;
NtFree_t      pFree;
NtRead_t      pRead;
NtWrite_t     pWrite;
NtProt_t      pProt;
NtThread_t    pThread;
NtCreateSec_t pCreateSec;
NtMapView_t   pMapView;

bool Resolve() {
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    if (!nt) return false;
    pAlloc     = (NtAlloc_t)GetProcAddress(nt, "NtAllocateVirtualMemory");
    pFree      = (NtFree_t)GetProcAddress(nt, "NtFreeVirtualMemory");
    pRead      = (NtRead_t)GetProcAddress(nt, "NtReadVirtualMemory");
    pWrite     = (NtWrite_t)GetProcAddress(nt, "NtWriteVirtualMemory");
    pProt      = (NtProt_t)GetProcAddress(nt, "NtProtectVirtualMemory");
    pThread    = (NtThread_t)GetProcAddress(nt, "NtCreateThreadEx");
    pCreateSec = (NtCreateSec_t)GetProcAddress(nt, "NtCreateSection");
    pMapView   = (NtMapView_t)GetProcAddress(nt, "NtMapViewOfSection");
    return pAlloc && pFree && pRead && pWrite && pProt && pThread && pCreateSec && pMapView;
}

bool RemoteRead(HANDLE proc, uintptr_t at, void* dst, size_t len) {
    SIZE_T got = 0;
    return pRead(proc, (PVOID)at, dst, len, &got) == 0 && got == len;
}

bool RemoteWrite(HANDLE proc, uintptr_t at, const void* src, size_t len) {
    SIZE_T put = 0;
    return pWrite(proc, (PVOID)at, (PVOID)src, len, &put) == 0 && put == len;
}

bool RemoteProtect(HANDLE proc, uintptr_t at, size_t len, DWORD prot) {
    void* addr = (void*)at;
    SIZE_T sz = len;
    ULONG old = 0;
    return pProt(proc, &addr, &sz, prot, &old) == 0;
}

// --- resolucion de exports contra el proceso destino -----------------------

struct ModEntry {
    std::wstring name;
    uintptr_t    base;
};

std::vector<ModEntry> RemoteModules(DWORD pid) {
    std::vector<ModEntry> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return out;
    MODULEENTRY32W me{ sizeof(me) };
    if (Module32FirstW(snap, &me)) {
        do {
            out.push_back({ me.szModule, (uintptr_t)me.modBaseAddr });
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return out;
}

uintptr_t RemoteModuleBase(const std::vector<ModEntry>& mods, const char* name) {
    std::wstring w(name, name + strlen(name));
    for (auto& m : mods) {
        if (m.name.size() == w.size() && _wcsicmp(m.name.c_str(), w.c_str()) == 0)
            return m.base;
    }
    return 0;
}

uintptr_t RemoteExport(HANDLE proc, uintptr_t modBase, const char* name, WORD ord, bool byOrd) {
    if (!modBase) return 0;

    IMAGE_DOS_HEADER dos{};
    if (!RemoteRead(proc, modBase, &dos, sizeof(dos))) return 0;
    if (dos.e_magic != IMAGE_DOS_SIGNATURE) return 0;

    IMAGE_NT_HEADERS64 nt{};
    if (!RemoteRead(proc, modBase + dos.e_lfanew, &nt, sizeof(nt))) return 0;
    if (nt.Signature != IMAGE_NT_SIGNATURE) return 0;

    const auto& dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress || !dir.Size) return 0;

    IMAGE_EXPORT_DIRECTORY exp{};
    if (!RemoteRead(proc, modBase + dir.VirtualAddress, &exp, sizeof(exp))) return 0;
    if (!exp.NumberOfFunctions) return 0;

    DWORD funcRva = 0;
    if (byOrd) {
        if (ord < exp.Base || ord >= exp.Base + exp.NumberOfFunctions) return 0;
        DWORD rva = 0;
        if (!RemoteRead(proc, modBase + exp.AddressOfFunctions + (ord - exp.Base) * 4, &rva, 4)) return 0;
        funcRva = rva;
    } else {
        for (DWORD i = 0; i < exp.NumberOfNames; ++i) {
            DWORD nameRva = 0;
            if (!RemoteRead(proc, modBase + exp.AddressOfNames + i * 4, &nameRva, 4)) continue;
            char buf[256]{};
            if (!RemoteRead(proc, modBase + nameRva, buf, sizeof(buf) - 1)) continue;
            if (strcmp(buf, name) != 0) continue;
            WORD idx = 0;
            if (!RemoteRead(proc, modBase + exp.AddressOfNameOrdinals + i * 2, &idx, 2)) return 0;
            DWORD rva = 0;
            if (!RemoteRead(proc, modBase + exp.AddressOfFunctions + idx * 4, &rva, 4)) return 0;
            funcRva = rva;
            break;
        }
    }
    if (!funcRva) return 0;

    if (funcRva >= dir.VirtualAddress && funcRva < dir.VirtualAddress + dir.Size) {
        char fwd[256]{};
        if (!RemoteRead(proc, modBase + funcRva, fwd, sizeof(fwd) - 1)) return 0;
        char* dot = strchr(fwd, '.');
        if (!dot) return 0;
        *dot = '\0';
        std::string target = std::string(fwd) + ".dll";
        return RemoteExport(proc, RemoteModuleBase(RemoteModules(GetProcessId(proc)), target.c_str()),
                            dot + 1, 0, false);
    }

    return modBase + funcRva;
}

// Un modulo que no esta cargado en el destino no se puede resolver con
// GetProcAddress del inyector: esa direccion vive en NUESTRO espacio, no en el
// suyo. Hay que forzar la carga alli y resolver contra su copia.
uintptr_t RemoteLoadModule(HANDLE proc, DWORD pid, const char* name) {
    auto mods = RemoteModules(pid);
    if (uintptr_t already = RemoteModuleBase(mods, name)) return already;

    SIZE_T len = strlen(name) + 1;
    void* mem = nullptr;
    if (pAlloc(proc, &mem, 0, &len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) != 0 || !mem) return 0;
    if (!RemoteWrite(proc, (uintptr_t)mem, name, len)) return 0;

    uintptr_t loadAddr = RemoteExport(proc, RemoteModuleBase(mods, "kernel32.dll"),
                                      "LoadLibraryA", 0, false);
    if (!loadAddr)
        loadAddr = RemoteExport(proc, RemoteModuleBase(mods, "kernelbase.dll"),
                                "LoadLibraryA", 0, false);
    if (!loadAddr) return 0;

    HANDLE th = nullptr;
    if (pThread(&th, 0x1FFFFF, nullptr, proc, (void*)loadAddr, mem, 0, 0, 0, 0, nullptr) != 0 || !th)
        return 0;
    WaitForSingleObject(th, 5000);
    CloseHandle(th);

    return RemoteModuleBase(RemoteModules(pid), name);
}

// --- stub de arranque ------------------------------------------------------

// Convencion x64: RSP alineada a 16 justo antes de cada `call`. Al entrar en un
// hilo RSP % 16 == 8, asi que tras `push rbp` (=> 0) solo se reserva un multiplo
// de 16 y la alineacion se mantiene en 0.
//
// rcx = base (lpParameter del hilo).
// 1) callbacks TLS con reason = DLL_PROCESS_ATTACH
// 2) RtlAddFunctionTable(base + pdata, size, base)  -> registra unwind
// 3) DllMain(base, DLL_PROCESS_ATTACH, 0) y guarda su retorno en `scratch`
std::vector<uint8_t> BuildStub(uintptr_t base, uintptr_t entry, uintptr_t addTable,
                               uint32_t pdataRva, uint32_t pdataSize,
                               uint32_t tlsOff, bool hasTls, uintptr_t scratch) {
    std::vector<uint8_t> c;
    c.reserve(0x100);
    auto u8  = [&](uint8_t v) { c.push_back(v); };
    auto u32 = [&](uint32_t v) { uint8_t b[4]; memcpy(b, &v, 4); c.insert(c.end(), b, b + 4); };
    auto u64 = [&](uint64_t v) { uint8_t b[8]; memcpy(b, &v, 8); c.insert(c.end(), b, b + 8); };

    u8(0x55);                                             // push rbp
    u8(0x48); u8(0x89); u8(0xE5);                         // mov rbp, rsp
    u8(0x48); u8(0x83); u8(0xEC); u8(0x30);               // sub rsp, 0x30
    u8(0x48); u8(0x89); u8(0x4C); u8(0x24); u8(0x20);     // mov [rsp+20h], rcx

    if (hasTls) {
        u8(0x48); u8(0x8B); u8(0x81); u32(tlsOff);        // mov rax, [rcx+tlsOff]
    } else {
        u8(0xB8); u32(0); u8(0x90); u8(0x90);             // mov eax, 0
    }
    u8(0x48); u8(0x89); u8(0x44); u8(0x24); u8(0x28);     // mov [rsp+28h], rax

    const size_t loop = c.size();
    u8(0x48); u8(0x8B); u8(0x44); u8(0x24); u8(0x28);     // mov rax, [rsp+28h]
    u8(0x48); u8(0x85); u8(0xC0);                         // test rax, rax
    u8(0x74); const size_t j1 = c.size(); u8(0x00);       // jz done
    u8(0x48); u8(0x8B); u8(0x00);                         // mov rax, [rax]
    u8(0x48); u8(0x85); u8(0xC0);                         // test rax, rax
    u8(0x74); const size_t j2 = c.size(); u8(0x00);       // jz done
    u8(0x48); u8(0x8B); u8(0x4C); u8(0x24); u8(0x20);     // mov rcx, [rsp+20h]
    u8(0xBA); u32(1);                                     // mov edx, 1
    u8(0x45); u8(0x33); u8(0xC0);                         // xor r8d, r8d
    u8(0xFF); u8(0xD0);                                   // call rax
    u8(0x48); u8(0x83); u8(0x44); u8(0x24); u8(0x28); u8(0x08); // add qword [rsp+28h], 8
    u8(0xEB); const size_t j3 = c.size(); u8(0x00);       // jmp loop

    const size_t done = c.size();

    u8(0x48); u8(0xB9); u64(base + pdataRva);             // mov rcx, base+pdataRva
    u8(0xBA); u32(pdataSize);                             // mov edx, pdataSize
    u8(0x49); u8(0xB8); u64(base);                        // mov r8, base
    u8(0x48); u8(0xB8); u64(addTable);                    // mov rax, RtlAddFunctionTable
    u8(0xFF); u8(0xD0);                                   // call rax

    u8(0x48); u8(0x8B); u8(0x4C); u8(0x24); u8(0x20);     // mov rcx, [rsp+20h]
    u8(0xBA); u32(1);                                     // mov edx, 1
    u8(0x45); u8(0x33); u8(0xC0);                         // xor r8d, r8d
    u8(0x48); u8(0xB8); u64(entry);                       // mov rax, entry
    u8(0xFF); u8(0xD0);                                   // call rax
    u8(0x48); u8(0xA3); u64(scratch);                     // mov [scratch], rax

    u8(0x48); u8(0x83); u8(0xC4); u8(0x30);               // add rsp, 30h
    u8(0x5D);                                             // pop rbp
    u8(0xC3);                                             // ret

    c[j1] = (uint8_t)(done - (j1 + 1));
    c[j2] = (uint8_t)(done - (j2 + 1));
    c[j3] = (uint8_t)(loop - (j3 + 1));
    return c;
}

bool WriteTempDll(const std::vector<uint8_t>& dll, std::wstring& out) {
    wchar_t dir[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, dir)) return false;

    wchar_t file[MAX_PATH]{};
    swprintf_s(file, L"%s~vmp%08X%04X.dll", dir, GetTickCount(),
               (unsigned)(GetCurrentProcessId() & 0xFFFF));

    HANDLE h = CreateFileW(file, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    DWORD put = 0;
    const BOOL ok = WriteFile(h, dll.data(), (DWORD)dll.size(), &put, nullptr);
    CloseHandle(h);
    if (!ok || put != dll.size()) { DeleteFileW(file); return false; }

    out = file;
    return true;
}

} // namespace

namespace manualmap {

bool Map(HANDLE proc, const std::vector<uint8_t>& dll, std::string& err) {
    return MapEx(proc, dll, err, nullptr);
}

bool MapEx(HANDLE proc, const std::vector<uint8_t>& dll, std::string& err, uintptr_t* baseOut) {
    if (dll.size() < 0x200) { err = "archivo invalido"; return false; }
    if (!proc) { err = "proceso invalido"; return false; }

    auto* dos = (PIMAGE_DOS_HEADER)dll.data();
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) { err = "no es PE"; return false; }
    auto* nt = (PIMAGE_NT_HEADERS64)(dll.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) { err = "cabecera NT invalida"; return false; }
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) { err = "no es x64"; return false; }
    if (nt->OptionalHeader.SizeOfImage < 0x1000) { err = "imagen vacia"; return false; }
    if (!nt->OptionalHeader.AddressOfEntryPoint) { err = "sin entry point"; return false; }
    if (!Resolve()) { err = "ntdll sin exports"; return false; }

    const DWORD pid = GetProcessId(proc);
    if (!pid) { err = "pid invalido"; return false; }

    // 1. fichero temporal: SEC_IMAGE necesita respaldo en disco
    std::wstring file;
    if (!WriteTempDll(dll, file)) { err = "no se pudo escribir el temporal"; return false; }

    HANDLE fh = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_EXECUTE, FILE_SHARE_READ,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fh == INVALID_HANDLE_VALUE) { DeleteFileW(file.c_str()); err = "no se pudo abrir el temporal"; return false; }

    // 2. seccion de imagen -> el mapeo queda MEM_IMAGE, no MEM_PRIVATE
    HANDLE sec = nullptr;
    NTSTATUS st = pCreateSec(&sec, SECTION_ALL_ACCESS, nullptr, nullptr,
                             PAGE_READWRITE, SEC_IMAGE, fh);
    CloseHandle(fh);
    if (st != 0 || !sec) {
        DeleteFileW(file.c_str());
        err = "NtCreateSection fallo (0x" + std::to_string((unsigned)st) + ")";
        return false;
    }
    DeleteFileW(file.c_str());

    // 3. vista en el destino
    void* base = nullptr;
    SIZE_T viewSize = 0;
    st = pMapView(sec, proc, &base, 0, 0, nullptr, &viewSize, 1, MEM_RESERVE, PAGE_READWRITE);
    CloseHandle(sec);
    if (st != 0 || !base) {
        err = "NtMapViewOfSection fallo (0x" + std::to_string((unsigned)st) + ")";
        return false;
    }

    const uintptr_t img = (uintptr_t)base;
    const uintptr_t delta = img - nt->OptionalHeader.ImageBase;

    // 4. reubicaciones. Si un write falla, la imagen queda mal rebasada y el
    //    fallo aparece en DllMain sin traza: se comprueba.
    if (delta && !(nt->FileHeader.Characteristics & IMAGE_FILE_RELOCS_STRIPPED)) {
        const auto& rdir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        if (rdir.Size) {
            auto* reloc = (PIMAGE_BASE_RELOCATION)(dll.data() + rdir.VirtualAddress);
            const uintptr_t end = (uintptr_t)reloc + rdir.Size;
            while ((uintptr_t)reloc + sizeof(IMAGE_BASE_RELOCATION) <= end &&
                   reloc->VirtualAddress && reloc->SizeOfBlock) {
                const DWORD count = (reloc->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
                WORD* entry = (WORD*)(reloc + 1);
                for (DWORD i = 0; i < count; ++i) {
                    if ((entry[i] >> 12) != IMAGE_REL_BASED_DIR64) continue;
                    const uintptr_t at = img + reloc->VirtualAddress + (entry[i] & 0xFFF);
                    uintptr_t val = 0;
                    if (!RemoteRead(proc, at, &val, sizeof(val))) continue;
                    val += delta;
                    if (!RemoteWrite(proc, at, &val, sizeof(val))) {
                        err = "reubicacion no escrita";
                        return false;
                    }
                }
                reloc = (PIMAGE_BASE_RELOCATION)((uint8_t*)reloc + reloc->SizeOfBlock);
            }
        }
    }

    // 5. imports resueltos SIEMPRE contra los modulos DEL DESTINO
    const auto& impDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (impDir.Size) {
        auto* imp = (PIMAGE_IMPORT_DESCRIPTOR)(dll.data() + impDir.VirtualAddress);
        for (; imp->Name; ++imp) {
            const char* modName = (const char*)dll.data() + imp->Name;

            const uintptr_t modBase = RemoteLoadModule(proc, pid, modName);
            if (!modBase) {
                err = std::string("import no disponible en el destino: ") + modName;
                return false;
            }

            auto* thunk = (PIMAGE_THUNK_DATA)(dll.data() +
                          (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
            for (DWORD i = 0; thunk->u1.AddressOfData; ++thunk, ++i) {
                uintptr_t fn = 0;
                if (thunk->u1.Ordinal & IMAGE_ORDINAL_FLAG) {
                    const WORD ord = (WORD)(thunk->u1.Ordinal & 0xFFFF);
                    fn = RemoteExport(proc, modBase, nullptr, ord, true);
                } else {
                    auto* by = (PIMAGE_IMPORT_BY_NAME)(dll.data() + thunk->u1.AddressOfData);
                    fn = RemoteExport(proc, modBase, (const char*)by->Name, 0, false);
                }
                if (!fn) {
                    err = std::string("funcion no resuelta en el destino: ") + modName;
                    return false;
                }
                if (!RemoteWrite(proc, img + imp->FirstThunk + i * sizeof(uintptr_t), &fn, sizeof(fn))) {
                    err = std::string("no se pudo escribir la IAT: ") + modName;
                    return false;
                }
            }
        }
    }

    // 6. permisos por seccion, cabeceras solo lectura
    {
        SIZE_T hdrSize = nt->OptionalHeader.SizeOfHeaders;
        void* hdr = (void*)img;
        ULONG old = 0;
        pProt(proc, &hdr, &hdrSize, PAGE_READONLY, &old);
    }
    auto* sc = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sc) {
        DWORD prot = PAGE_READONLY;
        if (sc->Characteristics & IMAGE_SCN_MEM_EXECUTE)
            prot = (sc->Characteristics & IMAGE_SCN_MEM_WRITE) ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
        else if (sc->Characteristics & IMAGE_SCN_MEM_WRITE)
            prot = PAGE_READWRITE;
        RemoteProtect(proc, img + sc->VirtualAddress,
                      sc->Misc.VirtualSize ? sc->Misc.VirtualSize : sc->SizeOfRawData, prot);
    }

    // 7. memoria para el stub y para el retorno de DllMain
    HMODULE ntll = GetModuleHandleW(L"ntdll.dll");
    auto addTable = (uintptr_t)GetProcAddress(ntll, "RtlAddFunctionTable");
    if (!addTable) { err = "sin RtlAddFunctionTable"; return false; }

    SIZE_T scratchSize = 8;
    void* scratchAt = nullptr;
    if (pAlloc(proc, &scratchAt, 0, &scratchSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) != 0 || !scratchAt) {
        err = "sin memoria para el scratch";
        return false;
    }
    const uint64_t sentinel = 0xDEADBEEFDEADBEEFull;
    RemoteWrite(proc, (uintptr_t)scratchAt, &sentinel, sizeof(sentinel));

    const auto& exc = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    const auto& tlsDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    const bool hasTls = tlsDir.Size && tlsDir.VirtualAddress;

    auto stub = BuildStub(img, img + nt->OptionalHeader.AddressOfEntryPoint, addTable,
                          exc.VirtualAddress, exc.Size,
                          hasTls ? tlsDir.VirtualAddress + 0x18 : 0, hasTls,
                          (uintptr_t)scratchAt);

    SIZE_T stubSize = stub.size();
    void* stubAt = nullptr;
    if (pAlloc(proc, &stubAt, 0, &stubSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) != 0 || !stubAt) {
        err = "sin memoria para el stub";
        return false;
    }
    if (!RemoteWrite(proc, (uintptr_t)stubAt, stub.data(), stub.size())) {
        err = "escritura del stub fallo";
        return false;
    }
    RemoteProtect(proc, (uintptr_t)stubAt, stub.size(), PAGE_EXECUTE_READ);

    // 8. arrancar. CreateFlags = 0: el hilo corre. Con 1 se queda SUSPENDIDO.
    HANDLE th = nullptr;
    st = pThread(&th, 0x1FFFFF, nullptr, proc, stubAt, (void*)img, 0, 0, 0, 0, nullptr);
    if (st != 0 || !th) {
        err = "NtCreateThreadEx fallo (0x" + std::to_string((unsigned)st) + ")";
        return false;
    }
    ResumeThread(th); // inocuo si ya corria
    WaitForSingleObject(th, 15000);
    CloseHandle(th);

    uint64_t ret = 0;
    RemoteRead(proc, (uintptr_t)scratchAt, &ret, sizeof(ret));

    if (ret == sentinel) { err = "DllMain no llego a ejecutarse (stub roto)"; return false; }
    if (ret == 0)        { err = "DllMain devolvio FALSE"; return false; }

    if (baseOut) *baseOut = img;
    return true;
}

} // namespace manualmap