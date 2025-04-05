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
volatile sig_atomic_t total_children = 0;
pid_t daemon_pid = 0;

void sigchld_handler(int sig) { 
    (void)sig;
    int status;
    pid_t pid;
    char buf[100];

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        int len = snprintf(buf, sizeof(buf), "Child PID %d exited with status %d\n", 
                          pid, WEXITSTATUS(status));
        write(STDOUT_FILENO, buf, len);
        child_count++;
    }
}

void daemon_signal_handler(int sig) {
    int log_fd = open(LOG_FILE, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (log_fd == -1) return;

    time_t now;
    time(&now);
    char time_buf[50];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", localtime(&now));

    switch(sig) {
        case SIGUSR1:
            dprintf(log_fd, "[%s] Received SIGUSR1 signal\n", time_buf);
            break;
        case SIGHUP:
            dprintf(log_fd, "[%s] Received SIGHUP signal\n", time_buf);
            break;
        case SIGTERM:
            dprintf(log_fd, "[%s] Received SIGTERM signal. Daemon exiting.\n", time_buf);
            close(log_fd);
            exit(EXIT_SUCCESS);
            break;
    }
    close(log_fd);
}

int become_daemon() {
    // First fork
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid > 0) _exit(EXIT_SUCCESS);

    // Create new session
    if (setsid() < 0) return -1;

    // Second fork
    pid = fork();
    if (pid < 0) return -1;
    if (pid > 0) _exit(EXIT_SUCCESS);

    // Set file mode creation mask
    umask(0);

    // Change working directory
    if (chdir("/") < 0) return -1;

    // Close all open file descriptors
    int maxfd = sysconf(_SC_OPEN_MAX);
    for (int fd = 0; fd < maxfd; fd++) close(fd);

    // Reopen stdin, stdout, stderr to /dev/null
    open("/dev/null", O_RDONLY); // stdin
    int log_fd = open(LOG_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log_fd == -1) return -1;
    dup2(log_fd, STDOUT_FILENO);
    dup2(log_fd, STDERR_FILENO);
    if (log_fd > STDERR_FILENO) close(log_fd);

    return 0;
}

void child_process1() {
    int fd1 = open(FIFO1, O_RDONLY);
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
    int fd = open(FIFO2, O_RDONLY);
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
        exit(EXIT_FAILURE);
    }

    int num1 = atoi(argv[1]);
    int num2 = atoi(argv[2]);

    // Clean up old FIFOs
    unlink(FIFO1);
    unlink(FIFO2);

    // Create FIFOs
    if (mkfifo(FIFO1, 0666) == -1 || mkfifo(FIFO2, 0666) == -1) {
        perror("mkfifo");
        exit(EXIT_FAILURE);
    }

    // Create daemon first
    daemon_pid = fork();
    if (daemon_pid == 0) {
        if (become_daemon() == -1) {
            perror("daemon");
            exit(EXIT_FAILURE);
        }

        signal(SIGUSR1, daemon_signal_handler);
        signal(SIGHUP, daemon_signal_handler);
        signal(SIGTERM, daemon_signal_handler);

        while (1) sleep(5);
    }

    // Set up SIGCHLD handler
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGCHLD, &sa, NULL) < 0) {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }

    // Write numbers to FIFO1
    int fd1 = open(FIFO1, O_WRONLY);
    if (fd1 == -1) {
        perror("open FIFO1");
        exit(EXIT_FAILURE);
    }

    int nums[2] = {num1, num2};
    if (write(fd1, nums, sizeof(nums)) != sizeof(nums)) {
        perror("write FIFO1");
        close(fd1);
        exit(EXIT_FAILURE);
    }
    close(fd1);

    // Create children
    total_children = 2;
    pid_t child1 = fork();
    if (child1 == 0) child_process1();

    pid_t child2 = fork();
    if (child2 == 0) child_process2();

    // Parent main loop
    while (child_count < total_children) {
        printf("proceeding\n");
        sleep(2);
    }

    // Cleanup
    unlink(FIFO1);
    unlink(FIFO2);
    printf("All children exited. Parent terminating.\n");
    return EXIT_SUCCESS;
}