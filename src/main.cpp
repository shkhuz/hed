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

bool is_char_printable(int c) {
    return c >= 32 && c <= 126;
}

enum EditorMode {
    NORMAL,
    INSERT,
    COMMAND,
    SEARCH,
};

enum EditorAction {
    CURSOR_UP,
    CURSOR_DOWN,
    CURSOR_LEFT,
    CURSOR_RIGHT,
    CURSOR_LINE_BEGIN,
    CURSOR_LINE_END,
    MARK_SET,
    CURSOR_TO_MARK_CUT,
    MODE_CHANGE_NORMAL,
    MODE_CHANGE_INSERT,
    MODE_CHANGE_COMMAND,
    MODE_CHANGE_SEARCH,
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

    ALT_ARROW_LEFT,
    ALT_ARROW_RIGHT,
    ALT_ARROW_UP,
    ALT_ARROW_DOWN,

    UNKNOWN_KEY = -1,
};

enum CmdlineStyle {
    NONE,
    ERROR,
};

enum ActionResult {
    SUCCESS,
    FAILURE,
    NOP,
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
    int cur_rx = 0;
    int cx;
    for (cx = 0; cx < row->len(); cx++) {
        if (row->data[cx] == '\t') {
            cur_rx += (TAB_STOP - 1) - (cur_rx % TAB_STOP);
        }
        cur_rx++;
        if (cur_rx > rx) return cx;
    }
    return cx;
}

struct EditorConfig {
    int screenrows;
    int screencols;
    int cx, cy, rx, tx;
    int tmpcx, tmpcy, tmprx, tmptx;
    int mx, my;
    int rowoff;
    int coloff;
    EditorMode mode;
    std::string path;
    bool dirty;
    int cmdx, cmdoff;

    termios ogtermios;
    std::string abuf;
    std::vector<EditorRow*> rows;
    std::string cmdline;
    time_t cmdline_msg_time;
    CmdlineStyle cmdline_style;
    int quit_times;
    std::string search_default;

    std::ofstream keylog;

    int numrows() {
        return (int)rows.size();
    }

    int cmdline_len() {
        return (int)cmdline.size();
    }

    EditorRow* get_row_at(int at) {
        if (at < 0 || at >= numrows()) return NULL;
        return rows[at];
    }

    void set_cpos(int cx, int cy) {
        this->cx = cx;
        this->cy = cy;
        this->tx = row_cx_to_rx(get_row_at(cy), cx);
    }

    void save_cursor_state() {
        tmpcx = cx;
        tmpcy = cy;
        tmprx = rx;
        tmptx = tx;
    }

    void restore_cursor_state() {
        cx = tmpcx;
        cy = tmpcy;
        rx = tmprx;
        tx = tmptx;
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
                case '1': {
                    switch (buf[3]) {
                        case ';': {
                            switch (buf[4]) {
                                case '3': {
                                    switch (buf[5]) {
                                        case 'A': return ALT_ARROW_UP;
                                        case 'B': return ALT_ARROW_DOWN;
                                        case 'C': return ALT_ARROW_RIGHT;
                                        case 'D': return ALT_ARROW_LEFT;
                                    }
                                } break;
                            }
                        } break;
                    }
                } break;
            }
        } else {
            switch (buf[1]) {
                case 'm': return ALT_M;
            }
        }
    } else if (nread == 1) {
        return buf[0];
    }
    return UNKNOWN_KEY;
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

void row_delete_range(EditorRow* row, int at, int len) {
    if (at < 0 || at+len > row->len() || len == 0) return;
    row->data.erase(at, len);
    update_row(row);
}

void row_append_string(EditorRow* row, const std::string& str) {
    row->data += str;
    update_row(row);
}

template<typename... Args>
void set_cmdline_msg_info(const std::string& fmt, Args... args) {
    E.cmdline = fmt::format(fmt, args...);
    E.cmdline_msg_time = time(NULL);
    E.cmdline_style = NONE;
}

