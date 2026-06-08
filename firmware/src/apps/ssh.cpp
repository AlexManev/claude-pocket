#include "ssh.h"

#include <M5Cardputer.h>
#include <M5Unified.h>
#include <WiFi.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/stream_buffer.h>
#include <freertos/task.h>

// LibSSH-ESP32 (ewpa/LibSSH-ESP32): libssh_esp32.h must come before the libssh
// headers — it wires libssh's crypto to the mbedTLS already linked for the
// cloud calls and exposes libssh_begin().
#include <libssh_esp32.h>
#include <libssh/libssh.h>

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../app.h"
#include "../settings/store.h"
#include "../theme.h"
#include "ssh_term.h"

namespace apps {
namespace ssh {

namespace {

// libssh wants a deep call stack for the key exchange — the 8 KB Arduino
// loopTask (SPEC §12.6) is nowhere near enough, so the session runs in its own
// task at the size the LibSSH-ESP32 examples use. On this no-PSRAM S3 that is a
// big slice of the ~90 KB heap (SPEC §12.2); SSH never runs alongside the voice
// pipeline, so the budget is workable, but if task creation ever fails for lack
// of heap this is the first knob to revisit.
constexpr uint32_t SSH_TASK_STACK = 51200;
constexpr const char* KNOWN_HOSTS_PATH = "/littlefs/known_hosts";

enum class UiState { FORM, CONNECTING, HOSTKEY, TERMINAL, FINISHED };

// Everything shared between the UI loop and the session task. The terminal grid
// and the status/fingerprint strings are guarded by `mutex`; the small flags
// are plain volatiles used for one-way signalling.
struct Shared {
    SemaphoreHandle_t mutex = nullptr;
    StreamBufferHandle_t tx = nullptr;     // keystrokes UI -> session
    TaskHandle_t task = nullptr;

    Term term;
    char status[96] = "";
    char fingerprint[200] = "";

    volatile UiState state = UiState::FORM;
    volatile int hostkey_decision = 0;     // 0 pending, 1 accept, -1 reject
    volatile bool stop_req = false;
    volatile bool task_running = false;
};
Shared g_sh;

// Connection parameters, copied out of the form before the task starts so the
// task reads stable storage while the user can't edit them.
char g_host[128] = "";
char g_user[64] = "";
char g_pass[64] = "";
unsigned int g_port = 22;

// --- form state (UI side) --------------------------------------------------
char f_host[128] = "";
char f_user[64] = "";
char f_pass[64] = "";
char f_port[8] = "22";
int f_field = 0;                            // 0 host, 1 user, 2 pass, 3 port

// --- UI repaint bookkeeping ------------------------------------------------
UiState g_last_state = UiState::FINISHED;   // != FORM so enter() repaints
char g_last_status[96] = "\x01";            // sentinel so first paint fires
bool g_want_leave = false;

uint16_t to565(uint32_t rgb) {
    return M5.Display.color565((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}

struct Lock {
    Lock() { if (g_sh.mutex) xSemaphoreTake(g_sh.mutex, portMAX_DELAY); }
    ~Lock() { if (g_sh.mutex) xSemaphoreGive(g_sh.mutex); }
};

void set_status(const char* fmt, ...) {
    Lock l;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_sh.status, sizeof(g_sh.status), fmt, ap);
    va_end(ap);
}

void set_state(UiState s) {
    Lock l;
    g_sh.state = s;
}

void finish(const char* fmt, ...) {
    Lock l;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_sh.status, sizeof(g_sh.status), fmt, ap);
    va_end(ap);
    g_sh.state = UiState::FINISHED;
}

// ---------------------------------------------------------------------------
// Session task
// ---------------------------------------------------------------------------

// TOFU host-key check. Records the SHA256 fingerprint for display, then either
// proceeds (known good), prompts the user (unknown/changed), or aborts. On
// accept the key is appended to the LittleFS known_hosts file.
bool verify_knownhost(ssh_session session) {
    ssh_key srvkey = nullptr;
    if (ssh_get_server_publickey(session, &srvkey) == SSH_OK) {
        unsigned char* hash = nullptr;
        size_t hlen = 0;
        if (ssh_get_publickey_hash(srvkey, SSH_PUBLICKEY_HASH_SHA256, &hash,
                                   &hlen) == 0) {
            char* fp = ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, hash,
                                                hlen);
            {
                Lock l;
                snprintf(g_sh.fingerprint, sizeof(g_sh.fingerprint), "%s",
                         fp ? fp : "(unknown)");
            }
            if (fp) ssh_string_free_char(fp);
            ssh_clean_pubkey_hash(&hash);
        }
        ssh_key_free(srvkey);
    }

