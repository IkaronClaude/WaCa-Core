// THE GAME'S SCENES WITH OUR CAST (tickets.md "Cutscenes from text": the ceremony type, a RankUp re-cast). A rank-up
// ceremony is not played by the rank-up quest task (that only promotes): the game state's handler builds a params
// block and calls the sequencer manager's VIRTUAL play (vtable +0x330; rcx = manager, rdx = &params, r8 = &blend
// float, r9 = &finish delegate), which loads the sequence registered under the tag (RegisteredSequences, by type) and
// plays it anchored the game's way. The params (0x88, from the rank-up caller 0x1418a8390 and the copy constructor
// 0x1412b3ba0 in the dump's exe):
//   +0x00 three flag bytes   +0x08 FString (the music event)   +0x18 float play rate (1)   +0x1C float   +0x20 int32
//   +0x28 TMap<FName, TWeakObjectPtr<AActor>> - binding name -> actor in its place (FSwtActiveSequenceStruct's
//         ActorsReplaced; the game passes it empty)   +0x78 bool   +0x7C int32 sequencer type   +0x80 FGameplayTag
// Anchors: the slot's function reads the tag and the type straight from the params (mov r8, [rdx+80h] / mov edx,
// [rdx+7Ch]) before anything else.
#include "sequences.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

#include <Unreal/FMemory.hpp>
#include <Unreal/FWeakObjectPtr.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UObject.hpp>

#include "log.h"
#include "scan.h"

using namespace RC;
using namespace RC::Unreal;

