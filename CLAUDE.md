# CLAUDE.md — orientation for Claude Code

You are working on **Claude Pocket**, firmware for the **M5Stack Cardputer
ADV** that turns the device into a voice assistant talking to the Anthropic
and OpenAI APIs.

**Read [`docs/SPEC.md`](docs/SPEC.md) first** — it carries both the original
brief and the implementation notes (§12) that explain the non-obvious
decisions in this codebase.

Quick orientation:

- **Board:** Cardputer ADV, ESP32-S3FN8 (StampS3A), **no PSRAM** (the main
  memory constraint — see SPEC §12.2).
- **Stack:** Arduino/C++ with M5Unified / M5GFX / M5Cardputer on
  `arduino-esp32 2.0.x` via `platformio.ini`. Do not try to upgrade to
  arduino-esp32 3.x — SPEC §12.1 explains why.
- **Pipeline:** Whisper (`gpt-4o-mini-transcribe`) → `claude-sonnet-4-6` →
  OpenAI TTS (`gpt-4o-mini-tts`, PCM). Mic samples and TTS audio both
  pass through LittleFS files rather than living in RAM.
- **Budgets** (set together so the UX feels symmetric): 30 s recording, 2.5 s
  VAD silence, `CLAUDE_MAX_TOKENS = 120` ≈ 30 s of speech, 1.5 MB
  LittleFS partition fits the resulting TTS body.
- **Look:** Anthropic brand palette (Ivory `#faf9f5`, Orange `#d97757`,
  Dark `#141413`); animated Claude spark on every state. See SPEC §7.
- **Secrets:** keys live in `firmware/config.h` (copy of
  `config.example.h`); **`config.h` is git-ignored** — never commit it.

## Current state

All shipping milestones (M0 – M6) are implemented under `firmware/src/`:
launcher, status bar, Snake (with difficulty menu), Settings (Wi-Fi
scanner, BT toggle, volume), the full Pocket voice pipeline with VAD,
auto-speak, conversation history, and animated spark across every state.

The **SSH** launcher entry (`firmware/src/apps/ssh*`) adds an interactive
remote shell: a connect form, a libssh session (LibSSH-ESP32, mbedTLS backend)
running in its own FreeRTOS task, a VT100-subset terminal emulator
(`ssh_term.cpp`, 40×15 cells), and TOFU host-key verification against a
LittleFS `known_hosts`. See SPEC §12.7 for the memory/threading constraints.

Open work:

- **SSH** is implemented but has **not been built or run on hardware yet** —
  PlatformIO wasn't available in the authoring environment. The first
  on-device build needs to confirm (a) LibSSH-ESP32 links against
  arduino-esp32 2.x's mbedTLS and (b) the 50 KB session-task stack actually
  fits the post-Wi-Fi heap on the no-PSRAM S3 (SPEC §12.7). If task creation
  OOMs, the failure is handled gracefully but the feature won't connect.
- **Claude Buddy** entry is still a stub; the BLE companion lives in the
  MicroPython buddy bundle and hasn't been ported yet.
- **Wake word (M7)** — needs a custom "Claude" ESP-SR WakeNet model,
  not started.
- TLS currently uses `setInsecure()`. For a public deployment, pin a root
  CA bundle and verify certificates.
