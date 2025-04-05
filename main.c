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
        printf("Child count: %d", child_count);
        fflush(stdout)

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
                fflush(stdout)
                fclose(log);
            }
            break;
        case SIGHUP:
            if (log) {
                fprintf(log, "[%s] Received SIGHUP signal\n", ctime(&now));
                fflush(stdout)
                fclose(log);
            }
            break;
        case SIGTERM:
            if (log) {
                fprintf(log, "[%s] Received SIGTERM signal. Daemon exiting.\n", ctime(&now));
                fflush(stdout)
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


    maxfd = sysconf(_SC_OPEN_MAX);
    if (maxfd == -1) maxfd = 1024;
    for (fd = 0; fd < maxfd; fd++) {
            close(fd);
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
    printf("Child 1 started\n");
    fflush(stdout)

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
    fflush(stdout);
    
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
    fflush(stdout)
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
    fflush(stdout)
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
    unlink(FIFO1);  // Remove if they exist
    unlink(FIFO2);
    
    if (mkfifo(FIFO1, 0666) == -1) {
        perror("mkfifo FIFO1");
        exit(EXIT_FAILURE);
    }
    printf("fifo1 created successfully\n");
    
    if (mkfifo(FIFO2, 0666) < 0) {
        perror("mkfifo FIFO2");
        unlink(FIFO1);
        exit(EXIT_FAILURE);
    }
    printf("fifo2 created successfully\n");

    // Create the daemon process
    pid_t daemon_pid = fork();
    if (daemon_pid == 0) {
        if (become_daemon() == -1) {
            perror("Failed to create daemon");
            exit(EXIT_FAILURE);
        }

        struct sigaction dsa;
        dsa.sa_handler = daemon_signal_handler;
        sigemptyset(&dsa.sa_mask);
        dsa.sa_flags = SA_RESTART;
        sigaction(SIGUSR1, &dsa, NULL);
        sigaction(SIGHUP, &dsa, NULL);
        sigaction(SIGTERM, &dsa, NULL);

        while (1) {
            sleep(5);
        }
    }

    // Set up SIGCHLD handler first
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGCHLD, &sa, NULL) < 0) {
        perror("sigaction");
        unlink(FIFO1);
        unlink(FIFO2);
        exit(EXIT_FAILURE);
    }
    printf("SIGCHLD handler assigned\n");
    fflush(stdout)

    // Create child processes first
    pid_t child1 = fork();
    if (child1 == -1) {
        perror("fork failed for child1");
        unlink(FIFO1);
        unlink(FIFO2);
        exit(EXIT_FAILURE);
    } else if (child1 == 0) {
        child_process1();  // This will block waiting for FIFO1 to be opened for writing
    }

    pid_t child2 = fork();
    if (child2 == -1) {
        perror("fork failed for child2");
        unlink(FIFO1);
        unlink(FIFO2);
        exit(EXIT_FAILURE);
    } else if (child2 == 0) {
        child_process2();  // This will sleep first
    }

    printf("Parent process started. Child1 PID: %d, Child2 PID: %d\n", child1, child2);
    fflush(stdout);

    // Now open FIFO1 for writing (after child1 is ready to read)
    int fd1 = open(FIFO1, O_WRONLY);
    if (fd1 == -1) {
        perror("Error opening FIFO1 in write mode");
        unlink(FIFO1);
        unlink(FIFO2);
        exit(EXIT_FAILURE);
    }

    int num[2] = {num1, num2};
    if (write(fd1, num, sizeof(num)) == -1) {
        perror("Error writing to FIFO1");
        close(fd1);
        unlink(FIFO1);
        unlink(FIFO2);
        exit(EXIT_FAILURE);
    }
    close(fd1);

    
    total_children = 2;

    printf("Main process PID: %d, entering main loop\n", getpid());
    fflush(stdout)

    while (child_count < total_children) {
        printf("proceeding...\n");
        sleep(2);
    }

    printf("All children have exited. Parent terminating.\n");
    fflush(stdout)
    unlink(FIFO1);
    unlink(FIFO2);
    exit(EXIT_SUCCESS);
}