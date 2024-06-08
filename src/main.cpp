#include <cstdio>
#include <termios.h>
#include <unistd.h>
#include <cstdlib>
#include <ctype.h>
#include <sys/ioctl.h>
#include <cerrno>
#include <cstdint>
#include <string>
#include <vector>
#include <fstream>
#include <cassert>

#include <fmt/format.h>

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

struct EditorRow {
    std::string data;

    int len() {
        return (int)data.size();
    }
};

struct EditorConfig {
    int screenrows;
    int screencols;
    int cx, cy;
    int rowoff;
    int coloff;
    EditorMode mode;

    termios ogtermios;
    std::string abuf;
    std::vector<EditorRow*> rows;
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

    void error_exit_from(const char* from) {
        disable_raw_mode();
        perror(from);
        exit(1);
    }

    void error_exit_with_msg(const char* s) {
        disable_raw_mode();
        fputs(s, stderr);
        fputs("\n", stderr);
        exit(1);
    }
}

void enable_raw_mode() {
    write(STDOUT_FILENO, "\x1b[?1049h", 8);
    if (tcgetattr(STDIN_FILENO, &E.ogtermios) == -1)
        core::error_exit_from("tcgetattr");

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
        core::error_exit_from("tcsetattr");
}

int read_key() {
    char c;
    int nread;
    while ((nread = read(STDIN_FILENO, &c, 1)) == 0);
    if (nread == -1 && errno != EAGAIN) core::error_exit_from("read");

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
        *rows = ws.ws_row-1; // TODO: temporary
    }
    return 0;
}

void open_file(const std::string& path) {
    std::ifstream f(path);
    std::string line;

    if (!f) core::error_exit_with_msg("file not found");
    while (std::getline(f, line)) {
        EditorRow* row = new EditorRow();
        row->data = line;
        E.rows.push_back(row);
    }
}

// =========== high level ==============

void ewrite(const std::string& str) {
    E.abuf.append(str);
}

void ewrite_cstr_with_len(const char* str, usize len) {
    E.abuf.append(str, len);
}

void ewrite_with_len(const std::string& str, usize len) {
    E.abuf.append(str, 0, len);
}

void do_action(EditorAction a) {
    EditorRow* row = (E.cy >= (int)E.rows.size()) ? NULL : E.rows[E.cy];
    switch (a) {
        case CURSOR_LEFT: {
            if (E.cx != 0) E.cx--;
            else if (E.cy > 0) {
                E.cy--;
                E.cx = E.rows[E.cy]->len();
            }
        } break;
        case CURSOR_RIGHT: {
            if (row && E.cx < row->len()) E.cx++;
            else if (row && E.cx == row->len()) {
                E.cy++;
                E.cx = 0;
            }
        } break;
        case CURSOR_UP:    if (E.cy != 0) E.cy--; break;
        case CURSOR_DOWN:  if (E.cy < (int)E.rows.size()) E.cy++; break;
        case CURSOR_LINE_BEGIN: E.cx = 0; break;
        case CURSOR_LINE_END: E.cx = E.screencols-1; break;
        case EDITOR_EXIT:  core::succ_exit(); break;
        default: assert(0 && "do_action(): unknown action");
    }

    row = (E.cy >= (int)E.rows.size()) ? NULL : E.rows[E.cy];
    int rowlen = row ? row->len() : 0;
    if (E.cx > rowlen) E.cx = rowlen;
}

void process_keypress() {
    int c = read_key();
    if (E.mode == NORMAL) {
        switch (c) {
            case CTRL_KEY('q'): do_action(EDITOR_EXIT); break;
            case CTRL_KEY('d'):
            case CTRL_KEY('e'): {
                int times = E.screenrows;
                while (times--)
                    do_action(c == CTRL_KEY('e') ? CURSOR_UP : CURSOR_DOWN);
            } break;
            case 'a': do_action(CURSOR_LINE_BEGIN); break;
            case ';': do_action(CURSOR_LINE_END); break;
            case ARROW_LEFT:  do_action(CURSOR_LEFT); break;
            case ARROW_RIGHT: do_action(CURSOR_RIGHT); break;
            case ARROW_UP:    do_action(CURSOR_UP); break;
            case ARROW_DOWN:  do_action(CURSOR_DOWN); break;
            case 'h': do_action(CURSOR_LEFT); break;
            case 'j': do_action(CURSOR_DOWN); break;
            case 'k': do_action(CURSOR_UP); break;
            case 'l': do_action(CURSOR_RIGHT); break;
            default: core::error_exit_with_msg("process_keypress(): unknown key");
        }

    } else if (E.mode == INSERT) {

    }
}

void scroll() {
    if (E.cy < E.rowoff) {
        E.rowoff = E.cy;
    }
    if (E.cy >= E.rowoff + E.screenrows) {
        E.rowoff = E.cy - E.screenrows + 1;
    }
    if (E.cx < E.coloff) {
        E.coloff = E.cx;
    }
    if (E.cx >= E.coloff + E.screencols) {
        E.coloff = E.cx - E.screencols + 1;
    }
}

void draw_rows() {
    for (int y = 0; y < E.screenrows; y++) {
        int filerow = y + E.rowoff;
        if (filerow >= (int)E.rows.size()) {
            if (E.rows.size() == 0 && y == E.screenrows / 3) {
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

        } else {
            int rowlen = E.rows[filerow]->len() - E.coloff;
            if (rowlen < 0) rowlen = 0;
            if (rowlen > E.screencols) rowlen = E.screencols;
            ewrite_cstr_with_len(&E.rows[filerow]->data.data()[E.coloff], rowlen);
        }

        ewrite("\x1b[K");
        //if (y < E.screenrows-1) { // TODO: temp
            ewrite("\r\n");
        //}
    }

    std::string debug_info = fmt::format("cx: {}, cy = {}, rowoff: {}", E.cx, E.cy, E.rowoff);
    int len = debug_info.size();
    if (len > E.screencols) len = E.screencols;
    ewrite_with_len(debug_info, len);
    ewrite("\x1b[K");
}

void refresh_screen() {
    scroll();
    E.abuf.clear();
    ewrite("\x1b[?25l");
    ewrite("\x1b[H");

    draw_rows();

    char buf[32];
    usize len = snprintf(
        buf,
        sizeof(buf)-1,
        "\x1b[%d;%dH",
        (E.cy-E.rowoff)+1,
        (E.cx-E.coloff)+1);
    ewrite(std::string(buf, 0, len));
    ewrite("\x1b[?25h");

    write(STDOUT_FILENO, E.abuf.data(), E.abuf.size());
}

void init_editor() {
    E.cx = 0;
    E.cy = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.mode = NORMAL;
    if (get_window_size(&E.screenrows, &E.screencols) == -1)
        core::error_exit_from("get_window_size");
    E.abuf.reserve(5*1024);
}

int main(int argc, char** argv) {
    enable_raw_mode();
    init_editor();
    if (argc >= 2) {
        open_file(argv[1]);
    }

    while (1) {
        refresh_screen();
        process_keypress();
    }

    return 0;
}

