#include <cstdio>
#include <termios.h>
#include <unistd.h>
#include <cstdlib>
#include <ctype.h>
#include <sys/ioctl.h>
#include <cerrno>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>
#include <fstream>
#include <cassert>
#include <ctime>
#include <cstdarg>

#include <fmt/format.h>
#include <libclipboard.h>

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
const int NUM_FORCE_QUIT_PRESS = 2;

// if set, outputs a `key.txt` file with keystokes and copy paste info.
#undef DBGLOG
// appends a line in the editor to show debug information
#undef DBGLINE

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
    CURSOR_FORWARD_WORD,
    CURSOR_BACKWARD_WORD,
    CURSOR_FIRST_ROW,
    CURSOR_LAST_ROW,
    CURSOR_PAGE_UP,
    CURSOR_PAGE_DOWN,
    CURSOR_NEXT_PARA,
    CURSOR_PREV_PARA,
    CHANGE_MODE_TO_NORMAL,
    CHANGE_MODE_TO_INSERT,
    CHANGE_MODE_TO_COMMAND,
    CHANGE_MODE_TO_SEARCH,
    SET_MARK,
    EXIT_EDITOR,
    FORCE_EXIT_EDITOR,
    SAVE_FILE,
    REPEAT_SEARCH_FORWARD,
    REPEAT_SEARCH_BACKWARD,

    CUT_CURSOR_MARK_REGION,
    INSERT_NEWLINE,
    INSERT_CHAR,
    INSERT_INDENT,
    DELETE_CURRENT_CHAR,
    DELETE_LEFT_CHAR,
    PASTE_FROM_CLIPBOARD,
    OPEN_LINE_BELOW_CURSOR,
};

enum EditorKey {
    BACKSPACE = 127,

    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    ALT_M,
    ALT_S,

    ALT_ARROW_LEFT,
    ALT_ARROW_RIGHT,
    ALT_ARROW_UP,
    ALT_ARROW_DOWN,

    UNKNOWN_KEY = -1,
};

#define EDSYN_HLT_NUMBER (1<<0)
#define EDSYN_HLT_STRING (1<<1)

struct EditorSyntax {
    std::string filetype;
    std::string* extmatch;
    std::string* keywords;
    std::string* types;
    std::string* consts;
    std::string singleline_comment_start;
    int flags;
};

std::string C_EXTS[] = {"c", "h", "cpp", ""};
std::string C_KEYWORDS[] = {
    "switch",
    "if",
    "while",
    "for",
    "break",
    "continue",
    "return",
    "else",
    "struct",
    "union",
    "typedef",
    "static",
    "enum",
    "class",
    "using",
    "namespace",
    "case",
    "const",
    "inline",
    "auto",
    "constexpr",
    "template",
    "typename",
    "const",
    "#include",
    "#pragma",
    "#define",
    "#if",
    "#ifdef",
    "#ifndef",
    "#elif",
    "#endif",
    "",
};
std::string C_TYPES[] = {
    "void",
    "char",
    "bool",
    "short",
    "int",
    "size_t",
    "ssize_t",
    "ptrdiff_t",
    "long",
    "float",
    "double",
    "",
};
std::string C_CONSTS[] = {
    "true",
    "false",
    "NULL",
    "",
};

EditorSyntax HLDB[] = {
    {
        "c",
        C_EXTS,
        C_KEYWORDS,
        C_TYPES,
        C_CONSTS,
        "//",
        EDSYN_HLT_NUMBER | EDSYN_HLT_STRING
    },
};
#define NUM_HLDBS (sizeof(HLDB) / sizeof(HLDB[0]))

enum EditorHighlight {
    HL_NORMAL = 0,
    HL_NUMBER,
    HL_STRING,
    HL_COMMENT,
    HL_KEYWORD,
    HL_TYPE,
    HL_CONST,
};

inline int hl_to_color(EditorHighlight hl) {
    switch (hl) {
        case HL_NUMBER: return 35;
        case HL_STRING: return 35;
        case HL_COMMENT: return 248;
        case HL_KEYWORD: return 63;
        case HL_TYPE: return 63;
        case HL_CONST: return 35;
        default: return 37;
    }
}

enum CmdlineStyle {
    NONE,
    ERROR,
};

struct UndoInfo {
    EditorAction type;
    std::string data;
    int x, y;
};

struct EditorRow {
    std::string data;
    std::string rdata;
    int rlen;
    u8* hl;

    int len() {
        return (int)data.size();
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
    int mx, my;
    int rowoff;
    int coloff;
    EditorMode mode;
    std::string path;
    bool dirty;
    int cmdx, cmdoff;
    int hltsx, hltsy, hltex, hltey;
    EditorSyntax* syn;
    bool indent_as_spaces;

    termios ogtermios;
    std::string abuf;
    std::vector<EditorRow*> rows;
    std::string cmdline;
    time_t cmdline_msg_time;
    CmdlineStyle cmdline_style;
    std::vector<UndoInfo> undos;
    // Index of `UndoInfo` that will be applied
    // if undo action called.
    int undo_pos;
    int quit_times;
    std::string search_default;
    clipboard_c* cb;

    std::ofstream keylog;

    int numrows() {
        return (int)rows.size();
    }

    int lastrow_idx() {
        return (int)rows.size()-1;
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

    char get_char(int cx, int cy) {
        if (cy >= numrows()) return '\0';
        if (cx == get_row_at(cy)->len()) return '\n';
        return get_row_at(cy)->data[cx];
    }

    char get_char_at_cpos() {
        return get_char(cx, cy);
    }

    char get_char_at_lpos() {
        int x = cx, y = cy;
        if (x == 0 && y == 0) return '\0';
        if (x == 0) {
            y--;
            x = get_row_at(y)->len();
        } else x--;
        return get_char(x, y);
    }

    bool is_cpos_at_end() {
        if (cy == lastrow_idx() && cx == get_row_at(cy)->len()) return true;
        return false;
    }

    void reset_hlt() {
        hltsx = 0;
        hltsy = 0;
        hltex = 0;
        hltey = 0;
    }

    int numundos() {
        return (int)undos.size();
    }

    UndoInfo* lastundo() {
        return &undos.data()[undos.size()-1];
    }

    void dbglog(const std::string& str) {
#ifdef DBGLOG
        keylog << str;
#endif
    }

    void dbglog(char c) {
#ifdef DBGLOG
        keylog << c;
#endif
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
            case '\x1b':    E.dbglog("[esc]"); break;
            case BACKSPACE: E.dbglog("[bksp]"); break;
            case '\r':      E.dbglog("[cr]"); break;
            case '\n':      E.dbglog("[nl]"); break;
            case '\t':      E.dbglog("[tab]"); break;
            default:        E.dbglog(std::string(1, buf[i])); break;
        }
        E.dbglog(' ');
    }
    E.dbglog('\n');

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
                case 's': return ALT_S;
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
#ifdef DBGLINE
        *rows = ws.ws_row-3;
#else
        // one line for status bar, one for cmdline
        *rows = ws.ws_row-2;
#endif
    }
    return 0;
}

