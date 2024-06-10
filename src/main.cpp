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
#include <ctime>

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

const int TAB_STOP = 4;
const int NUM_FORCE_QUIT_PRESS = 3;

bool str_startswith(const std::string& str, const std::string& startswith) {
    return str.rfind(startswith, 0) == 0;
}

enum EditorMode {
    NORMAL,
    INSERT,
    COMMAND,
};

enum EditorAction {
    CURSOR_UP,
    CURSOR_DOWN,
    CURSOR_LEFT,
    CURSOR_RIGHT,
    CURSOR_LINE_BEGIN,
    CURSOR_LINE_END,
    MODE_CHANGE_NORMAL,
    MODE_CHANGE_INSERT,
    MODE_CHANGE_COMMAND,
    NEWLINE_INSERT,
    LEFT_CHAR_DELETE,
    FILE_SAVE,
    EDITOR_EXIT,
};

enum EditorKey {
    BACKSPACE = 127,

    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    ALT_M,
};

struct EditorRow {
    std::string data;
    std::string rdata;

    int len() {
        return (int)data.size();
    }

    int rlen() {
        return (int)rdata.size();
    }
};

struct EditorConfig {
    int screenrows;
    int screencols;
    int cx, cy, rx;
    int rowoff;
    int coloff;
    EditorMode mode;
    std::string path;
    bool dirty;

    termios ogtermios;
    std::string abuf;
    std::vector<EditorRow*> rows;
    std::string cmdline;
    time_t cmdline_msg_time;
    int quit_times;

    std::ofstream keylog;

    int numrows() {
        return (int)rows.size();
    }

    EditorRow* get_row_at(int at) {
        if (at < 0 || at >= numrows()) return NULL;
        return rows[at];
    }
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
    char buf[64];
    int nread;
    while ((nread = read(STDIN_FILENO, buf, 64)) == 0);
    if (nread == -1 && errno != EAGAIN) core::error_exit_from("read");

    for (int i = 0; i < nread; i++) {
        switch (buf[i]) {
            case '\x1b': E.keylog << "[esc]"; break;
            case BACKSPACE: E.keylog << "[bksp]"; break;
            case '\r': E.keylog << "[nl]"; break;
            default: E.keylog << buf[i]; break;
        }
        E.keylog << ' ';
    }
    E.keylog << '\n';

    if (buf[0] == '\x1b' && nread == 1) {
        return '\x1b';
    } else if (nread > 1) {
        if (buf[1] == '[') {
            switch (buf[2]) {
                case 'A': return ARROW_UP;
                case 'B': return ARROW_DOWN;
                case 'C': return ARROW_RIGHT;
                case 'D': return ARROW_LEFT;
            }
        } else {
            switch (buf[1]) {
                case 'm': return ALT_M;
            }
        }
        return '\x1b';
    }
    return buf[0];
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
        // one line for status bar
        //*rows = ws.ws_row-2;
        *rows = ws.ws_row-3; // TODO: debug_temp
    }
    return 0;
}

int row_cx_to_rx(EditorRow* row, int cx) {
    if (!row) return 0;
    int rx = 0;
    for (int i = 0; i < cx; i++) {
        if (row->data[i] == '\t') {
            rx += (TAB_STOP-1) - (rx%TAB_STOP);
        }
        rx++;
    }
    return rx;
}

int row_rx_to_cx(EditorRow* row, int rx) {
    if (!row) return 0;
    int cx = 0;
    for (int i = 0; i < rx; i++) {
        if (cx > (row->len())) {
            cx = row->len();
            break;
        }
        if (row->data[cx] == '\t') {
            i += (TAB_STOP-1) - (i%TAB_STOP);
            if (i >= rx) break;
        }
        cx++;
    }
    return cx;
}

void update_row(EditorRow* row) {
    row->rdata.reserve(100);
    row->rdata.clear();
    for (int i = 0; i < row->len(); i++) {
        if (row->data[i] == '\t') {
            row->rdata.push_back(' ');
            while (row->rlen() % TAB_STOP != 0) {
                row->rdata.push_back(' ');
            }
        } else {
            row->rdata.push_back(row->data[i]);
        }
    }
    row->rdata.push_back('\0');
    E.dirty = true;
}

