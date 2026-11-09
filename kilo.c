/*** includes ***/

// Feature test macros: must come BEFORE the #includes. They tell the C library
// to expose getline() and strdup(), which -std=c99 would otherwise hide.
#define _DEFAULT_SOURCE
#define _GNU_SOURCE

#include <ctype.h>   // iscntrl(): is this char a control character (non-printable)?
#include <errno.h>   // errno and EAGAIN: error codes set by failed system calls
#include <fcntl.h>   // open(), O_RDWR, O_CREAT: low-level file opening
#include <stdarg.h>  // va_list, va_start, va_end: functions with "..." arguments
#include <stdio.h>   // snprintf(), sscanf(), perror(), FILE, fopen(), getline()
#include <stdlib.h>  // exit(), atexit(), malloc(), realloc(), free()
#include <string.h>  // memcpy(), memmove(), strlen(), strdup(), strerror()
#include <sys/ioctl.h> // ioctl(), struct winsize, TIOCGWINSZ: ask the terminal its size
#include <sys/types.h> // ssize_t: a signed size type (getline returns -1 on EOF)
#include <termios.h> // struct termios, tcgetattr(), tcsetattr(), terminal flags
#include <time.h>    // time(): used to expire the status message
#include <unistd.h>  // POSIX system calls (read, write, close, ftruncate...)

/*** defines ***/

#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 8   // a tab jumps to the next column divisible by 8
#define KILO_QUIT_TIMES 3 // times Ctrl-Q must be pressed to quit with unsaved changes

// Flags for editorSyntax.flags: each one is a single bit, so they can be combined with |.
#define HL_HIGHLIGHT_NUMBERS (1<<0)
#define HL_HIGHLIGHT_STRINGS (1<<1)

// Mimic what the Ctrl key does: zero out the top 3 bits of the character.
// CTRL_KEY('q') == 17, the byte the terminal sends for Ctrl-Q.
#define CTRL_KEY(k) ((k) & 0x1f)

// Named codes for special keys. Starting at 1000 keeps them out of the
// range of a normal char (0-255) so they can never collide with real characters.
// BACKSPACE is 127 because that is the byte terminals really send for it.
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

// What kind of thing each character in a row is, used to pick its color.
enum editorHighlight {
    HL_NORMAL = 0,
    HL_COMMENT,   // single-line comment
    HL_MLCOMMENT, // multi-line comment
    HL_KEYWORD1,  // control-flow keywords (if, while, ...)
    HL_KEYWORD2,  // type keywords (int, char, ...)
    HL_STRING,
    HL_NUMBER,
    HL_MATCH      // current search match
};

/*** data ***/

// One line of text in the file ("editor row").
typedef struct erow {
    int idx;       // this row's position in the file
    int size;      // length of chars
    int rsize;     // length of render
    char *chars;   // the line exactly as it is in the file
    char *render;  // the line as drawn on screen (tabs expanded to spaces)
    unsigned char *hl;    // one editorHighlight value per character in render
    int hl_open_comment;  // 1 if this row ends inside an unclosed /* comment */
} erow;

// Describes how to highlight one file type (language).
struct editorSyntax {
    char *filetype;                  // name shown in the status bar
    char **filematch;                // extensions (".c") or filename pieces; NULL-terminated
    char **keywords;                 // keywords; a trailing '|' marks a type keyword (KEYWORD2)
    char *singleline_comment_start;  // e.g. "//", or NULL if the language has none
    char *multiline_comment_start;   // e.g. "/*"
    char *multiline_comment_end;     // e.g. "*/"
    int flags;                       // which things to highlight (HL_HIGHLIGHT_* bits)
};

// Global editor state, so any function can reach it without passing arguments.
struct editorConfig {
    int cx, cy;                   // cursor position in the FILE (index into chars, row number)
    int rx;                       // cursor column in render (differs from cx when tabs exist)
    int rowoff;                   // first file row shown at the top of the screen (vertical scroll)
    int coloff;                   // first column shown at the left of the screen (horizontal scroll)
    int screenrows;               // rows available for text (terminal height minus the 2 bars)
    int screencols;               // terminal width in columns
    int numrows;                  // number of rows in the file
    erow *row;                    // dynamic array of all the file's rows
    int dirty;                    // nonzero if the file has unsaved changes
    char *filename;               // name of the open file, or NULL
    struct editorSyntax *syntax;  // highlighting rules for the open file, or NULL
    char statusmsg[80];           // text of the message bar
    time_t statusmsg_time;        // when the message was set (so it can expire)
    struct termios orig_termios;  // original terminal settings, restored on exit
};

struct editorConfig E;

/*** filetypes ***/

char *C_HL_extensions[] = {".c", ".h", ".cpp", NULL};

// keywords ending in '|' are types, highlighted in a second color
char *C_HL_keywords[] = {
    "switch", "if", "while", "for", "break", "continue", "return", "else",
    "struct", "union", "typedef", "static", "enum", "class", "case",

    "int|", "long|", "double|", "float|", "char|", "unsigned|", "signed|",
    "void|", NULL
};

