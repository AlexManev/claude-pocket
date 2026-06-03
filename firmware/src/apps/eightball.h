#pragma once

namespace apps {
namespace eightball {

// Magic 8-Ball. Optionally type a yes/no question, then shake the device
// (BMI270 accelerometer) or press Enter to roll. With Wi-Fi and a typed
// question, Claude answers in classic 8-Ball style; otherwise a random
// canonical answer is shown. Backtick exits to the launcher.
void enter();
void tick();

}  // namespace eightball
}  // namespace apps
