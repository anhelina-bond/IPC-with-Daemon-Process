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