// the highlight database: one entry per supported language
struct editorSyntax HLDB[] = {
    {
        "c",
        C_HL_extensions,
        C_HL_keywords,
        "//", "/*", "*/",
        HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
    },
};

// number of entries in HLDB (total bytes / bytes per entry)
#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

/*** prototypes ***/

// These let functions be called before their definitions appear in the file.
void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt, void (*callback)(char *, int));

/*** terminal ***/

// Print an error message and exit with failure status.
void die(const char *s) {
    // clear the screen first, so the error isn't buried under editor output
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    perror(s); // prints our label `s` plus a description of the current errno
    exit(1);   // non-zero exit status = failure (visible via `echo $?`)
}

// Restore the terminal to how we found it.
void disableRawMode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
        die("tcsetattr");
}

void enableRawMode() {
    // save the original settings so we can restore them later
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");

    // register disableRawMode to run automatically when the program exits
    atexit(disableRawMode);

    // work on a copy so E.orig_termios stays untouched
    struct termios raw = E.orig_termios;

    // input flags: BRKINT (break -> SIGINT), ICRNL (13 -> 10 translation),
    // INPCK (parity check), ISTRIP (strip 8th bit), IXON (Ctrl-S / Ctrl-Q)
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);

    // output flags: OPOST (turns "\n" into "\r\n"). we now print "\r\n" ourselves
    raw.c_oflag &= ~(OPOST);

    // control flags: use 8 bits per byte (this one is SET, not cleared)
    raw.c_cflag |= (CS8);

    // local flags: ECHO (print typed keys), ICANON (line-by-line input),
    // IEXTEN (Ctrl-V), ISIG (Ctrl-C / Ctrl-Z signals)
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

    // read() returns after at most 100 ms, even with no input (0 bytes read)
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

// Wait for one keypress and return it. Escape sequences (arrows, Home, etc.)
// are decoded into one of the editorKey values instead of several raw bytes.
int editorReadKey() {
    int nread;
    char c;

    // keep retrying until a byte arrives. nread == 0 is just the 100 ms timeout
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) die("read");
    }

    // 27 (esc) starts an escape sequence like esc [ A (up arrow)
    if (c == '\x1b') {
        char seq[3];

        // try to read the next 2 bytes. if they don't arrive (timeout),
        // the user just pressed the escape key by itself
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                // longer form: esc [ <digit> ~   (page up = esc[5~, delete = esc[3~ ...)
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1': return HOME_KEY;  // different terminals use
                        case '3': return DEL_KEY;   // different codes for
                        case '4': return END_KEY;   // home and end, so we
                        case '5': return PAGE_UP;   // accept all of them
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                }
            } else {
                // short form: esc [ <letter>
                switch (seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                }
            }
        } else if (seq[0] == 'O') {
            // some terminals send esc O H / esc O F for home / end
            switch (seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }

        // unrecognized sequence: treat it as a plain escape
        return '\x1b';
    } else {
        return c;
    }
}

// Fallback for finding the window size: ask the terminal where the cursor is.
// We move the cursor to the far bottom-right first, so its position = window size.
int getCursorPosition(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;

    // "esc [ 6 n" = "device status report": asks the terminal to report the cursor position
    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

    // the reply looks like "esc [ 24 ; 80 R". read until the 'R'
    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }
    buf[i] = '\0'; // make it a valid c string

    // check the reply starts with esc [ , then parse the two numbers after it
    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

    return 0;
}

