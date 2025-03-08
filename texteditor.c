#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>

/* Global editor config struct to store editor state */
struct editorConfig {
    int screenrows;          // Number of rows in editor window
    int screencols;          // Number of columns in editor window
    struct termios orig_termios;  // Original terminal attributes
};

struct editorConfig E;  // Global instance of editor config

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

/* Clear screen and reposition cursor */
void editorRefreshScreen() {
    write(STDOUT_FILENO, "\x1b[2J", 4);  // Clear entire screen
    write(STDOUT_FILENO, "\x1b[H", 3);   // Move cursor to top-left
}

/* Handle keypress input */
void editorProcessKeypress() {
    char c = '\0';
    read(STDIN_FILENO, &c, 1);  // Read one character
    
    switch (c) {
        case 'q':  // Quit on 'q' press
            write(STDOUT_FILENO, "\x1b[2J", 4);  // Clear screen
            write(STDOUT_FILENO, "\x1b[H", 3);   // Reset cursor
            exit(0);
            break;
    }
}

int main() {
    enableRawMode();  // Set up terminal for raw input
    
    while (1) {  // Main editor loop
        editorRefreshScreen();
        editorProcessKeypress();
    }
    
    return 0;
}
