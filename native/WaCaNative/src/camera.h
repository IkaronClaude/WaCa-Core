#pragma once

// Mouse look on the game's own camera (see camera.cpp)
namespace camera {
void Install();
// MouseLook|<on 0/1>|<yaw degrees per mouse unit>|<pitch degrees per mouse unit>: Lua turns it on in gameplay and off
// in menus (the cursor decision), with the sensitivity from keybinds.ini
void SetMouseLook(bool on, float yawScale, float pitchScale);
// CameraRelativeMove|<0/1>: right / left move the cat sideways relative to the camera instead of turning it
void SetCameraRelativeMove(bool on);
}