bool str_startswith(const std::string& str, const std::string& startswith) {
    return str.rfind(startswith, 0) == 0;
}

bool is_char_printable(int c) {
    return c >= 32 && c <= 126;
}

bool is_char_separator(int c) {
    return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) != NULL;
}

bool match_syn_word(std::string* wordlist, EditorRow* row, int* i, EditorHighlight hl) {
    bool found = false;
    for (int j = 0; wordlist[j] != ""; j++) {
        int klen = wordlist[j].size();
        if (!strncmp(&row->rdata[*i], wordlist[j].c_str(), klen) &&
            is_char_separator(row->rdata[*i+klen])) {
            memset(&row->hl[*i], hl, klen);
            *i += klen;
            found = true;
            break;
        }
    }
    return found;
}

void update_row_syntax(EditorRow* row) {
    int rlen = row->rlen;
    row->hl = (u8*)realloc(row->hl, rlen);
    memset(row->hl, HL_NORMAL, rlen);

    if (E.syn == NULL) return;

    std::string* keywords = E.syn->keywords;
    std::string* types = E.syn->types;
    std::string* consts = E.syn->consts;

    std::string scs = E.syn->singleline_comment_start;

    bool prev_sep = true;
    int which_string = 0;
    int i = 0;

    while (i < rlen) {
        char c = row->rdata[i];
        EditorHighlight prev_hl = (i > 0) ? (EditorHighlight)row->hl[i-1] : HL_NORMAL;

        if (scs.size() && !which_string) {
            if (!strncmp(&row->rdata[i], scs.c_str(), scs.size())) {
                memset(&row->hl[i], HL_COMMENT, rlen-i);
                break;
            }
        }

        if (E.syn->flags & EDSYN_HLT_STRING) {
            if (which_string) {
                row->hl[i] = HL_STRING;
                if (c == '\\' && i+1 < rlen) {
                    row->hl[i+1] = HL_STRING;
                    i += 2;
                    continue;
                }
                if (c == which_string) which_string = 0;
                i++;
                prev_sep = 1;
                continue;
            } else {
                if ((c == '"' || c == '\'')) {
                    which_string = c;
                    row->hl[i] = HL_STRING;
                    i++;
                    continue;
                }
            }
        }

        if (E.syn->flags & EDSYN_HLT_NUMBER) {
            if ((isdigit(c) && (prev_sep || prev_hl == HL_NUMBER)) || (c == '.' && prev_hl == HL_NUMBER)) {
                row->hl[i] = HL_NUMBER;
                i++;
                prev_sep = false;
                continue;
            }
        }

        if (prev_sep) {
            bool found = match_syn_word(keywords, row, &i, HL_KEYWORD);
            if (!found) {
                found = match_syn_word(types, row, &i, HL_TYPE);
                if (!found) {
                    found = match_syn_word(consts, row, &i, HL_CONST);
                }
            }

            if (found) {
                prev_sep = 0;
                continue;
            }
        }

        prev_sep = is_char_separator(c);
        i++;
    }
}

void update_row(EditorRow* row) {
    row->rdata.reserve(row->len());
    row->rdata.clear();
    for (int i = 0; i < row->len(); i++) {
        if (row->data[i] == '\t') {
            row->rdata.push_back(' ');
            while (row->rdata.size() % TAB_STOP != 0) {
                row->rdata.push_back(' ');
            }
        } else {
            row->rdata.push_back(row->data[i]);
        }
    }

    // Compute size before adding '\0'
    row->rlen = row->rdata.size();
    row->rdata.push_back('\0');
    E.dirty = true;

    update_row_syntax(row);
}

EditorRow* insert_row(int at, const std::string& data) {
    if (at < 0 || at > E.numrows()) return NULL;
    EditorRow* row = new EditorRow();
    row->data = data;
    row->hl = NULL;
    E.rows.insert(E.rows.begin() + at, row);
    update_row(row);
    return row;
}

void free_row(EditorRow* row) {
    free(row->hl);
    delete row;
}

std::string delete_row(int at) {
        if (at < 0 || at >= E.numrows()) return "";
    EditorRow* row = E.get_row_at(at);
    std::string rowdata = row->data;
    free_row(row);
    E.rows.erase(E.rows.begin() + at);
    E.dirty = true;
    return rowdata;
}

void row_insert_char(EditorRow* row, int at, int c) {
    if (at < 0 || at > row->len()) at = row->len();
    row->data.insert(at, 1, c);
    update_row(row);
}

void row_insert_string(EditorRow* row, int at, const std::string& str) {
    if (at < 0 || at > row->len()) at = row->len();
    row->data.insert(at, str);
    update_row(row);
}