// Get the terminal size. Returns 0 on success, -1 on failure.
int getWindowSize(int *rows, int *cols) {
    struct winsize ws;

    // ioctl asks the terminal driver for its size. it can fail on some systems,
    // and can "succeed" with a width of 0, so we check both
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        // fallback: "esc[999C" = cursor right 999, "esc[999B" = cursor down 999.
        // these stop at the screen edge, so the cursor ends up bottom-right
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        return getCursorPosition(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** syntax highlighting ***/

// Is c a character that separates "words"? Used to tell whole numbers and keywords
// apart from the middle of an identifier (the "if" in "gift" is not a keyword).
int is_separator(int c) {
    return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) != NULL;
}

// Fill row->hl: decide what highlight type every character in row->render gets.
void editorUpdateSyntax(erow *row) {
    // one hl entry per rendered char, all normal to start with
    row->hl = realloc(row->hl, row->rsize);
    memset(row->hl, HL_NORMAL, row->rsize);

    // unknown filetype: nothing to highlight
    if (E.syntax == NULL) return;

    // local shortcuts for the rules of this filetype
    char **keywords = E.syntax->keywords;
    char *scs = E.syntax->singleline_comment_start;
    char *mcs = E.syntax->multiline_comment_start;
    char *mce = E.syntax->multiline_comment_end;

    // lengths of the comment markers (0 if the language doesn't have them)
    int scs_len = scs ? strlen(scs) : 0;
    int mcs_len = mcs ? strlen(mcs) : 0;
    int mce_len = mce ? strlen(mce) : 0;

    int prev_sep = 1;   // was the previous char a separator? (start of line counts as one)
    int in_string = 0;  // 0 if not in a string, else the quote char that opened it (" or ')
    // we start inside a comment if the previous row ended inside one
    int in_comment = (row->idx > 0 && E.row[row->idx - 1].hl_open_comment);

    // a while loop instead of for, because some cases consume several chars at once
    int i = 0;
    while (i < row->rsize) {
        char c = row->render[i];
        // highlight type of the previous char, to continue numbers like 3.14
        unsigned char prev_hl = (i > 0) ? row->hl[i - 1] : HL_NORMAL;

        // single-line comment: "//" outside strings and /* */ comments colors the rest of the line
        if (scs_len && !in_string && !in_comment) {
            if (!strncmp(&row->render[i], scs, scs_len)) {
                memset(&row->hl[i], HL_COMMENT, row->rsize - i);
                break;
            }
        }

        // multi-line comment
        if (mcs_len && mce_len && !in_string) {
            if (in_comment) {
                row->hl[i] = HL_MLCOMMENT;
                if (!strncmp(&row->render[i], mce, mce_len)) {
                    // reached the closing marker: highlight all of it and leave the comment
                    memset(&row->hl[i], HL_MLCOMMENT, mce_len);
                    i += mce_len;
                    in_comment = 0;
                    prev_sep = 1;
                    continue;
                } else {
                    i++;
                    continue;
                }
            } else if (!strncmp(&row->render[i], mcs, mcs_len)) {
                // opening marker: highlight it and enter the comment
                memset(&row->hl[i], HL_MLCOMMENT, mcs_len);
                i += mcs_len;
                in_comment = 1;
                continue;
            }
        }

        // strings
        if (E.syntax->flags & HL_HIGHLIGHT_STRINGS) {
            if (in_string) {
                row->hl[i] = HL_STRING;
                // a backslash escapes the next char (so \" doesn't end the string)
                if (c == '\\' && i + 1 < row->rsize) {
                    row->hl[i + 1] = HL_STRING;
                    i += 2;
                    continue;
                }
                // only the same quote char that opened the string closes it
                if (c == in_string) in_string = 0;
                i++;
                prev_sep = 1;
                continue;
            } else {
                if (c == '"' || c == '\'') {
                    in_string = c; // remember which quote opened it
                    row->hl[i] = HL_STRING;
                    i++;
                    continue;
                }
            }
        }

        // numbers: a digit after a separator or another number char, or a '.' inside a number
        if (E.syntax->flags & HL_HIGHLIGHT_NUMBERS) {
            if ((isdigit(c) && (prev_sep || prev_hl == HL_NUMBER)) ||
                (c == '.' && prev_hl == HL_NUMBER)) {
                row->hl[i] = HL_NUMBER;
                i++;
                prev_sep = 0;
                continue;
            }
        }

        // keywords: only match at the start of a word
        if (prev_sep) {
            int j;
            for (j = 0; keywords[j]; j++) {
                int klen = strlen(keywords[j]);
                int kw2 = keywords[j][klen - 1] == '|'; // trailing | marks a type keyword
                if (kw2) klen--;

                // must match the text AND be followed by a separator ("int" but not "integer")
                if (!strncmp(&row->render[i], keywords[j], klen) &&
                    is_separator(row->render[i + klen])) {
                    memset(&row->hl[i], kw2 ? HL_KEYWORD2 : HL_KEYWORD1, klen);
                    i += klen;
                    break;
                }
            }
            // the loop only ends early (keywords[j] != NULL) if a keyword matched
            if (keywords[j] != NULL) {
                prev_sep = 0;
                continue;
            }
        }

        // nothing special: remember whether this char was a separator
        prev_sep = is_separator(c);
        i++;
    }

    // if this row's "ends inside a comment" status changed, the next row's
    // highlighting may change too, so update it (this ripples down the file)
    int changed = (row->hl_open_comment != in_comment);
    row->hl_open_comment = in_comment;
    if (changed && row->idx + 1 < E.numrows)
        editorUpdateSyntax(&E.row[row->idx + 1]);
}

// Map a highlight type to an ANSI foreground color code.
int editorSyntaxToColor(int hl) {
    switch (hl) {
        case HL_COMMENT:
        case HL_MLCOMMENT: return 36; // cyan
        case HL_KEYWORD1: return 33;  // yellow
        case HL_KEYWORD2: return 32;  // green
        case HL_STRING: return 35;    // magenta
        case HL_NUMBER: return 31;    // red
        case HL_MATCH: return 34;     // blue
        default: return 37;           // white
    }
}

// Pick E.syntax from the filename's extension, then rehighlight the whole file.
void editorSelectSyntaxHighlight() {
    E.syntax = NULL;
    if (E.filename == NULL) return;

    // pointer to the last '.' in the filename, or NULL if there is none
    char *ext = strrchr(E.filename, '.');

    for (unsigned int j = 0; j < HLDB_ENTRIES; j++) {
        struct editorSyntax *s = &HLDB[j];
        unsigned int i = 0;
        while (s->filematch[i]) {
            // patterns starting with '.' are extensions, others match anywhere in the name
            int is_ext = (s->filematch[i][0] == '.');
            if ((is_ext && ext && !strcmp(ext, s->filematch[i])) ||
                (!is_ext && strstr(E.filename, s->filematch[i]))) {
                E.syntax = s;

                // the filetype changed, so every row needs highlighting again
                for (int filerow = 0; filerow < E.numrows; filerow++) {
                    editorUpdateSyntax(&E.row[filerow]);
                }

                return;
            }
            i++;
        }
    }
}

/*** row operations ***/

// Convert a cursor index in chars (cx) to the matching column in render (rx).
// A tab is drawn as several spaces, so the two differ whenever a tab is before the cursor.
int editorRowCxToRx(erow *row, int cx) {
    int rx = 0;
    for (int j = 0; j < cx; j++) {
        if (row->chars[j] == '\t')
            // move to just before the next tab stop; the rx++ below lands on it
            rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
        rx++;
    }
    return rx;
}

// The reverse of editorRowCxToRx: convert a column in render (rx) back to an index in chars (cx).
// Needed by search, which finds matches in render but moves the cursor in chars.
int editorRowRxToCx(erow *row, int rx) {
    int cur_rx = 0;
    int cx;
    for (cx = 0; cx < row->size; cx++) {
        // same tab-stop math as editorRowCxToRx
        if (row->chars[cx] == '\t')
            cur_rx += (KILO_TAB_STOP - 1) - (cur_rx % KILO_TAB_STOP);
        cur_rx++;

        // stop at the first char whose rendered position passes rx
        if (cur_rx > rx) return cx;
    }
    return cx; // rx was past the end of the line
}

// Rebuild row->render from row->chars (currently: expand tabs into spaces).
void editorUpdateRow(erow *row) {
    // count tabs so we know how much memory render needs
    int tabs = 0;
    for (int j = 0; j < row->size; j++)
        if (row->chars[j] == '\t') tabs++;

    free(row->render); // free the old render (free(NULL) is safe)
    // each tab takes up to KILO_TAB_STOP chars; it already counts as 1 in size, hence the -1
    row->render = malloc(row->size + tabs * (KILO_TAB_STOP - 1) + 1);

    int idx = 0;
    for (int j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            // pad with spaces until the column is a multiple of the tab stop
            while (idx % KILO_TAB_STOP != 0) row->render[idx++] = ' ';
        } else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;

    editorUpdateSyntax(row); // render changed, so colors must be recomputed
}

// Insert a new row (a copy of the string s) at position `at` in the row array.
void editorInsertRow(int at, char *s, size_t len) {
    if (at < 0 || at > E.numrows) return; // invalid position, do nothing

    // grow the array by one erow
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    // shift every row from `at` onward down by one to open a gap
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));
    // every row after the new one moved down, so bump their idx
    for (int j = at + 1; j <= E.numrows; j++) E.row[j].idx++;

    E.row[at].idx = at;

    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    E.row[at].hl = NULL;
    E.row[at].hl_open_comment = 0;
    editorUpdateRow(&E.row[at]); // fill in render (and hl)

    E.numrows++;
    E.dirty++; // the file now differs from what's on disk
}

