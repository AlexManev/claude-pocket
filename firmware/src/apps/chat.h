#pragma once

namespace apps {
namespace chat {

// Keyboard text chat with Claude. Type a question, press Enter to send, and
// read the streamed reply on screen. Backspace edits, backtick exits to the
// launcher. Long replies auto-scroll. Text only — no mic, no TTS. Reuses
// net::claude_stream and the same rolling conversation history as Pocket.
void enter();
void tick();

}  // namespace chat
}  // namespace apps
