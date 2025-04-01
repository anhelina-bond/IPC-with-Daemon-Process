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
#include <signal.h>

#define FIFO1 "fifo1"
#define FIFO2 "fifo2"
#define LOG_FILE "daemon_log.txt"

volatile sig_atomic_t child_count = 0;
volatile sig_atomic_t total_children = 0;
pid_t daemon_pid = 0;

// Signal handler for SIGCHLD
void sigchld_handler(int sig) { 
    (void)sig;
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        // Log exit status safely
        char buf[100];
        int len = snprintf(buf, sizeof(buf), "Child PID %d exited with status %d\n", 
                           pid, WEXITSTATUS(status));
        write(STDOUT_FILENO, buf, len); // Async-safe logging

        child_count++;


    }
}

// Signal handler for daemon process
void daemon_signal_handler(int sig) {
    FILE *log = fopen(LOG_FILE, "a");
    time_t now;
    time(&now);
    
    switch(sig) {
        case SIGUSR1:
            if (log) {
                fprintf(log, "[%s] Received SIGUSR1 signal\n", ctime(&now));
                fclose(log);
            }
            break;
        case SIGHUP:
            if (log) {
                fprintf(log, "[%s] Received SIGHUP signal\n", ctime(&now));
                fclose(log);
            }
            break;
        case SIGTERM:
            if (log) {
                fprintf(log, "[%s] Received SIGTERM signal. Daemon exiting.\n", ctime(&now));
                fclose(log);
            }
            exit(EXIT_SUCCESS);
            break;
    }
}

// Function to become a daemon
int become_daemon() {
    int maxfd, fd;
    
    // First fork: Parent exits, child continues
    switch (fork()) {
        case -1: return -1;  // Error
        case 0: break;       // Child continues execution
        default: _exit(EXIT_SUCCESS);  // Parent exits
    }

    // Create new session and become session leader
    if (setsid() == -1) {
        perror("setsid failed");
        return -1;
    }

    // Second fork: Ensures the daemon is not a session leader
    switch (fork()) {
        case -1: return -1;  // Error
        case 0: break;       // Child continues execution
        default: _exit(EXIT_SUCCESS);  // Parent exits
    }


    int fifo1_fd = open(FIFO1, O_RDWR);
    int fifo2_fd = open(FIFO2, O_RDWR);

    maxfd = sysconf(_SC_OPEN_MAX);
    if (maxfd == -1) maxfd = 1024;
    for (fd = 0; fd < maxfd; fd++) {
        if (fd != fifo1_fd && fd != fifo2_fd) { // Keep FIFOs open
            close(fd);
        }
    }

    // Redirect standard file descriptors to /dev/null
    fd = open(LOG_FILE,  O_WRONLY | O_CREAT | O_APPEND);
    if (fd != -1) {
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        // if (fd > 2) close(fd);
    } else {
        perror("Failed to open log file");
        exit(EXIT_FAILURE);
    }

    return 0;
}


void child_process1() {
    sleep(10); // Simulated delay

    printf("Child 1 started\n");

    int fd1 = open(FIFO1, O_RDONLY);
    if (fd1 == -1) {
        perror("Error opening FIFO1 in Child 1");
        exit(EXIT_FAILURE);
    }

    int nums[2];
    ssize_t bytes_read = read(fd1, nums, sizeof(nums));
    if (bytes_read == -1) {
        perror("Error reading from FIFO1");
        exit(EXIT_FAILURE);
    }
    close(fd1);

    int larger = (nums[0] > nums[1]) ? nums[0] : nums[1];

    printf("Child 1: Comparing %d and %d, larger is %d\n", nums[0], nums[1], larger);

    int fd2 = open(FIFO2, O_WRONLY);
    if (fd2 == -1) {
        perror("Error opening FIFO2 in Child 1");
        exit(EXIT_FAILURE);
    }

    ssize_t bytes_written = write(fd2, &larger, sizeof(larger));
    if (bytes_written == -1) {
        perror("Error writing to FIFO2");
        exit(EXIT_FAILURE);
    }
    close(fd2);

    exit(EXIT_SUCCESS);
}


void child_process2() {
    sleep(10); // Simulated delay

    printf("Child 2 started\n");    
    int fd = open(FIFO2, O_RDONLY);
    if (fd == -1) {
        perror("Error opening FIFO2 in Child 2");
        exit(EXIT_FAILURE);
    }    
    
    int larger;
    ssize_t bytes_read = read(fd, &larger, sizeof(larger));
    if (bytes_read == -1) {
        perror("Error reading from FIFO2");
        exit(EXIT_FAILURE);
    }
    
    close(fd);

    printf("The larger number is: %d\n", larger);
    fflush(stdout);
    
    exit(EXIT_SUCCESS);
}


int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <num1> <num2>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    int num1 = atoi(argv[1]);
    int num2 = atoi(argv[2]);

    // Create FIFOs
    if (mkfifo(FIFO1, 0666) == -1) {
        if (errno != EEXIST) {
            perror("mkfifo FIFO1");
            exit(EXIT_FAILURE);
        }
    } else {
        printf("fifo1 created successfully\n");
    }
    if (mkfifo(FIFO2, 0666) < 0 && errno != EEXIST) {
        perror("mkfifo FIFO2");
        unlink(FIFO1);
        exit(EXIT_FAILURE);
    }else {
        printf("fifo2\n");
    }

    int fd1 = open(FIFO1, O_RDWR | O_NONBLOCK);

    if (fd1!=-1) {
        int num[2] = {num1, num2};
        write(fd1, num, sizeof(num));
        close(fd1);
    } else{
        perror("Error opening FIFO1 in write mode");
        exit(EXIT_FAILURE);
    }

    // Create the daemon process after forking children
    pid_t daemon_pid = fork();
    if (daemon_pid == 0) {
        if (become_daemon() == -1) {
            perror("Failed to create daemon");
            exit(EXIT_FAILURE);
        }

        // Register signal
        struct sigaction sa;
        sa.sa_handler = daemon_signal_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        sigaction(SIGUSR1, &sa, NULL);
        sigaction(SIGHUP, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);

        while (1) {
            time_t now;
            time(&now);
            printf("[%s] Daemon is running\n", ctime(&now));
            fflush(stdout);  // Ensure immediate writing
            sleep(5);
        }
    }

    // Set up SIGCHLD handler
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa, NULL) < 0) {
        perror("sigaction");
        unlink(FIFO1);
        unlink(FIFO2);
        exit(EXIT_FAILURE);
    }

    printf("sigchild assigned\n");

    // Create two child processes
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
    printf("Parent process started. Child1 PID: %d, Child2 PID: %d\n", child1, child2);
    fflush(stdout);


    
    total_children = 2;

    // Main process proceeds normally
    printf("Main process PID: %d, entering main loop\n", getpid());

    while (child_count < total_children) {
        printf("proceeding\n");
        sleep(2);
    }

    write(STDOUT_FILENO, "All children have exited. Parent terminating.\n", 47);
    exit(EXIT_SUCCESS);
}