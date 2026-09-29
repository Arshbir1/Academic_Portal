CC     = gcc
CFLAGS = -Wall -Wextra -Werror -O2 -pthread
SAN    = -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined
# Password hashing uses the system libcrypt (libxcrypt; package libcrypt-dev on Debian/Ubuntu).
SERVER_LIBS = -lcrypt

SERVER_SRCS = server.c admin.c faculty.c student.c session.c storage.c password.c net.c
CLIENT_SRCS = client.c net.c

all: server client

server: $(SERVER_SRCS) common.h
	$(CC) $(CFLAGS) -o $@ $(SERVER_SRCS) $(SERVER_LIBS)

client: $(CLIENT_SRCS) common.h
	$(CC) $(CFLAGS) -o $@ $(CLIENT_SRCS)

# AddressSanitizer + UndefinedBehaviorSanitizer builds, for testing.
sanitize: server-san client-san

server-san: $(SERVER_SRCS) common.h
	$(CC) $(CFLAGS) $(SAN) -o $@ $(SERVER_SRCS) $(SERVER_LIBS)

client-san: $(CLIENT_SRCS) common.h
	$(CC) $(CFLAGS) $(SAN) -o $@ $(CLIENT_SRCS)

# Test-only server with the crash/failure hook in save_data() (see storage.c).
fault-injection: server-fault

server-fault: $(SERVER_SRCS) common.h
	$(CC) $(CFLAGS) $(SAN) -DFAULT_INJECTION -o $@ $(SERVER_SRCS) $(SERVER_LIBS)

clean:
	rm -f server client server-san client-san server-fault

.PHONY: all sanitize fault-injection clean
