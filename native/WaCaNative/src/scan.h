// Byte-signature scanning over the game module's .text ("48 8B ?? E8 ?? ?? ?? ??").
#pragma once
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace scan {

struct Pattern {
    std::vector<uint8_t> Bytes;
    std::vector<bool> Mask;   // true = must match
};

inline Pattern Parse(const char* text) {
    Pattern p;
    for (const char* c = text; *c;) {
        if (*c == ' ') { ++c; continue; }
        if (c[0] == '?') {
            p.Bytes.push_back(0);
            p.Mask.push_back(false);
            c += (c[1] == '?') ? 2 : 1;
        } else {
            p.Bytes.push_back(static_cast<uint8_t>(strtoul(std::string(c, 2).c_str(), nullptr, 16)));
            p.Mask.push_back(true);
            c += 2;
        }
    }
    return p;
}

// the first match inside the main module's executable section, or nullopt
inline std::optional<uint8_t*> Find(const char* text) {
    auto p = Parse(text);
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint8_t* start = base + sec->VirtualAddress;
        size_t size = sec->Misc.VirtualSize;
        for (size_t off = 0; off + p.Bytes.size() <= size; ++off) {
            bool ok = true;
            for (size_t k = 0; k < p.Bytes.size(); ++k) {
                if (p.Mask[k] && start[off + k] != p.Bytes[k]) { ok = false; break; }
            }
            if (ok) return start + off;
        }
    }
    return std::nullopt;
}

// the target of an E8 / E9 rel32 at `at`
inline uint8_t* RelTarget(uint8_t* at) {
    return at + 5 + *reinterpret_cast<int32_t*>(at + 1);
}

// --- Anchoring a function on a string it uses (survives game updates better than raw bytes): the string, the code
// that references it, the function around that code (from the exe's own unwind table), checked by its prologue. ---

struct Section { uint8_t* start; size_t size; };

inline std::vector<Section> Sections(bool executable) {
    std::vector<Section> out;
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        bool exec = (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
        if (exec == executable) out.push_back({base + sec->VirtualAddress, sec->Misc.VirtualSize});
    }
    return out;
}

// every address of an exact ASCII string (its terminating NUL included) in the module's data
inline std::vector<uint8_t*> FindAscii(const char* text) {
    std::vector<uint8_t*> out;
    size_t n = strlen(text) + 1;
    for (const auto& s : Sections(false)) {
        for (size_t off = 0; off + n <= s.size; ++off) {
            if (s.start[off] == static_cast<uint8_t>(text[0]) && memcmp(s.start + off, text, n) == 0 &&
                (off == 0 || s.start[off - 1] == 0)) {
                out.push_back(s.start + off);
            }
        }
    }
    return out;
}

// the start of the function containing `code` (its primary unwind entry, chained entries followed), or null
inline uint8_t* FunctionStart(uint8_t* code) {
    DWORD64 imageBase = 0;
    auto* fn = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(code), &imageBase, nullptr);
    for (int guard = 0; fn && guard < 16; ++guard) {
        auto* info = reinterpret_cast<uint8_t*>(imageBase + fn->UnwindInfoAddress);
        if (!(info[0] >> 3 & 0x4)) return reinterpret_cast<uint8_t*>(imageBase + fn->BeginAddress);   // not chained
        uint8_t codes = info[2];
        auto* chained = reinterpret_cast<RUNTIME_FUNCTION*>(info + 4 + ((codes + 1) & ~1) * 2);
        fn = chained;
    }
    return nullptr;
}

// the functions that reference `target` RIP-relatively (lea / mov ..., [rip + disp32])
inline std::vector<uint8_t*> FunctionsReferencing(uint8_t* target) {
    std::vector<uint8_t*> out;
    for (const auto& s : Sections(true)) {
        for (size_t off = 0; off + 4 <= s.size; ++off) {
            uint8_t* p = s.start + off;
            if (p + 4 + *reinterpret_cast<int32_t*>(p) != target) continue;
            uint8_t* fn = FunctionStart(p);
            if (fn && std::find(out.begin(), out.end(), fn) == out.end()) out.push_back(fn);
        }
    }
    return out;
}

