#include "chat.h"

#include <ArduinoJson.h>
#include <M5Cardputer.h>
#include <M5Unified.h>
#include <WiFi.h>

#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

#include "../app.h"
#include "../net/anthropic.h"
#include "../theme.h"

namespace apps {
namespace chat {

namespace {

// INPUT      — composing a question, caret blinks in the input field.
// THINKING   — request in flight; claude_stream blocks the loop while it
//              streams, so we just show a banner and repaint as tokens land.
// SHOWING_REPLY — reply complete; input cleared, long replies auto-scroll.
// ERROR      — last send failed; the banner carries the reason.
enum class State { INPUT, THINKING, SHOWING_REPLY, ERROR };

State g_state = State::INPUT;

// Fixed buffers (no heap churn while streaming, same reasoning as Pocket's
// g_reply on the PSRAM-less S3). g_input mirrors settings' password buffer.
char g_input[160] = "";
int  g_input_len  = 0;
char g_reply[1024] = "";
char g_status[48]  = "";

std::string g_history_json = "[]";

int      g_reply_scroll = 0;       // top line offset into the wrapped reply
uint32_t g_last_scroll_ms = 0;

// --- layout (240x135, 14 px status bar painted by app::loop) ---------------
constexpr int HEADER_Y     = theme::CONTENT_TOP + 2;        // 16
constexpr int INPUT_Y      = theme::CONTENT_TOP + 16;        // 30  (frame top)
constexpr int INPUT_H      = 30;
constexpr int REPLY_TOP    = INPUT_Y + INPUT_H + 4;          // 64
constexpr int REPLY_BOTTOM = theme::SCREEN_H - 12;           // 123
constexpr int FOOTER_Y     = theme::SCREEN_H - 2;

uint16_t to565(uint32_t rgb) {
    return M5.Display.color565((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}

// Manual word-wrap — M5GFX's setTextWrap only breaks at the display edge, not
// a panel boundary. Lifted from pocket.cpp's inline wrap, parameterised on the
// panel width so the input field and reply panel can share it.
void wrap(const char* txt, int panel_w, float text_size,
          std::vector<std::string>& out) {
    out.clear();
    if (!txt || !*txt) return;
    const int char_w = (int)(6.0f * text_size);
    size_t max_chars = (size_t)(panel_w / char_w);
    if (max_chars < 8) max_chars = 8;
    std::string cur, word;
    std::string s = txt;
    s += ' ';
    for (char c : s) {
        if (c == ' ' || c == '\n') {
            if (!word.empty()) {
                if (cur.empty()) cur = word;
                else if (cur.size() + 1 + word.size() <= max_chars) {
                    cur += ' '; cur += word;
                } else {
                    out.push_back(cur); cur = word;
                }
                word.clear();
            }
            if (c == '\n' && !cur.empty()) { out.push_back(cur); cur.clear(); }
        } else {
            word += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
}

// Repaint only the reply panel — called per token during streaming so the
// header and input field don't flicker on every delta.
void paint_reply() {
    M5.Display.fillRect(0, REPLY_TOP, theme::SCREEN_W, REPLY_BOTTOM - REPLY_TOP,
                        to565(theme::IVORY));
    if (!g_reply[0]) return;

    std::vector<std::string> rlines;
    wrap(g_reply, theme::SCREEN_W - 8, 1.5f, rlines);
    const int line_px = (int)(8.0f * 1.5f) + 2;             // ~14
    const int room    = REPLY_BOTTOM - REPLY_TOP;
    int visible = room / line_px;
    if (visible < 1) visible = 1;

    const int total = (int)rlines.size();
    int max_scroll = total - visible;
    if (max_scroll < 0) max_scroll = 0;
    if (g_reply_scroll > max_scroll) g_reply_scroll = 0;    // cycle to top

    M5.Display.setTextColor(to565(theme::DARK), to565(theme::IVORY));
    M5.Display.setTextSize(1.5f);
    M5.Display.setTextDatum(top_left);
    int y = REPLY_TOP;
    for (int i = 0; i < visible && g_reply_scroll + i < total; ++i) {
        M5.Display.drawString(rlines[g_reply_scroll + i].c_str(), 4, y);
        y += line_px;
    }

    // A small "more below" cue when the reply overflows the panel.
    if (total > visible) {
        M5.Display.setTextColor(to565(theme::MID_GRAY), to565(theme::IVORY));
        M5.Display.setTextSize(1);
        M5.Display.setTextDatum(top_right);
        M5.Display.drawString("v", theme::SCREEN_W - 3, REPLY_BOTTOM - 9);
        M5.Display.setTextDatum(top_left);
    }
}

void paint() {
    // Content background (status bar at rows 0-13 is owned by app::loop).
    M5.Display.fillRect(0, theme::CONTENT_TOP, theme::SCREEN_W, theme::CONTENT_H,
                        to565(theme::IVORY));

    // Header banner — orange while busy / errored, like Pocket.
    bool busy = (g_state == State::THINKING || g_state == State::ERROR);
    const char* header =
        g_state == State::THINKING      ? "Thinking..."        :
        g_state == State::ERROR         ? g_status             :
        g_state == State::SHOWING_REPLY ? "Enter: ask again"   :
                                          "Ask Claude";
    M5.Display.setTextColor(busy ? to565(theme::ORANGE) : to565(theme::DARK),
                            to565(theme::IVORY));
    M5.Display.setTextSize(1.0f);
    M5.Display.setTextDatum(top_center);
    M5.Display.drawString(header, theme::SCREEN_W / 2, HEADER_Y);

    // Input field — framed in orange. Wrap the typed text and show the last
    // couple of lines so the caret stays visible as it grows.
    M5.Display.drawRect(2, INPUT_Y, theme::SCREEN_W - 4, INPUT_H, to565(theme::ORANGE));
    std::vector<std::string> ilines;
    wrap(g_input, theme::SCREEN_W - 12, 1.0f, ilines);
    if (ilines.empty()) ilines.push_back("");
    if (g_state == State::INPUT) ilines.back() += '_';      // caret
    const int iline_px = (int)(8.0f * 1.0f) + 2;            // 10
    int show = (int)ilines.size();
    if (show > 2) show = 2;                                 // last 2 lines
    int first = (int)ilines.size() - show;
    M5.Display.setTextColor(to565(theme::DARK), to565(theme::IVORY));
    M5.Display.setTextSize(1.0f);
    M5.Display.setTextDatum(top_left);
    int iy = INPUT_Y + 5;
    for (int i = first; i < (int)ilines.size(); ++i) {
        M5.Display.drawString(ilines[i].c_str(), 6, iy);
        iy += iline_px;
    }

    paint_reply();

    // Footer hint.
    M5.Display.setTextColor(to565(theme::MID_GRAY), to565(theme::IVORY));
    M5.Display.setTextSize(1);
    M5.Display.setTextDatum(bottom_left);
    M5.Display.drawString("`:back  Enter:send", 4, FOOTER_Y);
    M5.Display.setTextDatum(top_left);
}

void submit() {
    if (g_input_len == 0) return;
    if (WiFi.status() != WL_CONNECTED) {
        g_state = State::ERROR;
        snprintf(g_status, sizeof(g_status), "No WiFi");
        paint();
        return;
    }

    g_reply[0] = '\0';
    g_reply_scroll = 0;
    g_state = State::THINKING;
    paint();

    // Stream tokens straight into the fixed reply buffer and repaint the panel
    // as they arrive (same pattern as pocket.cpp run_pipeline). Empty
    // on_sentence callback — no TTS — and nullptr keeps Claude's default
    // (concise, CLAUDE_MAX_TOKENS) system prompt.
    auto on_token = [](const std::string& s) {
        size_t cur  = strlen(g_reply);
        size_t left = sizeof(g_reply) - 1 - cur;
        size_t take = s.size() < left ? s.size() : left;
        memcpy(g_reply + cur, s.data(), take);
        g_reply[cur + take] = '\0';
        paint_reply();
    };
    std::string full = net::claude_stream(g_input, g_history_json,
                                          on_token,
                                          [](const std::string&){},
                                          nullptr);
    if (full.empty()) {
        g_state = State::ERROR;
        snprintf(g_status, sizeof(g_status), "Claude didn't respond");
        paint();
        return;
    }

    // Append this turn and cap to the last 10 messages (5 exchanges) so the
    // next question carries context without outgrowing SRAM — copied from
    // pocket.cpp so both screens behave identically.
    {
        JsonDocument hist;
        if (deserializeJson(hist, g_history_json) != DeserializationError::Ok ||
            !hist.is<JsonArray>()) {
            hist.clear();
            hist.to<JsonArray>();
        }
        JsonArray arr = hist.as<JsonArray>();
        JsonObject u = arr.add<JsonObject>();
        u["role"] = "user";
        u["content"] = g_input;
        JsonObject a = arr.add<JsonObject>();
        a["role"] = "assistant";
        a["content"] = full;
        while (arr.size() > 10) arr.remove(0);
        g_history_json.clear();
        serializeJson(arr, g_history_json);
    }

    // Clear the field; the next keystroke starts a fresh question while the
    // reply stays on screen.
    g_input[0] = '\0';
    g_input_len = 0;
    g_state = State::SHOWING_REPLY;
    g_reply_scroll = 0;
    g_last_scroll_ms = millis();
    paint();
}

}  // namespace

void enter() {
    g_state = State::INPUT;
    g_input[0]  = '\0';
    g_input_len = 0;
    g_reply[0]  = '\0';
    g_reply_scroll = 0;
    g_status[0] = '\0';
    g_history_json = "[]";          // fresh conversation on every entry
    g_last_scroll_ms = millis();
    paint();
}

void tick() {
    // Hands-free reading: cycle long replies on screen so the user doesn't need
    // a scroll key (the arrow glyphs ; . , / are valid typed characters here).
    uint32_t now = millis();
    if (g_state == State::SHOWING_REPLY && g_reply[0] &&
        now - g_last_scroll_ms > 2000) {
        g_last_scroll_ms = now;
        g_reply_scroll++;
        paint_reply();
    }

    if (g_state == State::THINKING) return;   // loop is blocked while streaming
    if (!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()) return;
    auto status = M5Cardputer.Keyboard.keysState();

    if (status.enter) { submit(); return; }

    if (status.del) {                          // backspace
        if (g_input_len > 0) {
            g_input[--g_input_len] = '\0';
            if (g_state != State::INPUT) g_state = State::INPUT;
            paint();
        }
        return;
    }

    for (char k : status.word) {
        if (k == '`') {                        // backtick = back to launcher
            app::goto_screen(app::Screen::LAUNCHER);
            return;
        }
        if (k >= 0x20 && k < 0x7f && g_input_len < (int)sizeof(g_input) - 1) {
            g_input[g_input_len++] = k;
            g_input[g_input_len] = '\0';
            if (g_state != State::INPUT) {     // typing after a reply = new question
                g_state = State::INPUT;
                g_status[0] = '\0';
            }
            paint();
        }
    }
}

}  // namespace chat
}  // namespace apps
