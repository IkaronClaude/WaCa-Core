// THE GAME'S OWN TEXT FILL, ON DEMAND (tickets.md "Dialogue text bugs": the operator, 2026-10-31 - "there's likely
// dozens or hundreds of broken text lines"). Every dialogue / quest line goes through one function before it is shown:
// FText* Fill(FText* out, const FText* in) (exe 0x14162A000 in the dump) - it replaces {Player}, {Friend}, {The
// Traitor}, {Clan} and every narrative role ("{%s}" of the role's name, spaced and upper-cased: {THE WARRIOR FATHER})
// with the cats of THIS save, then handles what's left between braces. Calling it on every line of the string table
// with a real save shows exactly which lines come out broken. Anchor: the one function using the ASCII "{The
// Traitor}" that opens "mov r11, rsp / push rbp / push r13 / push r14 / push r15".
#include "fill.h"

#include <windows.h>

#include <cstdint>
#include <new>

#include <Unreal/FString.hpp>
#include <Unreal/FText.hpp>

#include <cwctype>
#include <mutex>
#include <set>

#include "MinHook.h"
#include "log.h"
#include "scan.h"

using namespace RC;
using namespace RC::Unreal;

namespace fill {
namespace {
using FillFn = FText* (*)(FText* out, const FText* in);
FillFn g_fill = nullptr;
FillFn g_original = nullptr;         // the game's fill, once hooked
bool g_tried = false;
bool g_fallback = false;
std::mutex g_seenMutex;
std::set<std::wstring> g_seen;

void Find() {
    if (g_tried) return;
    g_tried = true;
    g_fill = reinterpret_cast<FillFn>(scan::FunctionUsing("{The Traitor}", "4C 8B DC 55 41 55 41 56 41 57"));
    logx::Line(L"[fill] the game's text fill %s %p", g_fill ? L"at" : L"NOT FOUND - anchors disagree", g_fill);
}

// a role the game left unfilled - an upper-case name of two words or more ({THE AMBITIOUS APPRENTICE}); not the
// counters ({N}), not {CLAN}, not the promotion screens' {PLAYER WARRIOR NAME} (filled by their own code)
bool IsRole(const std::wstring& inner) {
    if (inner.size() < 4 || inner.find(L' ') == std::wstring::npos || inner.rfind(L"PLAYER", 0) == 0) return false;
    for (wchar_t c : inner) if (!(c == L' ' || (c >= L'A' && c <= L'Z'))) return false;
    return true;
}

// {SHADOW CLAN LEADER} -> "the ShadowClan leader", {THE AMBITIOUS APPRENTICE} -> "the ambitious apprentice"
std::wstring Words(const std::wstring& inner) {
    std::wstring w;
    for (wchar_t c : inner) w += static_cast<wchar_t>(std::towlower(c));
    const wchar_t* clans[][2] = { { L"thunder clan", L"ThunderClan" }, { L"river clan", L"RiverClan" }, { L"riverclan", L"RiverClan" },
                                  { L"shadow clan", L"ShadowClan" }, { L"wind clan", L"WindClan" } };
    bool clanFirst = false;
    for (auto& c : clans) {
        auto at = w.find(c[0]);
        if (at != std::wstring::npos) {
            w.replace(at, wcslen(c[0]), c[1]);
            if (at == 0) clanFirst = true;
        }
    }
    if (clanFirst) w = L"the " + w;
    return w;
}

std::wstring Fallback(const std::wstring& text, bool* changed) {
    std::wstring out;
    size_t at = 0;
    *changed = false;
    while (at < text.size()) {
        auto open = text.find(L'{', at);
        auto close = open == std::wstring::npos ? std::wstring::npos : text.find(L'}', open);
        if (close == std::wstring::npos) {
            out += text.substr(at);
            break;
        }
        out += text.substr(at, open - at);
        auto inner = text.substr(open + 1, close - open - 1);
        if (!IsRole(inner)) {
            out += text.substr(open, close - open + 1);
        } else {
            auto words = Words(inner);
            size_t k = out.size();
            while (k > 0 && (out[k - 1] == L'"' || out[k - 1] == L'\'' || out[k - 1] == L'>')) --k;
            while (k > 0 && out[k - 1] == L' ') --k;
            bool start = k == 0 || out[k - 1] == L'.' || out[k - 1] == L'!' || out[k - 1] == L'?' || out[k - 1] == L'\n';
            // (spoken TO - "Thank you, {X}." / "{X}, listen": no article - "Thank you, resentful elder.", not "..., the
            // resentful elder." (the ThunderClan sweep, 2026-11-01))
            wchar_t after = close + 1 < text.size() ? text[close + 1] : L'.';
            bool addressed = (k > 0 && out[k - 1] == L',' && (after == L'.' || after == L'!' || after == L'?' || after == L','))
                          || (start && after == L',');
            if (addressed && words.rfind(L"the ", 0) == 0) words = words.substr(4);
            // (a sentence's start: capitalised)
            if (start) words[0] = static_cast<wchar_t>(std::towupper(words[0]));
            out += words;
            *changed = true;
            std::lock_guard<std::mutex> lock(g_seenMutex);
            if (g_seen.insert(inner).second)
                logx::Line(L"[fill] {%s} has no cat in this save - shown as \"%s\"", inner.c_str(), words.c_str());
        }
        at = close + 1;
    }
    return out;
}

FText* Detour(FText* out, const FText* in) {
    auto* result = g_original(out, in);
    if (!g_fallback || !result) return result;
    std::wstring text(result->ToString());
    if (text.find(L'{') == std::wstring::npos) return result;
    bool changed = false;
    auto fixed = Fallback(text, &changed);
    if (changed) {
        result->~FText();
        new (result) FText(fixed.c_str());
    }
    return result;
}
}  // namespace

bool SetFallback(bool on) {
    Find();
    if (!g_fill) return false;
    if (!g_original) {
        if (MH_CreateHook(reinterpret_cast<void*>(g_fill), reinterpret_cast<void*>(&Detour), reinterpret_cast<void**>(&g_original)) != MH_OK ||
            MH_EnableHook(reinterpret_cast<void*>(g_fill)) != MH_OK) {
            logx::Line(L"[fill] the text fill could not be hooked");
            g_original = nullptr;
            return false;
        }
    }
    g_fallback = on;
    logx::Line(L"[fill] a role with no cat in this save: %s", on ? L"shown as its name in words" : L"left as the game has it");
    return true;
}

bool Fill(const std::wstring& in, std::wstring& out, bool asShown) {
    Find();
    if (!g_fill) return false;
    FText source(in.c_str());
    alignas(FText) unsigned char buffer[sizeof(FText)] = {};
    // (the game's own fill - without our fallback, so a sweep sees what the game alone does)
    auto* result = (g_original && !asShown ? g_original : g_fill)(reinterpret_cast<FText*>(buffer), &source);
    out = result ? std::wstring(result->ToString()) : L"";
    if (result) result->~FText();
    return true;
}
}  // namespace fill