// Free the memory owned by one row.
void editorFreeRow(erow *row) {
    free(row->render);
    free(row->chars);
    free(row->hl);
}

// Remove the row at position `at`.
void editorDelRow(int at) {
    if (at < 0 || at >= E.numrows) return; // invalid position, do nothing

    editorFreeRow(&E.row[at]);
    // shift the rows after it up by one to close the gap
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
    // every row after the deleted one moved up, so lower their idx
    for (int j = at; j < E.numrows - 1; j++) E.row[j].idx--;
    E.numrows--;
    E.dirty++;
}

// Put a character into a row's chars array then rebuild render
void editorRowInsertChar(erow *row, int at, int c) {
    if (at < 0 || at > row->size) at = row->size;    // clamp to a valid position
    row->chars = realloc(row->chars, row->size + 2); // +1 new char, +1 for '\0'
    // shift the tail right by one (+1 so the '\0' moves too)
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    editorUpdateRow(row); // refresh the tab-expanded copy
    E.dirty++;
}

// Append a string to the end of a row (used when joining two lines).
void editorRowAppendString(erow *row, char *s, size_t len) {
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.dirty++;
}

// Delete the character at index `at` in a row.
void editorRowDelChar(erow *row, int at) {
    if (at < 0 || at >= row->size) return; // nothing there
    // shift the tail left by one, overwriting the deleted char (and moving the '\0')
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}

/*** editor operations ***/

