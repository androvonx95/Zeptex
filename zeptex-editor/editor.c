#include "editor.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <errno.h>

#define MAX_LINES 1000
#define MAX_LINE_LEN 1024

char *lines[MAX_LINES];
size_t line_count = 0;
size_t scroll_offset = 0;

struct termios orig_termios;

volatile sig_atomic_t resize_flag = 0;  // flag set by SIGWINCH handler

// Terminal raw mode handling

// Restore terminal settings to normal
void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

// Enable raw terminal mode for direct input handling
void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);

    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);  // Disable echo and canonical mode
    raw.c_cc[VMIN] = 1;              // Minimum number of bytes to read
    raw.c_cc[VTIME] = 0;             // No timeout

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

// Window resize handling

// Handle window resize signal
void handle_resize(int sig) {
    (void)sig;
    resize_flag = 1;
}

// Set up window resize signal handler
void setup_sigwinch_handler() {
    struct sigaction sa = {
        .sa_handler = handle_resize,
        .sa_flags = 0
    };
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGWINCH, &sa, NULL) == -1) {
        perror("sigaction");
        exit(1);
    }
}

// File operations

// Load file content into editor buffer
void load_file(const char *filename) {
    FILE *f = fopen(filename, "r");
    if (!f) return;

    char buf[MAX_LINE_LEN];
    while (fgets(buf, sizeof(buf), f)) {
        if (line_count >= MAX_LINES) break;
        buf[strcspn(buf, "\n")] = 0;
        lines[line_count++] = strdup(buf);
    }
    fclose(f);
}

// Save editor content to file
void save_file(const char *filename) {
    FILE *f = fopen(filename, "w");
    if (!f) return;

    for (size_t i = 0; i < line_count; ++i)
        fprintf(f, "%s\n", lines[i]);
    fclose(f);
}

// Buffer operations

// Insert a new line at specified index
void insert_line(size_t index, const char *text) {
    if (line_count >= MAX_LINES || index == 0 || index > line_count + 1) return;
    for (size_t i = line_count; i >= index; --i)
        lines[i] = lines[i - 1];
    lines[index - 1] = strdup(text);
    line_count++;
}

// Delete line at specified index
void delete_line(size_t index) {
    if (index == 0 || index > line_count) return;
    free(lines[index - 1]);
    for (size_t i = index - 1; i < line_count - 1; ++i)
        lines[i] = lines[i + 1];
    line_count--;
    if (scroll_offset > 0 && scroll_offset >= line_count)
        scroll_offset = line_count ? line_count - 1 : 0;
}

// Display functions

// Draw command bar with editor commands
void draw_command_bar() {
    struct winsize w;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
    int width = w.ws_col;

    const char *cmds[] = {
        "i N TEXT -- insert line|",
        "d N -- delete line|",
        "↑/↓ scroll|",
        "w <filename> -- save|",
        "q -- Quit|"
    };
    int cmd_count = sizeof(cmds) / sizeof(cmds[0]);

    int total_cmd_len = 0;
    for (int i = 0; i < cmd_count; i++)
        total_cmd_len += strlen(cmds[i]);

    int total_spaces = width - total_cmd_len;
    int gap = total_spaces > 0 ? total_spaces / (cmd_count - 1) : 1;

    printf("\n");
    for (int i = 0; i < cmd_count; i++) {
        printf("\033[1;97m%s\033[0m", cmds[i]);
        if (i < cmd_count - 1)
            printf("%*s", gap, "");
    }
    printf("\n");
}

// Draw main editor buffer with title and content
void draw_buffer() {
    printf("\033[H\033[J");

    struct winsize w;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);

    const char *title = "ZEPTEX EDITOR version 1.0";
    int padding = (w.ws_col - (int)strlen(title)) / 2;
    if (padding < 0) padding = 0;

    printf("%*s\033[1;97m%s\033[0m\n\n", padding > 0 ? padding : 0, "", title);

    size_t usable_rows = (w.ws_row > 5) ? (w.ws_row - 5) : 1;

    size_t max_scroll = (line_count > usable_rows) ? (line_count - usable_rows) : 0;
    if (scroll_offset > max_scroll) scroll_offset = max_scroll;

    for (size_t i = 0; i < usable_rows; ++i) {
        size_t line_index = i + scroll_offset;
        if (line_index < line_count) {
            printf("%3zu | %s\n", line_index + 1, lines[line_index]);
        } else {
            printf("~\n");
        }
    }

    draw_command_bar();
    fflush(stdout);
}




