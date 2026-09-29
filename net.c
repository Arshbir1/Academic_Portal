#include "common.h"

/*
 * Transport helpers shared by the server and the client.
 *
 * TCP is a byte stream: one send() may be split across several recv() calls,
 * and several send()s may arrive in a single recv(). send() and recv() may
 * also transfer fewer bytes than asked, or fail with EINTR if a signal arrives.
 * These functions loop until a whole buffer or a whole message is transferred.
 */

// Send exactly len bytes. Returns 0 on success, -1 on error (errno is set).
int send_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

// Receive exactly len bytes.
// Returns len on success, fewer than len if the peer closed the connection
// first (0 means it closed before sending anything), or -1 on error.
ssize_t recv_all(int fd, void *buf, size_t len) {
    char *p = buf;
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, p + got, len - got, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            break; // peer closed the connection
        }
        got += (size_t)n;
    }
    return (ssize_t)got;
}

// Send one framed message whose payload is the C string text (without its NUL).
// Returns 0 on success, -1 on error.
int send_message(int fd, uint8_t type, const char *text) {
    size_t len = strlen(text);
    if (len > MAX_PAYLOAD) {
        errno = EMSGSIZE;
        return -1;
    }

    // Header and payload go out in one buffer: two small back-to-back sends
    // can be delayed by Nagle's algorithm interacting with delayed ACKs.
    char frame[MSG_HEADER_SIZE + MAX_PAYLOAD];
    uint32_t net_len = htonl((uint32_t)len);
    frame[0] = (char)type;
    memcpy(frame + 1, &net_len, sizeof(net_len));
    memcpy(frame + MSG_HEADER_SIZE, text, len);
    return send_all(fd, frame, MSG_HEADER_SIZE + len);
}

// Receive one framed message into buf and NUL-terminate the payload.
// capacity is the size of buf, so payloads longer than capacity - 1 are rejected
// before anything is read into buf.
// Returns the payload length, RECV_CLOSED, or RECV_ERROR.
ssize_t recv_message(int fd, uint8_t *type, char *buf, size_t capacity) {
    unsigned char header[MSG_HEADER_SIZE];
    ssize_t n = recv_all(fd, header, sizeof(header));
    if (n == 0) {
        return RECV_CLOSED;
    }
    if (n != (ssize_t)sizeof(header)) {
        return RECV_ERROR; // socket error, or closed partway through the header
    }

    uint32_t net_len;
    memcpy(&net_len, header + 1, sizeof(net_len));
    uint32_t len = ntohl(net_len);
    if (capacity == 0 || len > capacity - 1) {
        errno = EMSGSIZE;
        return RECV_ERROR;
    }

    n = recv_all(fd, buf, len);
    if (n != (ssize_t)len) {
        return RECV_ERROR; // socket error, or closed partway through the payload
    }
    buf[len] = '\0';
    *type = header[0];
    return (ssize_t)len;
}