void insert_row(int at, const std::string& data) {
    if (at < 0 || at > E.numrows()) return;
    EditorRow* row = new EditorRow();
    row->data = data;
    E.rows.insert(E.rows.begin() + at, row);
    update_row(row);
}

void free_row(EditorRow* row) {
    delete row;
}

void delete_row(int at) {
    if (at < 0 || at > E.numrows()) return;
    EditorRow* row = E.get_row_at(at);
    free_row(row);
    E.rows.erase(E.rows.begin() + at);
    E.dirty = true;
}

void row_insert_char(EditorRow* row, int at, int c) {
    if (at < 0 || at > row->len()) at = row->len();
    row->data.insert(at, 1, c);
    update_row(row);
}

void row_delete_char(EditorRow* row, int at) {
    if (at < 0 || at >= row->len()) return;
    row->data.erase(at, 1);
    update_row(row);
}

void row_append_string(EditorRow* row, const std::string& str) {
    row->data += str;
    update_row(row);
}

template<typename... Args>
void set_cmdline_msg(const std::string& fmt, Args... args) {
    E.cmdline = fmt::format(fmt, args...);
    E.cmdline_msg_time = time(NULL);
}

std::string rows_to_string() {
    std::string res;
    for (int i = 0; i < E.numrows(); i++) {
        res.append(E.get_row_at(i)->data);
        res.append("\n");
    }
    return res;
}

void save_file() {
    if (E.path == "") {
        set_cmdline_msg("no filename");
        return;
    }
    std::string tmp_path = E.path + ".tmp";
    std::ofstream f(tmp_path);
    if (!f) set_cmdline_msg("cannot open file for saving");

    std::string contents = rows_to_string();
    f << contents;
    f.close();
    if (!f) set_cmdline_msg("cannot write to file for saving");
    system(std::string("mv " + tmp_path + " " + E.path).c_str());
    set_cmdline_msg("{} bytes written", contents.size());
    E.dirty = false;
}