// Main editor loop
void run_editor(const char *filename) {
    char cmd[MAX_LINE_LEN] = {0};
    size_t cmd_len = 0;

    draw_buffer();
    printf(": ");
    fflush(stdout);
    
    while (1) {
        if (resize_flag) {
            draw_buffer();
            printf(": %s", cmd);
            fflush(stdout);
            resize_flag = 0;
        }

        char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n == -1) {
            if (errno == EINTR) {
                if (resize_flag) {
                    draw_buffer();
                    printf(": %s", cmd);
                    fflush(stdout);
                    resize_flag = 0;
                }
                continue;
            }
            continue;
        }
        if (n == 0) continue;

        if (c == '\r' || c == '\n') {
            cmd[cmd_len] = '\0';

            if (strcmp(cmd, "q") == 0) break;

            else if (cmd[0] == 'i') {
                int line_no = 0;
                char *p = cmd + 1; // points after 'i'
                
                // Check for exactly one space after 'i'
                if (*p != ' ') {
                    // invalid: no space immediately after 'i'
                    draw_buffer();
                    printf(": Invalid insert syntax. Use: i <line> <text>\n");
                    fflush(stdout);
                    goto after_command;
                }
                p++; // move past that one space
                
                // Now parse the line number - must be integer, no leading spaces allowed
                // So we scan digits from p until we find a space
                
                char *space_after_lineno = strchr(p, ' ');
                if (!space_after_lineno) {
                    // no space after lineno → no input text → invalid
                    draw_buffer();
                    printf(": Invalid insert syntax. Use: i <line> <text>\n");
                    fflush(stdout);
                    goto after_command;
                }
                
                // Temporarily null terminate after lineno to parse it
                *space_after_lineno = '\0';
                line_no = atoi(p);
                *space_after_lineno = ' '; // restore
                
                if (line_no <= 0) {
                    draw_buffer();
                    printf(": Invalid line number. Use: i <line> <text>\n");
                    fflush(stdout);
                    goto after_command;
                }
                
                // The input text is everything after that space (including multiple spaces)
                char *input_text = space_after_lineno + 1;
                
                insert_line((size_t)line_no, input_text);
                
                struct winsize w;
                ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
                size_t screen_lines = (w.ws_row > 3) ? (w.ws_row - 3) : 1;
            
                if ((size_t)line_no > scroll_offset + screen_lines)
                    scroll_offset = (size_t)line_no - screen_lines;
            
                size_t max_scroll = (line_count > screen_lines) ? (line_count - screen_lines) : 0;
                if (scroll_offset > max_scroll) scroll_offset = max_scroll;
                
            after_command:
                ;
            }

            
            else if (cmd[0] == 'a') {
                // Append command: 'a <text>' (exactly one space after 'a', then text)
                char *p = cmd + 1; // points after 'a'
            
                // Check for exactly one space after 'a'
                if (*p != ' ') {
                    draw_buffer();
                    printf(": Invalid append syntax. Use: a <text>\n");
                    fflush(stdout);
                    goto after_command_a;
                }
                p++; // move past that one space
            
                // Everything after that space is input text (including multiple spaces)
                char *input_text = p;
            
                if (*input_text == '\0') {
                    draw_buffer();
                    printf(": No text to append. Use: a <text>\n");
                    fflush(stdout);
                    goto after_command_a;
                }
            
                // Append at the end (line_count + 1)
                insert_line(line_count + 1, input_text);
            
                struct winsize w;
                ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
                size_t screen_lines = (w.ws_row > 3) ? (w.ws_row - 3) : 1;
            
                if (line_count > scroll_offset + screen_lines)
                    scroll_offset = line_count - screen_lines;
            
                size_t max_scroll = (line_count > screen_lines) ? (line_count - screen_lines) : 0;
                if (scroll_offset > max_scroll) scroll_offset = max_scroll;
            
            after_command_a:
                ;
            }
            
            
            else if (cmd[0] == 'd') {
                int line_no;
                if (sscanf(cmd, "d %d", &line_no) == 1)
                    delete_line((size_t)line_no);
            } else if (cmd[0] == 'w') {
                char fname[256];
                if (sscanf(cmd, "w %255s", fname) == 1)
                    save_file(fname);
                else if (filename)
                    save_file(filename);
            }

            cmd_len = 0;
            cmd[0] = '\0';
            draw_buffer();
            printf(": ");
            fflush(stdout);
            continue;
        } else if (c == 127 || c == '\b') {  // Backspace
            if (cmd_len > 0) cmd[--cmd_len] = '\0';
        } else if (c == '\033') {
            char seq[2];
            if (read(STDIN_FILENO, &seq[0], 1) != 1) continue;
            if (read(STDIN_FILENO, &seq[1], 1) != 1) continue;

            if (seq[0] == '[') {
                struct winsize w;
                ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
                size_t screen_lines = (w.ws_row > 3) ? (w.ws_row - 3) : 1;
                size_t max_scroll = (line_count > screen_lines) ? (line_count - screen_lines) : 0;

                if (seq[1] == 'A') {  // Up arrow
                    if (scroll_offset > 0) scroll_offset--;
                } else if (seq[1] == 'B') {  // Down arrow
                    if (scroll_offset < max_scroll) scroll_offset++;
                }
            }
        } else if (cmd_len < MAX_LINE_LEN - 1 && c >= 32 && c < 127) {
            cmd[cmd_len++] = c;
            cmd[cmd_len] = '\0';
        }

        draw_buffer();
        printf(": %s", cmd);
        fflush(stdout);
    }
}

// ========== MAIN ==========

int main(int argc, char **argv) {
    const char *filename = NULL;
    if (argc > 1) filename = argv[1];

    // Alt screen & cursor off
    printf("\033[?1049h\033[?25l");

    enable_raw_mode();

    setup_sigwinch_handler();

    if (filename) load_file(filename);

    run_editor(filename);

    disable_raw_mode();

    // Restore screen
    printf("\033[?1049l\033[?25h");

    for (size_t i = 0; i < line_count; ++i) free(lines[i]);

    return 0;
}