std::string row_delete_range(EditorRow* row, int at, int len) {
    if (at < 0 || at+len > row->len() || len == 0) return "";
    std::string copy = row->data.substr(at, len);
    row->data.erase(at, len);
    update_row(row);
    return copy;
}

void row_append_string(EditorRow* row, const std::string& str) {
    row->data += str;
    update_row(row);
}

int row_get_indent_col(EditorRow* row) {
    int indent = 0;
    for (int i = 0; i < row->len(); i++) {
        if (row->data[i] == '\t') {
            indent += TAB_STOP;
        } else if (row->data[i] == ' ') {
            indent++;
        } else break;
    }
    return indent;
}

template<typename... Args>
void set_cmdline_msg_info(const std::string& fmt, Args... args) {
    if (E.mode != COMMAND && E.mode != SEARCH) {
        E.cmdline = fmt::format(fmt, args...);
        E.cmdline_msg_time = time(NULL);
        E.cmdline_style = NONE;
    }
}

template<typename... Args>
void set_cmdline_msg_error(const std::string& fmt, Args... args) {
    if (E.mode != COMMAND && E.mode != SEARCH) {
        E.cmdline = fmt::format(fmt, args...);
        E.cmdline_msg_time = time(NULL);
        E.cmdline_style = ERROR;
    }
}

std::string rows_to_string() {
    std::string res;
    for (int i = 0; i < E.numrows(); i++) {
        res.append(E.get_row_at(i)->data);
        res.append("\n");
    }
    return res;
}

void _find_synhlt_with_ext() {
    E.syn = NULL;
    if (E.path == "") return;
    usize idx = E.path.find('.');
    if (idx == std::string::npos) return;
    std::string ext = E.path.substr(idx+1);
    if (ext == "") return;

    for (usize i = 0; i < NUM_HLDBS; i++) {
        EditorSyntax* s = &HLDB[i];
        int e = 0;
        std::string need = s->extmatch[e];
        while (need != "") {
            if (need == ext) {
                E.syn = s;
                return;
            }
            e++;
            need = s->extmatch[e];
        }
    }
}

void update_synhlt_from_ext() {
    _find_synhlt_with_ext();
    for (int r = 0; r < E.numrows(); r++) {
        update_row_syntax(E.get_row_at(r));
    }
}

void scroll_to(int x, int y) {
    if (y < E.rowoff) {
        E.rowoff = y;
    }
    if (y >= E.rowoff + (E.screenrows-5)) {
        E.rowoff = y - (E.screenrows-5) + 1;
    }
    if (x < E.coloff) {
        E.coloff = x;
    }
    if (x >= E.coloff + (E.screencols-5)) {
        E.coloff = x - (E.screencols-5) + 1;
    }
}

void scroll_cmdline() {
    if (E.cmdx < E.cmdoff) {
        E.cmdoff = E.cmdx;
    }
    if (E.cmdx >= E.cmdoff + (E.screencols-1)) {
        E.cmdoff = E.cmdx - (E.screencols-1) + 1;
    }
}

void push_undoinfo(EditorAction type, std::string data) {
    while (E.undo_pos != E.numundos()-1) E.undos.pop_back();

    E.undos.push_back({
        .type = type,
        .data = data,
        .x = E.cx,
        .y = E.cy,
    });
    E.undo_pos = E.numundos()-1;
}

const char* WHITESPACE = " \t\n\r\f\v";

void str_trim_leading_ws(std::string& s) {
    s.erase(0, s.find_first_not_of(WHITESPACE));
}

void str_trim_trailing_ws(std::string& s) {
    s.erase(s.find_last_not_of(WHITESPACE)+1);
}

void file_trim_trailing_ws() {
    for (int i = 0; i < E.numrows(); i++) {
        str_trim_trailing_ws(E.get_row_at(i)->data);
    }
}

void set_path(const std::string& path) {
    E.path = path;
    update_synhlt_from_ext();
}

void open_file(const std::string& path) {
    std::ifstream f(path);
    std::string line;

    if (!f) core::error_exit_with_msg("file not found");
    while (std::getline(f, line)) {
        insert_row(E.numrows(), line);
    }
    set_path(path);
    E.dirty = false;
}

void search_text_forward(const std::string& query, bool set_cursor_on_match) {
    if (query == "") {
        E.reset_hlt();
        return;
    }
    bool found = false;

    for (int i = E.cy; i < E.numrows(); i++) {
        EditorRow* row = E.rows[i];
        usize match = row->rdata.find(query, (i == E.cy) ? E.rx+1 : 0);
        if (match != std::string::npos) {
            if (set_cursor_on_match) E.set_cpos(row_rx_to_cx(row, match), i);
            E.hltsy = i;
            E.hltsx = match;
            E.hltey = i;
            E.hltex = match + query.size();
            scroll_to(match + query.size(), i);
            found = true;
            break;
        }
    }

    if (!found) {
        set_cmdline_msg_error("search reached EOF");
        E.reset_hlt();
    }
}

void search_text_backward(const std::string& query, bool set_cursor_on_match) {
    if (query == "") {
        E.reset_hlt();
        return;
    }
    bool found = false;

    for (int i = E.cy; i >= 0; i--) {
        // If at beginning of line, then skip current line
        if (i == E.cy && E.cx == 0) continue;
        EditorRow* row = E.rows[i];
        usize match = row->rdata.rfind(query, (i == E.cy) ? E.rx-1 : std::string::npos);
        if (match != std::string::npos) {
            if (set_cursor_on_match) E.set_cpos(row_rx_to_cx(row, match), i);
            E.hltsy = i;
            E.hltsx = match;
            E.hltey = i;
            E.hltex = match + query.size();
            scroll_to(match + query.size(), i);
            found = true;
            break;
        }
    }

    if (!found) {
        set_cmdline_msg_error("search reached BOF");
        E.reset_hlt();
    }
}

// =========== high level ==============
void ewrite(const std::string& str) {
    E.abuf.append(str);
}

