CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -pedantic
SRC = main.c
TARGET = daemon
ARGS = $(wordlist 2, $(words $(MAKECMDGOALS)), $(MAKECMDGOALS))
NUM_ARGS = $(words $(ARGS))

# Prevent make from treating args as targets
$(eval $(ARGS):;@:)

.PHONY: all compile clean run

all: clean compile

compile: $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

run: compile
ifeq ($(NUM_ARGS),2)
	@echo "Running daemon with arguments: $(ARGS)"
	@./$(TARGET) $(ARGS)
else
	@echo "Error: Exactly 2 arguments required"
	@echo "Usage: make run <num1> <num2>"
	@exit 1
endif

clean:
	rm -f $(TARGET) fifo1 fifo2 daemon_log.txt