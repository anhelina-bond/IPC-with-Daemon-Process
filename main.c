#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <string.h>
#include <errno.h>

#define FIFO1 "fifo1"
#define FIFO2 "fifo2"
#define LOG_FILE "daemon_log.txt"

volatile sig_atomic_t child_count = 0;
volatile sig_atomic_t total_children = 2; // We always create 2 children
pid_t daemon_pid = 0;

void log_message(const char *message) {
    int fd = open(LOG_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd == -1) return;
    
    time_t now;
    time(&now);
    char time_buf[50];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", localtime(&now));
    
    dprintf(fd, "[%s] %s\n", time_buf, message);
    close(fd);
}

void sigchld_handler(int sig) {
    (void)sig;
    int status;
    pid_t pid;
    char buf[100];
    
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        int len = snprintf(buf, sizeof(buf), "Child %d exited with status %d", 
                         pid, WEXITSTATUS(status));
        write(STDOUT_FILENO, buf, len);
        log_message(buf);
        child_count++;
    }
}

void daemon_signal_handler(int sig) {
    switch(sig) {
        case SIGUSR1:
            log_message("Received SIGUSR1");
            break;
        case SIGHUP:
            log_message("Received SIGHUP");
            break;
        case SIGTERM:
            log_message("Received SIGTERM - exiting");
            exit(EXIT_SUCCESS);
    }
}

int become_daemon() {
    // First fork to background the process
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid > 0) _exit(EXIT_SUCCESS);  // Parent exits

    // Create new session
    if (setsid() < 0) return -1;

    // Second fork to ensure we're not a session leader
    pid = fork();
    if (pid < 0) return -1;
    if (pid > 0) _exit(EXIT_SUCCESS);  // Parent exits

    // Set file mode creation mask
    umask(0);

    // Change to root directory
    if (chdir("/") < 0) return -1;

    // Close all open file descriptors
    int maxfd = sysconf(_SC_OPEN_MAX);
    if (maxfd == -1) maxfd = 1024;  // Fallback if sysconf fails
    for (int fd = 0; fd < maxfd; fd++) {
        close(fd);
    }

    // Reopen standard file descriptors
    open("/dev/null", O_RDONLY);  // stdin
    int log_fd = open(LOG_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log_fd < 0) return -1;

    // Redirect stdout and stderr to log file
    dup2(log_fd, STDOUT_FILENO);
    dup2(log_fd, STDERR_FILENO);

    // Close the original log_fd if it wasn't stdout/stderr
    if (log_fd > STDERR_FILENO) {
        close(log_fd);
    }

    return 0;
}

void child_process1() {
    // Wait for FIFO to be ready
    int fd1 = -1;
    while ((fd1 = open(FIFO1, O_RDONLY)) == -1 && errno == EINTR);
    if (fd1 == -1) {
        perror("Child1: open FIFO1");
        exit(EXIT_FAILURE);
    }

    int nums[2];
    if (read(fd1, nums, sizeof(nums)) != sizeof(nums)) {
        perror("Child1: read FIFO1");
        close(fd1);
        exit(EXIT_FAILURE);
    }
    close(fd1);

    int larger = (nums[0] > nums[1]) ? nums[0] : nums[1];

    int fd2 = open(FIFO2, O_WRONLY);
    if (fd2 == -1) {
        perror("Child1: open FIFO2");
        exit(EXIT_FAILURE);
    }

    if (write(fd2, &larger, sizeof(larger)) != sizeof(larger)) {
        perror("Child1: write FIFO2");
        close(fd2);
        exit(EXIT_FAILURE);
    }
    close(fd2);

    exit(EXIT_SUCCESS);
}

void child_process2() {
    sleep(10);
    // Wait for FIFO to be ready
    int fd = -1;
    while ((fd = open(FIFO2, O_RDONLY)) == -1 && errno == EINTR);
    if (fd == -1) {
        perror("Child2: open FIFO2");
        exit(EXIT_FAILURE);
    }

    int larger;
    if (read(fd, &larger, sizeof(larger)) != sizeof(larger)) {
        perror("Child2: read FIFO2");
        close(fd);
        exit(EXIT_FAILURE);
    }
    close(fd);

    printf("The larger number is: %d\n", larger);
    exit(EXIT_SUCCESS);
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <num1> <num2>\n", argv[0]);
        return EXIT_FAILURE;
    }

    int num1 = atoi(argv[1]);
    int num2 = atoi(argv[2]);

    // Clean up old FIFOs
    unlink(FIFO1);
    unlink(FIFO2);

    // Create FIFOs
    if (mkfifo(FIFO1, 0666) == -1 || mkfifo(FIFO2, 0666) == -1) {
        perror("mkfifo");
        return EXIT_FAILURE;
    }

    // Create daemon first
    daemon_pid = fork();
    if (daemon_pid == 0) {
        if (become_daemon() == -1) {
            perror("daemon");
            return EXIT_FAILURE;
        }

        signal(SIGUSR1, daemon_signal_handler);
        signal(SIGHUP, daemon_signal_handler);
        signal(SIGTERM, daemon_signal_handler);

        log_message("Daemon started");
        while (1) pause(); // Wait for signals
    }

    // Set up SIGCHLD handler
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa, NULL) < 0) {
        perror("sigaction");
        return EXIT_FAILURE;
    }

    // Write numbers to FIFO1
    int fd1 = open(FIFO1, O_WRONLY);
    if (fd1 == -1) {
        perror("open FIFO1");
        return EXIT_FAILURE;
    }

    int nums[2] = {num1, num2};
    if (write(fd1, nums, sizeof(nums)) != sizeof(nums)) {
        perror("write FIFO1");
        close(fd1);
        return EXIT_FAILURE;
    }
    close(fd1);

    // Create children
    pid_t child1 = fork();
    if (child1 == 0) child_process1();

    pid_t child2 = fork();
    if (child2 == 0) child_process2();

    // Parent waits for children
    while (child_count < total_children) {
        printf("Waiting for children... (%d/%d)\n", child_count, total_children);
        sleep(1);
    }

    // Cleanup
    unlink(FIFO1);
    unlink(FIFO2);
    printf("All children exited. Parent terminating.\n");
    return EXIT_SUCCESS;
}