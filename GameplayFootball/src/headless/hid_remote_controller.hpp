// DZFoot Headless — Programmable controller for server-side input injection

#ifndef _HPP_HID_REMOTE_CONTROLLER
#define _HPP_HID_REMOTE_CONTROLLER

#include "../hid/ihidevice.hpp"

namespace blunted {

  class HIDRemoteController : public IHIDevice {
    public:
      HIDRemoteController();
      virtual ~HIDRemoteController();

      virtual void LoadConfig();
      virtual void SaveConfig();
      virtual void Process();
      void SavePrevState();

      virtual bool GetButton(e_ButtonFunction buttonFunction);
      virtual float GetButtonValue(e_ButtonFunction buttonFunction);
      virtual void SetButton(e_ButtonFunction buttonFunction, bool state);
      virtual bool GetPreviousButtonState(e_ButtonFunction buttonFunction);
      virtual Vector3 GetDirection();

      void SetDirection(const Vector3& dir);
      void ResetButtons();
      void ResetNotSticky();

    protected:
      Vector3 direction_;
      bool buttons_[e_ButtonFunction_Size];
      bool prevButtons_[e_ButtonFunction_Size];
  };

}

#endif
