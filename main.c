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
#define CHILD_TIMEOUT 15  // 15 seconds timeout
#define MAX_CHILDREN 10

typedef struct {
    pid_t pid;
    time_t start_time;
    int timed_out;  // Flag to mark terminated processes
} ChildProcess;

ChildProcess child_table[MAX_CHILDREN];
int num_children = 0;
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

        // Mark child as terminated in table
        for (int i = 0; i < num_children; i++) {
            if (child_table[i].pid == pid) {
                child_table[i].timed_out = 1;
                break;
            }
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
    
    // Remove newline from ctime() output
    time_str[strlen(time_str)-1] = '\0';
    
    switch(sig) {
        case SIGUSR1:
            snprintf(buf, sizeof(buf), "[%s] SIGUSR1 received\n", time_str);
            break;
        case SIGHUP:
            snprintf(buf, sizeof(buf), "[%s] SIGHUP received\n", time_str);
            break;
        case SIGTERM:
            snprintf(buf, sizeof(buf), "[%s] SIGTERM received - daemon exiting\n", time_str);
            break;
        default:
            return;
    }
    
    write(STDERR_FILENO, buf, strlen(buf));
    
    if (sig == SIGTERM) {
        _exit(EXIT_SUCCESS);
    }
}


// Timeout monitoring function
void check_timeouts() {
    time_t now = time(NULL);
    
    for (int i = 0; i < num_children; i++) {
        if (!child_table[i].timed_out && 
            (now - child_table[i].start_time > CHILD_TIMEOUT)) {
            
            pid_t pid = child_table[i].pid;
            printf("Terminating child %d due to timeout\n", pid);
            fflush(stdout);
            
            // Try graceful termination first
            kill(pid, SIGTERM);
            sleep(1);
            
            // Force kill if still running
            if (waitpid(pid, NULL, WNOHANG) == 0) {
                kill(pid, SIGKILL);
                waitpid(pid, NULL, 0);  // Reap the zombie
            }
            
            child_table[i].timed_out = 1;
            child_count++;
        }
    }
}

// Become a daemon
int become_daemon() {
    // First fork
    switch (fork()) {
        case -1: return -1;
        case 0: break;
        default: _exit(EXIT_SUCCESS); // Parent exits
    }

    // Create new session
    if (setsid() == -1) {
        write(STDERR_FILENO, "setsid failed\n", 14);
        return -1;
    }

    // Second fork
    switch (fork()) {
        case -1: return -1;
        case 0: break;
        default: _exit(EXIT_SUCCESS);    // Session leader exits
    }
    
    // Open log file
    int log_fd = open(LOG_FILE, O_WRONLY|O_CREAT|O_APPEND , 0644);
    if (log_fd == -1) {
        write(STDERR_FILENO, "Failed to open log file\n", 23);
        return -1;
    }

    // Redirect stdio
    dup2(log_fd, STDOUT_FILENO);
    dup2(log_fd, STDERR_FILENO);
    close(log_fd);

    // Close all other descriptors
    int maxfd = sysconf(_SC_OPEN_MAX);
    for (int fd = 3; fd < maxfd; fd++) close(fd);

    // Redirect stdin from /dev/null
    int null_fd = open("/dev/null", O_RDONLY);
    if (null_fd != -1) {
        dup2(null_fd, STDIN_FILENO);
        close(null_fd);
    }

    return 0;
}

void child_process1() {
    sleep(10);
    printf("Child 1 started\n");
    fflush(stdout);
    
    int fd1 = open(FIFO1, O_RDONLY);
    if (fd1 == -1) exit(EXIT_FAILURE);

    int nums[2];
    if (read(fd1, nums, sizeof(nums)) == -1) exit(EXIT_FAILURE);
    close(fd1);

    int larger = (nums[0] > nums[1]) ? nums[0] : nums[1];
    printf("Child 1: Larger of %d and %d is %d\n", nums[0], nums[1], larger);
    fflush(stdout);
    int fd2 = open(FIFO2, O_WRONLY);
    if (fd2 == -1) exit(EXIT_FAILURE);
    
    if (write(fd2, &larger, sizeof(larger)) == -1) exit(EXIT_FAILURE);
    close(fd2);

    exit(EXIT_SUCCESS);
}