// These work on the row under the cursor and move the cursor.

void editorInsertChar(int c) {
    // cursor is on the empty line past the end of the file: add a real row first
    if (E.cy == E.numrows) {
        editorInsertRow(E.numrows, "", 0);
    }
    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    E.cx++; // move past the character we just typed
}

// Enter key: split the current line at the cursor.
void editorInsertNewline() {
    if (E.cx == 0) {
        // cursor at the start of the line: just insert an empty row above
        editorInsertRow(E.cy, "", 0);
    } else {
        // move everything after the cursor into a new row below
        erow *row = &E.row[E.cy];
        editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
        // insertRow may have moved the array in memory, so get the pointer again
        row = &E.row[E.cy];
        // truncate the current row at the cursor
        row->size = E.cx;
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }
    // cursor goes to the start of the new row
    E.cy++;
    E.cx = 0;
}

// Backspace: delete the character to the left of the cursor.
void editorDelChar() {
    if (E.cy == E.numrows) return;          // on the empty line past the end: nothing to delete
    if (E.cx == 0 && E.cy == 0) return;     // very start of the file: nothing before it

    erow *row = &E.row[E.cy];
    if (E.cx > 0) {
        // normal case: delete the char before the cursor and step left
        editorRowDelChar(row, E.cx - 1);
        E.cx--;
    } else {
        // at the start of a line: join this line onto the end of the previous one
        E.cx = E.row[E.cy - 1].size; // cursor lands at the join point
        editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
        editorDelRow(E.cy);
        E.cy--;
    }
}

/*** file i/o ***/

// Join all rows into one big string with '\n' after each. The caller must free it.
char *editorRowsToString(int *buflen) {
    // add up the lengths, +1 per row for the newline
    int totlen = 0;
    for (int j = 0; j < E.numrows; j++)
        totlen += E.row[j].size + 1;
    *buflen = totlen; // hand the length back through the pointer

    char *buf = malloc(totlen);
    char *p = buf;
    for (int j = 0; j < E.numrows; j++) {
        memcpy(p, E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        *p = '\n';
        p++;
    }

    return buf;
}

// Load a file into E.row, one row per line.
void editorOpen(char *filename) {
    free(E.filename);
    E.filename = strdup(filename); // our own copy, shown in the status bar

    editorSelectSyntaxHighlight(); // pick the highlighting from the file extension

    FILE *fp = fopen(filename, "r");
    if (!fp) die("fopen");

    char *line = NULL;     // getline() allocates and grows this buffer for us
    size_t linecap = 0;    // size of that buffer
    ssize_t linelen;       // length of the line read, or -1 at end of file

    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        // strip the trailing newline / carriage return; each row stores just the text
        while (linelen > 0 && (line[linelen - 1] == '\n' ||
                               line[linelen - 1] == '\r'))
            linelen--;
        editorInsertRow(E.numrows, line, linelen);
    }

    free(line);
    fclose(fp);
    E.dirty = 0; // loading the file doesn't count as modifying it
}