namespace sequences {
namespace {
constexpr int kPlaySlot = 0x330;

#pragma pack(push, 1)
struct Element {                     // TSetElement<TPair<FName, TWeakObjectPtr<AActor>>>
    FName Key;
    FWeakObjectPtr Value;
    int32_t HashNextId;
    int32_t HashIndex;
};
#pragma pack(pop)
static_assert(sizeof(Element) == 0x18, "element");

struct Params {
    uint8_t Flags[3];
    uint8_t Pad0[5];
    void* MusicData;                 // FString { data, num, max }
    int32_t MusicNum, MusicMax;
    float PlayRate;
    float Unknown1C;
    int32_t Unknown20;
    uint8_t Pad24[4];
    // TMap (TSet) - 0x50
    Element* Data;                   // +0x28 TSparseArray data
    int32_t Num, Max;
    uint32_t InlineBits[4];          // +0x38 TBitArray allocation flags (inline, 128 bits)
    void* SecondaryBits;             // +0x48
    int32_t NumBits, MaxBits;        // +0x50
    int32_t FirstFreeIndex, NumFreeIndices;
    int32_t InlineHash;              // +0x60 the hash (TInlineAllocator<1>)
    uint8_t Pad64[4];
    void* SecondaryHash;             // +0x68
    int32_t HashSize;                // +0x70
    uint8_t Pad74[4];
    uint8_t Bool78;                  // +0x78
    uint8_t Pad79[3];
    int32_t Type;                    // +0x7C
    FName Tag;                       // +0x80
};
static_assert(offsetof(Params, Data) == 0x28 && offsetof(Params, NumBits) == 0x50 && offsetof(Params, InlineHash) == 0x60 &&
                  offsetof(Params, HashSize) == 0x70 && offsetof(Params, Type) == 0x7C && offsetof(Params, Tag) == 0x80,
              "params layout");

// THE STARTED CALLBACK. The play's last argument is a delegate the manager EXECUTES (not ExecuteIfBound: an unbound
// one is a null read - 0x1412b7646, VM crash 2026-10-31) once the sequence is up, with the playing object. The game's
// is a weak lambda (a 0x28 instance: vtable, +0x10 handle, +0x18 weak object, +0x20 one capture) that wires the rank-up
// FINISH - promotion, achievements - so it is never reused. Ours: the same kind of instance (the game's own vtable for
// the generic slots, copied) with Execute / ExecuteIfSafe doing nothing and the two CreateCopy slots re-stamping our
// vtable on the copy the manager keeps. The vtable is found where the game builds such an instance: "lea rax, [vt0] /
// mov [rbx], rax / call (new handle) / mov [rbx+10h], rax / lea rax, [vt] / mov [rbx], rax / movzx eax, byte [..]",
// checked: CreateCopy (+0x40) allocates 0x28 and copies +0x20, the other (+0x50) allocates 0x28 at its base +0x28.
struct Delegate {                    // TDelegate's allocation: the instance, in 16-byte units
    void* Instance = nullptr;
    int32_t Size = 0;
    int32_t Pad = 0;
};

constexpr int kSlots = 13;
void* g_vtable[kSlots] = {};
using CopyFn = void (*)(void* self, void* base);
CopyFn g_copy8 = nullptr, g_copy9 = nullptr, g_copy10 = nullptr;

void Restamp(void* instance) {
    if (instance) *reinterpret_cast<void***>(instance) = g_vtable;
}
void Copy8(void* self, void* base) {
    g_copy8(self, base);
    Restamp(*reinterpret_cast<void**>(base));
}
void Copy9(void* self, void* base) {
    g_copy9(self, base);
    Restamp(*reinterpret_cast<void**>(base));
}
void Copy10(void* self, void* owner) {
    g_copy10(self, owner);
    Restamp(*reinterpret_cast<void**>(static_cast<uint8_t*>(owner) + 0x28));
}
void Execute(void*, void*) {}
bool ExecuteIfSafe(void*, void*) { return true; }

bool BuildVtable() {
    if (g_vtable[0]) return true;
    auto p = scan::Parse("48 8D 05 ?? ?? ?? ?? 48 89 03 E8 ?? ?? ?? ?? 48 89 43 10 48 8D 05 ?? ?? ?? ?? 48 89 03 0F B6 05");
    for (auto& sec : scan::Sections(true)) {
        for (size_t off = 0; off + p.Bytes.size() <= sec.size; ++off) {
            bool ok = true;
            for (size_t k = 0; k < p.Bytes.size() && ok; ++k) ok = !p.Mask[k] || sec.start[off + k] == p.Bytes[k];
            if (!ok) continue;
            uint8_t* lea = sec.start + off + 19;
            auto** vt = reinterpret_cast<void**>(lea + 7 + *reinterpret_cast<int32_t*>(lea + 3));
            auto* c8 = static_cast<uint8_t*>(vt[8]);
            auto* c10 = static_cast<uint8_t*>(vt[10]);
            if (!scan::BodyContains(c8, "B9 28 00 00 00", 1) || !scan::BodyContains(c8, "48 8B 4B 20 48 89 48 20", 1) ||
                !scan::BodyContains(c10, "48 8D 53 28 B9 28 00 00 00", 1))
                continue;
            for (int i = 0; i < kSlots; ++i) g_vtable[i] = vt[i];
            g_copy8 = reinterpret_cast<CopyFn>(vt[8]);
            g_copy9 = reinterpret_cast<CopyFn>(vt[9]);
            g_copy10 = reinterpret_cast<CopyFn>(vt[10]);
            g_vtable[8] = reinterpret_cast<void*>(&Copy8);
            g_vtable[9] = reinterpret_cast<void*>(&Copy9);
            g_vtable[10] = reinterpret_cast<void*>(&Copy10);
            g_vtable[11] = reinterpret_cast<void*>(&Execute);
            g_vtable[12] = reinterpret_cast<void*>(&ExecuteIfSafe);
            auto base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
            logx::Line(L"[sequences] a weak-lambda delegate's vtable at exe VA 0x%llX - our started callback copies it",
                       static_cast<unsigned long long>(reinterpret_cast<uint8_t*>(vt) - base) + 0x140000000ull);
            return true;
        }
    }
    logx::Line(L"[sequences] no weak-lambda delegate vtable found - anchors disagree");
    return false;
}

using PlayFn = void (*)(void* manager, Params* params, float* blend, Delegate* onStarted);
}  // namespace

bool Play(void* manager, const std::wstring& tag, int type, const std::vector<std::pair<std::wstring, void*>>& cast,
          const std::wstring& music) {
    if (!manager || tag.empty()) return false;
    auto* fn = (*reinterpret_cast<uint8_t***>(manager))[kPlaySlot / 8];
    if (!fn || !scan::BodyContains(fn, "4C 8B 82 80 00 00 00", 1) || !scan::BodyContains(fn, "8B 52 7C", 1)) {
        logx::Line(L"[sequences] the sequencer manager's play slot +0x%X (%p) doesn't read the params' tag / type - anchors disagree",
                   kPlaySlot, fn);
        return false;
    }
    Params p;
    std::memset(&p, 0, sizeof(p));
    p.PlayRate = 1.0f;
    p.MaxBits = 0x80;
    p.FirstFreeIndex = -1;
    p.Type = type;
    p.Tag = FName(tag.c_str(), FNAME_Add);
    // the music: an FString { data, num (with its NUL), max } in the engine's memory - copied by the manager, ours freed
    if (!music.empty()) {
        auto n = static_cast<int32_t>(music.size() + 1);
        auto* text = static_cast<wchar_t*>(FMemory::Malloc(sizeof(wchar_t) * n));
        std::memcpy(text, music.c_str(), sizeof(wchar_t) * n);
        p.MusicData = text;
        p.MusicNum = p.MusicMax = n;
    }
    // the cast: one bucket (HashSize 1) chaining every element - a valid set the engine copies with its own TSet copy
    int n = 0;
    for (auto& c : cast) if (c.second) ++n;
    if (n > 0 && n <= 128) {
        p.Data = static_cast<Element*>(FMemory::Malloc(sizeof(Element) * n));
        int i = 0;
        for (auto& c : cast) {
            if (!c.second) continue;
            auto& e = p.Data[i];
            std::memset(&e, 0, sizeof(e));
            e.Key = FName(c.first.c_str(), FNAME_Add);
            e.Value = FWeakObjectPtr(static_cast<UObject*>(c.second));
            e.HashNextId = i + 1 < n ? i + 1 : -1;
            e.HashIndex = 0;
            p.InlineBits[i / 32] |= 1u << (i % 32);
            ++i;
        }
        p.Num = p.Max = n;
        p.NumBits = n;
        p.InlineHash = 0;
        p.HashSize = 1;
    }
    if (!BuildVtable()) return false;
    float blend = 0.0f;
    Delegate started;
    started.Size = 3;                                            // 0x28 in 16-byte units
    started.Instance = FMemory::Malloc(0x30, 16);
    std::memset(started.Instance, 0, 0x30);
    auto* inst = static_cast<uint8_t*>(started.Instance);
    Restamp(inst);
    static uint64_t handle = 0x5741434100000000ull;             // a delegate handle of our own ("WACA" ...)
    *reinterpret_cast<uint64_t*>(inst + 0x10) = ++handle;
    *reinterpret_cast<FWeakObjectPtr*>(inst + 0x18) = FWeakObjectPtr(static_cast<UObject*>(manager));
    reinterpret_cast<PlayFn>(fn)(manager, &p, &blend, &started);
    FMemory::Free(started.Instance);
    // the manager took a copy (its own TSet copy into the async play) - ours is freed here, the game's way
    if (p.Data) FMemory::Free(p.Data);
    if (p.MusicData) FMemory::Free(p.MusicData);
    logx::Line(L"[sequences] play %s (type %d), %d cast%s%s", tag.c_str(), type, n, music.empty() ? L"" : L", music ",
               music.c_str());
    return true;
}
}  // namespace sequences