    ssh_known_hosts_e st = ssh_session_is_known_server(session);
    if (st == SSH_KNOWN_HOSTS_OK) return true;
    if (st == SSH_KNOWN_HOSTS_ERROR) {
        finish("Host key error");
        return false;
    }

    // UNKNOWN / NOT_FOUND / CHANGED / OTHER → ask the user.
    bool changed = (st == SSH_KNOWN_HOSTS_CHANGED || st == SSH_KNOWN_HOSTS_OTHER);
    set_status(changed ? "WARNING: host key changed!" : "Unknown host key");
    g_sh.hostkey_decision = 0;
    set_state(UiState::HOSTKEY);

    while (g_sh.hostkey_decision == 0 && !g_sh.stop_req) {
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    if (g_sh.stop_req) return false;
    if (g_sh.hostkey_decision != 1) {
        finish("Host key rejected");
        return false;
    }
    if (ssh_session_update_known_hosts(session) != SSH_OK) {
        finish("Could not save host key");
        return false;
    }
    return true;
}

void io_loop(ssh_channel channel) {
    static uint8_t rbuf[1024];   // static: keep these off the task's own stack
    static uint8_t wbuf[128];
    while (!g_sh.stop_req) {
        if (!ssh_channel_is_open(channel) || ssh_channel_is_eof(channel)) break;

        size_t got = xStreamBufferReceive(g_sh.tx, wbuf, sizeof(wbuf), 0);
        if (got > 0 && ssh_channel_write(channel, wbuf, got) == SSH_ERROR) break;

        int n = ssh_channel_read_nonblocking(channel, rbuf, sizeof(rbuf), 0);
        if (n > 0) {
            Lock l;
            g_sh.term.write(rbuf, (size_t)n);
        } else if (n == SSH_ERROR) {
            break;
        }
        int e = ssh_channel_read_nonblocking(channel, rbuf, sizeof(rbuf), 1);
        if (e > 0) {
            Lock l;
            g_sh.term.write(rbuf, (size_t)e);
        }
        vTaskDelay(pdMS_TO_TICKS(8));
    }
}

void run_session() {
    set_status("Connecting %s:%u", g_host, g_port);
    set_state(UiState::CONNECTING);

    static bool libssh_ready = false;
    if (!libssh_ready) {               // one-time crypto/runtime init
        libssh_begin();
        libssh_ready = true;
    }
    ssh_session session = ssh_new();
    if (!session) {
        finish("Out of memory");
        return;
    }

    ssh_options_set(session, SSH_OPTIONS_HOST, g_host);
    ssh_options_set(session, SSH_OPTIONS_USER, g_user);
    ssh_options_set(session, SSH_OPTIONS_PORT, &g_port);
    long timeout = 15;
    ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &timeout);
    int verbosity = SSH_LOG_NOLOG;
    ssh_options_set(session, SSH_OPTIONS_LOG_VERBOSITY, &verbosity);
    ssh_options_set(session, SSH_OPTIONS_KNOWNHOSTS, KNOWN_HOSTS_PATH);

    if (ssh_connect(session) != SSH_OK) {
        finish("Connect failed: %s", ssh_get_error(session));
        ssh_free(session);
        return;
    }

    if (!verify_knownhost(session)) {           // sets its own status
        ssh_disconnect(session);
        ssh_free(session);
        return;
    }

    set_status("Authenticating...");
    if (ssh_userauth_password(session, nullptr, g_pass) != SSH_AUTH_SUCCESS) {
        finish("Auth failed: %s", ssh_get_error(session));
        ssh_disconnect(session);
        ssh_free(session);
        return;
    }

    ssh_channel channel = ssh_channel_new(session);
    if (!channel || ssh_channel_open_session(channel) != SSH_OK) {
        finish("Channel open failed");
        if (channel) ssh_channel_free(channel);
        ssh_disconnect(session);
        ssh_free(session);
        return;
    }
    ssh_channel_request_pty_size(channel, "xterm", Term::COLS, Term::ROWS);
    if (ssh_channel_request_shell(channel) != SSH_OK) {
        finish("Shell request failed");
        ssh_channel_close(channel);
        ssh_channel_free(channel);
        ssh_disconnect(session);
        ssh_free(session);
        return;
    }

