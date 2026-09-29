#include "common.h"

static const char ADMIN_MENU[] =
    "Welcome to Admin Menu\n"
    "1. Add Student\n2. View Student Details\n3. Add Faculty\n4. View Faculty Details\n"
    "5. Activate Student\n6. Block Student\n7. Modify Student Details\n"
    "8. Modify Faculty Details\n9. Logout and Exit\nEnter Your Choice: ";

static const char *role_label(int role) {
    return role == STUDENT ? "Student" : "Faculty";
}

// Early check used only to fail fast before asking for more input.
// The answer may be stale by the time it is used, so every operation repeats
// the check under data_mutex before changing anything.
static int user_exists(const char *username, int role) {
    pthread_mutex_lock(&data_mutex);
    int exists = find_user(username, role) >= 0;
    pthread_mutex_unlock(&data_mutex);
    return exists;
}

// Keep courses[] consistent with a username change (caller holds data_mutex).
// Courses belong to a faculty member by username, and enrollment lists hold
// student usernames. new_name must be a zero-padded MAX_NAME array.
static void rename_in_courses(const char *old_name, const char *new_name, int role) {
    for (int i = 0; i < course_count; i++) {
        Course *c = &courses[i];
        if (role == FACULTY && strcmp(c->faculty, old_name) == 0) {
            memcpy(c->faculty, new_name, sizeof(c->faculty));
        }
        if (role == STUDENT) {
            for (int j = 0; j < c->enrolled; j++) {
                if (strcmp(c->students[j], old_name) == 0) {
                    memcpy(c->students[j], new_name, sizeof(c->students[j]));
                }
            }
        }
    }
}

// Menu 1 / 3: add a student or faculty account.
static void add_user(int sock, int role) {
    User new_user;
    memset(&new_user, 0, sizeof(new_user)); // no uninitialized bytes reach portal.dat
    new_user.role = role;
    new_user.active = 1;
    char password[MAX_PASSWORD];
    char success[64];
    snprintf(success, sizeof(success), "%s added successfully\n", role_label(role));

    // 1. Collect input and hash the password (no lock held: hashing is slow).
    if (prompt_text(sock, "Enter username: ", new_user.username, sizeof(new_user.username)) != INPUT_OK) {
        return;
    }
    if (user_exists(new_user.username, 0)) {
        send_message(sock, MSG_INFO, "Error: Username already exists\n");
        return;
    }
    int rc = prompt_secret(sock, "Enter password: ", password, sizeof(password));
    if (rc == INPUT_OK) {
        rc = hash_password(password, new_user.password_hash, sizeof(new_user.password_hash));
        if (rc < 0) {
            send_message(sock, MSG_INFO, HASH_FAILED_MSG);
        }
    }
    explicit_bzero(password, sizeof(password));
    if (rc != INPUT_OK) {
        return;
    }

    // 2. Re-check and apply under the lock: another admin may have taken the
    //    username while this one was typing the password.
    const char *reply;
    pthread_mutex_lock(&data_mutex);
    if (find_user(new_user.username, 0) >= 0) {
        reply = "Error: Username already exists\n";
    } else if (user_count >= MAX_USERS) {
        reply = "Error: User limit reached\n";
    } else {
        begin_update();
        users[user_count++] = new_user;
        reply = commit_update() == 0 ? success : SAVE_FAILED_MSG;
    }
    pthread_mutex_unlock(&data_mutex);

    // 3. Reply (no lock held).
    send_message(sock, MSG_INFO, reply);
}

// Menu 2 / 4: list students or faculty.
static void list_users(int sock, int role) {
    char details[MAX_PAYLOAD + 1];
    size_t used = 0;
    int found = 0;

    pthread_mutex_lock(&data_mutex);
    append_text(details, sizeof(details), &used, "%s Details:\n", role_label(role));
    for (int i = 0; i < user_count; i++) {
        if (users[i].role == role) {
            found = 1;
            if (append_text(details, sizeof(details), &used, "Username: %s, Active: %d\n",
                            users[i].username, users[i].active) < 0) {
                break;
            }
        }
    }
    pthread_mutex_unlock(&data_mutex);

    if (!found) {
        used = 0;
        append_text(details, sizeof(details), &used, "No %s found\n",
                    role == STUDENT ? "students" : "faculty");
    }
    send_message(sock, MSG_INFO, details);
}

