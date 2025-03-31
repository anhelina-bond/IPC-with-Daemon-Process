CC = gcc
CFLAGS = -Wall -Wextra -std=c11
SRC = main.c
TARGET = daemon

all: clean compile

compile: $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

clean:
	rm -f $(TARGET) /tmp/fifo1 /tmp/fifo2 /tmp/daemon_log.txt