void ewrite_char(char c) {
    E.abuf += c;
}

void ewrite_cstr_with_len(const char* str, usize len) {
    E.abuf.append(str, len);
}

void ewrite_with_len(const std::string& str, usize len) {
    E.abuf.append(str, 0, len);
}

void insert_empty_row_if_file_empty() {
    if (E.numrows() == 0) {
        insert_row(E.numrows(), "");
    }
}

void delete_empty_row_if_file_empty() {
    EditorRow* row = E.get_row_at(E.cy);
    if (E.numrows() == 1 && row->len() == 0) {
        delete_row(0);
    }
}

void update_cx_when_cy_changed() {
    if (E.numrows() != 0) {
        // We calculate E.cx from E.rx and update it
        // instead of directly updating E.rx
        // because E.rx is calculated on
        // every refresh (throwing the prev value away).
        // So we "choose" a E.cx which
        // will be converted to the needed E.rx in the
        // refresh stage.
        E.cx = row_rx_to_cx(E.get_row_at(E.cy), E.tx > E.rx ? E.tx : E.rx);
    }
}

void copy_to_clipboard(const std::string& text) {
    E.dbglog("[start]");
    E.dbglog(text);
    E.dbglog("[end]");

    clipboard_set_text_ex(E.cb, text.c_str(), text.size(), LCB_CLIPBOARD);
}

// ============= ACTIONS ==============

void do_cursor_up() {
    if (E.cy != 0) E.cy--;
    update_cx_when_cy_changed();
}

void do_cursor_down() {
    if (E.cy < E.lastrow_idx()) E.cy++;
    update_cx_when_cy_changed();
}

void do_cursor_left() {
    if (E.cx != 0) E.set_cpos(E.cx-1, E.cy);
    else if (E.cy > 0) {
        E.set_cpos(E.get_row_at(E.cy-1)->len(), E.cy-1);
    }
}

void do_cursor_right() {
    EditorRow* row = E.get_row_at(E.cy);
    if (E.cx < row->len()) E.set_cpos(E.cx+1, E.cy);
    else if (E.cy != (E.lastrow_idx()) && E.cx == row->len()) {
        E.set_cpos(0, E.cy+1);
    }
}

void do_cursor_line_begin() {
    E.set_cpos(0, E.cy);
}

void do_cursor_line_end() {
    EditorRow* row = E.get_row_at(E.cy);
    if (row) E.set_cpos(row->len(), E.cy);
}

void change_mode(EditorMode mode) {
    E.mode = mode;
    E.cmdline = "";
    E.cmdline_style = NONE;
    E.cmdx = 0;
    E.cmdoff = 0;
}

void do_change_mode_to_normal() {
    change_mode(NORMAL);
}

void do_change_mode_to_insert() {
    change_mode(INSERT);
}

void do_change_mode_to_command() {
    change_mode(COMMAND);
}

void do_change_mode_to_search() {
    change_mode(SEARCH);
}

void do_set_mark() {
    E.mx = E.cx;
    E.my = E.cy;
}

void do_cut_cursor_mark_region(bool hist) {
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
    } else {
        starty = E.cy;
        endy = E.cy;
        if (E.cx < E.mx) {
            startx = E.cx;
            endx = E.mx;
        } else if (E.mx < E.cx) {
            startx = E.mx;
            endx = E.cx;
        } else return;
    }

    std::string copy;
    if (startx == 0 && starty == 0 && endy == E.lastrow_idx() && endx == E.get_row_at(E.lastrow_idx())->len()) {
        int numrows = E.numrows();
        for (int i = 0; i < numrows; i++) {
            if (i != 0) copy += '\n';
            copy += delete_row(0);
        }
    } else if (starty == endy) {
        copy += row_delete_range(E.get_row_at(starty), startx, endx-startx);
    } else {
        EditorRow* startrow = E.get_row_at(starty);
        EditorRow* endrow = E.get_row_at(endy);
        bool startrow_deleted = false;

        if (startx == 0) {
            copy += delete_row(starty);
            startrow_deleted = true;
        } else {
            copy += row_delete_range(startrow, startx, startrow->len()-startx);
        }

        for (int i = starty+1; i < endy; i++) {
            copy += '\n';
            copy += delete_row(startrow_deleted ? starty : starty+1);
        }

        copy += '\n';
        if (startrow_deleted) {
            copy += row_delete_range(endrow, 0, endx);
        } else {
            row_append_string(
                startrow,
                row_delete_range(endrow, endx, endrow->len()-endx));
            copy += delete_row(starty+1);
        }
    }

    E.set_cpos(startx, starty);
    copy_to_clipboard(copy);
    if (hist) push_undoinfo(CUT_CURSOR_MARK_REGION, copy);
}

void do_cursor_forward_word() {
    while (!isalpha(E.get_char_at_cpos()) && !E.is_cpos_at_end())
        do_cursor_right();
    if (!E.is_cpos_at_end()) {
        while (isalpha(E.get_char_at_cpos())) do_cursor_right();
    }
}

void do_cursor_backward_word() {
    if (E.cx == 0 && E.cy == 0) return;
    while (!(isalpha(E.get_char_at_lpos()) || E.get_char_at_lpos() == '\0'))
        do_cursor_left();
    while (isalpha(E.get_char_at_lpos())) {
        do_cursor_left();
    }
}

void do_cursor_first_row() {
    E.cy = 0;
    update_cx_when_cy_changed();
}

void do_cursor_last_row() {
    E.cy = E.lastrow_idx();
    update_cx_when_cy_changed();
}

bool is_row_only_ws(EditorRow* row) {
    if (row->len() == 0) return true;
    for (int i = 0; i < row->len(); i++) {
        char c = row->data[i];
        if (c != '\t' && c != ' ') {
            return false;
        }
    }
    return true;
}