// Menu 5 / 6: activate (active = 1) or block (active = 0) a student.
static void set_student_active(int sock, int active) {
    char username[MAX_NAME];
    if (prompt_text(sock, "Enter student username: ", username, sizeof(username)) != INPUT_OK) {
        return;
    }

    const char *reply;
    pthread_mutex_lock(&data_mutex);
    int idx = find_user(username, STUDENT);
    if (idx < 0) {
        reply = "Error: Student not found\n";
    } else if (users[idx].active == active) {
        reply = active ? "Student is already active\n" : "Student is already blocked\n";
    } else {
        begin_update();
        users[idx].active = active;
        if (commit_update() == 0) {
            reply = active ? "Student activated successfully\n" : "Student blocked successfully\n";
        } else {
            reply = SAVE_FAILED_MSG;
        }
    }
    pthread_mutex_unlock(&data_mutex);

    send_message(sock, MSG_INFO, reply);
}

// Menu 7 / 8: change a student's or faculty member's username and password.
static void modify_user(int sock, int role) {
    char prompt[64], not_found[64], success[64];
    snprintf(prompt, sizeof(prompt), "Enter %s username: ", role == STUDENT ? "student" : "faculty");
    snprintf(not_found, sizeof(not_found), "Error: %s not found\n", role_label(role));
    snprintf(success, sizeof(success), "%s details modified successfully\n", role_label(role));
    char username[MAX_NAME];
    char new_username[MAX_NAME] = {0}; // zero-padded: copied whole into records
    char password[MAX_PASSWORD];
    char new_hash[MAX_HASH] = {0};

    // 1. Collect every field into locals. Shared state is not touched until all
    //    input has arrived, so a disconnect here leaves the user unchanged.
    if (prompt_text(sock, prompt, username, sizeof(username)) != INPUT_OK) {
        return;
    }
    if (!user_exists(username, role)) {
        send_message(sock, MSG_INFO, not_found);
        return;
    }
    if (prompt_text(sock, "Enter new username: ", new_username, sizeof(new_username)) != INPUT_OK) {
        return;
    }
    if (strcmp(new_username, username) != 0 && user_exists(new_username, 0)) {
        send_message(sock, MSG_INFO, "Error: New username already exists\n");
        return;
    }
    int rc = prompt_secret(sock, "Enter new password: ", password, sizeof(password));
    if (rc == INPUT_OK) {
        rc = hash_password(password, new_hash, sizeof(new_hash)); // slow: no lock held
        if (rc < 0) {
            send_message(sock, MSG_INFO, HASH_FAILED_MSG);
        }
    }
    explicit_bzero(password, sizeof(password));
    if (rc != INPUT_OK) {
        return;
    }

    // 2. Re-check everything and apply the complete update atomically.
    const char *reply;
    pthread_mutex_lock(&data_mutex);
    int idx = find_user(username, role);
    int existing = find_user(new_username, 0);
    if (idx < 0) {
        reply = not_found; // renamed by another admin meanwhile
    } else if (existing >= 0 && existing != idx) {
        reply = "Error: New username already exists\n";
    } else {
        begin_update();
        rename_in_courses(users[idx].username, new_username, role);
        memcpy(users[idx].username, new_username, sizeof(users[idx].username));
        memcpy(users[idx].password_hash, new_hash, sizeof(users[idx].password_hash));
        reply = commit_update() == 0 ? success : SAVE_FAILED_MSG;
    }
    pthread_mutex_unlock(&data_mutex);

    // 3. Reply (no lock held).
    send_message(sock, MSG_INFO, reply);
}

void handle_admin(int client_sock, User *current_user) {
    (void)current_user; // same signature as the other role handlers; admin menu does not need it

    while (1) {
        int choice;
        int rc = prompt_int(client_sock, ADMIN_MENU, 1, 9, &choice);
        if (rc == INPUT_CLOSED) {
            return;
        }
        if (rc == INPUT_INVALID) {
            continue; // error already sent; show the menu again
        }

        switch (choice) {
            case 1: add_user(client_sock, STUDENT); break;
            case 2: list_users(client_sock, STUDENT); break;
            case 3: add_user(client_sock, FACULTY); break;
            case 4: list_users(client_sock, FACULTY); break;
            case 5: set_student_active(client_sock, 1); break;
            case 6: set_student_active(client_sock, 0); break;
            case 7: modify_user(client_sock, STUDENT); break;
            case 8: modify_user(client_sock, FACULTY); break;
            case 9: return;
        }
        // If a helper stopped because the client disconnected, the next menu
        // prompt fails too and the loop exits there.
    }
}
