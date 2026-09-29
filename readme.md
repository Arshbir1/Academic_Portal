# Academia: Course Registration Portal

_A multi-user academic management system developed as a mini-project for CS-513 System Software at IIIT Bangalore._

---

## 📌 Overview
**Academia** is a client-server Course Registration Portal that allows Admins, Faculty, and Students to manage academic operations. Built using C, it leverages system calls, file-based storage, multithreading, and socket programming for secure and concurrent access.

---

## 🎓 Roles and Functionalities

### 👤 Admin
- Add new Students and Faculty
- View Student and Faculty details
- Activate or deactivate Student accounts
- Modify Student and Faculty details (username/password)
- Logout and exit

### 👨‍🏫 Faculty
- Add new Courses (with seat limits)
- Remove offered Courses
- View own offered Courses and enrollments
- Update Course details (name, max seats)
- Change password
- Logout and exit

### 🧑‍🎓 Student
- View all available Courses
- Enroll in Courses (if seats are available)
- Drop enrolled Courses
- View enrolled Courses
- Change password
- Logout and exit

---

## ⚙️ Technical Features

- **Socket Programming**: TCP server handles multiple concurrent clients
- **Message Framing**: every message is `[1-byte type][4-byte length, network byte order][payload]`, so message boundaries survive TCP's byte stream. Types: `MSG_INFO`, `MSG_PROMPT` (the client reads one line only for these), `MSG_INPUT`, `MSG_BYE`
- **Input Validation**: text input that is empty or too long for its field is rejected (never truncated); numbers are parsed with `strtol` and range-checked
- **Multithreading**: Separate thread per client connection using `pthread`
- **File Storage**: all users and courses live in one file, `portal.dat`, replaced atomically on every change (see [Persistence](#-persistence))
- **Synchronization**:
  - `data_mutex`: a single global mutex that protects `users[]`, `courses[]`, `user_count` and `course_count`
  - Every operation collects all client input first, then locks, re-checks its conditions, applies and saves the whole change, unlocks, and only then replies. The mutex is never held during network I/O, so a slow or stalled client cannot block other clients.
  - If saving fails, the in-memory change is rolled back and the client is told nothing was changed.
  - Renaming a user also updates course ownership and enrollment lists in the same critical section; a blocked student's existing session ends at the next menu action.
- **Security**: salted yescrypt password hashes via the system `libcrypt`, credentials never logged, passwords typed without echo (see [Security](#-security))
- **System Calls**: Usage of `open`, `read`, `write`, `fcntl`, `pthread`, etc.
- **Error Handling**: Detailed logging with timestamps and thread IDs

---

## 💻 Prerequisites

- **OS**: Linux (tested on Ubuntu); WSL supported
- **Compiler**: GCC with C11 support
- **Libraries**: POSIX threads (`pthread`); `libcrypt` from libxcrypt ≥ 4.1 (`sudo apt install libcrypt-dev` on Debian/Ubuntu; already present on Ubuntu 24.04)

---

## 🧪 Compilation

Use the provided Makefile (builds with `-Wall -Wextra -Werror -O2 -pthread`):

```bash
make            # builds ./server and ./client
make sanitize   # builds ./server-san and ./client-san with AddressSanitizer + UndefinedBehaviorSanitizer
make fault-injection  # test-only ./server-fault that can crash or fail at chosen points of a save
make clean
```

---

## 💾 Persistence

**Why a file and not a database?** The data set is small (at most 100 users and 100 courses, about 272 KB), and the project's purpose is to use POSIX file I/O directly. The complete state is rewritten on every change; at this size that is simpler than a database, a write-ahead log, or incremental updates, and it is still crash-safe.

**Format (`portal.dat`, version 2)** — one file, so users and courses are always committed together:

| Part | Contents | Size |
|---|---|---|
| Header | magic `"ACAD"`, format version, `user_count`, `course_count` (each `uint32`) | 16 bytes |
| User record ×`user_count` | role, active, username (50), password hash string (128) | 186 bytes |
| Course record ×`course_count` | course ID (10), name (50), faculty (50), max_seats, enrolled, 50 student-name slots (50 each) | 2618 bytes |

Integers are host byte order (this is a Linux project, not a portable format); strings are fixed width and NUL-padded.

**Why raw structs are not written.** Writing `User`/`Course` with `sizeof(struct)` would put compiler-chosen padding and layout, and anything else in the struct, into the file. `storage.c` copies each field into a byte buffer explicitly, so the file contains only application data and its layout is defined in one place.

**Save sequence** (`save_data()`, called with `data_mutex` held):

1. Write the complete new state to `portal.dat.tmp`.
2. `fsync()` the temp file — forces its data out of the kernel's page cache onto the disk.
3. `close()` it.
4. `rename("portal.dat.tmp", "portal.dat")` — atomic: anyone opening `portal.dat` sees either the whole old file or the whole new file, never a mix.
5. `fsync()` the directory — a rename changes the directory, and only this makes the rename itself survive a power failure.

A crash before step 4 leaves the old `portal.dat` untouched (plus a stale temp file, which is deleted at the next startup because it was never committed). A crash after step 4 leaves the new file. `portal.dat` is never partially written. If any step fails, the operation's in-memory change is rolled back and the client is told nothing was saved.

**Loading and corruption.** A missing `portal.dat` means a new installation: the default admin is created. An existing file is fully validated before it is accepted: magic, version, counts and exact file size; every string NUL-terminated; valid roles and flags; unique usernames and course IDs; `1 ≤ max_seats ≤ 50`, `0 ≤ enrolled ≤ max_seats`; every course's faculty is an existing faculty user; every enrolled name is an existing student, with no duplicates. If anything fails, the server logs the reason and **refuses to start**. It never treats a damaged file as empty, because the next save would then destroy the data.

**Why no WAL/journaling.** A write-ahead log earns its complexity when rewriting everything is too expensive. Here the whole file is small, so write-temp-then-rename gives the same all-or-nothing guarantee with far less code.

---

## 🔒 Security

This is a course project, not a production-secure system. What it does:

- **Passwords are never stored in plaintext.** `portal.dat` holds a hash string such as `$y$j9T$<salt>$<hash>`, produced by the system's `libcrypt` (libxcrypt, the library that hashes `/etc/shadow` on Linux) with its preferred method, **yescrypt**. No cryptography is implemented in this project.
- **Every password gets a random salt** (generated by `libcrypt` from the OS random source). Two users with the same password get different hashes, and precomputed tables of hashes are useless.
- **The hash is deliberately slow and memory-hard** (~10–20 ms per attempt at libcrypt's default cost). A legitimate login hardly notices; an attacker who steals `portal.dat` can try far fewer guesses per second than with a fast general-purpose hash (such as the djb2 hash earlier versions used, or a single SHA-256).
- **Hashing happens outside `data_mutex`.** Login copies the stored hash under the lock and verifies after unlocking; password changes hash first, then re-lock and re-check that the account is still active and its password was not changed meanwhile.
- **Changing your password requires the current password.** Admins can still reset student/faculty passwords.
- **Credentials are excluded from logs.** Logs record events ("Login failed for username: x", "Password change for 'x': succeeded") but never a password or a hash. Plaintext password buffers are wiped with `explicit_bzero` after use.
- **The client does not echo passwords.** Password prompts use a separate message type (`MSG_PROMPT_SECRET`); the client turns terminal echo off with `termios` for that line and always restores it, including on Ctrl+C.
- **`portal.dat` is created with mode `0600`.**

Limitations:

- **Network traffic is plaintext.** Passwords cross the TCP connection unencrypted. A real deployment would put the protocol inside TLS; that is outside the scope of this project.
- **No rate limiting or lockout** beyond one login attempt per connection, so online guessing is slowed only by the hash cost.
- **The default admin password is public** (`admin1` / `pass123`) and the admin menu cannot change it. Set `PORTAL_ADMIN_PASSWORD` before the **first** run to choose a different one.

---

## 🚀 Usage

### 1. Start the Server
```bash
./server
```
- On first run (no `portal.dat` in the working directory) creates the admin account `admin1`. Its password is taken from `PORTAL_ADMIN_PASSWORD` if set (recommended: `PORTAL_ADMIN_PASSWORD='...' ./server`), otherwise it is the default `pass123`. The admin password cannot be changed later from the menu.
- Runs on `PORT 8080`

### 2. Run the Client
```bash
./client
```
- Connects to `localhost:8080`
- Prompts for:
  - Login Type (1: Admin, 2: Faculty, 3: Student)
  - Username and Password
- Displays role-based menus

---

## 📖 Example Interaction

![image](https://github.com/user-attachments/assets/7cce003d-a108-4c46-94e6-b0b81147f6fd)
![image](https://github.com/user-attachments/assets/18cf02b0-b115-4fcb-9159-75c06b00f84c)
![image](https://github.com/user-attachments/assets/91d61d10-8db6-473f-afe8-bc98ceb126ea)


## 🗃️ File Structure

### 🔹 Source Files
- `common.h` — Shared definitions, structs (`User`, `Course`), function prototypes
- `server.c` — Main server logic, socket/thread management, login, commit/rollback of changes, hashing
- `storage.c` — The only code that touches `portal.dat`: serialization, validation, atomic save, load
- `password.c` — Password hashing and verification with the system `libcrypt` (yescrypt)
- `net.c` — `send_all`/`recv_all` and the framed `send_message`/`recv_message` (shared by server and client)
- `session.c` — Server-side input helpers: `prompt_text`, `prompt_int`, `parse_int`, `append_text`
- `client.c` — Client interface: prints server messages and answers prompts
- `admin.c` — Admin operations
- `faculty.c` — Faculty operations
- `student.c` — Student operations

### 🔸 Data Files
- `portal.dat` — All users (role, username, password hash, active flag) and courses (ID, name, faculty, seats, enrolled students), mode `0600`
- `portal.dat.tmp` — Exists only while a save is in progress

---

## ⚠️ Notes

- **Password storage changed**: `portal.dat` format version 1 (djb2 hashes) is not migrated; the server refuses to start with it. Delete it and let the server create a new one.
- **Data format changed**: older versions stored `users.dat` and `courses.dat`. They are not migrated; the server refuses to start while they exist without a `portal.dat`. Delete them before the first run.
- **One server per data directory**: `portal.dat` is not locked against a second server process started in the same directory; run only one.
- **Concurrency**: one thread per connected client, with no fixed cap on simultaneous clients. `MAX_CLIENTS` (10) is only the `listen()` backlog: the queue of connections waiting to be accepted.
- **Limits**:
  - Max 100 users (`MAX_USERS`)
  - Max 100 courses (`MAX_COURSES`)
  - Max 50 students per course (`MAX_STUDENTS_PER_COURSE`)

---

## ✅ Project Requirements Fulfilled

- File-based storage
- Role-based secure login and menus
- Admin-controlled account management
- Socket-based concurrent access
- Thread safety using mutexes and locks
- Menus as per assignment expectations (with useful additions)

---



