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
#define CHILD_TIMEOUT 7  // 7 seconds timeout

volatile sig_atomic_t child_count = 0;
volatile sig_atomic_t total_children = 0;

// Signal handler for SIGCHLD
void sigchld_handler(int sig) {
    (void)sig;
    int status;
    pid_t pid;
    char buf[100];
    time_t now;
    
    time(&now);
    char *time_str = ctime(&now);
    time_str[strlen(time_str)-1] = '\0'; // Remove newline
    
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (WIFEXITED(status)) {
            snprintf(buf, sizeof(buf), "[%s] Child %d exited with status %d\n",
                    time_str, pid, WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            snprintf(buf, sizeof(buf), "[%s] Child %d killed by signal %d\n",
                    time_str, pid, WTERMSIG(status));
        }
        write(STDOUT_FILENO, buf, strlen(buf));
        child_count++;
    }
}

// Daemon signal handler
void daemon_signal_handler(int sig) {
    char buf[100];
    time_t now;
    time(&now);
    char *time_str = ctime(&now);
    time_str[strlen(time_str)-1] = '\0';
    
    switch(sig) {
        case SIGTERM:
            snprintf(buf, sizeof(buf), "[%s] Received SIGTERM - shutting down\n", time_str);
            write(STDERR_FILENO, buf, strlen(buf));
            exit(EXIT_SUCCESS);
        case SIGHUP:
            snprintf(buf, sizeof(buf), "[%s] Received SIGHUP\n", time_str);
            write(STDERR_FILENO, buf, strlen(buf));
            break;
    }
}

// Become a daemon
void become_daemon() {
    pid_t pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS); // Parent exits
    
    // Create new session
    if (setsid() < 0) exit(EXIT_FAILURE);
    
    // Fork again to ensure we're not a session leader
    pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS);
    
    // Set up logging
    int log_fd = open(LOG_FILE, O_WRONLY|O_CREAT|O_APPEND, 0644);
    if (log_fd == -1) exit(EXIT_FAILURE);
    
    // Redirect stdio
    dup2(log_fd, STDOUT_FILENO);
    dup2(log_fd, STDERR_FILENO);
    close(log_fd);
    
    // Close all other descriptors
    for (int fd = sysconf(_SC_OPEN_MAX); fd >= 0; fd--) {
        close(fd);
    }
    
    // Set up signal handlers
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa, NULL);
    
    sa.sa_handler = daemon_signal_handler;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
}

void child_process1() {
    printf("Child 1 started\n");
    
    int fd1 = open(FIFO1, O_RDONLY);
    if (fd1 == -1) exit(EXIT_FAILURE);

    int nums[2];
    if (read(fd1, nums, sizeof(nums)) == -1) exit(EXIT_FAILURE);
    close(fd1);

    int larger = (nums[0] > nums[1]) ? nums[0] : nums[1];
    printf("Child 1: Larger of %d and %d is %d\n", nums[0], nums[1], larger);

    int fd2 = open(FIFO2, O_WRONLY);
    if (fd2 == -1) exit(EXIT_FAILURE);
    
    if (write(fd2, &larger, sizeof(larger)) == -1) exit(EXIT_FAILURE);
    close(fd2);

    exit(EXIT_SUCCESS);
}

void child_process2() {
    sleep(10); // This will trigger timeout
    printf("Child 2 started\n");
    
    int fd = open(FIFO2, O_RDONLY);
    if (fd == -1) exit(EXIT_FAILURE);
    
    int larger;
    if (read(fd, &larger, sizeof(larger)) == -1) exit(EXIT_FAILURE);
    close(fd);

    printf("The larger number is: %d\n", larger);
    exit(EXIT_SUCCESS);
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <num1> <num2>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // Become daemon
    become_daemon();
    
    // Create FIFOs
    unlink(FIFO1);
    unlink(FIFO2);
    
    if (mkfifo(FIFO1, 0666) == -1 || mkfifo(FIFO2, 0666) == -1) {
        perror("mkfifo failed");
        exit(EXIT_FAILURE);
    }

    // Fork child processes
    pid_t child1 = fork();
    if (child1 == 0) child_process1();
    
    pid_t child2 = fork();
    if (child2 == 0) child_process2();
    
    total_children = 2;
    printf("Daemon started. Child PIDs: %d, %d\n", child1, child2);

    // Parent (now daemon) writes to FIFO1
    int fd1 = open(FIFO1, O_WRONLY);
    if (fd1 == -1) {
        perror("open FIFO1 failed");
        exit(EXIT_FAILURE);
    }
    
    int nums[2] = {atoi(argv[1]), atoi(argv[2])};
    if (write(fd1, nums, sizeof(nums)) == -1) {
        perror("write to FIFO1 failed");
        exit(EXIT_FAILURE);
    }
    close(fd1);

    // Timeout monitoring
    time_t start_time = time(NULL);
    while (child_count < total_children) {
        time_t now = time(NULL);
        
        // Check for timeouts
        if (now - start_time > CHILD_TIMEOUT) {
            if (waitpid(child2, NULL, WNOHANG) == 0) { // Child2 still running
                printf("Terminating child2 (PID %d) due to timeout\n", child2);
                kill(child2, SIGTERM);
                sleep(1); // Give it a chance to terminate
                if (waitpid(child2, NULL, WNOHANG) == 0) {
                    kill(child2, SIGKILL);
                }
                child_count++;
            }
        }
        sleep(1);
    }

    // Cleanup
    unlink(FIFO1);
    unlink(FIFO2);
    printf("Daemon exiting\n");
    return EXIT_SUCCESS;
}