    {
        Lock l;
        g_sh.term.reset();
        g_sh.status[0] = '\0';
        g_sh.state = UiState::TERMINAL;
    }

    io_loop(channel);

    finish("Session closed");
    ssh_channel_send_eof(channel);
    ssh_channel_close(channel);
    ssh_channel_free(channel);
    ssh_disconnect(session);
    ssh_free(session);
}

void session_task(void*) {
    g_sh.task_running = true;
    run_session();
    g_sh.task_running = false;
    vTaskDelete(nullptr);
}

void shutdown_session() {
    if (g_sh.task) {
        g_sh.stop_req = true;
        // io_loop checks stop_req every ~8 ms; a blocking ssh_connect /
        // userauth can only be interrupted by its own 15 s timeout, so give it
        // a generous window.
        uint32_t t0 = millis();
        while (g_sh.task_running && millis() - t0 < 16000) delay(20);
        if (g_sh.task_running) {
            // Still stuck in a blocking call. Don't free the task handle or the
            // stream buffer it may still read — that would be a use-after-free.
            // start_session() reclaims on the next attempt once it has exited.
            return;
        }
        g_sh.task = nullptr;
    }
    if (g_sh.tx) {
        vStreamBufferDelete(g_sh.tx);
        g_sh.tx = nullptr;
    }
}

void start_session() {
    if (WiFi.status() != WL_CONNECTED) {
        finish("No WiFi - connect in Settings");
        return;
    }
    shutdown_session();                       // belt-and-braces: clear any prior

    g_sh.stop_req = false;
    g_sh.hostkey_decision = 0;
    g_sh.tx = xStreamBufferCreate(512, 1);
    if (!g_sh.tx) {
        finish("Out of memory");
        return;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(session_task, "ssh", SSH_TASK_STACK,
                                            nullptr, 3, &g_sh.task, 0);
    if (ok != pdPASS) {
        g_sh.task = nullptr;
        vStreamBufferDelete(g_sh.tx);
        g_sh.tx = nullptr;
        finish("Not enough memory for SSH");
    }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void paint_form() {
    M5.Display.fillRect(0, theme::CONTENT_TOP, theme::SCREEN_W, theme::CONTENT_H,
                        to565(theme::IVORY));
    M5.Display.setTextColor(to565(theme::ORANGE), to565(theme::IVORY));
    M5.Display.setTextSize(1.4f);
    M5.Display.setTextDatum(top_left);
    M5.Display.drawString("SSH", 6, theme::CONTENT_TOP + 2);

    struct Row { const char* label; const char* val; bool mask; };
    char masked[64];
    int plen = (int)strlen(f_pass);
    for (int i = 0; i < plen && i < 63; ++i) masked[i] = '*';
    masked[plen < 63 ? plen : 63] = '\0';
    Row rows[4] = {
        {"Host", f_host, false},
        {"User", f_user, false},
        {"Pass", masked, true},
        {"Port", f_port, false},
    };

    int y = theme::CONTENT_TOP + 22;
    const int row_h = 20;
    for (int i = 0; i < 4; ++i) {
        bool active = (i == f_field);
        M5.Display.setTextColor(to565(theme::MID_GRAY), to565(theme::IVORY));
        M5.Display.setTextSize(1.0f);
        M5.Display.drawString(rows[i].label, 6, y + 3);

        const int fx = 56, fw = theme::SCREEN_W - fx - 6, fh = 16;
        M5.Display.drawRect(fx, y, fw, fh,
                            to565(active ? theme::ORANGE : theme::LIGHT_GRAY));
        char shown[80];
        snprintf(shown, sizeof(shown), "%s%s", rows[i].val, active ? "_" : "");
        M5.Display.setTextColor(to565(theme::DARK), to565(theme::IVORY));
        M5.Display.setTextSize(1.0f);
        M5.Display.drawString(shown, fx + 4, y + 4);
        y += row_h;
    }

    M5.Display.setTextColor(to565(theme::MID_GRAY), to565(theme::IVORY));
    M5.Display.setTextSize(1.0f);
    M5.Display.setTextDatum(bottom_left);
    M5.Display.drawString("Enter:next/connect  `:back", 4, theme::SCREEN_H - 2);
    M5.Display.setTextDatum(top_left);
}

// Centered banner used for CONNECTING / HOSTKEY / FINISHED.
void paint_banner(UiState st, const char* status, const char* fp) {
    M5.Display.fillRect(0, theme::CONTENT_TOP, theme::SCREEN_W, theme::CONTENT_H,
                        to565(theme::IVORY));
    uint32_t color = (st == UiState::FINISHED || st == UiState::HOSTKEY)
                         ? theme::ORANGE
                         : theme::DARK;
    M5.Display.setTextColor(to565(color), to565(theme::IVORY));
    M5.Display.setTextSize(1.2f);
    M5.Display.setTextDatum(top_center);
    M5.Display.drawString(status, theme::SCREEN_W / 2, theme::CONTENT_TOP + 8);

    if (st == UiState::HOSTKEY) {
        M5.Display.setTextColor(to565(theme::DARK), to565(theme::IVORY));
        M5.Display.setTextSize(1.0f);
        M5.Display.setTextDatum(top_left);
        // SHA256 fingerprints are long; wrap onto two lines.
        char l1[33], l2[33];
        snprintf(l1, sizeof(l1), "%s", fp);
        l2[0] = '\0';
        if (strlen(fp) > 32) snprintf(l2, sizeof(l2), "%s", fp + 32);
        M5.Display.drawString(l1, 6, theme::CONTENT_TOP + 34);
        M5.Display.drawString(l2, 6, theme::CONTENT_TOP + 46);
        M5.Display.setTextColor(to565(theme::MID_GRAY), to565(theme::IVORY));
        M5.Display.setTextDatum(bottom_center);
        M5.Display.drawString("y: accept   n: reject", theme::SCREEN_W / 2,
                              theme::SCREEN_H - 2);
    } else if (st == UiState::FINISHED) {
        M5.Display.setTextColor(to565(theme::MID_GRAY), to565(theme::IVORY));
        M5.Display.setTextSize(1.0f);
        M5.Display.setTextDatum(bottom_center);
        M5.Display.drawString("Enter: new   `: back", theme::SCREEN_W / 2,
                              theme::SCREEN_H - 2);
    }
    M5.Display.setTextDatum(top_left);
}

// ---------------------------------------------------------------------------
// Keyboard
// ---------------------------------------------------------------------------

void tx_push(const void* data, size_t len) {
    if (g_sh.tx) xStreamBufferSend(g_sh.tx, data, len, 0);
}

void connect_from_form() {
    if (f_host[0] == '\0' || f_user[0] == '\0') {
        finish("Host and user required");
        return;
    }
    snprintf(g_host, sizeof(g_host), "%s", f_host);
    snprintf(g_user, sizeof(g_user), "%s", f_user);
    snprintf(g_pass, sizeof(g_pass), "%s", f_pass);
    int port = atoi(f_port);
    g_port = (port > 0 && port < 65536) ? (unsigned int)port : 22;

    // Persist the destination (not the password) so the form pre-fills.
    settings::store().ssh_host = f_host;
    settings::store().ssh_user = f_user;
    settings::store().ssh_port = (uint16_t)g_port;
    settings::save();

    start_session();
}

void form_keys() {
    auto status = M5Cardputer.Keyboard.keysState();
    char* buf;
    size_t cap;
    switch (f_field) {
        case 0: buf = f_host; cap = sizeof(f_host); break;
        case 1: buf = f_user; cap = sizeof(f_user); break;
        case 2: buf = f_pass; cap = sizeof(f_pass); break;
        default: buf = f_port; cap = sizeof(f_port); break;
    }
    int len = (int)strlen(buf);

    if (status.enter) {
        if (f_field < 3) {
            f_field++;
        } else {
            connect_from_form();
        }
        return;
    }
    if (status.del) {
        if (len > 0) buf[len - 1] = '\0';
        return;
    }
    for (char k : status.word) {
        if (k == '`') {
            app::goto_screen(app::Screen::LAUNCHER);
            return;
        }
        if (k < 0x20 || k >= 0x7f || k == ' ') continue;
        if (f_field == 3 && !isdigit((unsigned char)k)) continue;  // port = digits
        if (len < (int)cap - 1) {
            buf[len++] = k;
            buf[len] = '\0';
        }
    }
}

void terminal_keys() {
    auto status = M5Cardputer.Keyboard.keysState();
    bool fn = status.fn;

    if (status.enter) { uint8_t c = '\r'; tx_push(&c, 1); }
    if (status.del)   { uint8_t c = 0x7f; tx_push(&c, 1); }   // DEL
    if (status.tab)   { uint8_t c = '\t'; tx_push(&c, 1); }

    for (char k : status.word) {
        if (k == '`') {
            if (fn) {                          // Fn + esc/` = leave the app
                g_want_leave = true;
            } else {                           // bare esc/` = send Esc
                uint8_t e = 0x1b;
                tx_push(&e, 1);
            }
            continue;
        }
        if (fn) {                              // Fn + ; . , / = arrow keys
            const char* seq = nullptr;
            switch (k) {
                case ';': seq = "\x1b[A"; break;   // up
                case '.': seq = "\x1b[B"; break;   // down
                case ',': seq = "\x1b[D"; break;   // left
                case '/': seq = "\x1b[C"; break;   // right
            }
            if (seq) { tx_push(seq, 3); continue; }
        }
        if (status.ctrl && isalpha((unsigned char)k)) {
            uint8_t c = (uint8_t)(toupper((unsigned char)k) & 0x1f);
            tx_push(&c, 1);
            continue;
        }
        if (k >= 0x20 && k < 0x7f) {
            uint8_t c = (uint8_t)k;
            tx_push(&c, 1);
        }
    }
}

}  // namespace

void enter() {
    if (!g_sh.mutex) g_sh.mutex = xSemaphoreCreateMutex();

    // Pre-fill from the last destination; never restore the password.
    snprintf(f_host, sizeof(f_host), "%s", settings::store().ssh_host.c_str());
    snprintf(f_user, sizeof(f_user), "%s", settings::store().ssh_user.c_str());
    snprintf(f_port, sizeof(f_port), "%u",
             (unsigned)(settings::store().ssh_port ? settings::store().ssh_port : 22));
    f_pass[0] = '\0';
    f_field = 0;

    g_sh.state = UiState::FORM;
    g_sh.status[0] = '\0';
    g_want_leave = false;

    g_last_state = UiState::FINISHED;          // force first paint
    g_last_status[0] = '\x01';
    paint_form();
    g_last_state = UiState::FORM;
}

void tick() {
    // --- repaint -----------------------------------------------------------
    UiState st;
    char status_copy[96];
    char fp_copy[200];
    {
        Lock l;
        st = g_sh.state;
        snprintf(status_copy, sizeof(status_copy), "%s", g_sh.status);
        snprintf(fp_copy, sizeof(fp_copy), "%s", g_sh.fingerprint);
    }

    if (st == UiState::TERMINAL) {
        if (g_last_state != UiState::TERMINAL) {
            M5.Display.fillRect(0, theme::CONTENT_TOP, theme::SCREEN_W,
                                theme::CONTENT_H, to565(theme::IVORY));
            Lock l;
            g_sh.term.mark_all_dirty();
        }
        Lock l;
        g_sh.term.render(0, theme::CONTENT_TOP);
    } else if (st == UiState::FORM) {
        if (g_last_state != UiState::FORM) paint_form();
    } else {
        if (st != g_last_state || strcmp(status_copy, g_last_status) != 0) {
            paint_banner(st, status_copy, fp_copy);
        }
    }
    g_last_state = st;
    snprintf(g_last_status, sizeof(g_last_status), "%s", status_copy);

    // --- input -------------------------------------------------------------
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        auto status = M5Cardputer.Keyboard.keysState();
        switch (st) {
            case UiState::FORM:
                form_keys();
                break;
            case UiState::CONNECTING:
                for (char k : status.word)
                    if (k == '`') g_want_leave = true;
                break;
            case UiState::HOSTKEY:
                if (status.enter) {
                    g_sh.hostkey_decision = 1;
                } else {
                    for (char k : status.word) {
                        if (k == 'y' || k == 'Y') g_sh.hostkey_decision = 1;
                        else if (k == 'n' || k == 'N' || k == '`')
                            g_sh.hostkey_decision = -1;
                    }
                }
                break;
            case UiState::TERMINAL:
                terminal_keys();
                break;
            case UiState::FINISHED:
                if (status.enter) {
                    shutdown_session();
                    enter();                    // back to a fresh form
                    return;
                }
                for (char k : status.word)
                    if (k == '`') g_want_leave = true;
                break;
        }
    }

    if (g_want_leave) {
        g_want_leave = false;
        shutdown_session();
        app::goto_screen(app::Screen::LAUNCHER);
    }
}

}  // namespace ssh
}  // namespace apps
