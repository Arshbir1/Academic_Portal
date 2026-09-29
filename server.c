#include "common.h"
#include <signal.h>
#include <time.h>

User users[MAX_USERS];
Course courses[MAX_COURSES];
int user_count = 0;
int course_count = 0;
// The only lock in the server: protects users[], courses[], user_count and course_count.
pthread_mutex_t data_mutex = PTHREAD_MUTEX_INITIALIZER;

// Format the current local time. localtime_r is used because ctime() returns
// a static buffer shared by all threads.
static void format_time(char *buf, size_t size) {
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(buf, size, "%a %b %d %H:%M:%S %Y", &tm);
}

// Log errors with timestamp, errno, and thread ID
void log_error(const char *msg) {
    int saved_errno = errno; // formatting the time may overwrite errno
    char time_str[64];
    format_time(time_str, sizeof(time_str));
    fprintf(stderr, "[%s] TID %ld ERROR: %s (%s)\n", time_str, (long)gettid(), msg, strerror(saved_errno));
    fflush(stderr);
}

// Log message with thread ID. Never pass credentials or password hashes here.
void log_message(const char *msg) {
    char time_str[64];
    format_time(time_str, sizeof(time_str));
    fprintf(stderr, "[%s] TID %ld: %s\n", time_str, (long)gettid(), msg);
    fflush(stderr);
}

/*
 * Every change to shared state follows this pattern, all under data_mutex:
 *
 *     begin_update();            // remember the current state
 *     ...modify users[] / courses[]...
 *     if (commit_update() < 0)   // save; on failure the old state is restored
 *
 * so a client is never told an operation succeeded when it was not saved, and
 * memory never keeps a change that the disk does not have.
 * The undo copy is shared by all threads; that is safe because it is only used
 * between begin_update() and commit_update(), inside one hold of data_mutex.
 */
static User saved_users[MAX_USERS];
static Course saved_courses[MAX_COURSES];
static int saved_user_count;
static int saved_course_count;

void begin_update(void) {
    saved_user_count = user_count;
    saved_course_count = course_count;
    memcpy(saved_users, users, sizeof(User) * (size_t)user_count);
    memcpy(saved_courses, courses, sizeof(Course) * (size_t)course_count);
}

int commit_update(void) {
    if (save_data() == 0) {
        return 0;
    }
    log_message("Save failed; restoring the previous in-memory state");
    user_count = saved_user_count;
    course_count = saved_course_count;
    memcpy(users, saved_users, sizeof(User) * (size_t)user_count);
    memcpy(courses, saved_courses, sizeof(Course) * (size_t)course_count);
    // A save that fails before rename() leaves DATA_FILE holding the old state.
    // One that fails after rename() (directory fsync) may have left the new state
    // in place, so write the restored state again to make the file match memory.
    if (save_data() < 0) {
        log_message("Could not re-save the restored state; " DATA_FILE " may hold the rejected change");
    }
    return -1;
}

// Return the index of the user with this username (and role, unless role is 0), or -1.
// Caller must hold data_mutex.
int find_user(const char *username, int role) {
    for (int i = 0; i < user_count; i++) {
        if ((role == 0 || users[i].role == role) && strcmp(users[i].username, username) == 0) {
            return i;
        }
    }
    return -1;
}

// Return the index of the course with this ID, or -1. Caller must hold data_mutex.
int find_course(const char *course_id) {
    for (int i = 0; i < course_count; i++) {
        if (strcmp(courses[i].course_id, course_id) == 0) {
            return i;
        }
    }
    return -1;
}

// "Change Password" menu entry, shared by the faculty and student menus.
// Requires the current password, so an unattended logged-in session cannot be
// used to take over the account.
void change_password(int client_sock, User *current_user) {
    char current[MAX_PASSWORD];
    char password[MAX_PASSWORD];
    char stored_hash[MAX_HASH];
    char new_hash[MAX_HASH] = {0};
    char username[MAX_NAME];
    const char *reply = NULL;

    // 1. Collect input (no lock held).
    if (prompt_secret(client_sock, "Enter current password: ", current, sizeof(current)) != INPUT_OK ||
        prompt_secret(client_sock, "Enter new password: ", password, sizeof(password)) != INPUT_OK) {
        goto wipe;
    }

    // 2. Copy the stored hash under the lock ...
    pthread_mutex_lock(&data_mutex);
    memcpy(stored_hash, current_user->password_hash, sizeof(stored_hash));
    memcpy(username, current_user->username, sizeof(username));
    pthread_mutex_unlock(&data_mutex);

    // 3. ... and do both slow operations without it.
    if (!verify_password(current, stored_hash)) {
        reply = "Error: Current password is incorrect\n";
    } else if (hash_password(password, new_hash, sizeof(new_hash)) < 0) {
        reply = HASH_FAILED_MSG;
    } else {
        // 4. Re-check under the lock: while we were hashing, the account may have been
        //    blocked, or its password changed by another session or reset by an admin.
        pthread_mutex_lock(&data_mutex);
        if (!current_user->active) {
            reply = ACCOUNT_BLOCKED_MSG;
        } else if (strcmp(current_user->password_hash, stored_hash) != 0) {
            reply = "Error: The password was changed elsewhere; nothing was changed\n";
        } else {
            begin_update();
            memcpy(current_user->password_hash, new_hash, sizeof(current_user->password_hash));
            reply = commit_update() == 0 ? "Password changed successfully\n" : SAVE_FAILED_MSG;
        }
        pthread_mutex_unlock(&data_mutex);
    }

    // 5. Reply (no lock held).
    send_message(client_sock, MSG_INFO, reply);
    char msg[128];
    snprintf(msg, sizeof(msg), "Password change for '%s': %s", username,
             strcmp(reply, "Password changed successfully\n") == 0 ? "succeeded" : "rejected");
    log_message(msg);

wipe:
    explicit_bzero(current, sizeof(current));
    explicit_bzero(password, sizeof(password));
}