// Write the file to disk (Ctrl-S).
void editorSave() {
    // no filename yet (started with just ./kilo): ask for one
    if (E.filename == NULL) {
        E.filename = editorPrompt("Save as: %s (ESC to cancel)", NULL);
        if (E.filename == NULL) {
            editorSetStatusMessage("Save aborted");
            return;
        }
        // the new name may have a different extension, so re-pick the highlighting
        editorSelectSyntaxHighlight();
    }

    int len;
    char *buf = editorRowsToString(&len);

    // open for read/write, create the file if it doesn't exist.
    // 0644 = owner can read/write, everyone else can only read
    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    if (fd != -1) {
        // set the file to exactly len bytes first, then write. if write
        // fails we lose less data than if we had emptied the file
        if (ftruncate(fd, len) != -1) {
            if (write(fd, buf, len) == len) {
                close(fd);
                free(buf);
                E.dirty = 0; // saved: no more unsaved changes
                editorSetStatusMessage("%d bytes written to disk", len);
                return;
            }
        }
        close(fd);
    }

    // anything above that failed ends up here
    free(buf);
    editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

/*** find ***/

// Called by editorPrompt after every keypress, so the search updates as you type.
// `query` is what's been typed so far, `key` is the key just pressed.
void editorFindCallback(char *query, int key) {
    // static: remembered between calls
    static int last_match = -1; // row of the last match, or -1 if none yet
    static int direction = 1;   // 1 = search forward, -1 = search backward

    // remember the row we recolored blue and its original colors, so we can undo it
    static int saved_hl_line;
    static char *saved_hl = NULL;

    // undo the previous match's blue highlight before doing anything else
    if (saved_hl) {
        memcpy(E.row[saved_hl_line].hl, saved_hl, E.row[saved_hl_line].rsize);
        free(saved_hl);
        saved_hl = NULL;
    }

    // enter or escape ends the search: reset state and stop
    if (key == '\r' || key == '\x1b') {
        last_match = -1;
        direction = 1;
        return;
    } else if (key == ARROW_RIGHT || key == ARROW_DOWN) {
        direction = 1;  // jump to the next match
    } else if (key == ARROW_LEFT || key == ARROW_UP) {
        direction = -1; // jump to the previous match
    } else {
        // the query text changed: start searching again from the top
        last_match = -1;
        direction = 1;
    }

    // no previous match means we always start by going forward
    if (last_match == -1) direction = 1;
    int current = last_match;

    // check each row once, starting after the last match and wrapping around the file
    for (int i = 0; i < E.numrows; i++) {
        current += direction;
        if (current == -1) current = E.numrows - 1;
        else if (current == E.numrows) current = 0;

        erow *row = &E.row[current];
        // strstr returns a pointer to the match inside render, or NULL
        char *match = strstr(row->render, query);
        if (match) {
            last_match = current;
            E.cy = current;
            // match - row->render is the match's index in render; convert it to chars
            E.cx = editorRowRxToCx(row, match - row->render);
            // force editorScroll to scroll so the matching line ends up at the top
            E.rowoff = E.numrows;

            // save this row's colors, then paint the match blue
            saved_hl_line = current;
            saved_hl = malloc(row->rsize);
            memcpy(saved_hl, row->hl, row->rsize);
            memset(&row->hl[match - row->render], HL_MATCH, strlen(query));
            break;
        }
    }
}

// Ctrl-F: incremental search.
void editorFind() {
    // remember where the cursor and scroll were, in case the user cancels
    int saved_cx = E.cx;
    int saved_cy = E.cy;
    int saved_coloff = E.coloff;
    int saved_rowoff = E.rowoff;

    char *query = editorPrompt("Search: %s (Use ESC/Arrows/Enter)", editorFindCallback);

    if (query) {
        free(query); // enter pressed: keep the cursor on the match
    } else {
        // escape pressed: put everything back
        E.cx = saved_cx;
        E.cy = saved_cy;
        E.coloff = saved_coloff;
        E.rowoff = saved_rowoff;
    }
}

/*** append buffer ***/

// A growable string. We collect the whole screen in one of these, then send it
// with a single write(). Many small writes cause visible flicker.
struct abuf {
    char *b;  // pointer to the heap memory holding the text
    int len;  // current length
};

#define ABUF_INIT {NULL, 0} // empty buffer constructor

void abAppend(struct abuf *ab, const char *s, int len) {
    // make a block big enough for the old text + the new text
    char *new = realloc(ab->b, ab->len + len);
    if (new == NULL) return; // out of memory: skip the append

    memcpy(&new[ab->len], s, len); // copy new text onto the end
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf *ab) {
    free(ab->b);
}

/*** output ***/

// Adjust rowoff / coloff so the cursor is always inside the visible window.
void editorScroll() {
    // work out the cursor's column in the rendered (tab-expanded) line
    E.rx = 0;
    if (E.cy < E.numrows) {
        E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
    }

    // vertical: cursor above the window -> scroll up to it
    if (E.cy < E.rowoff) {
        E.rowoff = E.cy;
    }
    // cursor below the window -> scroll down just enough to show it
    if (E.cy >= E.rowoff + E.screenrows) {
        E.rowoff = E.cy - E.screenrows + 1;
    }
    // horizontal: same logic using columns
    if (E.rx < E.coloff) {
        E.coloff = E.rx;
    }
    if (E.rx >= E.coloff + E.screencols) {
        E.coloff = E.rx - E.screencols + 1;
    }
}

// Draw each row of the screen into the buffer.
void editorDrawRows(struct abuf *ab) {
    for (int y = 0; y < E.screenrows; y++) {
        int filerow = y + E.rowoff; // which row of the file belongs on this screen row

        if (filerow >= E.numrows) {
            // past the end of the file: draw "~" (and the welcome message if no file is open)
            if (E.numrows == 0 && y == E.screenrows / 3) {
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome),
                    "Kilo editor -- version %s", KILO_VERSION);
                if (welcomelen > E.screencols) welcomelen = E.screencols;

                // center it: put a '~' in column 0, then pad with spaces
                int padding = (E.screencols - welcomelen) / 2;
                if (padding) {
                    abAppend(ab, "~", 1);
                    padding--;
                }
                while (padding--) abAppend(ab, " ", 1);

                abAppend(ab, welcome, welcomelen);
            } else {
                abAppend(ab, "~", 1);
            }
        } else {
            // draw the part of the line that's visible after horizontal scrolling
            int len = E.row[filerow].rsize - E.coloff;
            if (len < 0) len = 0;                       // line is scrolled completely off the left
            if (len > E.screencols) len = E.screencols; // cut off what doesn't fit on the right

            // c = the visible chars, hl = their highlight types
            char *c = &E.row[filerow].render[E.coloff];
            unsigned char *hl = &E.row[filerow].hl[E.coloff];
            int current_color = -1; // -1 = default color; avoids resending the same color code

            for (int j = 0; j < len; j++) {
                if (iscntrl(c[j])) {
                    // control chars would mess up the screen: show them as inverted symbols
                    // ctrl-a..ctrl-z become A..Z, anything else becomes ?
                    char sym = (c[j] <= 26) ? '@' + c[j] : '?';
                    abAppend(ab, "\x1b[7m", 4); // invert colors
                    abAppend(ab, &sym, 1);
                    abAppend(ab, "\x1b[m", 3);  // this also reset the text color...
                    if (current_color != -1) {  // ...so put it back
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", current_color);
                        abAppend(ab, buf, clen);
                    }
                } else if (hl[j] == HL_NORMAL) {
                    // normal text: switch back to the default color if we were in another one
                    if (current_color != -1) {
                        abAppend(ab, "\x1b[39m", 5);
                        current_color = -1;
                    }
                    abAppend(ab, &c[j], 1);
                } else {
                    // highlighted text: only send a color code when the color changes
                    int color = editorSyntaxToColor(hl[j]);
                    if (color != current_color) {
                        current_color = color;
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
                        abAppend(ab, buf, clen);
                    }
                    abAppend(ab, &c[j], 1);
                }
            }
            // end of the row: back to the default color
            abAppend(ab, "\x1b[39m", 5);
        }

        // "esc[K" = erase from the cursor to the end of the line
        abAppend(ab, "\x1b[K", 3);
        // newline after every row, since the status bar is drawn below the last one
        abAppend(ab, "\r\n", 2);
    }
}