void open_file(const std::string& path) {
    std::ifstream f(path);
    std::string line;

    if (!f) core::error_exit_with_msg("file not found");
    while (std::getline(f, line)) {
        insert_row(E.numrows(), line);
    }
    E.path = path;
    E.dirty = false;
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

void insert_char(int c) {
    if (E.cy == E.numrows()) {
        insert_row(E.numrows(), "");
    }
    row_insert_char(E.get_row_at(E.cy), E.cx, c);
    E.cx++;
}

void insert_newline() {
    if (E.cx == 0) {
        insert_row(E.cy, "");
    } else {
        EditorRow* row = E.get_row_at(E.cy);
        insert_row(E.cy+1, row->data.substr(E.cx, row->len()-E.cx));
        row->data = row->data.substr(0, E.cx);
        update_row(row);
    }
    E.cy++;
    E.cx = 0;
}

void delete_char() {
    if (E.cy == E.numrows()) return;
    if (E.cx == 0 && E.cy == 0) return;

    EditorRow* row = E.get_row_at(E.cy);
    if (E.cx > 0) {
        row_delete_char(row, E.cx-1);
        E.cx--;
    } else {
        E.cx = E.get_row_at(E.cy-1)->len();
        row_append_string(E.get_row_at(E.cy-1), row->data);
        delete_row(E.cy);
        E.cy--;
    }
}

void do_action(EditorAction a) {
    EditorRow* row = E.get_row_at(E.cy);
    switch (a) {
        case CURSOR_UP: {
            if (E.cy != 0) E.cy--;
            if (E.cy < E.numrows()) {
                // We calculate E.cx from E.rx and update it
                // instead of directly updating E.rx
                // because E.rx is calculated on
                // every refresh (throwing the prev value away).
                // So we "choose" a E.cx which
                // will be converted to the needed E.rx in the
                // refresh stage.
                E.cx = row_rx_to_cx(E.get_row_at(E.cy), E.rx);
            }
        } break;

        case CURSOR_DOWN: {
            if (E.cy < E.numrows()) E.cy++;
            if (E.cy < E.numrows()) {
                E.cx = row_rx_to_cx(E.get_row_at(E.cy), E.rx);
            }
        } break;

        case CURSOR_LEFT: {
            if (E.cx != 0) E.cx--;
            else if (E.cy > 0) {
                E.cy--;
                E.cx = E.get_row_at(E.cy)->len();
            }
        } break;

        case CURSOR_RIGHT: {
            if (row && E.cx < row->len()) E.cx++;
            else if (row && E.cx == row->len()) {
                E.cy++;
                E.cx = 0;
            }
        } break;

        case CURSOR_LINE_BEGIN: E.cx = 0; break;
        case CURSOR_LINE_END: if (row) E.cx = row->len(); break;

        case MODE_CHANGE_NORMAL: {
            E.mode = NORMAL;
            E.cmdline = "";
        } break;

        case MODE_CHANGE_INSERT: E.mode = INSERT; break;

        case MODE_CHANGE_COMMAND: {
            E.mode = COMMAND;
            E.cmdline = "";
        } break;

        case NEWLINE_INSERT: insert_newline(); break;
        case LEFT_CHAR_DELETE: delete_char(); break;

        case FILE_SAVE: save_file(); break;

        case EDITOR_EXIT: {
            if (E.dirty && E.quit_times > 0) {
                set_cmdline_msg("File has unsaved changes: press C-Q {} more times to quit", E.quit_times);
                E.quit_times--;
            } else {
                core::succ_exit();
            }
        } return; // Always return from EDITOR_EXIT

        default: assert(0 && "do_action(): unknown action");
    }

    E.quit_times = NUM_FORCE_QUIT_PRESS;
    row = E.get_row_at(E.cy);
    int rowlen = row ? row->len() : 0;
    if (E.cx > rowlen) E.cx = rowlen;
}

void process_keypress() {
    int c = read_key();
    if (E.mode == NORMAL) {
        switch (c) {
            case 'i': do_action(MODE_CHANGE_INSERT); break;
            case CTRL_KEY('q'): do_action(EDITOR_EXIT); break;
            case CTRL_KEY('s'): do_action(FILE_SAVE); break;
            case CTRL_KEY('d'):
            case CTRL_KEY('e'): {
                if (c == CTRL_KEY('e')) {
                    E.cy = E.rowoff;
                } else if (c == CTRL_KEY('d')) {
                    E.cy = E.rowoff + E.screenrows - 1;
                    if (E.cy > E.numrows()) {
                        E.cy = E.numrows();
                    }
                }

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
            case 'w':
                do_action(CURSOR_RIGHT);
                do_action(LEFT_CHAR_DELETE);
                break;
            case ALT_M: do_action(MODE_CHANGE_COMMAND);
            case BACKSPACE: break;
            case '\r': break;
            case '\x1b': break;
            default: {
                set_cmdline_msg("invalid key '{}' in normal mode", (int)c);
            } break;
        }

    } else if (E.mode == INSERT) {
        switch (c) {
            case BACKSPACE: do_action(LEFT_CHAR_DELETE); break;
            case '\r':      do_action(NEWLINE_INSERT); break;
            case ARROW_LEFT:  do_action(CURSOR_LEFT); break;
            case ARROW_RIGHT: do_action(CURSOR_RIGHT); break;
            case ARROW_UP:    do_action(CURSOR_UP); break;
            case ARROW_DOWN:  do_action(CURSOR_DOWN); break;
            case '\x1b': do_action(MODE_CHANGE_NORMAL); break;
            default: {
                if (c >= 32 && c <= 126) insert_char(c);
                else set_cmdline_msg("non-printable key '{}' in insert mode", (int)c);
            } break;
        }

    } else if (E.mode == COMMAND) {
        switch (c) {
            case '\r': {
                std::string cmd = E.cmdline;
                do_action(MODE_CHANGE_NORMAL);

                if (cmd == "quit") do_action(EDITOR_EXIT);
                else if (str_startswith(cmd, "path")) {
                    E.path = cmd.substr(5);
                }
                else set_cmdline_msg("unknown command '{}'", cmd);
            } break;

            case '\x1b': do_action(MODE_CHANGE_NORMAL); break;

            default: {
                if (c >= 32 && c <= 126) E.cmdline += c;
            } break;
        }
    }
}

void scroll() {
    E.rx = 0;
    if (E.cy < E.numrows()) {
        E.rx = row_cx_to_rx(E.get_row_at(E.cy), E.cx);
    }

    if (E.cy < E.rowoff) {
        E.rowoff = E.cy;
    }
    if (E.cy >= E.rowoff + E.screenrows) {
        E.rowoff = E.cy - E.screenrows + 1;
    }
    if (E.rx < E.coloff) {
        E.coloff = E.rx;
    }
    if (E.rx >= E.coloff + E.screencols) {
        E.coloff = E.rx - E.screencols + 1;
    }
}

void draw_rows() {
    for (int y = 0; y < E.screenrows; y++) {
        int filerow = y + E.rowoff;
        if (filerow >= E.numrows()) {
            if (E.numrows() == 0 && y == E.screenrows / 3) {
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
            int rowlen = E.get_row_at(filerow)->rlen() - E.coloff;
            if (rowlen < 0) rowlen = 0;
            if (rowlen > E.screencols) rowlen = E.screencols;
            ewrite_cstr_with_len(&E.get_row_at(filerow)->rdata.data()[E.coloff], rowlen);
        }

        ewrite("\x1b[K");
        if (y < E.screenrows-1) {
            ewrite("\r\n");
        }
    }
}

void draw_status_bar() {
    ewrite("\r\n");
    if (E.mode == NORMAL || E.mode == COMMAND) {
        ewrite("\x1b[1;47;30m");
    } else {
        ewrite("\x1b[1;44;30m");
    }

    std::string lstatus = fmt::format(
            "[{}{}] {:.20}",
            E.dirty ? '*' : '-',
            E.mode == NORMAL || E.mode == COMMAND ? 'N' : 'I',
            E.path != "" ? E.path : "[No name]");
    int llen = lstatus.size();
    if (llen > E.screencols) llen = E.screencols;

    std::string rstatus = fmt::format("{}/{}", E.cy+1, E.numrows());
    int rlen = rstatus.size();

    ewrite_with_len(lstatus, llen);
    while (llen < E.screencols) {
        if (E.screencols-llen == rlen) {
            ewrite_with_len(rstatus, rlen);
            break;
        } else {
            ewrite(" ");
            llen++;
        }
    }

    ewrite("\x1b[m");
}

void draw_cmdline() {
    ewrite("\r\n");
    ewrite("\x1b[K");
    if (E.mode == COMMAND) {
        ewrite(":");
        ewrite(E.cmdline);
    } else {
        int len = (int)E.cmdline.size();
        if (len > E.screencols) len = E.screencols;
        if (len/* && time(NULL)-E.cmdline_msg_time < 2*/) {
            ewrite_with_len(E.cmdline, len);
        }
        E.cmdline = "";
    }
}

void draw_debug_info() {
    ewrite("\r\n");
    std::string debug_info = fmt::format(
        "cx: {}, rx: {}, cx (calc): {}, cy = {}, rowoff: {}",
        E.cx,
        E.rx,
        E.cy < E.numrows()
            ? row_rx_to_cx(E.get_row_at(E.cy), E.rx)
            : row_rx_to_cx(NULL, E.rx),
        E.cy,
        E.rowoff);
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
    draw_status_bar();
    draw_cmdline();
    draw_debug_info();

    char buf[32];
    usize len = snprintf(
        buf,
        sizeof(buf)-1,
        "\x1b[%d;%dH",
        (E.cy-E.rowoff)+1,
        (E.rx-E.coloff)+1);
    ewrite(std::string(buf, 0, len));
    ewrite("\x1b[?25h");

    write(STDOUT_FILENO, E.abuf.data(), E.abuf.size());
}

void init_editor() {
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.mode = NORMAL;
    E.dirty = false;
    if (get_window_size(&E.screenrows, &E.screencols) == -1)
        core::error_exit_from("get_window_size");
    E.abuf.reserve(5*1024);
    E.cmdline_msg_time = 0;
    E.quit_times = NUM_FORCE_QUIT_PRESS;
    E.keylog = std::ofstream("key.txt", std::ios_base::app);
    E.keylog << "\n============= new stream ==========\n";
}

int main(int argc, char** argv) {
    enable_raw_mode();
    init_editor();
    if (argc >= 2) {
        open_file(argv[1]);
    }

    set_cmdline_msg("HELP: Ctrl-S save, Ctrl-Q quit");

    while (1) {
        refresh_screen();
        process_keypress();
    }

    return 0;
}