template<typename... Args>
void set_cmdline_msg_error(const std::string& fmt, Args... args) {
    E.cmdline = fmt::format(fmt, args...);
    E.cmdline_msg_time = time(NULL);
    E.cmdline_style = ERROR;
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
        set_cmdline_msg_info("no filename");
        return;
    }
    std::string tmp_path = E.path + ".tmp";
    std::ofstream f(tmp_path);
    if (!f) set_cmdline_msg_error("cannot open file for saving");

    std::string contents = rows_to_string();
    f << contents;
    f.close();
    if (!f) set_cmdline_msg_error("cannot write to file for saving");
    system(std::string("mv " + tmp_path + " " + E.path).c_str());
    set_cmdline_msg_info("{} bytes written", contents.size());
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

void search_text_forward(const std::string& query) {
    if (query == "") return;
    bool found = false;

    for (int i = E.cy; i < E.numrows(); i++) {
        EditorRow* row = E.rows[i];
        usize match = row->rdata.find(query, (i == E.cy) ? E.rx+1 : 0);
        if (match != std::string::npos) {
            E.set_cpos(row_rx_to_cx(row, match), i);
            //E.rowoff = E.numrows();
            found = true;
            break;
        }
    }

    if (!found) {
        set_cmdline_msg_error("search reached EOF");
    }
}