void do_cursor_next_para() {
    if (E.cy == E.lastrow_idx()) return;
    E.cy++;

    while (E.cy != E.lastrow_idx() && is_row_only_ws(E.get_row_at(E.cy))) {
        E.cy++;
    }

    while (E.cy != E.lastrow_idx() && !is_row_only_ws(E.get_row_at(E.cy))) {
        E.cy++;
    }
    update_cx_when_cy_changed();
}

void do_cursor_prev_para() {
    if (E.cy == 0) return;
    E.cy--;

    while (E.cy != 0 && is_row_only_ws(E.get_row_at(E.cy))) {
        E.cy--;
    }

    while (E.cy != 0 && !is_row_only_ws(E.get_row_at(E.cy))) {
        E.cy--;
    }
    update_cx_when_cy_changed();
}

void do_insert_char(bool hist, int c);

void do_insert_indent(bool hist) {
    if (E.indent_as_spaces) {
        int spaces = ((TAB_STOP-1) - (E.rx%TAB_STOP)) + 1;
        for (int i = 0; i < spaces; i++) do_insert_char(hist, ' ');
    } else {
        do_insert_char(hist, '\t');
    }
}

void autoindent_just_after_newline() {
    if (E.cx != 0) return;
    int target_indent = 0;
    bool found_indent = false;

    int cy = E.cy-1;
    for (int i = cy; i >= 0; i--) {
        EditorRow* row = E.get_row_at(i);
        if (row->len() == 0) continue;
        int cx = row->len();

        for (; cx >= 0; cx--) {
            if (row->data[cx] != '\t' && row->data[cx] != ' ') {
                target_indent = row_get_indent_col(row);
                found_indent = true;
                break;
            }
        }

        if (found_indent) break;
    }

    if (found_indent) {
        int ts_multiple = target_indent / TAB_STOP;
        int ts_leftover = target_indent % TAB_STOP;

        for (int i = 0; i < ts_multiple; i++) {
            if (E.indent_as_spaces) {
                for (int i = 0; i < TAB_STOP; i++) {
                    do_insert_char(false, ' ');
                }
            } else {
                do_insert_char(false, '\t');
            }
        }

        for (int i = 0; i < ts_leftover; i++) {
            do_insert_char(false, ' ');
        }
    }
}

void do_insert_newline(bool hist, bool autoindent) {
    if (hist) push_undoinfo(INSERT_NEWLINE, std::string(1, '\n'));
    insert_empty_row_if_file_empty();

    if (E.cx == 0) {
        insert_row(E.cy, "");
    } else {
        EditorRow* row = E.get_row_at(E.cy);
        insert_row(E.cy+1, row->data.substr(E.cx, row->len()-E.cx));
        row->data = row->data.substr(0, E.cx);
        update_row(row);
    }
    E.set_cpos(0, E.cy+1);
    if (autoindent) autoindent_just_after_newline();
}

void do_insert_char(bool hist, int c) {
    if (hist) push_undoinfo(INSERT_CHAR, std::string(1, c));
    if (c == '\n') {
        // We do not autoindent because this code
        // can be called by other functions such
        // as the paste code.
        do_insert_newline(hist, false);
        return;
    }

    insert_empty_row_if_file_empty();

    row_insert_char(E.get_row_at(E.cy), E.cx, c);
    E.set_cpos(E.cx+1, E.cy);
}

void do_delete_left_char(bool hist) {
    if (E.cx == 0 && E.cy == 0) return;
    EditorRow* row = E.get_row_at(E.cy);

    if (E.cx > 0) {
        char c = row->data[E.cx-1];
        row_delete_range(row, E.cx-1, 1);
        E.set_cpos(E.cx-1, E.cy);
        if (hist) push_undoinfo(DELETE_LEFT_CHAR, std::string(1, c));
    } else {
        E.set_cpos(E.get_row_at(E.cy-1)->len(), E.cy-1);
        if (hist) push_undoinfo(DELETE_LEFT_CHAR, std::string(1, '\n'));
        row_append_string(E.get_row_at(E.cy), row->data);
        delete_row(E.cy+1);
    }

    delete_empty_row_if_file_empty();
}

void do_delete_current_char(bool hist) {
    EditorRow* row = E.get_row_at(E.cy);
    if (!row) return;

    if (E.cx == row->len()) {
        if (E.cy < E.lastrow_idx()) {
            if (hist) push_undoinfo(DELETE_CURRENT_CHAR, std::string(1, '\n'));
            row_append_string(row, E.get_row_at(E.cy+1)->data);
            delete_row(E.cy+1);
        }
    } else {
        if (hist) push_undoinfo(DELETE_CURRENT_CHAR, std::string(1, row->data[E.cx]));
        row_delete_range(row, E.cx, 1);
    }

    delete_empty_row_if_file_empty();
}

void do_paste_from_clipboard(bool hist) {
    const char* text_c = clipboard_text_ex(E.cb, NULL, LCB_CLIPBOARD);
    if (!text_c) {
        set_cmdline_msg_error("nothing to paste");
        return;
    }
    std::string text = std::string(text_c);
    free((void*)text_c);

    if (hist) push_undoinfo(PASTE_FROM_CLIPBOARD, text);
    usize sz = text.size();
    for (usize i = 0; i < sz; i++) {
        do_insert_char(false, text[i]);
    }
}

void do_open_line_below_cursor(bool hist) {
    if (hist) push_undoinfo(OPEN_LINE_BELOW_CURSOR, "");
    insert_row(E.cy+1, "");
    E.set_cpos(0, E.cy+1);
    autoindent_just_after_newline();
    do_change_mode_to_insert();
}