void child_process2() {
    sleep(10); // This will trigger timeout
    printf("Child 2 started\n");
    fflush(stdout);
    int fd = open(FIFO2, O_RDONLY);
    if (fd == -1) exit(EXIT_FAILURE);
    
    int larger;
    if (read(fd, &larger, sizeof(larger)) == -1) exit(EXIT_FAILURE);
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

    setvbuf(stdout, NULL, _IOLBF, 0);  // Line buffering
    setvbuf(stderr, NULL, _IOLBF, 0);  // Line buffering

    // Become daemon
    if (become_daemon() == -1) {
        fprintf(stderr, "Failed to create daemon\n");
        exit(EXIT_FAILURE);
    } else {
        struct sigaction dsa;
        dsa.sa_handler = daemon_signal_handler;
        sigemptyset(&dsa.sa_mask);
        dsa.sa_flags = SA_RESTART;
        sigaction(SIGUSR1, &dsa, NULL);
        sigaction(SIGHUP, &dsa, NULL);
        sigaction(SIGTERM, &dsa, NULL);

        // // Daemon main loop
        // while (1) {
        //     sleep(5);  // Check every 5 seconds
        // }
    }

    
    
    // Create FIFOs
    unlink(FIFO1);
    unlink(FIFO2);
    
    if (mkfifo(FIFO1, 0666) == -1) {
        fprintf(stderr, "mkfifo FIFO1 failed\n");
        exit(EXIT_FAILURE);
    }
    fprintf(stderr, "fifo1 created successfully\n");

    if (mkfifo(FIFO2, 0666) < 0) {
        fprintf(stderr, "mkfifo FIFO2 failed\n");
        unlink(FIFO1);
        exit(EXIT_FAILURE);
    }
    fprintf(stderr, "fifo2 created successfully\n");

     // Set up SIGCHLD handler
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGCHLD, &sa, NULL) < 0) {
        fprintf(stderr, "sigaction failed\n");
        unlink(FIFO1);
        unlink(FIFO2);
        exit(EXIT_FAILURE);
    }

    // Fork child processes
    pid_t child1 = fork();
    if (child1 == 0) {
        child_process1()
    } else {
        if (num_children < MAX_CHILDREN) {
            child_table[num_children].pid = child1;
            child_table[num_children].start_time = time(NULL);
            child_table[num_children].timed_out = 0;
            num_children++;
        }
    }
    
    pid_t child2 = fork();
    if (child2 == 0) {
        child_process2()
    } else {
        if (num_children < MAX_CHILDREN) {
            child_table[num_children].pid = child2;
            child_table[num_children].start_time = time(NULL);
            child_table[num_children].timed_out = 0;
            num_children++;
        }
    }
    
    total_children = 2;
    printf("Daemon started. Child PIDs: %d, %d\n", child1, child2);
    fflush(stdout);
    // Parent (daemon) writes to FIFO1
    int fd1 = open(FIFO1, O_WRONLY);
    if (fd1 == -1) {
        fprintf(stderr, "open FIFO1 failed");
        exit(EXIT_FAILURE);
    }
    
    int nums[2] = {atoi(argv[1]), atoi(argv[2])};
    if (write(fd1, nums, sizeof(nums)) == -1) {
        fprintf(stderr, "write to FIFO1 failed");
        exit(EXIT_FAILURE);
    }
    close(fd1);

   
    while (child_count < total_children) {
        // Timeout monitoring
        check_timeouts();
        printf("Proceeding...\n");
        fflush(stdout);
        sleep(2);
    }

    // Cleanup
    unlink(FIFO1);
    unlink(FIFO2);
    printf("Daemon exiting\n");
    fflush(stdout);
    return EXIT_SUCCESS;
}