// Inverted-color bar showing the file name and cursor position.
void editorDrawStatusBar(struct abuf *ab) {
    abAppend(ab, "\x1b[7m", 4); // "esc[7m" = invert colors (white background)

    char status[80], rstatus[80];
    // left side: filename (max 20 chars), line count, and a (modified) marker
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
        E.filename ? E.filename : "[No Name]", E.numrows,
        E.dirty ? "(modified)" : "");
    // right side: filetype and current line / total lines
    int rlen = snprintf(rstatus, sizeof(rstatus), "%s | %d/%d",
        E.syntax ? E.syntax->filetype : "no ft", E.cy + 1, E.numrows);

    if (len > E.screencols) len = E.screencols;
    abAppend(ab, status, len);

    // fill the rest of the bar with spaces, placing rstatus flush against the right edge
    while (len < E.screencols) {
        if (E.screencols - len == rlen) {
            abAppend(ab, rstatus, rlen);
            break;
        } else {
            abAppend(ab, " ", 1);
            len++;
        }
    }

    abAppend(ab, "\x1b[m", 3);  // "esc[m" = back to normal colors
    abAppend(ab, "\r\n", 2);    // room below for the message bar
}

// The last line of the screen: shows a temporary message for 5 seconds.
void editorDrawMessageBar(struct abuf *ab) {
    abAppend(ab, "\x1b[K", 3); // clear the line
    int msglen = strlen(E.statusmsg);
    if (msglen > E.screencols) msglen = E.screencols;
    // only show messages that are less than 5 seconds old
    if (msglen && time(NULL) - E.statusmsg_time < 5)
        abAppend(ab, E.statusmsg, msglen);
}

// Redraw the whole screen.
void editorRefreshScreen() {
    editorScroll(); // make sure the cursor is on screen first

    struct abuf ab = ABUF_INIT;

    abAppend(&ab, "\x1b[?25l", 6); // hide cursor while drawing (avoids flicker)
    abAppend(&ab, "\x1b[H", 3);    // move cursor to top-left

    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    // place the cursor. subtract the scroll offsets to convert from
    // file coordinates to screen coordinates, and +1 because terminals count from 1
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1,
                                              (E.rx - E.coloff) + 1);
    abAppend(&ab, buf, strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6); // show cursor again

    write(STDOUT_FILENO, ab.b, ab.len); // one single write for the whole frame
    abFree(&ab);
}

// printf-style function that sets the message bar text (the "..." is the format arguments).
void editorSetStatusMessage(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL); // remember when, so it can expire
}

/*** input ***/

// Show a prompt in the message bar and let the user type a line of text.
// `prompt` must contain "%s", which is replaced by what's typed so far.
// Returns the text (caller frees it), or NULL if the user pressed escape.
// `callback` (may be NULL) is called after every keypress with the text so far and the key.
char *editorPrompt(char *prompt, void (*callback)(char *, int)) {
    size_t bufsize = 128;
    char *buf = malloc(bufsize);

    size_t buflen = 0;
    buf[0] = '\0';

    while (1) {
        // redraw so the user sees the prompt and what they've typed
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();

        int c = editorReadKey();
        if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
            // erase the last typed character
            if (buflen != 0) buf[--buflen] = '\0';
        } else if (c == '\x1b') {
            // escape cancels: clear the message, free the buffer, return nothing
            editorSetStatusMessage("");
            if (callback) callback(buf, c);
            free(buf);
            return NULL;
        } else if (c == '\r') {
            // enter confirms, but only if something was typed
            if (buflen != 0) {
                editorSetStatusMessage("");
                if (callback) callback(buf, c);
                return buf;
            }
        } else if (!iscntrl(c) && c < 128) {
            // printable character: double the buffer if it's full, then append
            if (buflen == bufsize - 1) {
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }
            buf[buflen++] = c;
            buf[buflen] = '\0';
        }

        // let the caller react to this keypress (search uses this to update live)
        if (callback) callback(buf, c);
    }
}

