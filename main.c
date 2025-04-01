#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>
#include <sys/wait.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#define FIFO1 "fifo1"
#define FIFO2 "fifo2"
#define LOG_FILE "daemon_log.txt"
#define TIMEOUT 15  // Timeout for inactive processes (in seconds)

volatile sig_atomic_t child_count = 0;
volatile sig_atomic_t total_children = 2;
volatile sig_atomic_t shutdown_requested = 0;

// Signal handler for SIGCHLD (Zombie Protection)
void sigchld_handler(int sig) {
    (void)sig;
    int status;
    pid_t pid;
    
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        child_count++;
        FILE *log = fopen(LOG_FILE, "a");
        if (log) {
            fprintf(log, "Child process %d terminated with status %d\n", pid, WEXITSTATUS(status));
            fclose(log);
        }
    }
}

// Signal handler for SIGTERM and SIGINT (Graceful Shutdown)
void sigterm_handler(int sig) {
    (void)sig;
    shutdown_requested = 1;
}

// Convert the process into a daemon
int become_daemon() {
    pid_t pid = fork();
    if (pid > 0) exit(EXIT_SUCCESS);  // Parent exits

    if (setsid() == -1) exit(EXIT_FAILURE); // Become session leader

    pid = fork();
    if (pid > 0) exit(EXIT_SUCCESS);  // Second fork to detach from terminal

    chdir("/");  // Change working directory to root (optional)

    int maxfd = sysconf(_SC_OPEN_MAX);
    if (maxfd == -1) maxfd = 1024;
    for (int fd = 0; fd < maxfd; fd++) close(fd);

    int log_fd = open(LOG_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log_fd == -1) exit(EXIT_FAILURE);
    
    dup2(log_fd, STDOUT_FILENO);
    dup2(log_fd, STDERR_FILENO);
    close(log_fd);

    return 0;
}

// Log messages to the daemon log
void log_message(const char *message) {
    FILE *log = fopen(LOG_FILE, "a");
    if (log) {
        time_t now = time(NULL);
        fprintf(log, "[%s] %s\n", ctime(&now), message);
        fclose(log);
    }
}

// Child process 1: Reads two numbers from FIFO1, finds the larger number, and writes to FIFO2
void child_process1() {
    sleep(10);  // Sleep for 10 seconds before executing

    int fd1 = open(FIFO1, O_RDONLY);
    if (fd1 == -1) {
        perror("Error opening FIFO1 in Child 1");
        exit(EXIT_FAILURE);
    }

    int nums[2];
    if (read(fd1, nums, sizeof(nums)) <= 0) {
        perror("Failed to read from FIFO1");
        close(fd1);
        exit(EXIT_FAILURE);
    }
    close(fd1);

    int larger = (nums[0] > nums[1]) ? nums[0] : nums[1];

    int fd2 = open(FIFO2, O_WRONLY);
    if (fd2 == -1) {
        perror("Error opening FIFO2 in Child 1");
        exit(EXIT_FAILURE);
    }

    write(fd2, &larger, sizeof(larger));
    close(fd2);

    exit(EXIT_SUCCESS);
}

// Child process 2: Reads the larger number from FIFO2 and prints it
void child_process2() {
    sleep(10);  // Sleep for 10 seconds before executing

    int fd2 = open(FIFO2, O_RDONLY);
    if (fd2 == -1) {
        perror("Error opening FIFO2 in Child 2");
        exit(EXIT_FAILURE);
    }

    int larger;
    if (read(fd2, &larger, sizeof(larger)) <= 0) {
        perror("Failed to read from FIFO2");
        close(fd2);
        exit(EXIT_FAILURE);
    }
    close(fd2);

    printf("The larger number is: %d\n", larger);

    exit(EXIT_SUCCESS);
}

// Daemon process: Handles logging, IPC monitoring, and process management
void daemon_process() {
    if (become_daemon() == -1) {
        perror("Failed to create daemon");
        exit(EXIT_FAILURE);
    }

    log_message("Daemon started.");

    // Set up SIGTERM, SIGINT handlers
    struct sigaction sa;
    sa.sa_handler = sigterm_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    time_t last_active = time(NULL);

    while (!shutdown_requested) {
        sleep(1);

        if (child_count >= total_children) break;

        time_t now = time(NULL);
        if (now - last_active > TIMEOUT) {
            log_message("Timeout reached. Terminating inactive processes.");
            kill(0, SIGTERM);
            break;
        }
    }

    log_message("Daemon shutting down.");
    unlink(FIFO1);
    unlink(FIFO2);
    exit(EXIT_SUCCESS);
}

int main() {
    // Create FIFOs
    if (mkfifo(FIFO1, 0666) == -1 && errno != EEXIST) {
        perror("mkfifo FIFO1");
        exit(EXIT_FAILURE);
    }

    if (mkfifo(FIFO2, 0666) == -1 && errno != EEXIST) {
        perror("mkfifo FIFO2");
        unlink(FIFO1);
        exit(EXIT_FAILURE);
    }

    // Set up SIGCHLD handler for zombie protection
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    // Fork child processes
    pid_t child1 = fork();
    if (child1 == -1) {
        perror("fork failed for child1");
        exit(EXIT_FAILURE);
    } else if (child1 == 0) {
        child_process1();
    }

    pid_t child2 = fork();
    if (child2 == -1) {
        perror("fork failed for child2");
        exit(EXIT_FAILURE);
    } else if (child2 == 0) {
        child_process2();
    }

    // Start daemon process
    pid_t daemon_pid = fork();
    if (daemon_pid == 0) {
        daemon_process();
    }

    // Parent waits for children to finish
    while (child_count < total_children) {
        sleep(1);
    }

    printf("All children have exited. Parent terminating.\n");
    exit(EXIT_SUCCESS);
}
