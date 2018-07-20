/***
 * kilo.c
 * 20/07/18
 * Shaun Memon
 * https://viewsourcecode.org/snaptoken/kilo/
 ***/

/*** includes ***/

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/*** defines ***/

#define CTRL_KEY(k) ((k) & 0x1f)

/*** data ***/

//struct to hold global state of editor
struct editorConfig {
    int screenrows;
    int screencols;
    struct termios orig_termios;
};

struct editorConfig E;

/*** terminal ***/

/* die() - Prints error message and exit
 */
void die(const char *s) {
    //Clear screen and write error message to screen before exit
    write(STDOUT_FILENO, "\x1b[2J",4);
    write(STDOUT_FILENO, "\x1b[H",3);
    perror(s);
    exit(1);
}

void disableRawMode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
        die("tcsetattr");
}

/* enableRawMode() turns off various terminal flags to enable raw mode vs cononical mode.
 * Reveiving key presses right away, disabling \n being read and outputted as \r\n
 * Setting read timeout to 1, and minimum read amount to zero bytes, thus making read() non blocking.
 */
void enableRawMode() { 
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
        die("tcgetattr");

    //Set function to call upon exit (any means of exit)
    //This will disable raw mode and restore original termios struct upon exit.
    atexit(disableRawMode);

    struct termios raw = E.orig_termios;
    //set input flags
    //ICRNL disables input of \n as \r\n
    raw.c_iflag &= ~(ICRNL | IXON | BRKINT | INPCK | ISTRIP);
    //set output flags
    //OPOST disables output processing including outputting \n as \r\n
    raw.c_oflag &= ~(OPOST);
    //set control flags
    //CS8 is bit mask to set 8bits per input byte
    raw.c_cflag |= (CS8);
    //set other flags
    //ECHO disables echoing character input back on screen (i.e. like password enter)
    //ICANON disable canonical mode
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    //set read minimum to zero bytes
    raw.c_cc[VMIN] = 0;
    //set read timeout to 1
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        die("tcsetattr");
}

/* editorReadKey() awaits for input from the terminal and passes that value back.
 * Deals with low-level terminal input
 */
char editorReadKey() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) 
            die("read");
    }
    return c;
}

/* getWindowSize: query ioctl to TIOCGWINSZ Get WINdow SiZe
 * On Error: return -1, otherwise 0
 */
int getWindowSize(int *rows, int *cols) {
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        return -1;
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** output ***/

/* editorDrawRows() inserts '~' along the left column as in vi
 */
void editorDrawRows() {
    for (int y = 0; y < E.screenrows; y++) {
        write(STDOUT_FILENO, "~\r\n", 3);
    }
}

/* editorRefreshScreen() clears the screen.
 * https://vt100.net/docs/vt100-ug/chapter3.html#ED
 */
void editorRefreshScreen() {
    //\x1b is the escape character (hex 27). ESC+[ is escape sequence.
    //J is the VT100 command to clear screen with parameter 2, clear whole screen.
    write(STDOUT_FILENO, "\x1b[2J", 4);
    //H repositions the cursor with two arguemnts
    //as written is equivalent to '<ESC>[1;1H'
    write(STDOUT_FILENO, "\x1b[H", 3);

    editorDrawRows();

    write(STDOUT_FILENO, "\x1b[H", 3);
}

/*** input ***/

/* editorProcessKeypress() waits for a keypress and then handles it.
 * Role is to map keys to editor functions at a high level.
 */
void editorProcessKeypress() {
    char c = editorReadKey();

    switch(c) {
        case CTRL_KEY('q'):
            //clear screen on exit
            write(STDOUT_FILENO, "\x1b[2J",4);
            write(STDOUT_FILENO, "\x1b[H",3);
            exit(0);
            break;
    }
}

/*** init ***/

/* initEditor: init all fields of the editorConfig struct
 */
void initEditor() {
    //get window size
    if (getWindowSize(&E.screenrows, &E.screencols) == -1)
        die("getWindowSize");
}

int main (void) {
    enableRawMode();
    initEditor();
    
    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}
