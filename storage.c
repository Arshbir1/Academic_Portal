#include "common.h"
#include <stdarg.h>

/*
 * Persistence: the complete application state is stored in ONE file, DATA_FILE,
 * so users and courses are always committed together.
 *
 * On-disk format, version 2. Integers are uint32 in host byte order (this is a
 * Linux project; the file is not meant to move between architectures). Strings
 * are fixed width and NUL-padded.
 *
 *   header   magic "ACAD" (4) | version (4) | user_count (4) | course_count (4)       16 bytes
 *   user     role (4) | active (4) | username (MAX_NAME) | password hash (MAX_HASH)   186 bytes
 *   course   course_id (MAX_COURSE_ID) | name (MAX_COURSE_NAME) | faculty (MAX_NAME)
 *            | max_seats (4) | enrolled (4)
 *            | students (MAX_STUDENTS_PER_COURSE x MAX_NAME, unused slots zeroed)     2618 bytes
 *
 * The file is header, then user_count user records, then course_count course records.
 * Every field is copied into a byte buffer explicitly instead of writing the User
 * and Course structs with sizeof(): the file then never contains padding, compiler
 * layout decisions, or anything that is not application data.
 */

#define DATA_MAGIC "ACAD"
#define DATA_MAGIC_SIZE 4
#define DATA_VERSION 2 // version 1 stored a 50-byte djb2 hash; not migrated
#define HEADER_SIZE (DATA_MAGIC_SIZE + 3 * 4)
#define USER_RECORD_SIZE (4 + 4 + MAX_NAME + MAX_HASH)
#define COURSE_RECORD_SIZE (MAX_COURSE_ID + MAX_COURSE_NAME + MAX_NAME + 4 + 4 + \
                            MAX_STUDENTS_PER_COURSE * MAX_NAME)
#define MAX_FILE_SIZE (HEADER_SIZE + MAX_USERS * USER_RECORD_SIZE + MAX_COURSES * COURSE_RECORD_SIZE)

// Files written by versions before the single-file format.
#define LEGACY_USER_FILE "users.dat"
#define LEGACY_COURSE_FILE "courses.dat"

// The serialized file image (at most ~272 KB). Used by save_data(), whose caller
// holds data_mutex, and by load_data() before any client thread exists.
static unsigned char file_buf[MAX_FILE_SIZE];

#ifdef FAULT_INJECTION
/*
 * Test-only hook, compiled in only with -DFAULT_INJECTION (make fault-injection).
 *   PORTAL_FAULT="crash:<point>"  kills the process at that point of save_data()
 *   PORTAL_FAULT="fail:<point>"   makes that step report an I/O error
 * Points: after_write, after_fsync, before_rename, after_rename.
 */
static int fault_at(const char *point) {
    const char *spec = getenv("PORTAL_FAULT");
    const char *colon = spec ? strchr(spec, ':') : NULL;
    if (colon == NULL || strcmp(colon + 1, point) != 0) {
        return 0;
    }
    if (strncmp(spec, "crash:", 6) == 0) {
        log_message("FAULT INJECTION: process crashing inside save_data()");
        _exit(99);
    }
    errno = EIO;
    return 1;
}
#else
#define fault_at(point) 0
#endif

// ---------------------------------------------------------------- low-level I/O

// Write exactly len bytes (write() may write less than asked). Returns 0 or -1.
static int write_all(int fd, const unsigned char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

// Read up to len bytes, stopping early only at end of file. Returns the count or -1.
static ssize_t read_all(int fd, unsigned char *buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, buf + got, len - got);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            break;
        }
        got += (size_t)n;
    }
    return (ssize_t)got;
}

// rename() modifies the directory, not the file. Until the directory is fsync'd,
// a power failure can lose the rename even though the new file's data is on disk.
static int fsync_directory(const char *path) {
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        return -1;
    }
    int rc = fsync(fd);
    int saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return rc;
}

// ---------------------------------------------------------------- serialization

static unsigned char *put_u32(unsigned char *p, uint32_t value) {
    memcpy(p, &value, sizeof(value));
    return p + sizeof(value);
}

