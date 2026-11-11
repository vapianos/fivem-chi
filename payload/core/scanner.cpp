#include "core/scanner.h"
#include "utils/log.h"

#include <Windows.h>
#include <cstring>
#include <cstdlib>

namespace {
struct Pattern {
    std::vector<uint8_t> bytes;
    std::vector<bool>    mask; // true = comodin
    size_t len = 0;
};

bool Parse(const char* text, Pattern& out) {
    if (!text || !*text) return false;
    const char* p = text;
    while (*p) {
        while (*p == ' ') ++p;
        if (!*p) break;

        if (*p == '?' || *p == '*') {
            out.bytes.push_back(0);
            out.mask.push_back(true);
            while (*p == '?' || *p == '*') ++p;
        } else {
            char* end = nullptr;
            long v = strtol(p, &end, 16);
            if (end == p) return false;
            out.bytes.push_back(static_cast<uint8_t>(v & 0xFF));
            out.mask.push_back(false);
            p = end;
        }
        ++out.len;
    }
    return out.len >= 2;
}

bool Executable(DWORD protect) {
    if (protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    switch (protect & 0xFF) {
    case PAGE_EXECUTE:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

// Ancla con el primer byte no comodin y salta con memchr: buscar 20 bytes a
// fuerza bruta en un modulo de 3 MB tarda segundos y bloquea el escaneo.
uintptr_t ScanRange(uintptr_t start, uintptr_t end, const Pattern& pat) {
    if (end <= start || end - start < pat.len) return 0;
    const uint8_t* base = reinterpret_cast<const uint8_t*>(start);
    const size_t   last = static_cast<size_t>(end - start - pat.len);

    size_t anchor = 0;
    while (anchor < pat.len && pat.mask[anchor]) ++anchor;
    if (anchor == pat.len) return start;

    const uint8_t want = pat.bytes[anchor];

    size_t i = 0;
    while (i <= last) {
        const uint8_t* hit = reinterpret_cast<const uint8_t*>(
            memchr(base + i + anchor, want, last - i + 1));
        if (!hit) return 0;
        i = static_cast<size_t>(hit - base) - anchor;

        bool ok = true;
        for (size_t j = 0; j < pat.len; ++j) {
            if (pat.mask[j]) continue;
            if (base[i + j] != pat.bytes[j]) { ok = false; break; }
        }
        if (ok) return start + i;
        ++i;
    }
    return 0;
}
} // namespace

namespace scanner {

uintptr_t Find(const char* pattern, uintptr_t start, size_t size) {
    Pattern pat{};
    if (!Parse(pattern, pat)) return 0;

    const uintptr_t end = start + size;
    uintptr_t cursor = start;

    while (cursor < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(cursor), &mbi, sizeof(mbi)))
            break;

        const uintptr_t regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        uintptr_t regionEnd = regionBase + mbi.RegionSize;
        if (regionEnd > end) regionEnd = end;

        if (mbi.State == MEM_COMMIT && Executable(mbi.Protect)) {
            const uintptr_t from = cursor > regionBase ? cursor : regionBase;
            if (uintptr_t hit = ScanRange(from, regionEnd, pat))
                return hit;
        }
        cursor = regionEnd ? regionEnd : cursor + 0x1000;
    }
    return 0;
}

uintptr_t FindIn(const ModuleInfo& module, const char* pattern) {
    if (!module.valid()) return 0;
    return Find(pattern, module.base, module.size);
}

std::vector<uintptr_t> FindAll(const ModuleInfo& module, const char* pattern, size_t maxHits) {
    std::vector<uintptr_t> hits;
    if (!module.valid()) return hits;

    uintptr_t cursor = module.base;
    const uintptr_t end = module.base + module.size;
    while (cursor < end && hits.size() < maxHits) {
        uintptr_t hit = Find(pattern, cursor, static_cast<size_t>(end - cursor));
        if (!hit) break;
        hits.push_back(hit);
        cursor = hit + 1;
    }
    return hits;
}

uintptr_t FollowJumps(uintptr_t p) {
    for (int i = 0; i < 8 && p; ++i) {
        auto* b = reinterpret_cast<uint8_t*>(p);
        if (b[0] == 0xE9) { p = p + 5 + *reinterpret_cast<int32_t*>(b + 1); continue; }
        if (b[0] == 0xEB) { p = p + 2 + *reinterpret_cast<int8_t*>(b + 1); continue; }
        if (b[0] == 0xFF && b[1] == 0x25) {
            p = *reinterpret_cast<uintptr_t*>(p + 6 + *reinterpret_cast<int32_t*>(b + 2));
            continue;
        }
        if (b[0] == 0x48 && b[1] == 0xFF && b[2] == 0x25) {
            p = *reinterpret_cast<uintptr_t*>(p + 7 + *reinterpret_cast<int32_t*>(b + 3));
            continue;
        }
        break;
    }
    return p;
}

bool InExecutableSection(uintptr_t addr) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi))) return false;
    return mbi.State == MEM_COMMIT && Executable(mbi.Protect);
}

uintptr_t FindUnique(const ModuleInfo& module, const char* pattern, size_t* hitCount) {
    auto hits = FindAll(module, pattern, 8);
    if (hitCount) *hitCount = hits.size();
    if (hits.size() != 1) return 0;
    return FollowJumps(hits[0]);
}

uintptr_t ResolveRel32(uintptr_t addr, int offsetPos, int instrLen) {
    int32_t rel = 0;
    memcpy(&rel, reinterpret_cast<const void*>(addr + offsetPos), sizeof(rel));
    return addr + instrLen + static_cast<intptr_t>(rel);
}

uintptr_t ReadPtr(uintptr_t addr) {
    uintptr_t value = 0;
    memcpy(&value, reinterpret_cast<const void*>(addr), sizeof(value));
    return value;
}
} // namespace scanner