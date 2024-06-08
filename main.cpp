#include <cstdio>
#include <termios.h>
#include <unistd.h>
#include <cstdlib>
#include <ctype.h>
#include <sys/ioctl.h>
#include <cerrno>
#include <cstdint>
#include <string>
#include <cassert>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef size_t usize;

typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;
typedef ssize_t isize;

enum EditorMode {
    NORMAL,
    INSERT,
};

enum EditorAction {
    CURSOR_UP,
    CURSOR_DOWN,
    CURSOR_LEFT,
    CURSOR_RIGHT,
    CURSOR_LINE_BEGIN,
    CURSOR_LINE_END,
    EDITOR_EXIT,
};

enum EditorKey {
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
};

struct EditorConfig {
    termios ogtermios;
    int screenrows;
    int screencols;
    int cx, cy;
    EditorMode mode;
    std::string abuf;
};
EditorConfig E;

#define CTRL_KEY(k) ((k) & 0x1f)

void disable_raw_mode() {
    write(STDOUT_FILENO, "\x1b[?1049l", 8);
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.ogtermios) == -1) {
        perror("tcsetattr");
        exit(1);
    }
}

namespace core {
    void succ_exit() {
        disable_raw_mode();
        exit(0);
    }

    void error_exit(const char* s) {
        disable_raw_mode();
        perror(s);
        exit(1);
    }
}

void enable_raw_mode() {
    write(STDOUT_FILENO, "\x1b[?1049h", 8);
    if (tcgetattr(STDIN_FILENO, &E.ogtermios) == -1)
        core::error_exit("tcgetattr");

    termios raw = E.ogtermios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
#ifdef _DEBUG
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN);
#else
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
#endif
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        core::error_exit("tcsetattr");
}

int read_key() {
    char c;
    int nread;
    while ((nread = read(STDIN_FILENO, &c, 1)) == 0);
    if (nread == -1 && errno != EAGAIN) core::error_exit("read");

    if (c == '\x1b') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if (seq[0] == '[') {
            switch (seq[1]) {
                case 'A': return ARROW_UP;
                case 'B': return ARROW_DOWN;
                case 'C': return ARROW_RIGHT;
                case 'D': return ARROW_LEFT;
            }
        }
        return '\x1b';
    }
    return c;
}

int get_cursor_position(int* rows, int* cols) {
    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

    char buf[32];
    u32 i = 0;
    while (i < sizeof(buf)-1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }
    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;
    return 0;
}

int get_window_size(int* rows, int* cols) {
    winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        return get_cursor_position(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
    }
    return 0;
}

// =========== high level ==============

void ewrite(const std::string& str) {
    E.abuf.append(str);
}

void ewrite_with_len(const std::string& str, usize len) {
    E.abuf.append(str, 0, len);
}

void do_action(EditorAction a) {
    switch (a) {
        case CURSOR_LEFT:  if (E.cx != 0) E.cx--; break;
        case CURSOR_RIGHT: if (E.cx != E.screencols-1) E.cx++; break;
        case CURSOR_UP:    if (E.cy != 0) E.cy--; break;
        case CURSOR_DOWN:  if (E.cy != E.screenrows-1) E.cy++; break;
        case CURSOR_LINE_BEGIN: E.cx = 0; break;
        case CURSOR_LINE_END: E.cx = E.screencols-1; break;
        case EDITOR_EXIT:  core::succ_exit(); break;
        default: assert(0 && "do_action(): unknown action");
    }
}

void process_keypress() {
    int c = read_key();
    if (E.mode == NORMAL) {
        switch (c) {
            case CTRL_KEY('q'): do_action(EDITOR_EXIT); break;
            case CTRL_KEY('f'):
            case CTRL_KEY('c'): {
                int times = E.screenrows;
                while (times--)
                    do_action(c == CTRL_KEY('f') ? CURSOR_UP : CURSOR_DOWN);
            } break;
            case 'h': do_action(CURSOR_LINE_BEGIN); break;
            case ';': do_action(CURSOR_LINE_END); break;
            case ARROW_LEFT:  do_action(CURSOR_LEFT); break;
            case ARROW_RIGHT: do_action(CURSOR_RIGHT); break;
            case ARROW_UP:    do_action(CURSOR_UP); break;
            case ARROW_DOWN:  do_action(CURSOR_DOWN); break;
            default: assert(0 && "process_keypress(): unknown key");
        }

    } else if (E.mode == INSERT) {

    }
}

void draw_rows() {
    for (int y = 0; y < E.screenrows; y++) {
        if (y == E.screenrows / 3) {
            std::string welcome = "Unnamed editor -- version 0.0.1";
            usize len = welcome.size();
            if (len > (usize)E.screencols) len = E.screencols;

            usize padding = (E.screencols - len) / 2;
            if (padding) {
                ewrite("~");
                padding--;
            }
            while (padding) {
                ewrite(" ");
                padding--;
            }

            ewrite_with_len(welcome, len);
        } else {
            ewrite("~");
        }
        ewrite("\x1b[K");
        if (y < E.screenrows-1) {
            ewrite("\r\n");
        }
    }
}

void refresh_screen() {
    E.abuf.clear();
    ewrite("\x1b[?25l");
    ewrite("\x1b[H");

    draw_rows();

    char buf[32];
    usize len = snprintf(buf, sizeof(buf)-1, "\x1b[%d;%dH", E.cy+1, E.cx+1);
    ewrite(std::string(buf, 0, len));
    ewrite("\x1b[?25h");

    write(STDOUT_FILENO, E.abuf.data(), E.abuf.size());
}

void init_editor() {
    E.cx = 0;
    E.cy = 0;
    if (get_window_size(&E.screenrows, &E.screencols) == -1)
        core::error_exit("get_window_size");
    E.abuf.reserve(5*1024);
}

int main() {
    enable_raw_mode();
    init_editor();

    while (1) {
        refresh_screen();
        process_keypress();
    }

    return 0;
}

