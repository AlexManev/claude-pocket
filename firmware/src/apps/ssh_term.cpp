#include "ssh_term.h"

#include <M5Unified.h>

#include "../theme.h"

namespace apps {
namespace ssh {

namespace {
uint16_t to565(uint32_t rgb) {
    return M5.Display.color565((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}
constexpr int CELL_W = 6;
constexpr int CELL_H = 8;

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
}  // namespace

void Term::reset() {
    for (int r = 0; r < ROWS; ++r)
        for (int c = 0; c < COLS; ++c) cells_[r][c] = ' ';
    cur_row_ = cur_col_ = 0;
    prev_cur_row_ = prev_cur_col_ = 0;
    cursor_visible_ = true;
    saved_row_ = saved_col_ = 0;
    state_ = P::GROUND;
    shadow_valid_ = false;          // force a full repaint next render()
}

void Term::mark_all_dirty() { shadow_valid_ = false; }

void Term::scroll_up() {
    for (int r = 0; r < ROWS - 1; ++r)
        for (int c = 0; c < COLS; ++c) cells_[r][c] = cells_[r + 1][c];
    for (int c = 0; c < COLS; ++c) cells_[ROWS - 1][c] = ' ';
}

void Term::newline() {
    if (++cur_row_ >= ROWS) {
        scroll_up();
        cur_row_ = ROWS - 1;
    }
}

void Term::put(char ch) {
    if (cur_col_ >= COLS) {         // deferred auto-wrap
        cur_col_ = 0;
        newline();
    }
    cells_[cur_row_][cur_col_] = ch;
    cur_col_++;
}

void Term::erase_display(int mode) {
    // 0: cursor..end, 1: start..cursor, 2: whole screen.
    int start_r = 0, end_r = ROWS - 1;
    if (mode == 0) {
        for (int c = cur_col_; c < COLS; ++c) cells_[cur_row_][c] = ' ';
        start_r = cur_row_ + 1;
    } else if (mode == 1) {
        for (int c = 0; c <= cur_col_ && c < COLS; ++c) cells_[cur_row_][c] = ' ';
        end_r = cur_row_ - 1;
    }
    if (mode == 0 || mode == 2)
        for (int r = start_r; r <= end_r; ++r)
            for (int c = 0; c < COLS; ++c) cells_[r][c] = ' ';
    if (mode == 1)
        for (int r = 0; r <= end_r; ++r)
            for (int c = 0; c < COLS; ++c) cells_[r][c] = ' ';
}

void Term::erase_line(int mode) {
    if (mode == 0)
        for (int c = cur_col_; c < COLS; ++c) cells_[cur_row_][c] = ' ';
    else if (mode == 1)
        for (int c = 0; c <= cur_col_ && c < COLS; ++c) cells_[cur_row_][c] = ' ';
    else
        for (int c = 0; c < COLS; ++c) cells_[cur_row_][c] = ' ';
}

void Term::dispatch_csi(char final) {
    auto p = [&](int i, int def) -> int {
        if (i > nparam_) return def;
        return params_[i] == 0 ? def : params_[i];
    };
    switch (final) {
        case 'A': cur_row_ = clampi(cur_row_ - p(0, 1), 0, ROWS - 1); break;
        case 'B': cur_row_ = clampi(cur_row_ + p(0, 1), 0, ROWS - 1); break;
        case 'C': cur_col_ = clampi(cur_col_ + p(0, 1), 0, COLS - 1); break;
        case 'D': cur_col_ = clampi(cur_col_ - p(0, 1), 0, COLS - 1); break;
        case 'G': cur_col_ = clampi(p(0, 1) - 1, 0, COLS - 1); break;           // CHA
        case 'd': cur_row_ = clampi(p(0, 1) - 1, 0, ROWS - 1); break;           // VPA
        case 'H':
        case 'f':
            cur_row_ = clampi(p(0, 1) - 1, 0, ROWS - 1);
            cur_col_ = clampi(p(1, 1) - 1, 0, COLS - 1);
            break;
        case 'J': erase_display(p(0, 0)); break;
        case 'K': erase_line(p(0, 0)); break;
        case 'h':
            if (priv_ && p(0, 0) == 25) cursor_visible_ = true;
            break;
        case 'l':
            if (priv_ && p(0, 0) == 25) cursor_visible_ = false;
            break;
        // 'm' (SGR colour), 'r' (scroll region), 'P'/'@' (ins/del char) and the
        // rest are intentionally ignored — see the header note on scope.
        default: break;
    }
}

void Term::write(const uint8_t* data, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        uint8_t b = data[i];
        switch (state_) {
            case P::GROUND:
                if (b == 0x1b) {
                    state_ = P::ESC;
                } else if (b == '\r') {
                    cur_col_ = 0;
                } else if (b == '\n' || b == 0x0b || b == 0x0c) {
                    newline();
                } else if (b == '\b') {
                    if (cur_col_ > 0) cur_col_--;
                } else if (b == '\t') {
                    cur_col_ = clampi((cur_col_ / 8 + 1) * 8, 0, COLS - 1);
                } else if (b == 0x07) {
                    // bell — no buzzer wired for this; ignore.
                } else if (b >= 0x20 && b < 0x7f) {
                    put((char)b);
                } else if (b >= 0xc0) {
                    put('?');                 // UTF-8 lead byte: placeholder
                }
                // 0x80-0xbf (UTF-8 continuation) and other C0 controls: skip.
                break;
            case P::ESC:
                switch (b) {
                    case '[':
                        state_ = P::CSI;
                        nparam_ = 0;
                        for (int k = 0; k < 8; ++k) params_[k] = 0;
                        param_started_ = false;
                        priv_ = false;
                        break;
                    case ']': state_ = P::OSC; break;
                    case '(':
                    case ')': state_ = P::CHARSET; break;
                    case '7': saved_row_ = cur_row_; saved_col_ = cur_col_; state_ = P::GROUND; break;
                    case '8': cur_row_ = saved_row_; cur_col_ = saved_col_; state_ = P::GROUND; break;
                    case 'M':                 // reverse line feed
                        if (cur_row_ > 0) cur_row_--;
                        state_ = P::GROUND;
                        break;
                    case 'c': reset(); break;  // RIS
                    default: state_ = P::GROUND; break;
                }
                break;
            case P::CSI:
                if (b == '?') {
                    priv_ = true;
                } else if (b >= '0' && b <= '9') {
                    params_[nparam_] = params_[nparam_] * 10 + (b - '0');
                    param_started_ = true;
                } else if (b == ';') {
                    if (nparam_ < 7) nparam_++;
                    param_started_ = false;
                } else if (b >= 0x40 && b <= 0x7e) {
                    dispatch_csi((char)b);
                    state_ = P::GROUND;
                }
                // intermediate bytes (0x20-0x2f) are ignored.
                break;
            case P::OSC:                       // e.g. window-title; ends at BEL/ST
                if (b == 0x07) state_ = P::GROUND;
                else if (b == 0x1b) state_ = P::OSC_ESC;
                break;
            case P::OSC_ESC:
                state_ = P::GROUND;            // ST is ESC '\'; either way, done
                break;
            case P::CHARSET:                   // single designator byte, ignore
                state_ = P::GROUND;
                break;
        }
    }
}

void Term::draw_cell(int r, int c, bool cursor, int x0, int y0) {
    int x = x0 + c * CELL_W;
    int y = y0 + r * CELL_H;
    uint16_t fg = to565(theme::DARK);
    uint16_t bg = to565(theme::IVORY);
    if (cursor) {
        fg = bg;                       // glyph in the background colour...
        bg = to565(theme::ORANGE);     // ...on an orange cursor block
    }
    M5.Display.fillRect(x, y, CELL_W, CELL_H, bg);
    char ch = cells_[r][c];
    if (ch && ch != ' ') {
        M5.Display.setTextColor(fg, bg);
        M5.Display.setTextSize(1);
        M5.Display.setTextDatum(top_left);
        char s[2] = {ch, 0};
        M5.Display.drawString(s, x, y);
    }
}

void Term::render(int x0, int y0) {
    bool full = !shadow_valid_;
    for (int r = 0; r < ROWS; ++r) {
        for (int c = 0; c < COLS; ++c) {
            bool is_cur = cursor_visible_ && r == cur_row_ && c == cur_col_;
            bool was_cur = r == prev_cur_row_ && c == prev_cur_col_;
            bool changed = full || cells_[r][c] != shadow_[r][c];
            // Repaint the cell if its glyph changed, or if the cursor moved on
            // to / off of it.
            if (changed || is_cur || was_cur) {
                draw_cell(r, c, is_cur, x0, y0);
                shadow_[r][c] = cells_[r][c];
            }
        }
    }
    prev_cur_row_ = cur_row_;
    prev_cur_col_ = cur_col_;
    shadow_valid_ = true;
}

}  // namespace ssh
}  // namespace apps
