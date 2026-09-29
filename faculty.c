#include "common.h"

static const char FACULTY_MENU[] =
    "Welcome to Faculty Menu\n"
    "1. View Offering Courses\n2. Add New Course\n3. Remove Course\n"
    "4. Update Course Details\n5. Change Password\n6. Logout and Exit\nEnter Your Choice: ";

static const char NOT_OWNED_MSG[] = "Error: Course not found or not owned\n";

// Return the index of the course with this ID if it belongs to this faculty member, or -1.
// Caller must hold data_mutex (me->username can change if an admin renames the account).
static int find_owned_course(const char *course_id, const User *me) {
    int idx = find_course(course_id);
    if (idx >= 0 && strcmp(courses[idx].faculty, me->username) != 0) {
        return -1;
    }
    return idx;
}

// Early checks used only to fail fast before asking for more input. The answer
// may be stale by the time it is used, so operations repeat the check under the lock.
static int course_exists(const char *course_id) {
    pthread_mutex_lock(&data_mutex);
    int exists = find_course(course_id) >= 0;
    pthread_mutex_unlock(&data_mutex);
    return exists;
}

static int owns_course(const char *course_id, const User *me) {
    pthread_mutex_lock(&data_mutex);
    int owned = find_owned_course(course_id, me) >= 0;
    pthread_mutex_unlock(&data_mutex);
    return owned;
}

// Menu 1
static void view_offered_courses(int sock, const User *me) {
    char details[MAX_PAYLOAD + 1];
    size_t used = 0;

    pthread_mutex_lock(&data_mutex);
    append_text(details, sizeof(details), &used, "Offered Courses:\n");
    for (int i = 0; i < course_count; i++) {
        if (strcmp(courses[i].faculty, me->username) == 0) {
            if (append_text(details, sizeof(details), &used, "ID: %s, Name: %s, Seats: %d/%d\n",
                            courses[i].course_id, courses[i].name,
                            courses[i].enrolled, courses[i].max_seats) < 0) {
                break;
            }
        }
    }
    pthread_mutex_unlock(&data_mutex);

    send_message(sock, MSG_INFO, details);
}

// Menu 2
static void add_course(int sock, const User *me) {
    Course new_course;
    memset(&new_course, 0, sizeof(new_course)); // no uninitialized bytes reach courses.dat

    // 1. Collect input (no lock held).
    if (prompt_text(sock, "Enter course ID: ", new_course.course_id, sizeof(new_course.course_id)) != INPUT_OK) {
        return;
    }
    if (course_exists(new_course.course_id)) {
        send_message(sock, MSG_INFO, "Error: Course ID already exists\n");
        return;
    }
    if (prompt_text(sock, "Enter course name: ", new_course.name, sizeof(new_course.name)) != INPUT_OK ||
        prompt_int(sock, "Enter max seats: ", 1, MAX_STUDENTS_PER_COURSE, &new_course.max_seats) != INPUT_OK) {
        return;
    }

    // 2. Re-check and apply under the lock.
    const char *reply;
    pthread_mutex_lock(&data_mutex);
    if (find_course(new_course.course_id) >= 0) {
        reply = "Error: Course ID already exists\n";
    } else if (course_count >= MAX_COURSES) {
        reply = "Error: Course limit reached\n";
    } else {
        // Read the owner's name now, under the lock: it may have been renamed meanwhile.
        memcpy(new_course.faculty, me->username, sizeof(new_course.faculty));
        begin_update();
        courses[course_count++] = new_course;
        reply = commit_update() == 0 ? "Course added successfully\n" : SAVE_FAILED_MSG;
    }
    pthread_mutex_unlock(&data_mutex);

    // 3. Reply (no lock held).
    send_message(sock, MSG_INFO, reply);
}

// Menu 3
static void remove_course(int sock, const User *me) {
    char course_id[MAX_COURSE_ID];
    if (prompt_text(sock, "Enter course ID: ", course_id, sizeof(course_id)) != INPUT_OK) {
        return;
    }

    const char *reply;
    pthread_mutex_lock(&data_mutex);
    int idx = find_owned_course(course_id, me);
    if (idx < 0) {
        reply = NOT_OWNED_MSG;
    } else {
        begin_update();
        for (int j = idx; j < course_count - 1; j++) {
            courses[j] = courses[j + 1];
        }
        course_count--;
        reply = commit_update() == 0 ? "Course removed successfully\n" : SAVE_FAILED_MSG;
    }
    pthread_mutex_unlock(&data_mutex);

    send_message(sock, MSG_INFO, reply);
}

// Menu 4
static void update_course(int sock, const User *me) {
    char course_id[MAX_COURSE_ID];
    char new_name[MAX_COURSE_NAME] = {0}; // zero-padded: copied whole into the record
    int new_seats;

    // 1. Collect every field into locals, so a disconnect leaves the course unchanged.
    if (prompt_text(sock, "Enter course ID: ", course_id, sizeof(course_id)) != INPUT_OK) {
        return;
    }
    if (!owns_course(course_id, me)) {
        send_message(sock, MSG_INFO, NOT_OWNED_MSG);
        return;
    }
    if (prompt_text(sock, "Enter new course name: ", new_name, sizeof(new_name)) != INPUT_OK ||
        prompt_int(sock, "Enter new max seats: ", 1, MAX_STUDENTS_PER_COURSE, &new_seats) != INPUT_OK) {
        return;
    }

    // 2. Re-check and apply the complete update atomically. The course may have
    //    been removed, and students may have enrolled, while input was collected.
    const char *reply;
    pthread_mutex_lock(&data_mutex);
    int idx = find_owned_course(course_id, me);
    if (idx < 0) {
        reply = NOT_OWNED_MSG;
    } else {
        begin_update();
        memcpy(courses[idx].name, new_name, sizeof(courses[idx].name));
        courses[idx].max_seats = new_seats;
        if (courses[idx].max_seats < courses[idx].enrolled) {
            courses[idx].max_seats = courses[idx].enrolled; // never below current enrollment
        }
        reply = commit_update() == 0 ? "Course updated successfully\n" : SAVE_FAILED_MSG;
    }
    pthread_mutex_unlock(&data_mutex);

    // 3. Reply (no lock held).
    send_message(sock, MSG_INFO, reply);
}

void handle_faculty(int client_sock, User *current_user) {
    while (1) {
        int choice;
        int rc = prompt_int(client_sock, FACULTY_MENU, 1, 6, &choice);
        if (rc == INPUT_CLOSED) {
            return;
        }
        if (rc == INPUT_INVALID) {
            continue; // error already sent; show the menu again
        }

        switch (choice) {
            case 1: view_offered_courses(client_sock, current_user); break;
            case 2: add_course(client_sock, current_user); break;
            case 3: remove_course(client_sock, current_user); break;
            case 4: update_course(client_sock, current_user); break;
            case 5: change_password(client_sock, current_user); break;
            case 6: return;
        }
    }
}
