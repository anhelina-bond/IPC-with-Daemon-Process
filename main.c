#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <string.h>

#define FIFO1 "fifo1"
#define FIFO2 "fifo2"

int counter = 0;

// SIGCHLD handler
void sigchld_handler(int signo) {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        printf("Child process %d terminated.\n", pid);
        counter += 2;  // Increment by two as per assignment requirement
    }
    if (counter >= 2) {
        printf("All child processes finished, exiting.\n");
        exit(0);
    }
}

// Signal handlers for the daemon process
void handle_signal(int signo) {
    switch (signo) {
        case SIGUSR1:
            printf("Received SIGUSR1: Logging event.\n");
            break;
        case SIGTERM:
            printf("Received SIGTERM: Terminating daemon.\n");
            exit(0);
            break;
        case SIGHUP:
            printf("Received SIGHUP: Reloading configuration.\n");
            break;
    }
}

void daemonize() {
    pid_t pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS);

    umask(0);
    setsid();
    chdir("/");
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <num1> <num2>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    
    int num1 = atoi(argv[1]);
    int num2 = atoi(argv[2]);
    int result = 0;
    
    signal(SIGCHLD, sigchld_handler);
    
    mkfifo(FIFO1, 0666);
    mkfifo(FIFO2, 0666);
    
    pid_t child1 = fork();
    if (child1 == 0) {
        int fd = open(FIFO1, O_WRONLY);
        write(fd, &num1, sizeof(int));
        write(fd, &num2, sizeof(int));
        close(fd);
        exit(0);
    }
    
    pid_t child2 = fork();
    if (child2 == 0) {
        int fd = open(FIFO1, O_RDONLY);
        int a, b;
        read(fd, &a, sizeof(int));
        read(fd, &b, sizeof(int));
        close(fd);
        int larger = (a > b) ? a : b;
        fd = open(FIFO2, O_WRONLY);
        write(fd, &larger, sizeof(int));
        close(fd);
        exit(0);
    }
    
    daemonize();
    signal(SIGUSR1, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGHUP, handle_signal);
    
    int fd = open(FIFO2, O_RDONLY);
    read(fd, &result, sizeof(int));
    close(fd);
    printf("Larger number is: %d\n", result);
    
    while (1) {
        sleep(2);
        printf("Proceeding...\n");
    }
    return 0;
}
