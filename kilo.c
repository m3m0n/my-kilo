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
#include <unistd.h>
#include <termios.h>

/*** defines ***/

#define CTRL_KEY(k) ((k) & 0x1f)

/*** data ***/

struct termios orig_termios;

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
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios) == -1)
        die("tcsetattr");
}

/* enableRawMode() turns off various terminal flags to enable raw mode vs cononical mode.
 * Reveiving key presses right away, disabling \n being read and outputted as \r\n
 * Setting read timeout to 1, and minimum read amount to zero bytes, thus making read() non blocking.
 */
void enableRawMode() { 
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1)
        die("tcgetattr");

    //Set function to call upon exit (any means of exit)
    //This will disable raw mode and restore original termios struct upon exit.
    atexit(disableRawMode);

    struct termios raw = orig_termios;
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

/*** output ***/

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

int main (void) {
    enableRawMode();
    
    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}