// Move the cursor, keeping it inside the file's text.
void editorMoveCursor(int key) {
    // the row the cursor is on, or NULL if it's on the empty line past the end of the file
    erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

    switch (key) {
        case ARROW_LEFT:
            if (E.cx != 0) {
                E.cx--;
            } else if (E.cy > 0) {
                // at the start of a line: wrap to the end of the previous line
                E.cy--;
                E.cx = E.row[E.cy].size;
            }
            break;
        case ARROW_RIGHT:
            if (row && E.cx < row->size) {
                E.cx++;
            } else if (row && E.cx == row->size) {
                // at the end of a line: wrap to the start of the next line
                E.cy++;
                E.cx = 0;
            }
            break;
        case ARROW_UP:
            if (E.cy != 0) E.cy--;
            break;
        case ARROW_DOWN:
            // may go one past the last row (the empty line after the file)
            if (E.cy < E.numrows) E.cy++;
            break;
    }

    // the new line may be shorter than the old one: snap cx back to its end
    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0;
    if (E.cx > rowlen) E.cx = rowlen;
}

// Wait for a key, then act on it.
void editorProcessKeypress() {
    // static: keeps its value between calls, counting consecutive ctrl-q presses
    static int quit_times = KILO_QUIT_TIMES;

    int c = editorReadKey();

    switch (c) {
        case '\r':
            editorInsertNewline();
            break;

        case CTRL_KEY('q'):
            // unsaved changes: warn, and only quit after pressing ctrl-q several times in a row
            if (E.dirty && quit_times > 0) {
                editorSetStatusMessage("WARNING!!! File has unsaved changes. "
                    "Press Ctrl-Q %d more times to quit.", quit_times);
                quit_times--;
                return; // skip the reset at the bottom so the count survives
            }
            // clear the screen on the way out so the shell prompt starts clean
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;

        case CTRL_KEY('s'):
            editorSave();
            break;

        case CTRL_KEY('f'):
            editorFind();
            break;

        case HOME_KEY:
            E.cx = 0;
            break;
        case END_KEY:
            // end of the current line (not the edge of the screen anymore)
            if (E.cy < E.numrows)
                E.cx = E.row[E.cy].size;
            break;

        case BACKSPACE:
        case CTRL_KEY('h'): // ctrl-h sends byte 8, which is what old terminals used for backspace
        case DEL_KEY:
            // delete key = move right, then backspace (deletes the char under the cursor)
            if (c == DEL_KEY) editorMoveCursor(ARROW_RIGHT);
            editorDelChar();
            break;

        case PAGE_UP:
        case PAGE_DOWN:
            {   // braces needed: we declare a variable inside a case
                // first put the cursor at the top / bottom of the visible window...
                if (c == PAGE_UP) {
                    E.cy = E.rowoff;
                } else if (c == PAGE_DOWN) {
                    E.cy = E.rowoff + E.screenrows - 1;
                    if (E.cy > E.numrows) E.cy = E.numrows;
                }
                // ...then move a full screen's worth of rows
                int times = E.screenrows;
                while (times--)
                    editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
            }
            break;

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;

        case CTRL_KEY('l'): // ctrl-l normally refreshes the screen; we redraw constantly anyway
        case '\x1b':        // ignore stray escape sequences we don't handle
            break;

        default:
            // any other key is a normal character: type it into the text
            editorInsertChar(c);
            break;
    }

    // any key other than ctrl-q resets the quit confirmation counter
    quit_times = KILO_QUIT_TIMES;
}

/*** init ***/

void initEditor() {
    E.cx = 0; // cursor starts at the top-left
    E.cy = 0;
    E.rx = 0;
    E.rowoff = 0;   // not scrolled
    E.coloff = 0;
    E.numrows = 0;  // no file loaded yet
    E.row = NULL;
    E.dirty = 0;    // no unsaved changes yet
    E.filename = NULL;
    E.syntax = NULL; // no highlighting until a file with a known extension is opened
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;

    if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
    E.screenrows -= 2; // reserve the bottom 2 lines for the status bar and message bar
}

int main(int argc, char *argv[]) {
    enableRawMode();
    initEditor();

    // if a filename was given on the command line (./kilo file.txt), load it
    if (argc >= 2) {
        editorOpen(argv[1]);
    }

    editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find");

    // main loop: redraw, then wait for a key. ctrl-q exits from inside
    // editorProcessKeypress()
    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}