// Store a string in a fixed-width field, NUL-padded, so no stray bytes from
// memory reach the file and identical states produce identical files.
static unsigned char *put_string(unsigned char *p, const char *s, size_t width) {
    size_t len = strnlen(s, width - 1);
    memcpy(p, s, len);
    memset(p + len, 0, width - len);
    return p + width;
}

static const unsigned char *get_u32(const unsigned char *p, uint32_t *value) {
    memcpy(value, p, sizeof(*value));
    return p + sizeof(*value);
}

// Copies the raw field; the caller must validate it before using it as a C string.
static const unsigned char *get_string(const unsigned char *p, char *dst, size_t width) {
    memcpy(dst, p, width);
    return p + width;
}

// Encode the whole in-memory state into buf. Returns the number of bytes used.
static size_t serialize_state(unsigned char *buf) {
    unsigned char *p = buf;
    memcpy(p, DATA_MAGIC, DATA_MAGIC_SIZE);
    p += DATA_MAGIC_SIZE;
    p = put_u32(p, DATA_VERSION);
    p = put_u32(p, (uint32_t)user_count);
    p = put_u32(p, (uint32_t)course_count);

    for (int i = 0; i < user_count; i++) {
        const User *u = &users[i];
        p = put_u32(p, (uint32_t)u->role);
        p = put_u32(p, (uint32_t)u->active);
        p = put_string(p, u->username, MAX_NAME);
        p = put_string(p, u->password_hash, MAX_HASH);
    }
    for (int i = 0; i < course_count; i++) {
        const Course *c = &courses[i];
        p = put_string(p, c->course_id, MAX_COURSE_ID);
        p = put_string(p, c->name, MAX_COURSE_NAME);
        p = put_string(p, c->faculty, MAX_NAME);
        p = put_u32(p, (uint32_t)c->max_seats);
        p = put_u32(p, (uint32_t)c->enrolled);
        for (int j = 0; j < MAX_STUDENTS_PER_COURSE; j++) {
            p = put_string(p, j < c->enrolled ? c->students[j] : "", MAX_NAME);
        }
    }
    return (size_t)(p - buf);
}

// ---------------------------------------------------------------- validation

