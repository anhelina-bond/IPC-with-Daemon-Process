#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <errno.h>

#define FIFO1 "fifo1"
#define FIFO2 "fifo2"
#define LOG_FILE "daemon.log"

volatile sig_atomic_t child_counter = 0;
volatile sig_atomic_t total_children = 0;
volatile sig_atomic_t daemon_pid = 0;

// Signal handler for SIGCHLD
void sigchld_handler(int sig) {
    (void)sig; // Explicitly mark as unused to avoid warning
    int status;
    pid_t pid;
    
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if(pid != daemon_pid){
            printf("Child process %d exited with status %d\n", pid, WEXITSTATUS(status));
            child_counter += 1; // Increment by two as per requirements
        }
       
        
        // Log to daemon if it's running
        if (daemon_pid > 0) {
            char log_msg[256];
            time_t now;
            time(&now);
            snprintf(log_msg, sizeof(log_msg), "[%s] Child PID %d exited with status %d\n", 
                    ctime(&now), pid, WEXITSTATUS(status));
            
            int log_fd = open(LOG_FILE, O_WRONLY | O_APPEND | O_CREAT, 0644);
            if (log_fd >= 0) {
                write(log_fd, log_msg, strlen(log_msg));
                close(log_fd);
            }
        }
    }
}

// Daemon signal handler
void daemon_signal_handler(int sig) {
    int log_fd = open(LOG_FILE, O_WRONLY | O_APPEND | O_CREAT, 0644);
    time_t now;
    time(&now);
    
    char log_msg[256];
    
    switch(sig) {
        case SIGUSR1:
            snprintf(log_msg, sizeof(log_msg), "[%s] Daemon received SIGUSR1\n", ctime(&now));
            break;
        case SIGHUP:
            snprintf(log_msg, sizeof(log_msg), "[%s] Daemon received SIGHUP\n", ctime(&now));
            break;
        case SIGTERM:
            snprintf(log_msg, sizeof(log_msg), "[%s] Daemon received SIGTERM, shutting down\n", ctime(&now));
            if (log_fd >= 0) {
                write(log_fd, log_msg, strlen(log_msg));
                close(log_fd);
            }
            exit(0);
            break;
    }
    
    if (log_fd >= 0) {
        write(log_fd, log_msg, strlen(log_msg));
        close(log_fd);
    }
}

// Function to create a daemon process
void create_daemon() {
    pid_t pid = fork();
    
    if (pid < 0) {
        perror("fork failed for daemon");
        exit(EXIT_FAILURE);
    }
    
    if (pid > 0) {
        daemon_pid = pid;
        return; // Parent returns
    }
    
    // Child (daemon) continues
    umask(0);
    
    pid_t sid = setsid();
    if (sid < 0) {
        perror("setsid failed");
        exit(EXIT_FAILURE);
    }
    
    if ((chdir("/")) < 0) {
        perror("chdir failed");
        exit(EXIT_FAILURE);
    }
    
    // Close standard file descriptors
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    
    // Redirect stdout and stderr to log file
    int log_fd = open(LOG_FILE, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (log_fd < 0) {
        exit(EXIT_FAILURE);
    }
    
    dup2(log_fd, STDOUT_FILENO);
    dup2(log_fd, STDERR_FILENO);
    close(log_fd);
    
    // Set up signal handlers for daemon
    signal(SIGUSR1, daemon_signal_handler);
    signal(SIGHUP, daemon_signal_handler);
    signal(SIGTERM, daemon_signal_handler);
    
    // Daemon main loop
    while (1) {
        // Monitor FIFOs and child processes here
        sleep(5); // Check every 5 seconds
    }
}

// Child process 1: Reads numbers and determines larger one
void child_process1() {
    // Wait briefly to ensure FIFO is created
    sleep(1);
    
    int fd1 = open(FIFO1, O_RDONLY);
    if (fd1 < 0) {
        perror("Child1: Failed to open FIFO1");
        exit(EXIT_FAILURE);
    }
    
    int nums[2];
    if (read(fd1, nums, sizeof(nums)) != sizeof(nums)) {
        perror("Child1: Failed to read numbers");
        close(fd1);
        exit(EXIT_FAILURE);
    }
    close(fd1);
    
    sleep(10); // As per requirements
    
    int larger = (nums[0] > nums[1]) ? nums[0] : nums[1];
    
    int fd2 = open(FIFO2, O_WRONLY);
    if (fd2 < 0) {
        perror("Child1: Failed to open FIFO2");
        exit(EXIT_FAILURE);
    }
    
    if (write(fd2, &larger, sizeof(larger)) != sizeof(larger)) {
        perror("Child1: Failed to write larger number");
        close(fd2);
        exit(EXIT_FAILURE);
    }
    close(fd2);
    
    exit(EXIT_SUCCESS);
}

// Child process 2: Reads and prints the larger number
void child_process2() {
    // Wait briefly to ensure FIFO is created
    sleep(1);
    
    int fd2 = open(FIFO2, O_RDONLY);
    if (fd2 < 0) {
        perror("Child2: Failed to open FIFO2");
        exit(EXIT_FAILURE);
    }
    
    int larger;
    if (read(fd2, &larger, sizeof(larger)) != sizeof(larger)) {
        perror("Child2: Failed to read larger number");
        close(fd2);
        exit(EXIT_FAILURE);
    }
    close(fd2);
    
    sleep(10); // As per requirements
    
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
    (void)num1; // Use these variables to avoid unused warning
    (void)num2;
    
    // Create FIFOs
    unlink(FIFO1); // Remove if they already exist
    unlink(FIFO2);
    
    if (mkfifo(FIFO1, 0666) < 0) {
        perror("Failed to create FIFO1");
        exit(EXIT_FAILURE);
    }
    
    if (mkfifo(FIFO2, 0666) < 0) {
        perror("Failed to create FIFO2");
        unlink(FIFO1);
        exit(EXIT_FAILURE);
    }
    
    // Set up SIGCHLD handler
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    
    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
        perror("sigaction failed");
        unlink(FIFO1);
        unlink(FIFO2);
        exit(EXIT_FAILURE);
    }
    
    // Write numbers to FIFO1
    int fd1 = open(FIFO1, O_WRONLY);
    if (fd1 < 0) {
        perror("Failed to open FIFO1 for writing");
        unlink(FIFO1);
        unlink(FIFO2);
        exit(EXIT_FAILURE);
    }
    
    int nums[2] = {num1, num2};
    if (write(fd1, nums, sizeof(nums)) != sizeof(nums)) {
        perror("Failed to write to FIFO1");
        close(fd1);
        unlink(FIFO1);
        unlink(FIFO2);
        exit(EXIT_FAILURE);
    }
    close(fd1);
    
    // Create daemon process
    create_daemon();    
    
    
    // Create child processes
    pid_t pid1 = fork();
    if (pid1 < 0) {
        perror("fork failed for child1");
        unlink(FIFO1);
        unlink(FIFO2);
        exit(EXIT_FAILURE);
    }
    
    if (pid1 == 0) {
        child_process1();
    }
    
    pid_t pid2 = fork();
    if (pid2 < 0) {
        perror("fork failed for child2");
        unlink(FIFO1);
        unlink(FIFO2);
        exit(EXIT_FAILURE);
    }
    
    if (pid2 == 0) {
        child_process2();
    }
    
    total_children = 2;
    
    // Parent process loop
    while (child_counter < total_children) {
        printf("proceeding\n");
        sleep(2);
    }
    
    // Clean up FIFOs
    unlink(FIFO1);
    unlink(FIFO2);
    
    printf("All child processes completed. Exiting.\n");
    return 0;
}