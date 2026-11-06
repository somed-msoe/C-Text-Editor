/*** includes ***/

#include <ctype.h>   // iscntrl(): is this char a control character (non-printable)?
#include <errno.h>   // errno and EAGAIN: error codes set by failed system calls
#include <stdio.h>   // snprintf(), sscanf(), perror()
#include <stdlib.h>  // exit(), atexit(), realloc(), free()
#include <string.h>  // memcpy(), strlen()
#include <sys/ioctl.h> // ioctl(), struct winsize, TIOCGWINSZ: ask the terminal its size
#include <termios.h> // struct termios, tcgetattr(), tcsetattr(), terminal flags
#include <unistd.h>  // POSIX system calls (low-level, unbuffered I/O and OS utilities)

/*** defines ***/

#define KILO_VERSION "0.0.1"

// Mimic what the Ctrl key does: zero out the top 3 bits of the character.
// CTRL_KEY('q') == 17, the byte the terminal sends for Ctrl-Q.
#define CTRL_KEY(k) ((k) & 0x1f)

// Named codes for special keys. Starting at 1000 keeps them out of the
// range of a normal char (0-255) so they can never collide with real characters.
// Each following name is automatically +1 (1001, 1002, ...).
enum editorKey {
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

/*** data ***/

// Global editor state, so any function can reach it without passing arguments.
struct editorConfig {
    int cx, cy;                   // cursor column and row (0-indexed)
    int screenrows;               // terminal height in rows
    int screencols;               // terminal width in columns
    struct termios orig_termios;  // original terminal settings, restored on exit
};

struct editorConfig E;

/*** terminal ***/

// Print an error message and exit with failure status.
void die(const char *s) {
    // Clear the screen first, so the error isn't buried under editor output.
    // "\x1b[2J" = erase whole screen, "\x1b[H" = move cursor to top-left.
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
    // Save the original settings so we can restore them later.
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");

    // Register disableRawMode to run automatically when the program exits.
    atexit(disableRawMode);

    // Work on a copy so E.orig_termios stays untouched.
    struct termios raw = E.orig_termios;

    // Input flags: BRKINT (break -> SIGINT), ICRNL (13 -> 10 translation),
    // INPCK (parity check), ISTRIP (strip 8th bit), IXON (Ctrl-S / Ctrl-Q).
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);

    // Output flags: OPOST (turns "\n" into "\r\n"). We now print "\r\n" ourselves.
    raw.c_oflag &= ~(OPOST);

    // Control flags: use 8 bits per byte (this one is SET, not cleared).
    raw.c_cflag |= (CS8);

    // Local flags: ECHO (print typed keys), ICANON (line-by-line input),
    // IEXTEN (Ctrl-V), ISIG (Ctrl-C / Ctrl-Z signals).
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

    // read() returns after at most 100 ms, even with no input (0 bytes read).
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

// Wait for one keypress and return it. Escape sequences (arrows, Home, etc.)
// are decoded into one of the editorKey values instead of several raw bytes.
int editorReadKey() {
    int nread;
    char c;

    // Keep retrying until a byte arrives. nread == 0 is just the 100 ms timeout.
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) die("read");
    }

    // 27 (ESC) starts an escape sequence like ESC [ A (up arrow).
    if (c == '\x1b') {
        char seq[3];

        // Try to read the next 2 bytes. If they don't arrive (timeout),
        // the user just pressed the Escape key by itself.
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                // Longer form: ESC [ <digit> ~   (Page Up = ESC[5~, Delete = ESC[3~ ...)
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1': return HOME_KEY;  // different terminals use
                        case '3': return DEL_KEY;   // different codes for
                        case '4': return END_KEY;   // Home and End, so we
                        case '5': return PAGE_UP;   // accept all of them
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                }
            } else {
                // Short form: ESC [ <letter>
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
            // Some terminals send ESC O H / ESC O F for Home / End.
            switch (seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }

        // Unrecognized sequence: treat it as a plain Escape.
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

    // "ESC [ 6 n" = "Device Status Report": asks the terminal to report the cursor position.
    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

    // The reply looks like "ESC [ 24 ; 80 R". Read until the 'R'.
    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }
    buf[i] = '\0'; // make it a valid C string

    // Check the reply starts with ESC [ , then parse the two numbers after it.
    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

    return 0;
}

