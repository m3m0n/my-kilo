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
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/*** defines ***/

#define KILO_VERSION "0.0.1"

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

/* getCursorPosition calculates window size when cursor is in bottom left corner
 */
int getCursorPosition(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;

    //issues command to get cursor position
    if (write(STDOUT_FILENO, "\x1b[6n)", 4) != 4) return -1;

    //Read terminal response as \x1b[posY;posXR
    while (i < sizeof(buf) -1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break; //Response ends in 'R'
        i++;
    }
    buf[i]='\0';

    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    //use sscanf to parse response
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;
 
    return 0;
}

/* getWindowSize: query ioctl to TIOCGWINSZ Get WINdow SiZe
 * On Error: return -1, otherwise 0
 */
int getWindowSize(int *rows, int *cols) {
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        //ioctl does not work on every system, fall back method to get window size
        //move cursor 999C (right) and 999B (down). Both are guaranteed to not go offscreen.
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) 
            return -1;
        return getCursorPosition(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** append buffer ***/
//aquire all text to write to screen and then write all at once
//This avoids annoying delays and screen flickers
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

/*** output ***/

/* editorDrawRows() inserts '~' along the left column as in vi
 */
void editorDrawRows(struct abuf *ab) {
    for (int y = 0; y < E.screenrows; y++) {

        if (y == E.screenrows /3) {
            //Draw welcome message
            char welcome[80];
            int welcomeLen = snprintf(welcome, sizeof(welcome), "Kilo Editor -- version %s", KILO_VERSION);
            //Ensure not to go past end of terminal window
            if (welcomeLen > E.screencols) welcomeLen = E.screencols;

            //Centre welcome message
            int padding = (E.screencols - welcomeLen) /2;
            if (padding) {
                abAppend(ab, "~", 1);
                padding--;
            }
            while (padding--) abAppend(ab, " ", 1);

            abAppend(ab, welcome, welcomeLen);
        } else {
            abAppend(ab, "~", 1);
        }
        abAppend(ab, "\x1b[K", 3); //K commands clears a line, default arg=0, clear line to right of cursor.
        if (y < E.screenrows -1) 
            abAppend(ab, "\r\n", 2);
    }
}

/* editorRefreshScreen() clears the screen.
 * https://vt100.net/docs/vt100-ug/chapter3.html#ED
 */
void editorRefreshScreen() {
    //to avoid multiple consecutive writes to screen, which increases the chances of choppy reponsiveness, 
    //write everything to a buffer and then write it to screen all at once
    struct abuf ab = ABUF_INIT;

    //\x1b is the escape character (hex 27). ESC+[ is escape sequence.
    //h and l commands are the set and reset modes to turn on and off various terminal features
    //in this case we use it to hide the cursor while we draw the screen and then place it back
    abAppend(&ab, "\x1b[?25l", 6);
    //J is the VT100 command to clear screen with parameter 2, clear whole screen.
    //abAppend(&ab, "\x1b[2J", 4);
    //H repositions the cursor with two arguemnts
    //as written is equivalent to '<ESC>[1;1H'
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);

    abAppend(&ab, "\x1b[H", 3); //position cursor @1;1
    abAppend(&ab, "\x1b[?25h", 6); //reshow the cursor

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
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
