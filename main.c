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
    int status;
    pid_t pid;
    
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (pid == -1) {
            perror("waitpid");
            return;
        }
        
        child_count += 2; // Increment by 2 as per requirements
        
        // Log the exit status
        FILE *log = fopen(LOG_FILE, "a");
        if (log) {
            time_t now;
            time(&now);
            fprintf(log, "[%s] Child PID %d exited with status %d\n", 
                    ctime(&now), pid, WEXITSTATUS(status));
            fclose(log);
        }
        
        printf("Child PID %d exited with status %d\n", pid, WEXITSTATUS(status));
        
        if (child_count >= total_children) {
            printf("All children have exited. Parent terminating.\n");
            exit(EXIT_SUCCESS);
        }
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
void become_daemon() {
    pid_t pid = fork();
    
    if (pid < 0) {
        perror("fork");
        exit(EXIT_FAILURE);
    }
    
    if (pid > 0) {
        // Parent exits
        exit(EXIT_SUCCESS);
    }
    
    // Child continues
    daemon_pid = getpid();
    
    // Create a new session
    if (setsid() < 0) {
        perror("setsid");
        exit(EXIT_FAILURE);
    }
    
    // Change working directory
    chdir("/");
    
    // Close standard file descriptors
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    
    // Redirect stdout and stderr to log file
    int fd = open(LOG_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        exit(EXIT_FAILURE);
    }
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    close(fd);
    
    // Set up signal handlers
    signal(SIGUSR1, daemon_signal_handler);
    signal(SIGHUP, daemon_signal_handler);
    signal(SIGTERM, daemon_signal_handler);
    
    // Log daemon startup
    time_t now;
    time(&now);
    printf("[%s] Daemon started with PID %d\n", ctime(&now), daemon_pid);
}

// Child process 1: Reads numbers and determines the larger one
void child_process1() {
    // Open FIFO1 for reading
    int fd1 = open(FIFO1, O_RDONLY);
    if (fd1 < 0) {
        perror("child1: open FIFO1");
        exit(EXIT_FAILURE);
    }
    
    // Read the two integers
    int nums[2];
    if (read(fd1, nums, sizeof(nums)) != sizeof(nums)) {
        perror("child1: read from FIFO1");
        close(fd1);
        exit(EXIT_FAILURE);
    }
    close(fd1);
    
    sleep(10); // Sleep for 10 seconds as required
    
    // Determine the larger number
    int larger = (nums[0] > nums[1]) ? nums[0] : nums[1];
    
    // Open FIFO2 for writing
    int fd2 = open(FIFO2, O_WRONLY);
    if (fd2 < 0) {
        perror("child1: open FIFO2");
        exit(EXIT_FAILURE);
    }
    
    // Write the larger number to FIFO2
    if (write(fd2, &larger, sizeof(larger)) < 0) {
        perror("child1: write to FIFO2");
        close(fd2);
        exit(EXIT_FAILURE);
    }
    close(fd2);
    
    exit(EXIT_SUCCESS);
}

// Child process 2: Reads the command and prints the larger number
void child_process2() {
    // Open FIFO2 for reading
    int fd = open(FIFO2, O_RDONLY);
    if (fd < 0) {
        perror("child2: open FIFO2");
        exit(EXIT_FAILURE);
    }
    
    sleep(10); // Sleep for 10 seconds as required
    
    // Read the larger number
    int larger;
    if (read(fd, &larger, sizeof(larger)) < 0) {
        perror("child2: read from FIFO2");
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
    int result = 0; // As per requirements
    
    // Create FIFOs
    if (mkfifo(FIFO1, 0666) < 0 && errno != EEXIST) {
        perror("mkfifo FIFO1");
        exit(EXIT_FAILURE);
    }
    if (mkfifo(FIFO2, 0666) < 0 && errno != EEXIST) {
        perror("mkfifo FIFO2");
        unlink(FIFO1);
        exit(EXIT_FAILURE);
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
    
    // Fork to create daemon process
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork for daemon");
        unlink(FIFO1);
        unlink(FIFO2);
        exit(EXIT_FAILURE);
    }
    
    if (pid == 0) {
        // Daemon process
        become_daemon();
        
        // Daemon main loop
        while (1) {
            sleep(5); // Check every 5 seconds
        }
    } else {
        // Parent process continues
        
        // Open FIFO1 for writing
        int fd1 = open(FIFO1, O_WRONLY);
        if (fd1 < 0) {
            perror("parent: open FIFO1");
            unlink(FIFO1);
            unlink(FIFO2);
            exit(EXIT_FAILURE);
        }
        
        // Write the two numbers to FIFO1
        int nums[2] = {num1, num2};
        if (write(fd1, nums, sizeof(nums)) < 0) {
            perror("parent: write to FIFO1");
            close(fd1);
            unlink(FIFO1);
            unlink(FIFO2);
            exit(EXIT_FAILURE);
        }
        close(fd1);
        
        // Create two child processes
        pid_t child1 = fork();
        if (child1 < 0) {
            perror("fork child1");
            unlink(FIFO1);
            unlink(FIFO2);
            exit(EXIT_FAILURE);
        }
        
        if (child1 == 0) {
            // Child process 1
            child_process1();
        } else {
            pid_t child2 = fork();
            if (child2 < 0) {
                perror("fork child2");
                unlink(FIFO1);
                unlink(FIFO2);
                exit(EXIT_FAILURE);
            }
            
            if (child2 == 0) {
                // Child process 2
                child_process2();
            } else {
                // Parent process
                total_children = 2; // We have 2 children
                
                // Main loop - print "proceeding" every 2 seconds
                while (1) {
                    printf("proceeding\n");
                    sleep(2);
                }
            }
        }
    }
    
    return 0;
}