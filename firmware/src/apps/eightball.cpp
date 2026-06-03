#include "eightball.h"

#include <M5Cardputer.h>
#include <M5Unified.h>
#include <WiFi.h>

#include <math.h>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

#include "../app.h"
#include "../net/anthropic.h"
#include "../theme.h"

namespace apps {
namespace eightball {

namespace {

// INPUT  — idle / composing the question, ball shows "8".
// ROLLING — consulting (blocking Claude call), card shows "• • •".
// REVEAL — verdict shown in the answer card.
enum class State { INPUT, ROLLING, REVEAL };

State g_state = State::INPUT;

char g_question[96] = "";
int  g_q_len = 0;
char g_answer[64] = "";

bool     g_has_imu = false;
bool     g_armed = true;          // ready to detect the next shake
uint32_t g_last_roll_ms = 0;

// The twenty canonical Magic 8-Ball answers — used verbatim for silent
// (no-question) rolls and as the offline / Claude-failure fallback so the
// ball *always* answers. Ten affirmative, five non-committal, five negative,
// just like the real toy.
const char* const CANNED[] = {
    "It is certain", "It is decidedly so", "Without a doubt",
    "Yes definitely", "You may rely on it", "As I see it, yes",
    "Most likely", "Outlook good", "Yes", "Signs point to yes",
    "Reply hazy, try again", "Ask again later", "Better not tell you now",
    "Cannot predict now", "Concentrate and ask again",
    "Don't count on it", "My reply is no", "My sources say no",
    "Outlook not so good", "Very doubtful",
};
constexpr int N_CANNED = sizeof(CANNED) / sizeof(CANNED[0]);

// Persona override for claude_stream — keeps the verdict short and in
// character instead of Pocket's spoken-answer default prompt.
constexpr const char* EIGHTBALL_PROMPT =
    "You are a mystical Magic 8-Ball. The user asks a question. Reply with "
    "exactly one short verdict in the classic Magic 8-Ball style (such as: "
    "It is certain / Without a doubt / Reply hazy, try again / Better not "
    "tell you now / Don't count on it / Outlook not so good / My sources say "
    "no). Consider their question, but answer ONLY with the verdict: 6 words "
    "maximum, no quotation marks, no trailing punctuation, no explanation.";

// Shake thresholds in g. At rest the accel magnitude sits near 1.0; a
// deliberate shake spikes well past 1.9. Require it to settle back below
// 1.25 before arming the next roll so one shake = one answer.
constexpr float SHAKE_HI   = 1.9f;
constexpr float SHAKE_LO   = 1.25f;
constexpr uint32_t ROLL_COOLDOWN_MS = 700;

// --- layout (240x135, 14 px status bar painted by app::loop) ---------------
constexpr int BALL_CX = theme::SCREEN_W / 2;
constexpr int BALL_CY = theme::CONTENT_TOP + 26;   // 40
constexpr int BALL_R  = 22;
constexpr int CARD_X  = 8;
constexpr int CARD_Y  = 66;
constexpr int CARD_W  = theme::SCREEN_W - 16;       // 224
constexpr int CARD_H  = 34;
constexpr int Q_Y     = CARD_Y + CARD_H + 4;        // 104
constexpr int FOOTER_Y = theme::SCREEN_H - 2;

uint16_t to565(uint32_t rgb) {
    return M5.Display.color565((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}

// Manual word-wrap (same idiom as the other screens) for the answer card.
void wrap(const char* txt, int panel_w, float text_size,
          std::vector<std::string>& out) {
    out.clear();
    if (!txt || !*txt) return;
    const int char_w = (int)(6.0f * text_size);
    size_t max_chars = (size_t)(panel_w / char_w);
    if (max_chars < 6) max_chars = 6;
    std::string cur, word, s = txt;
    s += ' ';
    for (char c : s) {
        if (c == ' ' || c == '\n') {
            if (!word.empty()) {
                if (cur.empty()) cur = word;
                else if (cur.size() + 1 + word.size() <= max_chars) { cur += ' '; cur += word; }
                else { out.push_back(cur); cur = word; }
                word.clear();
            }
        } else {
            word += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
}

void draw_ball() {
    // Cream "cue ball" with an orange 8 so it reads against the dark bg.
    M5.Display.fillCircle(BALL_CX, BALL_CY, BALL_R, to565(theme::DARK));
    M5.Display.fillCircle(BALL_CX, BALL_CY, BALL_R / 2, to565(theme::IVORY));
    M5.Display.setTextColor(to565(theme::ORANGE), to565(theme::IVORY));
    M5.Display.setTextSize(1.6f);
    M5.Display.setTextDatum(middle_center);
    M5.Display.drawString("8", BALL_CX, BALL_CY + 1);
}

void draw_card() {
    // Framed answer window.
    M5.Display.fillRoundRect(CARD_X, CARD_Y, CARD_W, CARD_H, 5, to565(theme::LIGHT_GRAY));
    M5.Display.drawRoundRect(CARD_X, CARD_Y, CARD_W, CARD_H, 5, to565(theme::ORANGE));

    const char* text =
        g_state == State::ROLLING ? "* * *" :
        g_state == State::REVEAL  ? g_answer :
                                    "";
    if (!text[0]) return;

    std::vector<std::string> lines;
    wrap(text, CARD_W - 12, 1.5f, lines);
    int show = (int)lines.size();
    if (show > 2) show = 2;
    const int line_px = (int)(8.0f * 1.5f) + 2;     // 14
    int start_y = CARD_Y + (CARD_H - show * line_px) / 2 + line_px / 2;

    uint16_t col = (g_state == State::ROLLING) ? to565(theme::MID_GRAY)
                                               : to565(theme::DARK);
    M5.Display.setTextColor(col, to565(theme::LIGHT_GRAY));
    M5.Display.setTextSize(1.5f);
    M5.Display.setTextDatum(middle_center);
    for (int i = 0; i < show; ++i) {
        M5.Display.drawString(lines[i].c_str(), BALL_CX, start_y + i * line_px);
    }
}

void draw_question() {
    M5.Display.fillRect(0, Q_Y - 1, theme::SCREEN_W, 14, to565(theme::IVORY));
    std::string line = "? ";
    line += g_question;
    if (g_state == State::INPUT) line += '_';          // caret while typing
    M5.Display.setTextColor(to565(theme::MID_GRAY), to565(theme::IVORY));
    M5.Display.setTextSize(1.0f);
    M5.Display.setTextDatum(top_center);
    // Show only the tail so a long question keeps the caret on screen.
    const size_t max_chars = 38;
    if (line.size() > max_chars) line = line.substr(line.size() - max_chars);
    M5.Display.drawString(line.c_str(), BALL_CX, Q_Y);
}

void paint() {
    M5.Display.fillRect(0, theme::CONTENT_TOP, theme::SCREEN_W, theme::CONTENT_H,
                        to565(theme::IVORY));
    draw_ball();
    draw_card();
    draw_question();

    M5.Display.setTextColor(to565(theme::MID_GRAY), to565(theme::IVORY));
    M5.Display.setTextSize(1);
    M5.Display.setTextDatum(bottom_left);
    M5.Display.drawString(g_has_imu ? "`:back  shake or Enter to ask"
                                    : "`:back  Enter to ask",
                          4, FOOTER_Y);
    M5.Display.setTextDatum(top_left);
}

// Repaint just the card — used for the brief ROLLING → REVEAL swap so the
// ball and question line don't flicker.
void paint_card_only() { draw_card(); }

void roll() {
    g_state = State::ROLLING;
    g_armed = false;
    g_last_roll_ms = millis();
    g_answer[0] = '\0';
    paint_card_only();                  // show "* * *" while we consult

    std::string verdict;
    // A typed question gets a real Claude reading; a silent shake gets a
    // classic canned answer (instant, and works offline).
    if (g_q_len > 0 && WiFi.status() == WL_CONNECTED) {
        verdict = net::claude_stream(g_question, "",
                                     [](const std::string&){},
                                     [](const std::string&){},
                                     EIGHTBALL_PROMPT);
        // Trim stray whitespace / trailing punctuation Claude might add.
        while (!verdict.empty() &&
               (verdict.back() == '\n' || verdict.back() == ' ' ||
                verdict.back() == '.'  || verdict.back() == '"')) {
            verdict.pop_back();
        }
    }
    if (verdict.empty()) {
        verdict = CANNED[random(N_CANNED)];
    }

    strncpy(g_answer, verdict.c_str(), sizeof(g_answer) - 1);
    g_answer[sizeof(g_answer) - 1] = '\0';
    g_state = State::REVEAL;
    paint();
}

}  // namespace

void enter() {
    g_state = State::INPUT;
    g_question[0] = '\0';
    g_q_len = 0;
    g_answer[0] = '\0';
    g_armed = true;
    g_last_roll_ms = 0;

    // BMI270 is normally brought up by M5.begin(); enable it if not, and
    // degrade to Enter-only rolling if there's no IMU on this unit.
    if (!M5.Imu.isEnabled()) M5.Imu.begin();
    g_has_imu = M5.Imu.isEnabled();

    // Mix some entropy into the PRNG for the canned-answer path.
    randomSeed(micros());
    paint();
}

void tick() {
    // Shake detection — the headline interaction. Skipped while a roll is in
    // flight (the Claude call blocks the loop anyway).
    if (g_has_imu && g_state != State::ROLLING) {
        float ax, ay, az;
        if (M5.Imu.getAccel(&ax, &ay, &az)) {
            float mag = sqrtf(ax * ax + ay * ay + az * az);
            if (mag < SHAKE_LO) g_armed = true;
            if (g_armed && mag > SHAKE_HI &&
                millis() - g_last_roll_ms > ROLL_COOLDOWN_MS) {
                roll();
                return;
            }
        }
    }

    if (!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()) return;
    auto status = M5Cardputer.Keyboard.keysState();

    if (status.enter) { roll(); return; }

    if (status.del) {
        if (g_q_len > 0) {
            g_question[--g_q_len] = '\0';
            g_state = State::INPUT;
            paint();
        }
        return;
    }

    for (char k : status.word) {
        if (k == '`') {
            app::goto_screen(app::Screen::LAUNCHER);
            return;
        }
        if (k >= 0x20 && k < 0x7f && g_q_len < (int)sizeof(g_question) - 1) {
            if (g_state == State::REVEAL) {     // typing after a verdict = new question
                g_answer[0] = '\0';
            }
            g_state = State::INPUT;
            g_question[g_q_len++] = k;
            g_question[g_q_len] = '\0';
            paint();
        }
    }
}

}  // namespace eightball
}  // namespace apps