static void *handle_client(void *arg) {
    int client_sock = *(int *)arg;
    free(arg);
    User *current_user = NULL;
    char msg[128];

    log_message("Handling new client");
    int role;
    char username[MAX_NAME];
    char password[MAX_PASSWORD];
    if (prompt_int(client_sock, "Enter Login Type (1. Admin, 2. Faculty, 3. Student): ",
                   ADMIN, STUDENT, &role) != INPUT_OK ||
        prompt_text(client_sock, "Enter username: ", username, sizeof(username)) != INPUT_OK ||
        prompt_secret(client_sock, "Enter password: ", password, sizeof(password)) != INPUT_OK) {
        explicit_bzero(password, sizeof(password));
        send_message(client_sock, MSG_BYE, "Login aborted\n");
        close(client_sock);
        return NULL;
    }
    snprintf(msg, sizeof(msg), "Login attempt: role=%d, username='%s'", role, username);
    log_message(msg);

    // Copy what login needs while holding the lock; verify and reply after unlocking,
    // because verification is deliberately slow.
    // For an unknown username, verify against a dummy hash anyway, so the response
    // time does not reveal which usernames exist.
    static const char dummy_hash[] = "$y$j9T$JHhfCQZiMDu.VfkYfseuT.$.iKjOslZuo1gwveLQ7WMruzGDI6IiQzGN5azbCXarP9";
    char stored_hash[MAX_HASH] = {0};
    int active = 0;
    pthread_mutex_lock(&data_mutex);
    int idx = find_user(username, role);
    if (idx >= 0) {
        memcpy(stored_hash, users[idx].password_hash, sizeof(stored_hash));
        active = users[idx].active;
    }
    pthread_mutex_unlock(&data_mutex);

    int password_ok = verify_password(password, idx >= 0 ? stored_hash : dummy_hash);
    explicit_bzero(password, sizeof(password));

    if (idx >= 0 && password_ok) {
        if (role == STUDENT && !active) {
            send_message(client_sock, MSG_BYE, "Error: Account is inactive\n");
            close(client_sock);
            return NULL;
        }
        // users[] entries are never removed or moved, so this pointer stays valid.
        // Its fields may still change (rename, block): read them under data_mutex.
        current_user = &users[idx];
        snprintf(msg, sizeof(msg), "Login succeeded for username: %s", username);
        log_message(msg);
    }

    if (current_user) {
        send_message(client_sock, MSG_INFO, "Login successful\n");
        switch (role) {
            case ADMIN: handle_admin(client_sock, current_user); break;
            case FACULTY: handle_faculty(client_sock, current_user); break;
            case STUDENT: handle_student(client_sock, current_user); break;
        }
        // Fails harmlessly (EPIPE) if the handler returned because the client disconnected.
        send_message(client_sock, MSG_BYE, "Goodbye\n");
    } else {
        snprintf(msg, sizeof(msg), "Login failed for username: %s", username);
        log_message(msg);
        send_message(client_sock, MSG_BYE, "Error: Invalid credentials\n");
    }

    close(client_sock);
    return NULL;
}

int main(void) {
    int server_sock, client_sock;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len;
    pthread_t tid;

    // Writing to a socket whose peer has gone away raises SIGPIPE, whose default
    // action kills the whole process. Ignoring it makes send() fail with EPIPE
    // instead, which the thread serving that client handles as a disconnect.
    signal(SIGPIPE, SIG_IGN);

    // No client threads exist yet; the lock is taken only to follow the rule
    // that users[] and courses[] are touched with data_mutex held.
    pthread_mutex_lock(&data_mutex);
    int loaded = load_data();
    pthread_mutex_unlock(&data_mutex);
    if (loaded < 0) {
        exit(1); // the reason has been logged; never overwrite a data file we could not load
    }

    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        log_error("Socket creation failed");
        exit(1);
    }

    int opt = 1;
    if (setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        log_error("Setsockopt failed");
        exit(1);
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(PORT);

    if (bind(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        log_error("Bind failed");
        exit(1);
    }

    if (listen(server_sock, MAX_CLIENTS) < 0) {
        log_error("Listen failed");
        exit(1);
    }

    printf("Server running on port %d...\n", PORT);
    fflush(stdout); // stdout is fully buffered when redirected to a file

    while (1) {
        client_len = sizeof(client_addr); // value-result argument: reset before every accept()
        client_sock = accept(server_sock, (struct sockaddr *)&client_addr, &client_len);
        if (client_sock < 0) {
            log_error("Accept failed");
            continue;
        }

        int *client_sock_ptr = malloc(sizeof(int));
        if (client_sock_ptr == NULL) {
            log_error("Failed to allocate client socket argument");
            close(client_sock);
            continue;
        }
        *client_sock_ptr = client_sock;
        int err = pthread_create(&tid, NULL, handle_client, client_sock_ptr);
        if (err != 0) {
            errno = err; // pthread functions return the error code instead of setting errno
            log_error("Thread creation failed");
            free(client_sock_ptr);
            close(client_sock);
            continue; // tid is not a valid thread, so it must not be detached
        }
        pthread_detach(tid);
    }

    close(server_sock);
    return 0;
}
