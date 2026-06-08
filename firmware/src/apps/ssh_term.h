#pragma once

#include <stddef.h>
#include <stdint.h>

namespace apps {
namespace ssh {

// A tiny fixed-size VT100/xterm-subset terminal emulator. It parses the raw
// byte stream coming off the SSH channel into a character grid and renders the
// grid to the Cardputer's 240x135 display with the 6x8 base font (40x15 cells,
// see the constants below).
//
// Scope is deliberately a "good enough for a login shell" subset: printable
// text, CR/LF/BS/TAB/BEL, cursor movement (CUU/CUD/CUF/CUB/CUP), erase
// (ED/EL), and cursor show/hide. SGR colour is parsed and ignored (the whole
// terminal is rendered monochrome in the Claude palette), and full-screen TUIs
// like vim/htop will render imperfectly. UTF-8 is not decoded — multibyte
// sequences collapse to '?' so the column grid stays aligned.
//
// The class is pure data + drawing; it knows nothing about threading. The SSH
// session task feeds bytes via write() and the UI task calls render(), both
// under the same external mutex (see ssh.cpp).
class Term {
   public:
    static constexpr int COLS = 40;   // 40 * 6px = 240px
    static constexpr int ROWS = 15;   // 15 * 8px = 120px (under the 14px bar)

    void reset();

    // Feed raw bytes from the remote shell. Updates the grid + cursor.
    void write(const uint8_t* data, size_t n);

    // Draw the grid at (x0, y0). Only cells that changed since the last render
    // are repainted, so this is cheap to call every UI tick.
    void render(int x0, int y0);

    // Force every cell to repaint on the next render() (e.g. after the screen
    // was used by another view).
    void mark_all_dirty();

   private:
    void put(char ch);
    void newline();
    void scroll_up();
    void erase_display(int mode);
    void erase_line(int mode);
    void dispatch_csi(char final);
    void draw_cell(int r, int c, bool cursor, int x0, int y0);

    char cells_[ROWS][COLS] = {};
    char shadow_[ROWS][COLS];          // last-rendered frame (for diffing)
    bool shadow_valid_ = false;

    int cur_row_ = 0;
    int cur_col_ = 0;
    int prev_cur_row_ = 0;
    int prev_cur_col_ = 0;
    bool cursor_visible_ = true;

    int saved_row_ = 0;
    int saved_col_ = 0;

    // Escape/CSI parser state.
    enum class P { GROUND, ESC, CSI, OSC, OSC_ESC, CHARSET };
    P state_ = P::GROUND;
    int params_[8] = {};
    int nparam_ = 0;
    bool param_started_ = false;
    bool priv_ = false;
};

}  // namespace ssh
}  // namespace apps
