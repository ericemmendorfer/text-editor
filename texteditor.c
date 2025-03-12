#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <time.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>

/* Text style definitions */
#define STYLE_NORMAL    0
#define STYLE_BOLD      1
#define STYLE_DIM       2
#define STYLE_ITALIC    3
#define STYLE_UNDERLINE 4

/* Color definitions */
#define COLOR_DEFAULT   0
#define COLOR_RED       1
#define COLOR_GREEN     2
#define COLOR_YELLOW    3
#define COLOR_BLUE      4
#define COLOR_MAGENTA   5
#define COLOR_CYAN      6
#define COLOR_WHITE     7

/* Style structure */
typedef struct {
    unsigned char bold : 1;
    unsigned char dim : 1;
    unsigned char italic : 1;
    unsigned char underline : 1;
    unsigned char fg_color : 4;
    unsigned char bg_color : 4;
} text_style;

/* Data structure for storing a row of text */
typedef struct erow {
    int size;
    char *chars;
    int rsize;
    char *render;
    text_style *styles;  // Array of style information for each character
} erow;

/* Global editor config struct to store editor state */
struct editorConfig {
    int cx, cy;              // Cursor position (x, y)
    int rx;                  // Render x position (for tabs)
    int rowoff;             // Row offset for scrolling
    int coloff;             // Column offset for scrolling
    int screenrows;         // Number of rows in editor window
    int screencols;         // Number of columns in editor window
    int numrows;            // Number of rows in file
    erow *row;             // Array of rows
    char *filename;         // Currently open filename
    char statusmsg[80];     // Status message
    time_t statusmsg_time;  // Status message time
    struct termios orig_termios;  // Original terminal attributes
    int dirty;              // File modified flag
};

struct editorConfig E;  // Global instance of editor config

/* Editor operations */
enum editorKey {
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

#define EDITOR_VERSION "0.1.0"
#define TAB_STOP 8
#define QUIT_TIMES 3
#define CTRL_KEY(k) ((k) & 0x1f)

/* Print error message and exit */
void die(const char *s) {
    write(STDOUT_FILENO, "\x1b[2J", 4);  // Clear screen
    write(STDOUT_FILENO, "\x1b[H", 3);   // Reposition cursor to top-left
    perror(s);
    exit(1);
}

/* Restore terminal to original attributes */
void disableRawMode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
        die("tcsetattr");
}

/* Configure terminal for raw mode input */
void enableRawMode() {
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
    atexit(disableRawMode);  // Register cleanup function
    
    struct termios raw = E.orig_termios;
    // Modify terminal flags:
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);  // Input flags
    raw.c_oflag &= ~(OPOST);  // Output flags
    raw.c_cflag |= (CS8);     // Control flags
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);  // Local flags
    raw.c_cc[VMIN] = 0;   // Return immediately with any number of bytes
    raw.c_cc[VTIME] = 1;  // 100ms timeout
    
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

/* Buffer for building output */
struct abuf {
    char *b;
    int len;
};
#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
    char *new = realloc(ab->b, ab->len + len);
    if (new == NULL) return;
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf *ab) {
    free(ab->b);
}

/* Read keypress and handle escape sequences */
int editorReadKey() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) die("read");
    }

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
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }
        return '\x1b';
    }
    return c;
}

void editorDrawRows(struct abuf *ab) {
    int y;
    for (y = 0; y < E.screenrows; y++) {
        int filerow = y + E.rowoff;
        if (filerow >= E.numrows) {
            if (E.numrows == 0 && y == E.screenrows / 3) {
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome),
                    "Text Editor -- version %s", EDITOR_VERSION);
                if (welcomelen > E.screencols) welcomelen = E.screencols;
                int padding = (E.screencols - welcomelen) / 2;
                if (padding) {
                    abAppend(ab, "~", 1);
                    padding--;
                }
                while (padding--) abAppend(ab, " ", 1);
                
                // Display welcome message in bold blue
                text_style welcome_style = {0};
                welcome_style.bold = 1;
                welcome_style.fg_color = COLOR_BLUE;
                editorRenderStyle(ab, welcome_style);
                abAppend(ab, welcome, welcomelen);
                abAppend(ab, "\x1b[0m", 4);  // Reset styles
            } else {
                abAppend(ab, "~", 1);
            }
        } else {
            int len = E.row[filerow].rsize - E.coloff;
            if (len < 0) len = 0;
            if (len > E.screencols) len = E.screencols;
            
            char *c = &E.row[filerow].render[E.coloff];
            text_style *styles = &E.row[filerow].styles[E.coloff];
            text_style current_style = {0};
            
            for (int i = 0; i < len; i++) {
                if (memcmp(&current_style, &styles[i], sizeof(text_style)) != 0) {
                    editorRenderStyle(ab, styles[i]);
                    current_style = styles[i];
                }
                abAppend(ab, &c[i], 1);
            }
            abAppend(ab, "\x1b[0m", 4);  // Reset styles at end of line
        }

        abAppend(ab, "\x1b[K", 3);
        if (y < E.screenrows - 1) {
            abAppend(ab, "\r\n", 2);
        }
    }
}

