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
#include <sys/ipc.h>
#include <sys/shm.h>

#define FIFO1 "fifo1"
#define FIFO2 "fifo2"
#define LOG_FILE "daemon_log.txt"
#define CHILD_TIMEOUT 30  // 30 seconds timeout
#define MAX_CHILDREN 10

// Structure to track child processes
typedef struct {
    pid_t pid;
    time_t start_time;
} child_process;

// Shared memory structure
typedef struct {
    child_process children[MAX_CHILDREN];
    int num_children;
} shared_data;

volatile sig_atomic_t child_count = 0;
volatile sig_atomic_t total_children = 0;
pid_t daemon_pid = 0;
int shmid = -1;
int daemon_pipe[2];

// Signal handler for SIGCHLD
void sigchld_handler(int sig) {
    (void)sig;
    int status;
    pid_t pid;
    char buf[100];
    
    // Attach to shared memory
    shared_data *shared = shmat(shmid, NULL, 0);
    if (shared == (void*)-1) {
        write(STDERR_FILENO, "Failed to attach shared memory in SIGCHLD handler\n", 48);
        return;
    }

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        // Remove from tracking
        for (int i = 0; i < shared->num_children; i++) {
            if (shared->children[i].pid == pid) {
                // Enhanced exit status reporting
                if (WIFEXITED(status)) {
                    snprintf(buf, sizeof(buf), 
                            "Child %d exited normally with status %d\n",
                            pid, WEXITSTATUS(status));
                } 
                else if (WIFSIGNALED(status)) {
                    snprintf(buf, sizeof(buf),
                            "Child %d killed by signal %d (%s)\n",
                            pid, WTERMSIG(status), strsignal(WTERMSIG(status)));
                }
                else if (WIFSTOPPED(status)) {
                    snprintf(buf, sizeof(buf),
                            "Child %d stopped by signal %d\n",
                            pid, WSTOPSIG(status));
                }
                write(STDOUT_FILENO, buf, strlen(buf));
                child_count++;
                // Shift array down
                memmove(&shared->children[i], &shared->children[i+1],
                       (shared->num_children - i - 1) * sizeof(child_process));
                shared->num_children--;
                break;
            }
        }
        
    }
    
    // Handle waitpid errors (except ECHILD which means no children)
    if (pid == -1 && errno != ECHILD) {
        snprintf(buf, sizeof(buf), "waitpid error: %s\n", strerror(errno));
        write(STDERR_FILENO, buf, strlen(buf));
    }
    
    // Detach from shared memory
    shmdt(shared);
}


// Signal handler for daemon process
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
        if (shmid != -1) {
            shmctl(shmid, IPC_RMID, NULL);
        }
        _exit(EXIT_SUCCESS);
    }
}

// Timeout checking function
void check_timeouts() {
    shared_data *shared = shmat(shmid, NULL, 0);
    if (shared == (void*)-1) {
        write(STDERR_FILENO, "Failed to attach shared memory for timeout check\n", 50);
        return;
    }

    time_t now = time(NULL);
    
    for (int i = 0; i < shared->num_children; i++) {
        if (now - shared->children[i].start_time > CHILD_TIMEOUT) {
            pid_t pid = shared->children[i].pid;
            
            char buf[100];
            snprintf(buf, sizeof(buf), "Terminating stalled child PID %d\n", pid);
            write(STDERR_FILENO, buf, strlen(buf));

            kill(pid, SIGTERM);
            sleep(1);  // Grace period
            
            if (waitpid(pid, NULL, WNOHANG) == 0) {
                kill(pid, SIGKILL);
                waitpid(pid, NULL, 0);
            }
            
            // Remove from tracking
            memmove(&shared->children[i], &shared->children[i+1],
                   (shared->num_children - i - 1) * sizeof(child_process));
            shared->num_children--;
            i--;
        }
    }
    
    shmdt(shared);
}

