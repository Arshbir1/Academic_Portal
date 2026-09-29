# AcademicPortal

A concurrent TCP client-server course registration system built in **C** using POSIX sockets and pthreads.

## Features

* Role-based access for **Admin, Faculty, and Students**
* TCP client-server communication with **length-prefixed message framing**
* Concurrent clients using **one thread per connection**
* Thread-safe course enrollment with mutex synchronization
* Crash-safe persistence using **versioned binary serialization**
* Atomic file updates using `fsync()` and `rename()`
* Password storage using **salted yescrypt hashes**
* Hidden password input using `termios`
* Input validation and graceful client disconnect handling

## Architecture

```text
Client
   │
   ▼
TCP Socket
   │
   ▼
Server ──► Client Thread
              │
              ▼
         Shared State
              │
         data_mutex
              │
              ▼
          portal.dat
```

For state-changing operations:

```text
Receive Input → Lock → Re-check → Modify → Persist → Unlock → Reply
```

Network I/O and password hashing are performed outside the critical section.

## Testing

The project has been tested with:

* **ASan / UBSan / TSan**
* TCP fragmentation and coalescing
* Concurrent enrollment and race conditions
* Client disconnects and slow clients
* Persistence failures and corrupted files
* `kill -9` crash/fault-injection tests
* Password security and concurrency tests

**336 regression, security, persistence, and concurrency checks** pass.

## Build & Run

```bash
make
./server
```

In another terminal:

```bash
./client
```

## Tech Stack

**C · POSIX Sockets · pthreads · TCP · libcrypt/yescrypt · termios · Make**