// Get the terminal size. Returns 0 on success, -1 on failure.
int getWindowSize(int *rows, int *cols) {
    struct winsize ws;

    // ioctl asks the terminal driver for its size. It can fail on some systems,
    // and can "succeed" with a width of 0, so we check both.
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        // Fallback: "ESC[999C" = cursor right 999, "ESC[999B" = cursor down 999.
        // These stop at the screen edge, so the cursor ends up bottom-right.
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        return getCursorPosition(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
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
    // Make a block big enough for the old text + the new text.
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

// Draw each row of the screen into the buffer.
void editorDrawRows(struct abuf *ab) {
    for (int y = 0; y < E.screenrows; y++) {
        // Show the welcome message 1/3 of the way down the screen.
        if (y == E.screenrows / 3) {
            char welcome[80];
            int welcomelen = snprintf(welcome, sizeof(welcome),
                "Kilo editor -- version %s", KILO_VERSION);
            if (welcomelen > E.screencols) welcomelen = E.screencols; // truncate if narrow

            // Center it: put a '~' in column 0, then pad with spaces.
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

        // "ESC[K" = erase from the cursor to the end of the line.
        // Cheaper than clearing the whole screen every refresh.
        abAppend(ab, "\x1b[K", 3);

        // No newline after the last row, or the screen would scroll.
        if (y < E.screenrows - 1) {
            abAppend(ab, "\r\n", 2);
        }
    }
}

// Redraw the whole screen.
void editorRefreshScreen() {
    struct abuf ab = ABUF_INIT;

    abAppend(&ab, "\x1b[?25l", 6); // hide cursor while drawing (avoids flicker)
    abAppend(&ab, "\x1b[H", 3);    // move cursor to top-left

    editorDrawRows(&ab);

    // Move the cursor to its real position. Terminal coordinates start at 1,
    // but our E.cx / E.cy start at 0, hence the +1.
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
    abAppend(&ab, buf, strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6); // show cursor again

    write(STDOUT_FILENO, ab.b, ab.len); // one single write for the whole frame
    abFree(&ab);
}

/*** input ***/

// Move the cursor, without letting it leave the screen.
void editorMoveCursor(int key) {
    switch (key) {
        case ARROW_LEFT:
            if (E.cx != 0) E.cx--;
            break;
        case ARROW_RIGHT:
            if (E.cx != E.screencols - 1) E.cx++;
            break;
        case ARROW_UP:
            if (E.cy != 0) E.cy--;
            break;
        case ARROW_DOWN:
            if (E.cy != E.screenrows - 1) E.cy++;
            break;
    }
}

// Wait for a key, then act on it.
void editorProcessKeypress() {
    int c = editorReadKey();

    switch (c) {
        case CTRL_KEY('q'):
            // Clear the screen on the way out so the shell prompt starts clean.
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;

        case HOME_KEY:
            E.cx = 0;
            break;
        case END_KEY:
            E.cx = E.screencols - 1;
            break;

        case PAGE_UP:
        case PAGE_DOWN:
            {   // braces needed: we declare a variable inside a case
                // Page Up/Down = move the cursor a full screen's worth of rows.
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
    }
}

/*** init ***/

void initEditor() {
    E.cx = 0; // cursor starts at the top-left
    E.cy = 0;

    if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
}

int main() {
    enableRawMode();
    initEditor();

    // Main loop: redraw, then wait for a key. Ctrl-Q exits from inside
    // editorProcessKeypress().
    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}
