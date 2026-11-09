# Kilo Text Editor

A text editor in C, built by following the tutorial [Build Your Own Text Editor](https://viewsourcecode.org/snaptoken/kilo/01.setup.html).

Build and run (Linux, macOS, or WSL): `make && ./kilo [filename]`

## Features

- Raw mode terminal input (no echo, no line buffering)
- Cursor movement (arrows, Home/End, Page Up/Down) with scrolling
- Insert, delete, and newline editing
- Open and save files (Ctrl-S)
- Incremental search (Ctrl-F), with arrow keys to jump between matches
- Syntax highlighting (numbers, strings, comments, keywords)
- Status bar and message bar

| Key | Action |
|-----|--------|
| Ctrl-S | Save |
| Ctrl-Q | Quit (press 3 times if there are unsaved changes) |
| Ctrl-F | Search |

## What I Learned

### C keywords and syntax

- `struct`: groups related data (`erow`, `editorConfig`, `abuf`). `typedef struct` lets you skip writing `struct` every time.

### Memory

- `malloc`, `realloc`, `free`: manual heap memory management.
- `memcpy`, `memmove`: copy bytes. `memmove` is safe for overlapping regions.
- Dynamic buffer pattern (`abuf`): grow with `realloc`, append with `memcpy`, release with `free`.
- Every `malloc` needs a matching `free`, or you leak memory.

### Terminal and system programming

- Canonical vs. raw mode: the terminal normally waits for Enter and handles Ctrl-C, Ctrl-Z, etc. for you.
- `termios`: `tcgetattr` / `tcsetattr` read and change terminal flags (`ECHO`, `ICANON`, `ISIG`, `IXON`, `OPOST`, ...).
- `atexit()`: registers a function to restore the terminal on exit.
- `read()` / `write()`: low-level I/O on file descriptors (`STDIN_FILENO`, `STDOUT_FILENO`).
- `ioctl(TIOCGWINSZ)`: get the terminal window size.
- `errno` / `perror()`: error reporting from system calls.
- `open`, `ftruncate`, `close`: low-level file saving.
- `getline()`: reads a line of any length from a `FILE *`.

### Escape sequences

- Start with `\x1b` (the Esc byte) followed by `[`.
- `\x1b[2J` clears the screen, `\x1b[H` moves the cursor home, `\x1b[<row>;<col>H` positions it.
- `\x1b[7m` inverts colors (status bar), `\x1b[m` resets formatting.
- `\x1b[3<n>m` sets text color (31 red, 32 green, 33 yellow, 34 blue, 35 magenta, 36 cyan).
- Arrow keys, Home, End, Delete, and Page Up/Down arrive as multi-byte escape sequences that vary by terminal.

### Editor design

- Each line is an `erow` with `chars` (raw text) and `render` (what is drawn, with tabs expanded).
- Separate cursor position (`cx`, `cy`) from rendered position (`rx`) because of tabs.
- Build the whole screen into one buffer and `write()` it once per refresh to avoid flicker.
- Row/column offsets (`rowoff`, `coloff`) handle scrolling.
- A `dirty` flag tracks unsaved changes.
- Syntax highlighting: one highlight byte per rendered char, plus a table of per-filetype rules (`editorSyntax`).
- Search uses a callback that runs on every keypress in the prompt.