// Record why the file was rejected. Returns -1 so callers can write `return reject(...)`.
static int reject(char *why, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static int reject(char *why, size_t size, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(why, size, fmt, args);
    va_end(args);
    return -1;
}

// A fixed-width field may only be used as a C string if it has a NUL within its width.
static int is_valid_string(const char *field, size_t width) {
    return memchr(field, '\0', width) != NULL && field[0] != '\0';
}

// Decode and validate a file image into users[] / courses[].
// Returns 0, or -1 with the reason written to why.
static int decode_state(const unsigned char *buf, size_t size, char *why, size_t why_size) {
    if (size < HEADER_SIZE) {
        return reject(why, why_size, "file is %zu bytes, smaller than the %d-byte header", size, HEADER_SIZE);
    }
    if (memcmp(buf, DATA_MAGIC, DATA_MAGIC_SIZE) != 0) {
        return reject(why, why_size, "bad magic number (not a portal data file)");
    }
    uint32_t version, ucount, ccount;
    const unsigned char *p = buf + DATA_MAGIC_SIZE;
    p = get_u32(p, &version);
    p = get_u32(p, &ucount);
    p = get_u32(p, &ccount);
    if (version != DATA_VERSION) {
        return reject(why, why_size, "unsupported format version %u (expected %d)", version, DATA_VERSION);
    }
    if (ucount > MAX_USERS || ccount > MAX_COURSES) {
        return reject(why, why_size, "user_count %u or course_count %u out of range", ucount, ccount);
    }
    size_t expected = HEADER_SIZE + ucount * USER_RECORD_SIZE + ccount * COURSE_RECORD_SIZE;
    if (size != expected) {
        return reject(why, why_size, "file is %zu bytes but its header describes %zu bytes", size, expected);
    }

    user_count = (int)ucount;
    for (int i = 0; i < user_count; i++) {
        User *u = &users[i];
        uint32_t role, active;
        p = get_u32(p, &role);
        p = get_u32(p, &active);
        p = get_string(p, u->username, MAX_NAME);
        p = get_string(p, u->password_hash, MAX_HASH);
        if (role != ADMIN && role != FACULTY && role != STUDENT) {
            return reject(why, why_size, "user %d: invalid role %u", i, role);
        }
        if (active > 1) {
            return reject(why, why_size, "user %d: invalid active flag %u", i, active);
        }
        if (!is_valid_string(u->username, MAX_NAME) || !is_valid_string(u->password_hash, MAX_HASH)) {
            return reject(why, why_size, "user %d: username or password hash is empty or not NUL-terminated", i);
        }
        if (u->password_hash[0] != '$') {
            return reject(why, why_size, "user %d: password field is not a crypt() hash string", i);
        }
        u->role = (int)role;
        u->active = (int)active;
        for (int k = 0; k < i; k++) {
            if (strcmp(users[k].username, u->username) == 0) {
                return reject(why, why_size, "users %d and %d have the same username '%s'", k, i, u->username);
            }
        }
    }

    course_count = (int)ccount;
    for (int i = 0; i < course_count; i++) {
        Course *c = &courses[i];
        uint32_t max_seats, enrolled;
        p = get_string(p, c->course_id, MAX_COURSE_ID);
        p = get_string(p, c->name, MAX_COURSE_NAME);
        p = get_string(p, c->faculty, MAX_NAME);
        p = get_u32(p, &max_seats);
        p = get_u32(p, &enrolled);
        for (int j = 0; j < MAX_STUDENTS_PER_COURSE; j++) {
            p = get_string(p, c->students[j], MAX_NAME);
        }

        if (!is_valid_string(c->course_id, MAX_COURSE_ID) || !is_valid_string(c->name, MAX_COURSE_NAME) ||
            !is_valid_string(c->faculty, MAX_NAME)) {
            return reject(why, why_size, "course %d: ID, name or faculty is empty or not NUL-terminated", i);
        }
        if (max_seats < 1 || max_seats > MAX_STUDENTS_PER_COURSE) {
            return reject(why, why_size, "course '%s': max_seats %u out of range", c->course_id, max_seats);
        }
        if (enrolled > max_seats) {
            return reject(why, why_size, "course '%s': enrolled %u exceeds max_seats %u", c->course_id, enrolled, max_seats);
        }
        c->max_seats = (int)max_seats;
        c->enrolled = (int)enrolled;
        for (int k = 0; k < i; k++) {
            if (strcmp(courses[k].course_id, c->course_id) == 0) {
                return reject(why, why_size, "courses %d and %d have the same ID '%s'", k, i, c->course_id);
            }
        }
        if (find_user(c->faculty, FACULTY) < 0) {
            return reject(why, why_size, "course '%s': faculty '%s' is not an existing faculty user", c->course_id, c->faculty);
        }
        for (int j = 0; j < c->enrolled; j++) {
            if (!is_valid_string(c->students[j], MAX_NAME)) {
                return reject(why, why_size, "course '%s': enrolled student %d is empty or not NUL-terminated", c->course_id, j);
            }
            if (find_user(c->students[j], STUDENT) < 0) {
                return reject(why, why_size, "course '%s': enrolled '%s' is not an existing student", c->course_id, c->students[j]);
            }
            for (int k = 0; k < j; k++) {
                if (strcmp(c->students[k], c->students[j]) == 0) {
                    return reject(why, why_size, "course '%s': student '%s' is enrolled twice", c->course_id, c->students[j]);
                }
            }
        }
    }
    return 0;
}

// ---------------------------------------------------------------- save / load

/*
 * Save the complete state (caller holds data_mutex). Returns 0 on success, -1 on error.
 *
 *   1. write the new state to DATA_TMP_FILE
 *   2. fsync it            -> its contents are on disk
 *   3. close it
 *   4. rename it over DATA_FILE  -> atomic: DATA_FILE is the old file or the new one
 *   5. fsync the directory -> the rename itself is on disk
 *
 * If any step before the rename fails, DATA_FILE still holds the previous complete
 * state and the temp file is removed.
 */
int save_data(void) {
    size_t len = serialize_state(file_buf);

    int fd = open(DATA_TMP_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0600); // contains password hashes
    if (fd < 0) {
        log_error("save: cannot create " DATA_TMP_FILE);
        return -1;
    }
    if (write_all(fd, file_buf, len) < 0 || fault_at("after_write")) {
        log_error("save: write to " DATA_TMP_FILE " failed");
        goto fail_close;
    }
    if (fsync(fd) < 0 || fault_at("after_fsync")) {
        log_error("save: fsync of " DATA_TMP_FILE " failed");
        goto fail_close;
    }
    if (close(fd) < 0) {
        log_error("save: close of " DATA_TMP_FILE " failed");
        goto fail_unlink;
    }
    if (fault_at("before_rename") || rename(DATA_TMP_FILE, DATA_FILE) < 0) {
        log_error("save: rename to " DATA_FILE " failed");
        goto fail_unlink;
    }
    if (fault_at("after_rename") || fsync_directory(".") < 0) {
        log_error("save: directory fsync failed; the rename may not survive a power loss");
        return -1;
    }
    return 0;

fail_close:
    close(fd);
fail_unlink:
    unlink(DATA_TMP_FILE);
    return -1;
}

// First run: no data file exists yet. Create the default administrator and save.
static int init_new_installation(void) {
    if (access(LEGACY_USER_FILE, F_OK) == 0 || access(LEGACY_COURSE_FILE, F_OK) == 0) {
        log_message("Found " LEGACY_USER_FILE "/" LEGACY_COURSE_FILE " from an older version. The data format "
                    "changed to a single " DATA_FILE "; delete the old files and restart.");
        return -1;
    }
    // The admin menu has no "change my password", so allow choosing the initial
    // admin password at installation time instead of always using the documented default.
    const char *password = getenv("PORTAL_ADMIN_PASSWORD");
    if (password == NULL) {
        password = "pass123";
        log_message("No " DATA_FILE " found: first run, creating admin1 with the DEFAULT password "
                    "(set PORTAL_ADMIN_PASSWORD before the first run to choose one)");
    } else if (password[0] == '\0' || strlen(password) >= MAX_PASSWORD) {
        log_message("PORTAL_ADMIN_PASSWORD must be 1 to 49 characters; refusing to start");
        return -1;
    } else {
        log_message("No " DATA_FILE " found: first run, creating admin1 with the password from PORTAL_ADMIN_PASSWORD");
    }

    memset(&users[0], 0, sizeof(users[0]));
    strcpy(users[0].username, "admin1");
    if (hash_password(password, users[0].password_hash, sizeof(users[0].password_hash)) < 0) {
        return -1;
    }
    users[0].role = ADMIN;
    users[0].active = 1;
    user_count = 1;
    course_count = 0;
    return save_data();
}

// Load the state at startup (caller holds data_mutex). A missing file means a new
// installation. Returns -1 if an existing file cannot be read or fails validation:
// the server must then refuse to start rather than replace the user's data.
int load_data(void) {
    char msg[320];

    // A temp file can only be left by a save that was interrupted before its rename,
    // so it was never committed. DATA_FILE still holds the last committed state.
    if (unlink(DATA_TMP_FILE) == 0) {
        log_message("Removed stale " DATA_TMP_FILE " from an interrupted save (it was never committed)");
    } else if (errno != ENOENT) {
        log_error("Cannot remove stale " DATA_TMP_FILE);
        return -1;
    }

    int fd = open(DATA_FILE, O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) {
            return init_new_installation();
        }
        log_error("Cannot open " DATA_FILE);
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        log_error("Cannot stat " DATA_FILE);
        close(fd);
        return -1;
    }
    char why[200];
    int rc = 0;
    if (st.st_size < 0 || (size_t)st.st_size > MAX_FILE_SIZE) {
        snprintf(why, sizeof(why), "file size %lld bytes is larger than any valid file", (long long)st.st_size);
        rc = -1;
    } else {
        size_t size = (size_t)st.st_size;
        ssize_t n = read_all(fd, file_buf, size);
        if (n < 0) {
            log_error("Cannot read " DATA_FILE);
            close(fd);
            return -1;
        }
        rc = decode_state(file_buf, (size_t)n, why, sizeof(why));
    }
    close(fd);

    if (rc < 0) {
        snprintf(msg, sizeof(msg), "Refusing to start: " DATA_FILE " is corrupt: %s", why);
        log_message(msg);
        return -1;
    }
    snprintf(msg, sizeof(msg), "Loaded %d users and %d courses from " DATA_FILE, user_count, course_count);
    log_message(msg);
    return 0;
}
