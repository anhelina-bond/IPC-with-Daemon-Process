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

#define FIFO1 "/tmp/fifo1"
#define FIFO2 "/tmp/fifo2"
#define LOG_FILE "/tmp/daemon_log.txt"

volatile sig_atomic_t child_count = 0;
int total_children = 2;
pid_t daemon_pid = 0;

// Signal handler for SIGCHLD
void sigchld_handler(int sig) {
    (void)sig;
    int status;
    pid_t pid;
    FILE *log = fopen(LOG_FILE, "a");

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (log) {
            time_t now = time(NULL);
            fprintf(log, "[%s] Child PID %d exited with status %d\n", 
                    ctime(&now), pid, WEXITSTATUS(status));
        }
        printf("Child PID %d exited with status %d\n", pid, WEXITSTATUS(status));
        child_count++;
    }
    if (log) fclose(log);
}

// Daemon signal handler
void daemon_signal_handler(int sig) {
    FILE *log = fopen(LOG_FILE, "a");
    time_t now = time(NULL);
    if (log) {
        fprintf(log, "[%s] Received signal %d\n", ctime(&now), sig);
        fclose(log);
    }
    if (sig == SIGTERM) {
        unlink(FIFO1);
        unlink(FIFO2);
        exit(EXIT_SUCCESS);
    }
}

// Function to become a daemon
int become_daemon() {
    if (fork() > 0) _exit(EXIT_SUCCESS);
    if (setsid() == -1) return -1;
    if (fork() > 0) _exit(EXIT_SUCCESS);
    umask(0);
    chdir("/");
    for (int fd = 0; fd < 1024; fd++) close(fd);
    int log_fd = open(LOG_FILE, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (log_fd != -1) {
        dup2(log_fd, STDOUT_FILENO);
        dup2(log_fd, STDERR_FILENO);
        if (log_fd > 2) close(log_fd);
    }
    return 0;
}

void child_process1() {
    int fd1 = open(FIFO1, O_RDONLY);
    if (fd1 == -1) exit(EXIT_FAILURE);
    int nums[2];
    if (read(fd1, nums, sizeof(nums)) == -1) exit(EXIT_FAILURE);
    close(fd1);
    int larger = (nums[0] > nums[1]) ? nums[0] : nums[1];
    int fd2 = open(FIFO2, O_WRONLY);
    if (fd2 == -1) exit(EXIT_FAILURE);
    write(fd2, &larger, sizeof(larger));
    close(fd2);
    exit(EXIT_SUCCESS);
}

void child_process2() {
    int fd = open(FIFO2, O_RDONLY);
    if (fd == -1) exit(EXIT_FAILURE);
    sleep(10);
    int larger;
    if (read(fd, &larger, sizeof(larger)) == -1) exit(EXIT_FAILURE);
    close(fd);
    printf("The larger number is: %d\n", larger);
    exit(EXIT_SUCCESS);
}

void log_message(const char *message) {
    FILE *log = fopen(LOG_FILE, "a");
    if (log) {
        time_t now = time(NULL);
        fprintf(log, "[%s] %s\n", ctime(&now), message);
        fclose(log);
    }
}

int main(int argc, char *argv[]) {
    if (argc != 3) exit(EXIT_FAILURE);
    int nums[2] = {atoi(argv[1]), atoi(argv[2])};
    if (mkfifo(FIFO1, 0666) < 0 && errno != EEXIST) exit(EXIT_FAILURE);
    if (mkfifo(FIFO2, 0666) < 0 && errno != EEXIST) { unlink(FIFO1); exit(EXIT_FAILURE); }
    int fd1 = open(FIFO1, O_WRONLY);
    if (fd1 == -1) exit(EXIT_FAILURE);
    write(fd1, nums, sizeof(nums));
    close(fd1);
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);
    if (fork() == 0) child_process1();
    if (fork() == 0) child_process2();
    if (fork() == 0) {
        if (become_daemon() == -1) exit(EXIT_FAILURE);
        log_message("Daemon started.");
        while (1) { sleep(5); log_message("Daemon is alive."); }
    }
    while (child_count < total_children) {
        printf("proceeding\n");
        sleep(2);
    }
    unlink(FIFO1);
    unlink(FIFO2);
    return 0;
}
