CC = gcc
CFLAGS = -Wall -Wextra -std=c11
SRC = main.c
TARGET = daemon

all: clean compile

compile: $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

clean:
	rm -f $(TARGET) fifo1 fifo2 daemon_log.txt