void search_text_backward(const std::string& query) {
    if (query == "") return;
    bool found = false;

    for (int i = E.cy; i >= 0; i--) {
        // If at beginning of line, then skip current line
        if (i == E.cy && E.cx == 0) continue;
        EditorRow* row = E.rows[i];
        usize match = row->rdata.rfind(query, (i == E.cy) ? E.rx-1 : std::string::npos);
        if (match != std::string::npos) {
            E.set_cpos(row_rx_to_cx(row, match), i);
            //E.rowoff = E.numrows();
            found = true;
            break;
        }
    }

    if (!found) {
        set_cmdline_msg_error("search reached BOF");
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

void insert_char(int c) {
    if (E.cy == E.numrows()) {
        insert_row(E.numrows(), "");
    }
    row_insert_char(E.get_row_at(E.cy), E.cx, c);
    E.set_cpos(E.cx+1, E.cy);
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
    E.set_cpos(0, E.cy+1);
}

ActionResult delete_char() {
    if (E.cx == 0 && E.cy == 0) return NOP;

    if (E.cy == E.numrows()) {
        E.set_cpos(E.get_row_at(E.cy-1)->len(), E.cy-1);
    } else {
        EditorRow* row = E.get_row_at(E.cy);
        if (E.cx > 0) {
            row_delete_range(row, E.cx-1, 1);
            E.set_cpos(E.cx-1, E.cy);
        } else {
            E.set_cpos(E.get_row_at(E.cy-1)->len(), E.cy-1);
            row_append_string(E.get_row_at(E.cy), row->data);
            delete_row(E.cy+1);
        }
    }
    return SUCCESS;
}

ActionResult do_action(EditorAction a) {
    EditorRow* row = E.get_row_at(E.cy);
    ActionResult result = SUCCESS;
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
                E.cx = row_rx_to_cx(E.get_row_at(E.cy), E.tx > E.rx ? E.tx : E.rx);
            }
        } break;

        case CURSOR_DOWN: {
            if (E.cy < E.numrows()) E.cy++;
            if (E.cy < E.numrows()) {
                E.cx = row_rx_to_cx(E.get_row_at(E.cy), E.tx > E.rx ? E.tx : E.rx);
            }
        } break;

        case CURSOR_LEFT: {
            if (E.cx != 0) E.set_cpos(E.cx-1, E.cy);
            else if (E.cy > 0) {
                E.set_cpos(E.get_row_at(E.cy-1)->len(), E.cy-1);
            }
        } break;

        case CURSOR_RIGHT: {
            if (row && E.cx < row->len()) E.set_cpos(E.cx+1, E.cy);
            else if (row && E.cx == row->len()) {
                E.set_cpos(0, E.cy+1);
            }
        } break;

        case CURSOR_LINE_BEGIN: {
            E.set_cpos(0, E.cy);
        } break;

        case CURSOR_LINE_END: {
            if (row) E.set_cpos(row->len(), E.cy);
        } break;

        case MODE_CHANGE_NORMAL: {
            E.mode = NORMAL;
            E.cmdline = "";
            E.cmdline_style = NONE;
        } break;

        case MODE_CHANGE_INSERT: E.mode = INSERT; break;

        case MODE_CHANGE_COMMAND:
        case MODE_CHANGE_SEARCH: {
            E.mode = (a == MODE_CHANGE_COMMAND) ? COMMAND : SEARCH;
            E.cmdline = "";
            E.cmdline_style = NONE;
            E.cmdx = 0;
            E.cmdoff = 0;
        } break;

        case MARK_SET: {
            E.mx = E.cx;
            E.my = E.cy;
        } break;

        case CURSOR_TO_MARK_CUT: {
            int startx, starty, endx, endy;
            if (E.my < E.cy) {
                starty = E.my;
                endy = E.cy;
                startx = E.mx;
                endx = E.cx;
            } else if (E.cy < E.my) {
                starty = E.cy;
                endy = E.my;
                startx = E.cx;
                endx = E.mx;
            } else if (E.my == E.cy) {
                starty = E.cy;
                endy = E.cy;
                if (E.cx < E.mx) {
                    startx = E.cx;
                    endx = E.mx;
                } else if (E.mx < E.cx) {
                    startx = E.mx;
                    endx = E.cx;
                } else if (E.cx == E.mx) return NOP;
            }

            if (starty == endy) {
                row_delete_range(E.get_row_at(starty), startx, endx-startx);
                E.set_cpos(startx, E.cy);
            } else {
                EditorRow* startrow = E.get_row_at(starty);
                row_delete_range(startrow, startx, startrow->len()-startx);
                row_append_string(
                    startrow,
                    E.get_row_at(endy)->data.substr(endx));

                for (int i = endy; i > starty; i--) {
                    delete_row(i);
                }

                E.set_cpos(startx, starty);
            }
        } break;

        case NEWLINE_INSERT: insert_newline(); break;
        case LEFT_CHAR_DELETE: result = delete_char(); break;

        case FILE_SAVE: save_file(); break;

        case EDITOR_EXIT: {
            if (E.dirty && E.quit_times > 0) {
                set_cmdline_msg_error("File has unsaved changes: press C-Q {} more times to quit", E.quit_times);
                E.quit_times--;
            } else {
                core::succ_exit();
            }
        } return result; // Always return from EDITOR_EXIT

        default: assert(0 && "do_action(): unknown action");
    }

    E.quit_times = NUM_FORCE_QUIT_PRESS;
    row = E.get_row_at(E.cy);
    int rowlen = row ? row->len() : 0;
    if (E.cx > rowlen) {
        E.cx = rowlen;
    }

    return result;
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

            case 'd': do_action(MARK_SET); break;
            case 'f': do_action(CURSOR_TO_MARK_CUT); break;

            case '\'': {
                if (E.search_default == "") {
                    set_cmdline_msg_error("empty prev search");
                } else {
                    search_text_forward(E.search_default);
                }
            } break;

            case '"': {
                if (E.search_default == "") {
                    set_cmdline_msg_error("empty prev search");
                } else {
                    search_text_backward(E.search_default);
                }
            } break;

            case 'w':
                if (E.cy == E.numrows()) return;
                E.save_cursor_state();
                do_action(CURSOR_RIGHT);
                if (do_action(LEFT_CHAR_DELETE) == NOP) {
                    E.restore_cursor_state();
                }
                break;
            case ALT_M: do_action(MODE_CHANGE_COMMAND); break;
            case '/': do_action(MODE_CHANGE_SEARCH); break;
            case BACKSPACE: break;
            case '\r': break;
            case '\x1b': break;
            default: {
                set_cmdline_msg_error("invalid key '{}' in normal mode", (int)c);
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
                if (is_char_printable(c)) insert_char(c);
                else set_cmdline_msg_error("non-printable key '{}' in insert mode", (int)c);
            } break;
        }

    } else if (E.mode == COMMAND || E.mode == SEARCH) {
        switch (c) {
            case '\r': {
                std::string txt = E.cmdline;
                EditorMode mode = E.mode;
                do_action(MODE_CHANGE_NORMAL);

                if (mode == COMMAND) {
                    if (txt == "quit") do_action(EDITOR_EXIT);
                    else if (str_startswith(txt, "path")) {
                        E.path = txt.substr(5);
                    }
                    else set_cmdline_msg_error("unknown command '{}'", txt);
                } else if (mode == SEARCH) {
                    E.search_default = txt;
                    search_text_forward(txt);
                }
            } break;

            case BACKSPACE: {
                if (E.cmdx > 0) {
                    E.cmdline.erase(E.cmdx-1, 1);
                    E.cmdx--;
                }
            } break;

            case ARROW_LEFT:  if (E.cmdx > 0) E.cmdx--; break;
            case ARROW_RIGHT: if (E.cmdx < E.cmdline_len()) E.cmdx++; break;

            case ALT_ARROW_LEFT: {
                E.cmdx = 0;
            } break;

            case ALT_ARROW_RIGHT: {
                E.cmdx = E.cmdline_len();
            } break;

            case '\x1b': do_action(MODE_CHANGE_NORMAL); break;

            default: {
                if (is_char_printable(c)) {
                    E.cmdline.insert(E.cmdx, 1, c);
                    E.cmdx++;
                }

                if (E.mode == SEARCH) {
                }
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

    if (E.cmdx < E.cmdoff) {
        E.cmdoff = E.cmdx;
    }
    if (E.cmdx >= E.cmdoff + (E.screencols-1)) {
        E.cmdoff = E.cmdx - (E.screencols-1) + 1;
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
    if (E.mode == INSERT) {
        ewrite("\x1b[1;47;30m");
    } else {
        ewrite("\x1b[1;44;30m");
    }

    std::string lstatus = fmt::format(
            "[{}{}] {:.20}",
            E.dirty ? '*' : '-',
            E.mode == INSERT ? 'I' : 'N',
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
    if (E.mode == COMMAND || E.mode == SEARCH) {
        if (E.mode == COMMAND) ewrite(":");
        else if (E.mode == SEARCH) ewrite("/");
        int len = E.cmdline_len();
        if (len > (E.screencols-1)) len = (E.screencols-1);
        ewrite_cstr_with_len(&E.cmdline.data()[E.cmdoff], len);
    } else {
        if (E.cmdline_style == ERROR) ewrite("\x1b[41;37m");
        int len = E.cmdline_len();
        if (len > E.screencols) len = E.screencols;
        if (len/* && time(NULL)-E.cmdline_msg_time < 2*/) {
            ewrite_with_len(E.cmdline, len);
        }
        if (E.cmdline_style == ERROR) ewrite("\x1b[0m");

        E.cmdline = "";
        E.cmdline_style = NONE;
    }
}

void draw_debug_info() {
    ewrite("\r\n");
    std::string debug_info = fmt::format(
        "cmdx: {}, cmdoff: {}, len(cmd): {}, rows: {}, cx = {}, cy: {}, cx (calc): {}, rx: {}, tx: {}",
        E.cmdx,
        E.cmdoff,
        E.cmdline.size(),
        E.numrows(),
        E.cx,
        E.cy,
        row_rx_to_cx(E.get_row_at(E.cy), E.rx),
        E.rx,
        E.tx);
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
    usize len;
    if (E.mode == COMMAND || E.mode == SEARCH) {
        len = snprintf(
            buf,
            sizeof(buf)-1,
            "\x1b[%d;%dH",
            // +2 makes it go from last row to cmdline
            E.screenrows+2,
            (E.cmdx-E.cmdoff)+2);
    } else {
        len = snprintf(
            buf,
            sizeof(buf)-1,
            "\x1b[%d;%dH",
            (E.cy-E.rowoff)+1,
            (E.rx-E.coloff)+1);
    }
    ewrite(std::string(buf, 0, len));
    ewrite("\x1b[?25h");

    write(STDOUT_FILENO, E.abuf.data(), E.abuf.size());
}

void init_editor() {
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.tx = 0;
    E.tmpcx = 0;
    E.tmpcy = 0;
    E.tmprx = 0;
    E.tmptx = 0;
    E.mx = 0;
    E.my = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.mode = NORMAL;
    E.dirty = false;
    E.cmdx = 0;
    E.cmdoff = 0;
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

    set_cmdline_msg_info("HELP: Ctrl-S save, Ctrl-Q quit");

    while (1) {
        refresh_screen();
        process_keypress();
    }

    return 0;
}