int become_daemon() {
    // First fork
    switch (fork()) {
        case -1: 
            return -1;
        case 0: 
            break;
        default: 
            _exit(EXIT_SUCCESS); // Parent exits
    }

    // Create new session
    if (setsid() == -1) {
        write(STDERR_FILENO, "setsid failed\n", 14);
        return -1;
    }

    // Second fork
    switch (fork()) {
        case -1: 
            return -1;
        case 0: 
            break;
        default: 
            _exit(EXIT_SUCCESS);    // Session leader exits
    }

    pid_t mypid = getpid();
    write(daemon_pipe[1], &mypid, sizeof(mypid));  // Send PID to parent
    close(daemon_pipe[1]);  // Close write end
    
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

    daemon_pid = getpid();

    return 0;
}

void child_process1() {
    printf("Child 1 started\n");
    fflush(stdout);

    int fd1 = open(FIFO1, O_RDONLY);
    if (fd1 == -1) {
        printf("Error opening FIFO1 in Child 1\n");
        fflush(stdout);
        exit(EXIT_FAILURE);
    }

    int nums[2];
    if (read(fd1, nums, sizeof(nums)) == -1) {
        printf("Error reading from FIFO1\n");
        fflush(stdout);
        close(fd1);
        exit(EXIT_FAILURE);
    }
    close(fd1);

    int larger = (nums[0] > nums[1]) ? nums[0] : nums[1];
    printf("Child 1: Comparing %d and %d, larger is %d\n", nums[0], nums[1], larger);
    fflush(stdout);

    int fd2 = open(FIFO2, O_WRONLY);
    if (fd2 == -1) {
        printf("Error opening FIFO2 in Child 1\n");
        fflush(stdout);
        exit(EXIT_FAILURE);
    }

    if (write(fd2, &larger, sizeof(larger)) == -1) {
        printf("Error writing to FIFO2\n");
        fflush(stdout);
        close(fd2);
        exit(EXIT_FAILURE);
    }
    close(fd2);

    exit(EXIT_SUCCESS);
}

