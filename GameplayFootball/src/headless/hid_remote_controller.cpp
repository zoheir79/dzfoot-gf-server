// DZFoot Headless — Programmable controller implementation

#include "hid_remote_controller.hpp"

namespace blunted {

  HIDRemoteController::HIDRemoteController() {
    deviceType = e_HIDeviceType_Keyboard;
    identifier = "DZFootRemote";
    direction_.Set(0, 0, 0);
    for (int i = 0; i < e_ButtonFunction_Size; i++) {
      buttons_[i] = false;
      prevButtons_[i] = false;
    }
  }

  HIDRemoteController::~HIDRemoteController() {}

  void HIDRemoteController::LoadConfig() {}
  void HIDRemoteController::SaveConfig() {}

  void HIDRemoteController::Process() {
    for (int i = 0; i < e_ButtonFunction_Size; i++) {
      prevButtons_[i] = buttons_[i];
    }
  }

  bool HIDRemoteController::GetButton(e_ButtonFunction buttonFunction) {
    if (buttonFunction >= 0 && buttonFunction < e_ButtonFunction_Size) return buttons_[buttonFunction];
    return false;
  }

  float HIDRemoteController::GetButtonValue(e_ButtonFunction buttonFunction) {
    return GetButton(buttonFunction) ? 1.0f : 0.0f;
  }

  void HIDRemoteController::SetButton(e_ButtonFunction buttonFunction, bool state) {
    if (buttonFunction >= 0 && buttonFunction < e_ButtonFunction_Size) buttons_[buttonFunction] = state;
  }

  bool HIDRemoteController::GetPreviousButtonState(e_ButtonFunction buttonFunction) {
    if (buttonFunction >= 0 && buttonFunction < e_ButtonFunction_Size) return prevButtons_[buttonFunction];
    return false;
  }

  Vector3 HIDRemoteController::GetDirection() {
    return direction_;
  }

  void HIDRemoteController::SetDirection(const Vector3& dir) {
    direction_ = dir;
  }

  void HIDRemoteController::ResetButtons() {
    for (int i = 0; i < e_ButtonFunction_Size; i++) {
      buttons_[i] = false;
    }
    direction_.Set(0, 0, 0);
  }

  void HIDRemoteController::ResetNotSticky() {
    // In the old AIControlledKeyboard, "not sticky" meant resetting direction and action buttons
    // but keeping sprint/pressure/etc. For remote controllers, we reset everything each tick.
    ResetButtons();
  }

}
