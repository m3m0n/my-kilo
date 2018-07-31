/***
 * kilo.c
 * 20/07/18
 * Shaun Memon
 * https://viewsourcecode.org/snaptoken/kilo/
 ***/

/*** includes ***/

//defines get rid of compiler complaint re: implicit def'n of getline
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*** defines ***/

#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 4

#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
    BACKSPACE = 127,
    ARROW_LEFT = 1000, //move out of char range so as to not overlap with characters
    ARROW_RIGHT ,
    ARROW_UP ,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

/*** data ***/

//struct to hold a row of text
//rsize and render hold the rendered text, i.e. ti display tabs correctly (using spaces)
typedef struct erow {
    int size;
    int rsize;
    char *chars;
    char *render;
} erow;

//struct to hold global state of editor
struct editorConfig {
    int cx, cy; //cursor positions
    int rx;     //cursor position within rendered row
    int rowoff; //row offset for scrolling
    int coloff;
    int screenrows;
    int screencols;
    int numrows;
    erow *row;
    char *filename;
    char statusmsg[80];
    time_t statusmsg_time;
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
int editorReadKey() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) 
            die("read");
    }

    //Enable moving cursor with arrow keys. Arrow keys return <ESC>[+A-D
    if (c == '\x1b') {
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if (seq[0] == '[') {
            //PageUp&PageDown sent as <esc>[5~ and <esc>[6~
            if (seq[1] >= '0' && seq[1] <='9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                }
            } else {
                switch (seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                }
            }
        } else if (seq[0] =='O') {
            switch (seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }
        return '\x1b';
    } else {
        return c;
    }
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

/*** row operations ***/

int editorRowCxToRx(erow *row, int cx) {
    int rx = 0;
    
    for (int j = 0; j < cx; j++) {
        if (row->chars[j] == '\t')
            rx += (KILO_TAB_STOP -1) - (rx % KILO_TAB_STOP);
        rx++;
    }
    return rx;
}

/* editorUpdateRow takes a row and creates the string that will actually be displayed on screen
 * This will be used to correctly display tabs as well as other typically non-visible characters
 */
void editorUpdateRow(erow *row) {
    int tabs = 0;

    for (int j = 0; j < row->size; j++)
        if (row->chars[j] == '\t') tabs++;

    free(row->render);
    row->render = malloc(row->size + tabs*(KILO_TAB_STOP -1) + 1);

    int idx = 0;
    for (int j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while (idx % KILO_TAB_STOP != 0) row->render[idx++] = ' ';
        } else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;
}

void editorAppendRow(char *s, size_t len) {
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    
    int at = E.numrows;

    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    editorUpdateRow(&E.row[at]);

    E.numrows++;
}

void editorRowInsertChar(erow *row, int at, int c) {
    if (at < 0 || at > row->size) at = row->size;

    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    editorUpdateRow(row);
}

/*** editor operations ***/

void editorInsertChar(int c) {
    if (E.cy == E.numrows) {
        editorAppendRow("", 0);
    }
    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    E.cx++;
}

/*** file i/o ***/

void editorOpen(char *filename) {
    free(E.filename);
    E.filename = strdup(filename);

    FILE *fp = fopen(filename, "r");
    if (!fp) die("fopen");

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;

    while ((linelen = getline(&line, &linecap, fp)) != -1) {
    
        //strip off \r\n at end of line
        while (linelen > 0 && (line[linelen - 1] == '\n' ||
                               line[linelen - 1] == '\r'))
            linelen--;

        editorAppendRow(line,linelen);
    }
    free(line);
    fclose(fp);
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

void editorScroll() {
    E.rx = 0;
    if (E.cy < E.numrows) 
        E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);

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

/* editorDrawRows() inserts '~' along the left column as in vi
 */
void editorDrawRows(struct abuf *ab) {
    for (int y = 0; y < E.screenrows; y++) {
        int filerow = y + E.rowoff;

        if (filerow >= E.numrows) {
            if (E.numrows == 0 && y == E.screenrows /3) {
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
        } else {
            int len = E.row[filerow].rsize - E.coloff;
            if (len < 0) len = 0;
            if (len > E.screencols) len = E.screencols;
            abAppend(ab, &E.row[filerow].render[E.coloff], len);
        }

        abAppend(ab, "\x1b[K", 3); //K commands clears a line, default arg=0, clear line to right of cursor.
        abAppend(ab, "\r\n", 2);
    }
}

void editorDrawStatusBar(struct abuf *ab) {
    abAppend(ab, "\x1b[7m", 4); //<esc>[7m switches terminal to inverted colours

    char status[80], rstatus[80];

    //print file name and number of lines
    int len = snprintf(status, sizeof(status), "%.20s - %d lines",
            E.filename ? E.filename : "[No File]", E.numrows);
    //print current line/total lines (right side)
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", 
            E.cy + 1, E.numrows); //current line is in cy

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
    abAppend(ab, "\x1b[m", 3);//<esc>[m switches back to normal formatting
    abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab) {
    abAppend(ab, "\x1b[K", 3); //clear the line
    int msglen = strlen(E.statusmsg);
    if (msglen > E.screencols) msglen = E.screencols;
    //display message for 5 seconds
    if (msglen && time(NULL) - E.statusmsg_time < 5)
        abAppend(ab, E.statusmsg, msglen);
}

/* editorRefreshScreen() clears the screen.
 * https://vt100.net/docs/vt100-ug/chapter3.html#ED
 */
void editorRefreshScreen() {
    editorScroll();

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
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    //position the cursor in the right place as given in EditorState
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy - E.rowoff + 1, E.rx - E.coloff + 1);
    abAppend(&ab, buf, strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6); //reshow the cursor

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

/*** input ***/

void editorMoveCursor(int key) {
    erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

    switch (key) {
        case ARROW_LEFT:
            if (E.cx != 0) E.cx--;
            //Move left from beginning of line move to end of previous line
            else if (E.cy > 0) {
                E.cy--;
                E.cx = E.row[E.cy].size;
            }
            break;
        case ARROW_RIGHT:
            if (row && E.cx < row->size) E.cx++;
            else if (row && E.cx == row->size) {
                E.cy++;
                E.cx = 0;
            }
            break;
        case ARROW_UP:
            if (E.cy != 0) E.cy--;
            break;
        case ARROW_DOWN:
            if (E.cy < E.numrows - 1) E.cy++;
            break;
    }

    row = E.cy >= E.numrows ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0;
    if (E.cx > rowlen) E.cx = rowlen;

}

/* editorProcessKeypress() waits for a keypress and then handles it.
 * Role is to map keys to editor functions at a high level.
 */
void editorProcessKeypress() {
    int c = editorReadKey();

    switch(c) {
        case '\r':
            /* TODO */
            break;

        case CTRL_KEY('q'):
            //clear screen on exit
            write(STDOUT_FILENO, "\x1b[2J",4);
            write(STDOUT_FILENO, "\x1b[H",3);
            exit(0);
            break;

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;
        
        case PAGE_UP:
        case PAGE_DOWN:
            {
                if (c==PAGE_UP)
                    E.cy = E.rowoff;
                else if (c==PAGE_DOWN) {
                    E.cy = E.rowoff + E.screenrows -1;
                    if (E.cy > E.numrows) E.cy = E.numrows;
                }

                int times = E.screenrows;
                while (times--)
                    editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
            }
            break;

        case HOME_KEY:
            E.cx = 0;
            break;
        case END_KEY:
            if (E.cy < E.numrows)
                E.cx = E.row[E.cy].size;
            break;
        
        //ctrl(h) sends control code 8 which is original bksp.
        //today delete key sends <esc>[3~ and bksp is 127 (DEL)
        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
            /* TODO */
            break;
        
        //ctrl(l) traditionally to refresh screen. uncessesary as refresh on every keypress
        //esc used to ignore all other keys as editorReadKey returns esc on unhandled escape seq.
        case CTRL_KEY('l'):
        case '\x1b':
            break;

        default:
            editorInsertChar(c);
            break;

    }
}

/*** init ***/

/* initEditor: init all fields of the editorConfig struct
 */
void initEditor() {
    E.cx=0;
    E.cy=0;
    E.rx=0;
    E.rowoff=0;
    E.coloff=0;
    E.numrows=0;
    E.row = NULL;
    E.filename = NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;

    //get window size
    if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
    E.screenrows -= 2;
}

int main (int argc, char *argv[]) {
    enableRawMode();
    initEditor();
    
    if (argc >= 2) {
        editorOpen(argv[1]);
    }

    editorSetStatusMessage("HELP: Ctrl-Q to quite");
    
    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}
