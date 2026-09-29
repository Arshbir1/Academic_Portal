#ifndef COMMON_H
#define COMMON_H

// gettid() is a GNU extension: glibc only declares it when _GNU_SOURCE is
// defined before the first system header. Every .c file includes this header
// first, so defining it here covers the whole project.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

#define PORT 8080
#define MAX_CLIENTS 10
#define MAX_NAME 50
#define MAX_PASSWORD 50   // plaintext password buffer (up to 49 characters, as typed)
#define MAX_HASH 128      // stored hash string, e.g. yescrypt "$y$j9T$<salt>$<hash>" is 73 characters
#define MAX_USERS 100
#define MAX_COURSES 100
#define MAX_COURSE_ID 10
#define MAX_COURSE_NAME 50
#define MAX_STUDENTS_PER_COURSE 50

// The complete application state lives in one file in the server's working directory.
#define DATA_FILE "portal.dat"
#define DATA_TMP_FILE "portal.dat.tmp"

#define ADMIN 1
#define FACULTY 2
#define STUDENT 3

typedef struct {
    int role;
    char username[MAX_NAME];
    char password_hash[MAX_HASH]; // never the plaintext password
    int active;
} User;

typedef struct {
    char course_id[MAX_COURSE_ID];
    char name[MAX_COURSE_NAME];
    char faculty[MAX_NAME];
    int max_seats;
    int enrolled;
    char students[MAX_STUDENTS_PER_COURSE][MAX_NAME];
} Course;

/*
 * Shared server state, defined in server.c.
 *
 * Locking model: data_mutex is the only lock. It protects users[], courses[],
 * user_count and course_count, including every field of every record.
 *
 * Rules every handler follows:
 *   1. Collect all input from the client first, with data_mutex NOT held.
 *   2. Lock, then re-check every condition (checks made before locking may be stale).
 *   3. Apply the whole change and save it, still holding the lock.
 *   4. Unlock, then send the reply.
 * data_mutex is never held during network I/O (send/recv/prompt_*), so a slow
 * or stalled client can only ever block its own thread.
 */
extern User users[MAX_USERS];
extern Course courses[MAX_COURSES];
extern int user_count;
extern int course_count;
extern pthread_mutex_t data_mutex;

/*
 * Wire protocol (net.c)
 *
 * TCP delivers a byte stream, not messages, so every message is framed:
 *
 *     [type: 1 byte][length: 4 bytes, network byte order][payload: length bytes]
 *
 * The payload is text and carries no NUL terminator on the wire.
 */
#define MSG_HEADER_SIZE 5
#define MSG_INFO   1  // server -> client: text to display
#define MSG_PROMPT 2  // server -> client: text to display, then reply with exactly one MSG_INPUT
#define MSG_INPUT  3  // client -> server: one line typed by the user
#define MSG_BYE    4  // server -> client: final text; the server closes the connection next
#define MSG_PROMPT_SECRET 5  // like MSG_PROMPT, but the client must not echo what is typed (passwords)

#define MAX_PAYLOAD 16384  // largest payload either side sends or accepts (fits every listing)
#define MAX_INPUT   256    // longest line (in characters) a client may send

// recv_message() error results
#define RECV_CLOSED -1  // peer closed the connection cleanly between messages
#define RECV_ERROR  -2  // socket error, connection closed mid-message, or oversized message

int send_all(int fd, const void *buf, size_t len);
ssize_t recv_all(int fd, void *buf, size_t len);
int send_message(int fd, uint8_t type, const char *text);
ssize_t recv_message(int fd, uint8_t *type, char *buf, size_t capacity);

// Asking the client for input (session.c, server only)
#define INPUT_OK       0
#define INPUT_INVALID -1  // reply rejected; an error message has already been sent to the client
#define INPUT_CLOSED  -2  // client disconnected or broke the protocol

int parse_int(const char *s, int min, int max, int *out);
int prompt_text(int fd, const char *prompt, char *buf, size_t capacity);
int prompt_secret(int fd, const char *prompt, char *buf, size_t capacity);
int prompt_int(int fd, const char *prompt, int min, int max, int *out);
int append_text(char *buf, size_t capacity, size_t *used, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

// storage.c (caller holds data_mutex)
int save_data(void);  // 0 on success; -1 if nothing was committed (or see commit_update)
int load_data(void);  // 0 on success; -1 if the server must refuse to start

// server.c
void log_message(const char *msg);
void log_error(const char *msg);

// password.c (deliberately slow: never call with data_mutex held)
int hash_password(const char *password, char *hash, size_t hash_size);  // 0 on success, -1 on error
int verify_password(const char *password, const char *stored_hash);    // 1 if it matches
int find_user(const char *username, int role);  // role 0 matches any role; caller holds data_mutex
int find_course(const char *course_id);         // caller holds data_mutex
void begin_update(void);                        // caller holds data_mutex
int commit_update(void);                        // caller holds data_mutex
void change_password(int client_sock, User *current_user);

#define SAVE_FAILED_MSG     "Error: Could not save changes to disk; nothing was changed\n"
#define ACCOUNT_BLOCKED_MSG "Error: Your account has been blocked\n"
#define HASH_FAILED_MSG     "Error: Could not process the password; nothing was changed\n"

void handle_admin(int client_sock, User *current_user);
void handle_faculty(int client_sock, User *current_user);
void handle_student(int client_sock, User *current_user);

#endif
