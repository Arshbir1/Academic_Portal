#include "common.h"

static const char STUDENT_MENU[] =
    "Welcome to Student Menu\n"
    "1. View All Courses\n2. Enroll New Course\n3. Drop Course\n"
    "4. View Enrolled Courses\n5. Change Password\n6. Logout and Exit\nEnter Your Choice: ";

static int is_student_enrolled(const Course *course, const char *username) {
    for (int i = 0; i < course->enrolled; i++) {
        if (strcmp(course->students[i], username) == 0) {
            return 1;
        }
    }
    return 0;
}

// Menu 1
static void view_all_courses(int sock) {
    char details[MAX_PAYLOAD + 1];
    size_t used = 0;

    pthread_mutex_lock(&data_mutex);
    append_text(details, sizeof(details), &used, "All Courses:\n");
    for (int i = 0; i < course_count; i++) {
        if (append_text(details, sizeof(details), &used, "ID: %s, Name: %s, Faculty: %s, Seats: %d/%d\n",
                        courses[i].course_id, courses[i].name, courses[i].faculty,
                        courses[i].enrolled, courses[i].max_seats) < 0) {
            break;
        }
    }
    pthread_mutex_unlock(&data_mutex);

    send_message(sock, MSG_INFO, details);
}

// Menu 2
static void enroll_course(int sock, const User *me) {
    char course_id[MAX_COURSE_ID];
    if (prompt_text(sock, "Enter course ID: ", course_id, sizeof(course_id)) != INPUT_OK) {
        return;
    }

    // Every check and the insert happen in one critical section, so two students
    // racing for the last seat cannot both pass the capacity check.
    const char *reply;
    pthread_mutex_lock(&data_mutex);
    int idx = find_course(course_id);
    if (!me->active) {
        reply = ACCOUNT_BLOCKED_MSG;
    } else if (idx < 0) {
        reply = "Error: Course not found\n";
    } else if (courses[idx].enrolled >= courses[idx].max_seats) {
        reply = "Error: Course is full\n";
    } else if (is_student_enrolled(&courses[idx], me->username)) {
        reply = "Error: Already enrolled\n";
    } else {
        // enrolled < max_seats <= MAX_STUDENTS_PER_COURSE, so the slot is in bounds.
        Course *c = &courses[idx];
        begin_update();
        memcpy(c->students[c->enrolled], me->username, sizeof(c->students[c->enrolled]));
        c->enrolled++;
        reply = commit_update() == 0 ? "Enrolled successfully\n" : SAVE_FAILED_MSG;
    }
    pthread_mutex_unlock(&data_mutex);

    send_message(sock, MSG_INFO, reply);
}

// Menu 3
static void drop_course(int sock, const User *me) {
    char course_id[MAX_COURSE_ID];
    if (prompt_text(sock, "Enter course ID: ", course_id, sizeof(course_id)) != INPUT_OK) {
        return;
    }

    const char *reply = "Error: Not enrolled in course\n";
    pthread_mutex_lock(&data_mutex);
    int idx = find_course(course_id);
    if (!me->active) {
        reply = ACCOUNT_BLOCKED_MSG;
    } else if (idx < 0) {
        reply = "Error: Course not found\n";
    } else {
        Course *c = &courses[idx];
        for (int j = 0; j < c->enrolled; j++) {
            if (strcmp(c->students[j], me->username) == 0) {
                begin_update();
                for (int k = j; k < c->enrolled - 1; k++) {
                    memcpy(c->students[k], c->students[k + 1], sizeof(c->students[k]));
                }
                c->enrolled--;
                reply = commit_update() == 0 ? "Course dropped successfully\n" : SAVE_FAILED_MSG;
                break;
            }
        }
    }
    pthread_mutex_unlock(&data_mutex);

    send_message(sock, MSG_INFO, reply);
}

// Menu 4
static void view_enrolled_courses(int sock, const User *me) {
    char details[MAX_PAYLOAD + 1];
    size_t used = 0;

    pthread_mutex_lock(&data_mutex);
    append_text(details, sizeof(details), &used, "Enrolled Courses:\n");
    for (int i = 0; i < course_count; i++) {
        if (is_student_enrolled(&courses[i], me->username)) {
            if (append_text(details, sizeof(details), &used, "ID: %s, Name: %s, Faculty: %s\n",
                            courses[i].course_id, courses[i].name, courses[i].faculty) < 0) {
                break;
            }
        }
    }
    pthread_mutex_unlock(&data_mutex);

    send_message(sock, MSG_INFO, details);
}

static int account_active(const User *me) {
    pthread_mutex_lock(&data_mutex);
    int active = me->active;
    pthread_mutex_unlock(&data_mutex);
    return active;
}

void handle_student(int client_sock, User *current_user) {
    // An admin can block a student who is already logged in. The session ends
    // at the next menu or menu choice; operations that change data also re-check
    // active inside their own critical section, since the block can happen while
    // the student is still typing that operation's input.
    while (account_active(current_user)) {
        int choice;
        int rc = prompt_int(client_sock, STUDENT_MENU, 1, 6, &choice);
        if (rc == INPUT_CLOSED) {
            return;
        }
        if (rc == INPUT_INVALID) {
            continue; // error already sent; show the menu again
        }
        if (!account_active(current_user)) {
            break; // blocked while the menu was on screen
        }

        switch (choice) {
            case 1: view_all_courses(client_sock); break;
            case 2: enroll_course(client_sock, current_user); break;
            case 3: drop_course(client_sock, current_user); break;
            case 4: view_enrolled_courses(client_sock, current_user); break;
            case 5: change_password(client_sock, current_user); break;
            case 6: return;
        }
    }
    send_message(client_sock, MSG_INFO, ACCOUNT_BLOCKED_MSG);
}
