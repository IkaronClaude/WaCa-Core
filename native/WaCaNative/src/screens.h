#pragma once

// The game's own popups opened the game's way (see screens.cpp)
namespace screens {
// open a popup (EWidgetsIds: 86 = the fresh-kill pile ...) on a screen (USwtScreen - the HUD's CurrentScreen), as a
// dialogue's "create screen" action does; false when the anchors disagree
bool OpenPopup(void* screen, int widgetId);
}
