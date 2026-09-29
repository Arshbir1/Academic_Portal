#include "common.h"
#include <signal.h>
#include <termios.h>

#define READ_EOF      -1
#define READ_TOO_LONG -2

static struct termios saved_termios;
static volatile sig_atomic_t echo_disabled = 0;

// Read one line from stdin into buf (capacity bytes, including the NUL) and
// remove the trailing newline. Returns the line length, READ_EOF at end of input,
// or READ_TOO_LONG if the line has more than capacity - 1 characters; in that
// case the rest of the line is discarded, so nothing is silently truncated.
static int read_line(char *buf, size_t capacity) {
    if (fgets(buf, (int)capacity, stdin) == NULL) {
        return READ_EOF;
    }
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') {
        buf[--len] = '\0';
        return (int)len;
    }
    // No newline: either the input ended, or the line did not fit in buf.
    int c = getchar();
    if (c == '\n' || c == EOF) {
        return (int)len; // the line was exactly capacity - 1 characters long
    }
    while (c != '\n' && c != EOF) {
        c = getchar();
    }
    return READ_TOO_LONG;
}

// If Ctrl+C (or SIGTERM) arrives while echo is off, restore the terminal before
// dying, or the user's shell would be left not echoing. tcsetattr, signal and
// raise are async-signal-safe.
static void restore_terminal_and_die(int sig) {
    if (echo_disabled) {
        tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

// Like read_line, but with terminal echo turned off (for passwords).
// Echo is always turned back on before returning, whatever read_line returned.
static int read_secret_line(char *buf, size_t capacity) {
    if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &saved_termios) < 0) {
        return read_line(buf, capacity); // input is not a terminal: nothing is echoed anyway
    }
    struct termios no_echo = saved_termios;
    no_echo.c_lflag &= ~(tcflag_t)ECHO;
    echo_disabled = 1; // set first, so a signal during tcsetattr still restores
    tcsetattr(STDIN_FILENO, TCSANOW, &no_echo);

    int n = read_line(buf, capacity);

    tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios);
    echo_disabled = 0;
    printf("\n"); // the Enter key was not echoed either
    return n;
}

int main(void) {
    // If the server goes away, send() fails with EPIPE instead of killing the client.
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, restore_terminal_and_die);
    signal(SIGTERM, restore_terminal_and_die);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Socket creation failed");
        exit(1);
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Connection failed");
        exit(1);
    }

    char payload[MAX_PAYLOAD + 1];
    char line[MAX_INPUT + 1];
    int status = 1;

    while (1) {
        uint8_t type;
        ssize_t len = recv_message(sock, &type, payload, sizeof(payload));
        if (len == RECV_CLOSED) {
            printf("Server disconnected\n");
            break;
        }
        if (len < 0) {
            printf("Connection error or malformed message from server\n");
            break;
        }
        if (type != MSG_INFO && type != MSG_PROMPT && type != MSG_PROMPT_SECRET && type != MSG_BYE) {
            printf("Unexpected message type %u from server\n", (unsigned)type);
            break;
        }

        fwrite(payload, 1, (size_t)len, stdout);
        if (type == MSG_BYE) {
            status = 0;
            break;
        }
        if (type == MSG_INFO) {
            continue;
        }

        // MSG_PROMPT / MSG_PROMPT_SECRET: the server is now waiting for exactly one line.
        fflush(stdout);
        int secret = type == MSG_PROMPT_SECRET;
        int n;
        while ((n = secret ? read_secret_line(line, sizeof(line)) : read_line(line, sizeof(line))) == READ_TOO_LONG) {
            printf("Input too long (max %d characters). Try again: ", MAX_INPUT);
            fflush(stdout);
        }
        if (n == READ_EOF) {
            printf("\nEnd of input\n");
            break;
        }
        int sent = send_message(sock, MSG_INPUT, line);
        explicit_bzero(line, sizeof(line)); // may have been a password
        if (sent < 0) {
            printf("Server disconnected\n");
            break;
        }
    }

    close(sock);
    return status;
}