void do_save_file() {
    file_trim_trailing_ws();

    if (E.path == "") {
        set_cmdline_msg_error("no filename");
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

void do_exit_editor() {
    if (E.dirty && E.quit_times > 0) {
        set_cmdline_msg_error("File has unsaved changes: press [backtick] {} more times to quit", E.quit_times);
        E.quit_times--;
    } else {
        core::succ_exit();
    }
}

void do_force_exit_editor() {
    core::succ_exit();
}

void cursor_page_up_down(bool down) {
    if (down) {
        E.cy = E.rowoff + E.screenrows - 1;
        if (E.cy > E.lastrow_idx()) {
            E.cy = E.lastrow_idx();
        }
    } else {
        E.cy = E.rowoff;
    }
    update_cx_when_cy_changed();

    int times = E.screenrows;
    while (times--) {
        if (down) do_cursor_down();
        else do_cursor_up();
    }
}

void do_cursor_page_up() {
    cursor_page_up_down(false);
}

void do_cursor_page_down() {
    cursor_page_up_down(true);
}

void repeat_search(bool forward) {
    if (E.search_default == "") {
        set_cmdline_msg_error("empty prev search");
    } else {
        if (forward) search_text_forward(E.search_default, true);
        else search_text_backward(E.search_default, true);
    }
}

void do_repeat_search_forward() {
    repeat_search(true);
}

void do_repeat_search_backward() {
    repeat_search(false);
}

void do_undo_or_redo(bool undo) {
    if (E.numundos() == 0 || (undo && E.undo_pos == -1)) {
        set_cmdline_msg_error("already at oldest change");
        return;
    }
    if (!undo && E.undo_pos == E.numundos()-1) {
        set_cmdline_msg_error("already at newest change");
        return;
    }

    int uidx = undo ? E.undo_pos : E.undo_pos+1;
    if (undo) E.undo_pos--;
    else E.undo_pos++;

    UndoInfo u = E.undos[uidx];

    if ((u.type == INSERT_CHAR && undo)
            || (u.type == INSERT_NEWLINE && undo)
            || (u.type == DELETE_CURRENT_CHAR && !undo)
            || (u.type == DELETE_LEFT_CHAR && !undo)) {
        E.set_cpos(u.x, u.y);
        do_delete_current_char(false);
    } else if ((u.type == DELETE_CURRENT_CHAR && undo)
            || (u.type == DELETE_LEFT_CHAR && undo)
            || (u.type == INSERT_CHAR && !undo)
            || (u.type == INSERT_NEWLINE && !undo)) {
        E.set_cpos(u.x, u.y);
        do_insert_char(false, u.data[0]);
        int x = u.x;
        int y = u.y;

        if (u.type == INSERT_CHAR) x = u.x+1;
        else if (u.type == INSERT_NEWLINE) x = 0;
        if (u.type == INSERT_NEWLINE) y += 1;

        if (u.type != DELETE_LEFT_CHAR) E.set_cpos(x, y);
    } else if ((u.type == CUT_CURSOR_MARK_REGION && undo)
            || (u.type == PASTE_FROM_CLIPBOARD && !undo)) {
        E.set_cpos(u.x, u.y);
        for (int i = 0; i < (int)u.data.size(); i++) {
            do_insert_char(false, u.data[i]);
        }
    } else if ((u.type == PASTE_FROM_CLIPBOARD && undo)
            || (u.type == CUT_CURSOR_MARK_REGION && !undo)) {
        E.set_cpos(u.x, u.y);
        for (int i = 0; i < (int)u.data.size(); i++) {
            do_delete_current_char(false);
        }
    } else if (u.type == OPEN_LINE_BELOW_CURSOR) {
        E.set_cpos(u.x, u.y);
        if (undo) {
            delete_row(u.y+1);
        } else {
            do_open_line_below_cursor(false);
            do_change_mode_to_normal();
        }
    } else {
        set_cmdline_msg_error("[internal] don't know how to undo last change");
    }

    if (undo && E.undo_pos == -1) E.dirty = false;
}

void do_action(int action, ...) {
    switch (action) {
        case CURSOR_UP:                      do_cursor_up(); break;
        case CURSOR_DOWN:                    do_cursor_down(); break;
        case CURSOR_LEFT:                    do_cursor_left(); break;
        case CURSOR_RIGHT:                   do_cursor_right(); break;
        case CURSOR_LINE_BEGIN:              do_cursor_line_begin(); break;
        case CURSOR_LINE_END:                do_cursor_line_end(); break;
        case CHANGE_MODE_TO_NORMAL:          do_change_mode_to_normal(); break;
        case CHANGE_MODE_TO_INSERT:          do_change_mode_to_insert(); break;
        case CHANGE_MODE_TO_COMMAND:         do_change_mode_to_command(); break;
        case CHANGE_MODE_TO_SEARCH:          do_change_mode_to_search(); break;
        case SET_MARK:                       do_set_mark(); break;
        case CUT_CURSOR_MARK_REGION:         do_cut_cursor_mark_region(true); break;
        case CURSOR_FORWARD_WORD:            do_cursor_forward_word(); break;
        case CURSOR_BACKWARD_WORD:           do_cursor_backward_word(); break;
        case CURSOR_FIRST_ROW:               do_cursor_first_row(); break;
        case CURSOR_LAST_ROW:                do_cursor_last_row(); break;
        case INSERT_NEWLINE:                 do_insert_newline(true, true); break;
        case DELETE_LEFT_CHAR:               do_delete_left_char(true); break;
        case DELETE_CURRENT_CHAR:            do_delete_current_char(true); break;
        case PASTE_FROM_CLIPBOARD:           do_paste_from_clipboard(true); break;
        case OPEN_LINE_BELOW_CURSOR:         do_open_line_below_cursor(true); break;
        case SAVE_FILE:                      do_save_file(); break;
        case EXIT_EDITOR:                    do_exit_editor(); return;
        case FORCE_EXIT_EDITOR:              do_force_exit_editor(); return;
        case CURSOR_PAGE_UP:                 do_cursor_page_up(); break;
        case CURSOR_PAGE_DOWN:               do_cursor_page_down(); break;
        case CURSOR_PREV_PARA:               do_cursor_prev_para(); break;
        case CURSOR_NEXT_PARA:               do_cursor_next_para(); break;
        case REPEAT_SEARCH_FORWARD:          do_repeat_search_forward(); break;
        case REPEAT_SEARCH_BACKWARD:         do_repeat_search_backward(); break;
        case INSERT_CHAR: {
            va_list args;
            va_start(args, action);
            int c = va_arg(args, int);
            va_end(args);
            do_insert_char(true, c);
        } break;
    }

    EditorRow* row = E.get_row_at(E.cy);
    int rowlen = row ? row->len() : 0;
    if (E.cx > rowlen) {
        E.cx = rowlen;
    }

    E.quit_times = NUM_FORCE_QUIT_PRESS;
    E.reset_hlt();
}

void parse_and_run_command(const std::string& cmd) {
    std::string cmd_nows = cmd;
    str_trim_leading_ws(cmd_nows);
    str_trim_trailing_ws(cmd_nows);

    if (cmd_nows == "") {
        set_cmdline_msg_error("empty command");
        return;
    };

    std::vector<std::string> cmdspl;
    std::istringstream ss(cmd_nows);
    std::string s;
    while (std::getline(ss, s, ' ')) {
        cmdspl.push_back(s);
    }

    const std::string& name = cmdspl[0];
    if (name == "set") {
    } else if (name == "exit") {
        if (cmdspl.size() == 1) do_action(EXIT_EDITOR);
        else if (cmdspl[1] == "--force") do_action(FORCE_EXIT_EDITOR);
        else set_cmdline_msg_error("exit: unknown extra arguments");
    } else {
        set_cmdline_msg_error("unknown command '{}'", name);
    }
}

void process_keypress() {
    int c = read_key();
    if (E.mode == NORMAL) {
        switch (c) {
            case 'i':                   do_action(CHANGE_MODE_TO_INSERT); break;
            case 'w':                   do_action(DELETE_CURRENT_CHAR); break;
            case '`':                   do_action(EXIT_EDITOR); break;
            case 'U':                   do_action(CURSOR_PAGE_UP); break;
            case 'M':                   do_action(CURSOR_PAGE_DOWN); break;
            case 'a':                   do_action(CURSOR_LINE_BEGIN); break;
            case ';':                   do_action(CURSOR_LINE_END); break;
            case ARROW_LEFT:            do_action(CURSOR_LEFT); break;
            case ARROW_DOWN:            do_action(CURSOR_DOWN); break;
            case ARROW_UP:              do_action(CURSOR_UP); break;
            case ARROW_RIGHT:           do_action(CURSOR_RIGHT); break;
            case 'h':                   do_action(CURSOR_LEFT); break;
            case 'j':                   do_action(CURSOR_DOWN); break;
            case 'k':                   do_action(CURSOR_UP); break;
            case 'l':                   do_action(CURSOR_RIGHT); break;
            case 'o':                   do_action(CURSOR_FORWARD_WORD); break;
            case 'n':                   do_action(CURSOR_BACKWARD_WORD); break;
            case 'u':                   do_action(CURSOR_PREV_PARA); break;
            case 'm':                   do_action(CURSOR_NEXT_PARA); break;
            case ',':                   do_action(OPEN_LINE_BELOW_CURSOR); break;
            case 'd':                   do_action(SET_MARK); break;
            case 'f':                   do_action(CUT_CURSOR_MARK_REGION); break;
            case 'c':                   do_action(PASTE_FROM_CLIPBOARD); break;
            case 'b':                   do_action(REPEAT_SEARCH_FORWARD); break;
            case 'B':                   do_action(REPEAT_SEARCH_BACKWARD); break;
            case ALT_M:                 do_action(CHANGE_MODE_TO_COMMAND); break;
            case ALT_S:                 do_action(SAVE_FILE); break;
            case '/':                   do_action(CHANGE_MODE_TO_SEARCH); break;
            case BACKSPACE:             break;
            case '\r':                  break;
            case '\x1b':                break;
            case 'G':                   do_action(CURSOR_LAST_ROW); break;
            case 'g': {
                c = read_key();
                switch (c) {
                    case 'g':           do_action(CURSOR_FIRST_ROW); break;
                    case '\x1b':        break;
                    default: set_cmdline_msg_error("invalid key 'g {}' in normal mode", (int)c);
                }
            } break;
            case 'e': do_undo_or_redo(true); break;
            case 'E': do_undo_or_redo(false); break;
            default: set_cmdline_msg_error("invalid key '{}' in normal mode", (int)c);
        }

    } else if (E.mode == INSERT) {
        switch (c) {
            case BACKSPACE:             do_action(DELETE_LEFT_CHAR); break;
            case '\r':                  do_action(INSERT_NEWLINE); break;
            case '\t':                  do_insert_indent(true); break;
            case ARROW_LEFT:            do_action(CURSOR_LEFT); break;
            case ARROW_DOWN:            do_action(CURSOR_DOWN); break;
            case ARROW_UP:              do_action(CURSOR_UP); break;
            case ARROW_RIGHT:           do_action(CURSOR_RIGHT); break;
            case '\x1b':                do_action(CHANGE_MODE_TO_NORMAL); break;
            default: {
                if (is_char_printable(c)) do_action(INSERT_CHAR, c);
                else set_cmdline_msg_error("non-printable key '{}' in insert mode", (int)c);
            } break;
        }

    } else if (E.mode == COMMAND || E.mode == SEARCH) {
        switch (c) {
            case '\r': {
                std::string txt = E.cmdline;
                EditorMode mode = E.mode;
                do_action(CHANGE_MODE_TO_NORMAL);

                if (mode == COMMAND) {
                    parse_and_run_command(txt);
                } else if (mode == SEARCH) {
                    E.search_default = txt;
                    search_text_forward(txt, true);
                }
            } break;

            case BACKSPACE: {
                if (E.cmdx > 0) {
                    E.cmdline.erase(E.cmdx-1, 1);
                    E.cmdx--;
                } else if (E.cmdx == 0 && E.cmdline.size() == 0) {
                    do_action(CHANGE_MODE_TO_NORMAL);
                }

                if (E.mode == SEARCH) {
                    search_text_forward(E.cmdline, false);
                }
            } break;

            case CTRL_KEY('h'):  if (E.cmdx > 0) E.cmdx--; break;
            case CTRL_KEY('l'): if (E.cmdx < E.cmdline_len()) E.cmdx++; break;

            case ALT_ARROW_LEFT: {
                E.cmdx = 0;
            } break;

            case ALT_ARROW_RIGHT: {
                E.cmdx = E.cmdline_len();
            } break;

            case '\x1b': {
                do_action(CHANGE_MODE_TO_NORMAL);
            } break;

            default: {
                if (is_char_printable(c)) {
                    E.cmdline.insert(E.cmdx, 1, c);
                    E.cmdx++;
                }

                if (E.mode == SEARCH) {
                    search_text_forward(E.cmdline, false);
                }
            } break;
        }
    }
}

void update_rx() {
    E.rx = 0;
    if (E.cy < E.numrows()) {
        E.rx = row_cx_to_rx(E.get_row_at(E.cy), E.cx);
    }
}

void draw_rows() {
    for (int y = 0; y < E.screenrows; y++) {
        int filerow = y + E.rowoff;
        ewrite("\x1b[K");
        if (filerow >= E.numrows()) {
            if (E.numrows() == 0 && y == E.screenrows / 3) {
                std::string welcome = "hed editor -- maintained by shkhuz";
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
            int rowlen = E.get_row_at(filerow)->rlen - E.coloff;
            if (rowlen < 0) rowlen = 0;
            if (rowlen > E.screencols) rowlen = E.screencols;

            const char* c = &E.get_row_at(filerow)->rdata.data()[E.coloff];
            u8* hl = &E.get_row_at(filerow)->hl[E.coloff];
            int current_color = -1;

            // We go till i == rowlen because hlt end is exclusive
            // so we need to one after the last character to check if
            // hlt is ended.
            // But we exit early before printing because there is
            // no character at i == rowlen.
            for (int i = 0; i <= rowlen; i++) {
                int filei = i + E.coloff;
                if (filerow == E.hltsy && filei == E.hltsx) {
                    ewrite("\x1b[44m");
                }
                if (filerow == E.hltey && filei == E.hltex) {
                    ewrite("\x1b[49m");
                }
                if (i == rowlen) break;

                if (iscntrl(c[i])) {
                    char sym = (c[i] <= 26) ? '@'+c[i] : '?';
                    ewrite("\x1b[7m");
                    ewrite_char(sym);
                    ewrite("\x1b[m");
                    if (current_color != -1) {
                        ewrite(fmt::format("\x1b[{}m", current_color));
                    }
                } else if (hl[i] == HL_NORMAL) {
                    if (current_color != -1) {
                        ewrite("\x1b[0m");
                        current_color = -1;
                    }
                    ewrite_cstr_with_len(&c[i], 1);
                } else {
                    int color = hl_to_color((EditorHighlight)hl[i]);
                    if (color != current_color) {
                        current_color = color;
                        if (hl[i] == HL_KEYWORD || hl[i] == HL_TYPE)
                            ewrite(fmt::format("\x1b[1;38;5;{}m", color));
                        else if (hl[i] == HL_COMMENT)
                            ewrite(fmt::format("\x1b[38;5;{}m", color));
                        else
                            ewrite(fmt::format("\x1b[{}m", color));
                    }
                    ewrite_cstr_with_len(&c[i], 1);
                }
            }
            ewrite("\x1b[39m");
        }

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
            "[{}{}] {}",
            E.dirty ? '*' : '-',
            E.mode == INSERT ? 'I' : 'N',
            E.path != "" ? E.path : "[No name]");
    int llen = lstatus.size();
    if (llen > E.screencols) llen = E.screencols;

    std::string rstatus = fmt::format(
        "{} {}/{} ",
        E.syn ? E.syn->filetype : "none",
        E.cy+1,
        E.numrows());
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
    ewrite("\x1b[K");
    std::string debug_info = fmt::format(
        "undo_pos: {}, cmdx: {}, cmdoff: {}, len(cmd): {}, rows: {}, cx = {}, cy: {}, cx (calc): {}, rx: {}, tx: {}",
        E.undo_pos,
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
}

void refresh_screen() {
    if (E.mode != COMMAND && E.mode != SEARCH) {
        update_rx();
        scroll_to(E.rx, E.cy);
    }
    scroll_cmdline();

    E.abuf.clear();
    ewrite("\x1b[?25l");
    ewrite("\x1b[H");

    draw_rows();
    draw_status_bar();
    draw_cmdline();
#ifdef DBGLINE
    draw_debug_info();
#endif

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
    E.mx = 0;
    E.my = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.mode = NORMAL;
    E.dirty = false;
    E.cmdx = 0;
    E.cmdoff = 0;
    E.syn = NULL;
    E.indent_as_spaces = true;
    E.reset_hlt();
    if (get_window_size(&E.screenrows, &E.screencols) == -1)
        core::error_exit_from("get_window_size");
    E.abuf.reserve(5*1024);
    E.cmdline_msg_time = 0;
    E.quit_times = NUM_FORCE_QUIT_PRESS;
    E.undo_pos = -1;
#ifdef DBGLOG
    E.keylog = std::ofstream("key.txt", std::ios_base::app);
    E.keylog << "\n============= new stream ==========\n";
#endif
    E.cb = clipboard_new(NULL);
}

int main(int argc, char** argv) {
    enable_raw_mode();
    init_editor();
    if (argc >= 2) {
        open_file(argv[1]);
    }

    set_cmdline_msg_info("HELP: Alt-s save, ` quit");

    while (1) {
        refresh_screen();
        process_keypress();
    }

    return 0;
}
