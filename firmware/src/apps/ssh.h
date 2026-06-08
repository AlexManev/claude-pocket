#pragma once

namespace apps {
namespace ssh {

// SSH client. A connect form (host / user / password / port) opens an
// interactive shell on a remote machine and renders it through a small VT100
// emulator (apps/ssh_term).
//
// Because libssh does blocking socket I/O and needs a much larger call stack
// than the 8 KB Arduino loopTask (SPEC §12.6), the whole session runs in its
// own FreeRTOS task. That task only mutates a shared terminal grid + status
// under a mutex; all drawing and key handling stay on the UI loop, so M5GFX is
// never touched from two tasks. Host keys are verified against a TOFU
// known_hosts file in LittleFS rather than blindly trusted.
//
// Key map in the terminal (the Cardputer has no dedicated Esc/Ctrl/arrows for
// a shell, so a few are remapped):
//   - esc/`  key      -> sends Esc (0x1b) to the remote (so vim works)
//   - Fn + ` key      -> leave the SSH app, back to the launcher
//   - Ctrl + letter   -> control char (Ctrl-C, Ctrl-D, Ctrl-L, ...)
//   - Fn + ; . , /    -> arrow keys (Up / Down / Left / Right)
//   - Tab / Enter / Backspace -> tab / CR / DEL as usual
void enter();
void tick();

}  // namespace ssh
}  // namespace apps
