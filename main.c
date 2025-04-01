#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>
#include <string.h>
#include <time.h>

#define FIFO1 "fifo1"
#define FIFO2 "fifo2"
#define LOG_FILE "daemon_log.txt"

volatile sig_atomic_t child_count = 0;
volatile sig_atomic_t total_children = 2;
volatile sig_atomic_t shutdown_requested = 0;
volatile sig_atomic_t result = 0;

void log_message(const char *message) {
    FILE *log = fopen(LOG_FILE, "a");
    if (log) {
        time_t now = time(NULL);
        fprintf(log, "[%s] %s\n", ctime(&now), message);
        fclose(log);
    }
}

void sigchld_handler(int sig) {
    (void)sig;
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        child_count++;
        char msg[100];
        snprintf(msg, sizeof(msg), "Child process %d exited with status %d", pid, WEXITSTATUS(status));
        log_message(msg);
        printf("Child %d exited with status %d\n", pid, WEXITSTATUS(status));
    }
}

void sigterm_handler(int sig) {
    (void)sig;
    shutdown_requested = 1;
    log_message("Daemon received SIGTERM. Shutting down.");
}

void child_process1() {
    sleep(10);  

    int fd1 = open(FIFO1, O_RDONLY);
    if (fd1 == -1) {
        perror("Child 1: Error opening FIFO1");
        exit(EXIT_FAILURE);
    }

    int nums[2];
    if (read(fd1, nums, sizeof(nums)) <= 0) {
        perror("Child 1: Failed to read from FIFO1");
        close(fd1);
        exit(EXIT_FAILURE);
    }
    close(fd1);

    int larger = (nums[0] > nums[1]) ? nums[0] : nums[1];
    result = larger;  

    int fd2 = open(FIFO2, O_WRONLY);
    if (fd2 == -1) {
        perror("Child 1: Error opening FIFO2");
        exit(EXIT_FAILURE);
    }

    write(fd2, &larger, sizeof(larger));
    printf("Child 1: Sent %d to FIFO2\n", larger);
    close(fd2);

    exit(EXIT_SUCCESS);
}

void child_process2() {
    sleep(10);  

    int fd2 = open(FIFO2, O_RDONLY);
    if (fd2 == -1) {
        perror("Child 2: Error opening FIFO2");
        exit(EXIT_FAILURE);
    }

    int larger;
    if (read(fd2, &larger, sizeof(larger)) <= 0) {
        perror("Child 2: Failed to read from FIFO2");
        close(fd2);
        exit(EXIT_FAILURE);
    }
    close(fd2);

    printf("The larger number is: %d\n", larger);
    fflush(stdout);

    exit(EXIT_SUCCESS);
}

void daemon_process() {
    log_message("Daemon started.");

    struct sigaction sa;
    sa.sa_handler = sigterm_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    while (!shutdown_requested) {
        sleep(1);
        if (child_count >= total_children) break;
    }

    log_message("Daemon shutting down.");
    unlink(FIFO1);
    unlink(FIFO2);
    exit(EXIT_SUCCESS);
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <int1> <int2>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    int num1 = atoi(argv[1]);
    int num2 = atoi(argv[2]);

    unlink(FIFO1);
    unlink(FIFO2);

    if (mkfifo(FIFO1, 0666) == -1 && errno != EEXIST) {
        perror("mkfifo FIFO1");
        exit(EXIT_FAILURE);
    }

    if (mkfifo(FIFO2, 0666) == -1 && errno != EEXIST) {
        perror("mkfifo FIFO2");
        unlink(FIFO1);
        exit(EXIT_FAILURE);
    }

    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

     int fd1 = open(FIFO1, O_WRONLY);
    if (fd1 == -1) {
        perror("Parent: Error opening FIFO1");
        exit(EXIT_FAILURE);
    }
    write(fd1, &num1, sizeof(num1));
    write(fd1, &num2, sizeof(num2));
    close(fd1);

    int fd2 = open(FIFO2, O_WRONLY);
    if (fd2 == -1) {
        perror("Parent: Error opening FIFO2");
        exit(EXIT_FAILURE);
    }
    char command[] = "determine_larger";
    write(fd2, command, strlen(command) + 1);
    close(fd2);

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


    pid_t daemon_pid = fork();
    if (daemon_pid == 0) {
        daemon_process();
    }

    int counter = 0;
    while (counter < total_children) {
        printf("Proceeding...\n");
        sleep(2);
    }

    printf("All children have exited. Parent terminating.\n");
    return 0;
}
