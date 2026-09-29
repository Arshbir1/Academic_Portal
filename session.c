#include "common.h"
#include <stdarg.h>

/*
 * Server-side helpers for one client session: asking for input, validating it,
 * and building text replies. Everything received from the client is treated as
 * untrusted: it is length-checked and parsed here before any handler uses it.
 */

// Parse the whole string s as a base-10 integer in [min, max].
// Returns 0 and stores the value in *out on success. Returns -1 for empty input,
// trailing characters ("10x"), overflow, or a value outside the range.
// strtol is used instead of atoi because atoi cannot report any of these errors.
int parse_int(const char *s, int min, int max, int *out) {
    char *end;
    errno = 0;
    long value = strtol(s, &end, 10);
    if (end == s || *end != '\0' || errno == ERANGE) {
        return -1;
    }
    if (value < min || value > max) {
        return -1;
    }
    *out = (int)value;
    return 0;
}

// Send a prompt (MSG_PROMPT or MSG_PROMPT_SECRET) and read the client's one-line
// reply into buf (capacity bytes, including the NUL). Replies that are empty or do
// not fit are rejected, never truncated: the client is told why and INPUT_INVALID
// is returned. Returns INPUT_OK, INPUT_INVALID, or INPUT_CLOSED.
static int prompt_reply(int fd, uint8_t prompt_type, const char *prompt, char *buf, size_t capacity) {
    char reply[MAX_INPUT + 1];
    uint8_t type;

    if (send_message(fd, prompt_type, prompt) < 0) {
        log_message("Client disconnected (send failed)");
        return INPUT_CLOSED;
    }
    ssize_t len = recv_message(fd, &type, reply, sizeof(reply));
    int rc = INPUT_OK;
    if (len < 0) {
        log_message(len == RECV_CLOSED ? "Client disconnected"
                                       : "Client connection failed or sent a malformed message");
        rc = INPUT_CLOSED;
    } else if (type != MSG_INPUT || strlen(reply) != (size_t)len) {
        log_message("Client broke the protocol (unexpected message type or embedded NUL)");
        rc = INPUT_CLOSED;
    } else if (len == 0) {
        send_message(fd, MSG_INFO, "Error: Input cannot be empty\n");
        rc = INPUT_INVALID;
    } else if ((size_t)len >= capacity) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Error: Input too long (max %zu characters)\n", capacity - 1);
        send_message(fd, MSG_INFO, msg);
        rc = INPUT_INVALID;
    } else {
        memcpy(buf, reply, (size_t)len + 1);
    }
    explicit_bzero(reply, sizeof(reply)); // it may have been a password
    return rc;
}

int prompt_text(int fd, const char *prompt, char *buf, size_t capacity) {
    return prompt_reply(fd, MSG_PROMPT, prompt, buf, capacity);
}

// For passwords: identical validation, but the client does not echo the input.
int prompt_secret(int fd, const char *prompt, char *buf, size_t capacity) {
    return prompt_reply(fd, MSG_PROMPT_SECRET, prompt, buf, capacity);
}

// Like prompt_text, but the reply must be a whole number in [min, max].
int prompt_int(int fd, const char *prompt, int min, int max, int *out) {
    char text[MAX_INPUT + 1];
    int rc = prompt_text(fd, prompt, text, sizeof(text));
    if (rc != INPUT_OK) {
        return rc;
    }
    if (parse_int(text, min, max, out) < 0) {
        char msg[80];
        snprintf(msg, sizeof(msg), "Error: Enter a whole number from %d to %d\n", min, max);
        send_message(fd, MSG_INFO, msg);
        return INPUT_INVALID;
    }
    return INPUT_OK;
}

// Append formatted text at buf + *used, never writing past capacity.
// Returns 0 on success; returns -1 if the text did not fit, in which case *used is
// unchanged and the caller should stop appending (buf stays NUL-terminated).
int append_text(char *buf, size_t capacity, size_t *used, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf + *used, capacity - *used, fmt, args);
    va_end(args);
    if (n < 0 || (size_t)n >= capacity - *used) {
        buf[*used] = '\0';
        return -1;
    }
    *used += (size_t)n;
    return 0;
}