void child_process2() {
    sleep(10);
    printf("Child 2 started\n");    
    fflush(stdout);
    int fd = open(FIFO2, O_RDONLY);
    if (fd == -1) {
        printf("Error opening FIFO2 in Child 2\n");
        fflush(stdout);
        exit(EXIT_FAILURE);
    }    
    
    int larger;
    if (read(fd, &larger, sizeof(larger)) == -1) {
        printf("Error reading from FIFO2\n");
        fflush(stdout);
        close(fd);
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

    setvbuf(stdout, NULL, _IOLBF, 0);  // Line buffering
    setvbuf(stderr, NULL, _IOLBF, 0);  // Line buffering

    // Setup shared memory
    shmid = shmget(IPC_PRIVATE, sizeof(shared_data), IPC_CREAT | 0666);
    if (shmid == -1) {
        perror("shmget failed");
        exit(EXIT_FAILURE);
    }

    // Initialize shared memory
    shared_data *shared = shmat(shmid, NULL, 0);
    if (shared == (void*)-1) {
        perror("shmat failed");
        shmctl(shmid, IPC_RMID, NULL);
        exit(EXIT_FAILURE);
    }
    memset(shared, 0, sizeof(shared_data));
    shmdt(shared);

    int log_fd = open(LOG_FILE, O_WRONLY|O_CREAT|O_APPEND, 0644);
    if (log_fd == -1) {
        perror("Failed to open log file");
        shmctl(shmid, IPC_RMID, NULL);
        exit(EXIT_FAILURE);
    }
    
    // Redirect stdout/stderr
    dup2(log_fd, STDOUT_FILENO);
    dup2(log_fd, STDERR_FILENO);
    close(log_fd);

    if (pipe(daemon_pipe) == -1) {
        perror("pipe creation failed");
        exit(EXIT_FAILURE);
    }

    // Fork the daemon
    pid_t fork_pid = fork();
    if (fork_pid == 0) {
        close(daemon_pipe[0]);  // Close read end in child
        if (become_daemon() == -1) {
            fprintf(stderr, "Failed to create daemon\n");
            shmctl(shmid, IPC_RMID, NULL);
            exit(EXIT_FAILURE);
        }

        struct sigaction dsa;
        dsa.sa_handler = daemon_signal_handler;
        sigemptyset(&dsa.sa_mask);
        dsa.sa_flags = SA_RESTART;
        sigaction(SIGUSR1, &dsa, NULL);
        sigaction(SIGHUP, &dsa, NULL);
        sigaction(SIGTERM, &dsa, NULL);

        // Daemon main loop
        while (1) {
            check_timeouts();
            sleep(5);  // Check every 5 seconds
        }
    } else {
        close(daemon_pipe[1]);  // Close write end in parent
        
        // Read the daemon's PID
        read(daemon_pipe[0], &daemon_pid, sizeof(daemon_pid));
        close(daemon_pipe[0]);
        
        printf("Daemon PID: %d\n", daemon_pid);
    }
    
    int num1 = atoi(argv[1]);
    int num2 = atoi(argv[2]);

    // Create FIFOs
    unlink(FIFO1);
    unlink(FIFO2);
    
    if (mkfifo(FIFO1, 0666) == -1) {
        fprintf(stderr, "mkfifo FIFO1 failed\n");
        shmctl(shmid, IPC_RMID, NULL);
        exit(EXIT_FAILURE);
    }
    fprintf(stderr, "fifo1 created successfully\n");

    if (mkfifo(FIFO2, 0666) < 0) {
        fprintf(stderr, "mkfifo FIFO2 failed\n");
        unlink(FIFO1);
        shmctl(shmid, IPC_RMID, NULL);
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
        shmctl(shmid, IPC_RMID, NULL);
        exit(EXIT_FAILURE);
    }

    // Create child processes
    pid_t child1 = fork();
    if (child1 == -1) {
        fprintf(stderr, "fork failed for child1\n");
        unlink(FIFO1);
        unlink(FIFO2);
        shmctl(shmid, IPC_RMID, NULL);
        exit(EXIT_FAILURE);
    } else if (child1 == 0) {
        child_process1();
    } else {
        // Add to shared memory
        shared = shmat(shmid, NULL, 0);
        if (shared != (void*)-1 && shared->num_children < MAX_CHILDREN) {
            shared->children[shared->num_children].pid = child1;
            shared->children[shared->num_children].start_time = time(NULL);
            shared->num_children++;
            shmdt(shared);
        }
    }

    pid_t child2 = fork();
    if (child2 == -1) {
        fprintf(stderr, "fork failed for child2\n");
        unlink(FIFO1);
        unlink(FIFO2);
        shmctl(shmid, IPC_RMID, NULL);
        exit(EXIT_FAILURE);
    } else if (child2 == 0) {
        child_process2();
    } else {
        // Add to shared memory
        shared = shmat(shmid, NULL, 0);
        if (shared != (void*)-1 && shared->num_children < MAX_CHILDREN) {
            shared->children[shared->num_children].pid = child2;
            shared->children[shared->num_children].start_time = time(NULL);
            shared->num_children++;
            shmdt(shared);
        }
    }

    total_children = 2;
    printf("Parent process started. Child1 PID: %d, Child2 PID: %d\n", child1, child2);
fflush(stdout);

    // Open FIFO1 for writing
    int fd1 = open(FIFO1, O_WRONLY);
    if (fd1 == -1) {
        fprintf(stderr, "Error opening FIFO1 in write mode\n");
        unlink(FIFO1);
        unlink(FIFO2);
        shmctl(shmid, IPC_RMID, NULL);
        exit(EXIT_FAILURE);
    }

    int num[2] = {num1, num2};
    if (write(fd1, num, sizeof(num)) == -1) {
        fprintf(stderr, "Error writing to FIFO1\n");
        close(fd1);
        unlink(FIFO1);
        unlink(FIFO2);
        shmctl(shmid, IPC_RMID, NULL);
        exit(EXIT_FAILURE);
    }
    close(fd1);

    // Main loop
    while (child_count < total_children) {
        printf("Proceeding...\n");
        fflush(stdout);
        sleep(2);
    }

    printf("All children have exited. Parent terminating.\n");
    fflush(stdout);
    unlink(FIFO1);
    unlink(FIFO2);
    shmctl(shmid, IPC_RMID, NULL);
    kill(daemon_pid, SIGTERM);
    exit(EXIT_SUCCESS);
} 