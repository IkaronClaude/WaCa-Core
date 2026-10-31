// THE GAME'S POPUPS ON DEMAND (tickets.md "Fresh kill without hunger": the pile screen only opens from talking to the
// pile's cat). A dialogue's create-screen action (UQuestDialogueActionCreateScreen: EWidgetsIds WidgetId +0xF8) opens
// its popup with ONE virtual on the screen the dialogue runs in - USwtScreen vtable +0x5D0 (rcx = screen, dl = widget
// id, r8 = 0, r9d = -1) - and then reads the popup from the screen's CurrentPopup (+0x398). Anchor: that call site in
// the action ("mov r9d, -1 / movzx edx, byte [rdi+0F8h] / xor r8d, r8d / mov rcx, rbx / call [rax+5D0h]"), unique in
// the exe; the slot is read from it.
#include "screens.h"

#include <windows.h>

#include <cstdint>

#include "log.h"
#include "scan.h"

namespace screens {
namespace {
using OpenFn = void (*)(void* screen, uint8_t widgetId, void* unknown, int32_t index);
int g_slot = -1;
}  // namespace

bool OpenPopup(void* screen, int widgetId) {
    if (!screen || widgetId <= 0 || widgetId > 255) return false;
    if (g_slot < 0) {
        auto at = scan::Find("41 B9 FF FF FF FF 0F B6 97 F8 00 00 00 45 33 C0 48 8B CB FF 90 ?? ?? 00 00");
        if (!at) {
            logx::Line(L"[screens] the create-screen action's popup call isn't there - anchors disagree");
            g_slot = 0;
            return false;
        }
        g_slot = *reinterpret_cast<int32_t*>(*at + 21);
        logx::Line(L"[screens] a screen opens its popups at vtable +0x%X", g_slot);
    }
    if (g_slot == 0) return false;
    auto* fn = (*reinterpret_cast<void***>(screen))[g_slot / 8];
    reinterpret_cast<OpenFn>(fn)(screen, static_cast<uint8_t>(widgetId), nullptr, -1);
    logx::Line(L"[screens] popup %d opened", widgetId);
    return true;
}
}  // namespace screens