void editorDrawStatusBar(struct abuf *ab) {
    abAppend(ab, "\x1b[7m", 4);
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
        E.filename ? E.filename : "[No Name]", E.numrows,
        E.dirty ? "(modified)" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d",
        E.cy + 1, E.numrows);
    if (len > E.screencols) len = E.screencols;
    abAppend(ab, status, len);
    while (len < E.screencols) {
        if (E.screencols - len == rlen) {
            abAppend(ab, rstatus, rlen);
            break;
        } else {
            abAppend(ab, " ", 1);
            len++;
        }
    }
    abAppend(ab, "\x1b[m", 3);
    abAppend(ab, "\r\n", 2);
}

void editorRefreshScreen() {
    struct abuf ab = ABUF_INIT;

    abAppend(&ab, "\x1b[?25l", 6);
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1,
                                             (E.cx - E.coloff) + 1);
    abAppend(&ab, buf, strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

void editorProcessKeypress() {
    static int quit_times = QUIT_TIMES;
    int c = editorReadKey();

    switch (c) {
        case '\r':
            /* TODO: Handle enter key */
            break;

        case CTRL_KEY('q'):
            if (E.dirty && quit_times > 0) {
                quit_times--;
                return;
            }
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;

        // Style shortcuts
        case CTRL_KEY('b'):  // Bold
            {
                text_style style = {0};
                style.bold = 1;
                editorApplyStyle(E.cy, E.cx, E.cx + 1, style);
            }
            break;
            
        case CTRL_KEY('u'):  // Underline
            {
                text_style style = {0};
                style.underline = 1;
                editorApplyStyle(E.cy, E.cx, E.cx + 1, style);
            }
            break;
            
        case CTRL_KEY('i'):  // Italic
            {
                text_style style = {0};
                style.italic = 1;
                editorApplyStyle(E.cy, E.cx, E.cx + 1, style);
            }
            break;
            
        case CTRL_KEY('r'):  // Red text
            {
                text_style style = {0};
                style.fg_color = COLOR_RED;
                editorApplyStyle(E.cy, E.cx, E.cx + 1, style);
            }
            break;
            
        case CTRL_KEY('g'):  // Green text
            {
                text_style style = {0};
                style.fg_color = COLOR_GREEN;
                editorApplyStyle(E.cy, E.cx, E.cx + 1, style);
            }
            break;

        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
            if (c == DEL_KEY) editorMoveCursor(ARROW_RIGHT);
            editorDelChar();
            break;

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;

        case CTRL_KEY('l'):
        case '\x1b':
            break;

        default:
            editorInsertChar(c);
            break;
    }
    quit_times = QUIT_TIMES;
}

/* Row operations */
void editorUpdateRow(erow *row) {
    int tabs = 0;
    int j;
    for (j = 0; j < row->size; j++)
        if (row->chars[j] == '\t') tabs++;

    free(row->render);
    row->render = malloc(row->size + tabs*(TAB_STOP - 1) + 1);

    int idx = 0;
    for (j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while (idx % TAB_STOP != 0) row->render[idx++] = ' ';
        } else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;
}

void editorInsertRow(int at, char *s, size_t len) {
    if (at < 0 || at > E.numrows) return;

    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    // Initialize styles
    E.row[at].styles = malloc(len * sizeof(text_style));
    memset(E.row[at].styles, 0, len * sizeof(text_style));

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    editorUpdateRow(&E.row[at]);

    E.numrows++;
    E.dirty++;
}

void editorFreeRow(erow *row) {
    free(row->render);
    free(row->chars);
    free(row->styles);
}

void editorDelRow(int at) {
    if (at < 0 || at >= E.numrows) return;
    editorFreeRow(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
    E.numrows--;
    E.dirty++;
}

void editorRowInsertChar(erow *row, int at, int c) {
    if (at < 0 || at > row->size) at = row->size;
    row->chars = realloc(row->chars, row->size + 2);
    row->styles = realloc(row->styles, (row->size + 1) * sizeof(text_style));
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    memmove(&row->styles[at + 1], &row->styles[at], (row->size - at) * sizeof(text_style));
    row->size++;
    row->chars[at] = c;
    memset(&row->styles[at], 0, sizeof(text_style));  // Initialize new style as normal
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowAppendString(erow *row, char *s, size_t len) {
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowDelChar(erow *row, int at) {
    if (at < 0 || at >= row->size) return;
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}

/* Editor operations */
void editorInsertChar(int c) {
    if (E.cy == E.numrows) {
        editorInsertRow(E.numrows, "", 0);
    }
    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    E.cx++;
}

void editorDelChar() {
    if (E.cy == E.numrows) return;
    if (E.cx == 0 && E.cy == 0) return;

    erow *row = &E.row[E.cy];
    if (E.cx > 0) {
        editorRowDelChar(row, E.cx - 1);
        E.cx--;
    } else {
        E.cx = E.row[E.cy-1].size;
        editorRowAppendString(&E.row[E.cy-1], row->chars, row->size);
        editorDelRow(E.cy);
        E.cy--;
    }
}

void editorMoveCursor(int key) {
    erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

    switch (key) {
        case ARROW_LEFT:
            if (E.cx != 0) {
                E.cx--;
            } else if (E.cy > 0) {
                E.cy--;
                E.cx = E.row[E.cy].size;
            }
            break;
        case ARROW_RIGHT:
            if (row && E.cx < row->size) {
                E.cx++;
            } else if (row && E.cx == row->size) {
                E.cy++;
                E.cx = 0;
            }
            break;
        case ARROW_UP:
            if (E.cy != 0) {
                E.cy--;
            }
            break;
        case ARROW_DOWN:
            if (E.cy < E.numrows) {
                E.cy++;
            }
            break;
    }

    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0;
    if (E.cx > rowlen) {
        E.cx = rowlen;
    }
}

/* Initialize editor */
void initEditor() {
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.numrows = 0;
    E.row = NULL;
    E.dirty = 0;
    E.filename = NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;

    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        die("ioctl");
    } else {
        E.screenrows = ws.ws_row - 2;  // Leave room for status bar
        E.screencols = ws.ws_col;
    }
}

/* Style operations */
void editorApplyStyle(int row, int start, int end, text_style style) {
    if (row < 0 || row >= E.numrows) return;
    erow *r = &E.row[row];
    if (start < 0) start = 0;
    if (end > r->size) end = r->size;
    
    for (int i = start; i < end; i++) {
        r->styles[i] = style;
    }
    editorUpdateRow(r);
}

void editorRenderStyle(struct abuf *ab, text_style style) {
    // Reset all styles first
    abAppend(ab, "\x1b[0m", 4);
    
    // Apply new styles
    if (style.bold) abAppend(ab, "\x1b[1m", 4);
    if (style.dim) abAppend(ab, "\x1b[2m", 4);
    if (style.italic) abAppend(ab, "\x1b[3m", 4);
    if (style.underline) abAppend(ab, "\x1b[4m", 4);
    
    // Apply colors if not default
    if (style.fg_color != COLOR_DEFAULT) {
        char color_str[16];
        snprintf(color_str, sizeof(color_str), "\x1b[3%dm", style.fg_color);
        abAppend(ab, color_str, strlen(color_str));
    }
    if (style.bg_color != COLOR_DEFAULT) {
        char color_str[16];
        snprintf(color_str, sizeof(color_str), "\x1b[4%dm", style.bg_color);
        abAppend(ab, color_str, strlen(color_str));
    }
}

int main(int argc, char *argv[]) {
    enableRawMode();
    initEditor();
    
    if (argc >= 2) {
        // TODO: Add file loading functionality
    }
    
    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }
    
    return 0;
}