// whether function `from` calls / jumps to `to` (E8 / E9 rel32) within `depth` levels of calls - a second, independent
// anchor: a target found by its string must also sit where the call graph says it does. Scans each function's
// body: its unwind entry plus the contiguous entries chained back to it (separate cold parts aside)
inline bool Reaches(uint8_t* from, uint8_t* to, int depth) {
    if (!from || !to) return false;
    DWORD64 imageBase = 0;
    auto* fn = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(from), &imageBase, nullptr);
    if (!fn) return false;
    auto* begin = reinterpret_cast<uint8_t*>(imageBase + fn->BeginAddress);
    auto* end = reinterpret_cast<uint8_t*>(imageBase + fn->EndAddress);
    // a big function's later parts are separate unwind entries chained back to it, right after it
    for (int guard = 0; guard < 64; ++guard) {
        auto* next = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(end), &imageBase, nullptr);
        if (!next || reinterpret_cast<uint8_t*>(imageBase + next->BeginAddress) != end || FunctionStart(end) != begin) break;
        end = reinterpret_cast<uint8_t*>(imageBase + next->EndAddress);
    }
    std::vector<uint8_t*> callees;
    for (uint8_t* p = begin; p + 5 <= end; ++p) {
        if (*p != 0xE8 && *p != 0xE9) continue;
        uint8_t* target = RelTarget(p);
        if (target == to) return true;
        if (depth > 1 && RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(target), &imageBase, nullptr) &&
            FunctionStart(target) == target && std::find(callees.begin(), callees.end(), target) == callees.end())
            callees.push_back(target);
    }
    for (uint8_t* c : callees)
        if (Reaches(c, to, depth - 1)) return true;
    return false;
}

// whether function `fn`'s body (as Reaches scans it) holds `pattern` at least `count` times - a second anchor for a
// function whose body is known (an immediate it uses, a call layout)
inline bool BodyContains(uint8_t* fn, const char* pattern, int count) {
    DWORD64 imageBase = 0;
    auto* entry = fn ? RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(fn), &imageBase, nullptr) : nullptr;
    if (!entry) return false;
    auto* end = reinterpret_cast<uint8_t*>(imageBase + entry->EndAddress);
    for (int guard = 0; guard < 64; ++guard) {
        auto* next = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(end), &imageBase, nullptr);
        if (!next || reinterpret_cast<uint8_t*>(imageBase + next->BeginAddress) != end || FunctionStart(end) != fn) break;
        end = reinterpret_cast<uint8_t*>(imageBase + next->EndAddress);
    }
    auto p = Parse(pattern);
    int found = 0;
    for (uint8_t* at = fn; at + p.Bytes.size() <= end; ++at) {
        bool ok = true;
        for (size_t k = 0; k < p.Bytes.size() && ok; ++k) ok = !p.Mask[k] || at[k] == p.Bytes[k];
        found += ok;
    }
    return found >= count;
}

// the one function `fn` calls (E8, a function start; with jumps also E9 tail calls) whose body holds `pattern` - a
// function with no string of its own, anchored on an anchored caller plus what it does; none or several = null
inline uint8_t* CalleeWhere(uint8_t* fn, const char* pattern, bool jumps = false) {
    DWORD64 imageBase = 0;
    auto* entry = fn ? RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(fn), &imageBase, nullptr) : nullptr;
    if (!entry) return nullptr;
    auto* end = reinterpret_cast<uint8_t*>(imageBase + entry->EndAddress);
    for (int guard = 0; guard < 64; ++guard) {
        auto* next = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(end), &imageBase, nullptr);
        if (!next || reinterpret_cast<uint8_t*>(imageBase + next->BeginAddress) != end || FunctionStart(end) != fn) break;
        end = reinterpret_cast<uint8_t*>(imageBase + next->EndAddress);
    }
    uint8_t* found = nullptr;
    for (uint8_t* p = fn; p + 5 <= end; ++p) {
        if (*p != 0xE8 && !(jumps && *p == 0xE9)) continue;
        uint8_t* t = RelTarget(p);
        if (t == found || !RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(t), &imageBase, nullptr) || FunctionStart(t) != t ||
            !BodyContains(t, pattern, 1))
            continue;
        if (found) return nullptr;                       // ambiguous
        found = t;
    }
    return found;
}

// the one function that uses the ASCII string `text` and starts with `prologue` (pattern syntax), or null
inline uint8_t* FunctionUsing(const char* text, const char* prologue) {
    auto p = Parse(prologue);
    uint8_t* found = nullptr;
    for (uint8_t* str : FindAscii(text)) {
        for (uint8_t* fn : FunctionsReferencing(str)) {
            bool ok = true;
            for (size_t k = 0; k < p.Bytes.size() && ok; ++k) ok = !p.Mask[k] || fn[k] == p.Bytes[k];
            if (!ok) continue;
            if (found && found != fn) return nullptr;      // ambiguous: refuse rather than hook the wrong one
            found = fn;
        }
    }
    return found;
}

}  // namespace scan
