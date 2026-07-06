#pragma once

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/core/version.h"
#include "remote_webview.h"

namespace esphome {
namespace remote_webview {

// Automation things below
template<typename... Ts> class TriggerOnFrameUpdateAction : public Action<Ts...> {
 public:
  explicit TriggerOnFrameUpdateAction(RemoteWebView *ea) : ea_(ea) {}

// ESPHome 2025.11.0 changed Action::play to take const Ts &... (PR #11704);
// the old by-value override only compiles for an empty parameter pack.
#if defined(ESPHOME_VERSION_CODE) && ESPHOME_VERSION_CODE >= VERSION_CODE(2025, 11, 0)
  void play(const Ts &...x) override {
    this->ea_->trigger_on_frame_update();
  }
#else
  void play(Ts... x) override {
    this->ea_->trigger_on_frame_update();
  }
#endif

 protected:
  RemoteWebView *ea_;
};

// On Frame Update Trigger
class OnFrameUpdateTrigger : public Trigger<> {
 public:
  explicit OnFrameUpdateTrigger(RemoteWebView *parent) {
    parent->add_on_frame_update_callback([this]() { this->trigger(); });
  }
};

}  // namespace remote_webview
}  // namespace esphome
