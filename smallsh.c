#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <fcntl.h>
#include <signal.h>

// constants and necessary global variables
#define MAX_COMMAND_LENGTH 2048
#define MAX_ARGUMENTS 512
int lastForegroundStatus = 0;
pid_t foregroundChildPid = -1;
int allowBackground = 1;
int backgroundFlag = 0;
int backgroundTerminationInitiated = 0;

// Function prototypes
void sigintHandler(int signo);
void sigtstpHandler(int signo);
void setupSignalHandlers(int background);
char* prompt();
char** tokenizeCommand(char* command);
char* expand_pid(char *command, pid_t shellPID);
void handleStatus(int status);
void executeCommand(char** args, int background);
void freeTokenizedCommand(char** args);
void sigtermHandler(int signo);

// seting up necessary signals
void setupSignalHandlers(int background) {
    struct sigaction sa;

    // Set up the signal handler for SIGINT
    sa.sa_handler = sigintHandler;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);

    // Set up the signal handler for SIGTSTP
    sa.sa_handler = sigtstpHandler;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGTSTP, &sa, NULL);

    // Set up the signal handler for SIGTERM
    sa.sa_handler = sigtermHandler;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGTERM, &sa, NULL);

    backgroundFlag = background;
}

// asks for prompt. This uses a standard colon.
char* prompt() {
    printf(": ");
    fflush(stdout);
    char* command = malloc(MAX_COMMAND_LENGTH);  // Allocate dynamic memory
    fgets(command, MAX_COMMAND_LENGTH, stdin);
    if (strlen(command) > 0 && command[strlen(command) - 1] == '\n') {
        command[strlen(command) - 1] = '\0';  // Remove the newline character
    }
    return command;
}

// if starts with # or the line is blank, nothing happens
int handleBlankAndComment(char* command){
    if(command[0] == '#' || strlen(command) == 0){
        return 1;
    }
    return 0;
}

// spliting up the command into tokens so different arguments can be processed.
char** tokenizeCommand(char* command){
    char* token;
    char** args = malloc(MAX_ARGUMENTS * sizeof(char*));  // Allocate dynamic memory
    // handle memory allocation failure
    if (args == NULL) {
        perror("malloc");
        exit(1);
    }
    int argCount = 0;
    // handle spacing
    token = strtok(command, " ");
    while (token != NULL && argCount < MAX_ARGUMENTS - 1) {
        args[argCount++] = strdup(token);  // Allocate memory for each token
        token = strtok(NULL, " ");
    }
    args[argCount] = NULL;
    return args;
}

// expand $ symbols by replacing with the PID number
char* expand_pid(char *command, pid_t shellPID) {
    char *pid_str = malloc(10);  // Assuming the process ID can be represented in 10 characters
    sprintf(pid_str, "%d", shellPID);

    char *result = malloc(strlen(command) + 1);  // Allocate memory for the result
    if (result == NULL) {
        perror("malloc");
        exit(1);
    }

    char *pos = strstr(command, "$$");
    if (pos != NULL) {
        // Copy the part of the command before $$
        strncpy(result, command, pos - command);
        result[pos - command] = '\0';  // Null-terminate the string

        // Concatenate the PID string
        strcat(result, pid_str);

        // Concatenate the rest of the command
        strcat(result, pos + 2);
    } else {
        // If $$ is not found, just copy the original command
        strcpy(result, command);
    }

    free(pid_str);

    return result;
}

// handle the status command with either the exit status or last termination signal
void handleStatus(int status){
    // Print the exit status if the process terminated normally
    if (WIFEXITED(status)) {
        printf("exit value %d\n", WEXITSTATUS(status));
    }
    // Print the terminating signal if the process was terminated by a signal
    else if (WIFSIGNALED(status)) {
        printf("terminated by signal %d\n", WTERMSIG(status));
    }
}

// kill child process with SIGINT
void sigintHandler(int signo) {
    if (signo == SIGINT) {
        // Do nothing in the parent and background processes

        // In the foreground child process, terminate itself
        if (getpid() == foregroundChildPid) {
            exit(2);  // Use a different exit status to indicate termination by SIGINT
        }
    }
}

// Toggle between foreground and background processes with SIGTSTP
void sigtstpHandler(int signo) {
    if (signo == SIGTSTP) {
        // Ignore if signal is received by the shell itself
        if (getpid() != 0) {
            // Toggle between foreground and background modes
            allowBackground = (allowBackground == 1) ? 0 : 1;

            // Informative message about the mode change
            if (allowBackground) {
                printf("\nExiting foreground-only mode\n");
                fflush(stdout);
            } else {
                // If entering foreground-only mode, block background processes
                printf("\nEntering foreground-only mode (& is now ignored)\n");
                fflush(stdout);
                // Check if there's a background process and wait for it to finish
                pid_t terminatedChild;
                int childStatus;
                while ((terminatedChild = waitpid(-1, &childStatus, WNOHANG)) > 0) {
                    printf("background pid %d is done: ", terminatedChild);
                    handleStatus(childStatus);
                }
            }
        } else {
            // If the signal is received by the shell itself, print a message
            printf("\nForeground-only mode cannot be applied to the shell process\n");
            fflush(stdout);
        }
    }
}

// handles SIGTERM signal
// In the sigtermHandler function
void sigtermHandler(int signo) {
    if (signo == SIGTERM) {
        int background = backgroundFlag;
        // Terminate background processes with "sleep" command
        if (background && !backgroundTerminationInitiated) {
            backgroundTerminationInitiated = 1;  // Set the flag to prevent further initiation
            fflush(stdout);
            pid_t myPGID = getpgid(0);  // Get the process group ID of the shell

            // Execute the 'pkill' command only for processes that are still running
            char pgid_str[20];
            sprintf(pgid_str, "-%d", myPGID);

            // Use execlp to run the 'pkill' command
            if (execlp("pkill", "pkill", "-SIGTERM", "-g", pgid_str, NULL) == -1) {
                perror("execlp");
                exit(1);
            }
        }
    }
}


// execute command for child process
void executeCommand(char** args, int background) {
    pid_t childPid = fork();
    // handle fork failure
    if (childPid == -1) {
        perror("fork");
        exit(1);
    } else if (childPid == 0) {
        // Child process

        // Set up signal handler for SIGINT in the child
        signal(SIGINT, SIG_DFL);

        // Check for input redirection
        int i = 0;
        int inputRedirectionIndex = -1;
        while (args[i] != NULL) {
            if (strcmp(args[i], "<") == 0) {
                // handle input redirection
                inputRedirectionIndex = i;
                break;
            }
            i++;
        }

        if (inputRedirectionIndex != -1) {
            // Open the file for reading
            int fileDescriptor = open(args[inputRedirectionIndex + 1], O_RDONLY);
            if (fileDescriptor == -1) {
                perror("open");
                exit(1);
            }

            // Redirect stdin to the file
            if (dup2(fileDescriptor, STDIN_FILENO) == -1) {
                perror("dup2");
                exit(1);
            }

            // Close the file descriptor as it's no longer needed
            close(fileDescriptor);

            // Shift the arguments to remove the "<" and its argument
            int j = inputRedirectionIndex;
            while (args[j] != NULL) {
                args[j] = args[j + 2];
                j++;
            }
        }

        // Check for output redirection
        i = 0;
        int outputRedirectionIndex = -1;
        while (args[i] != NULL) {
            if (strcmp(args[i], ">") == 0) {
                // handle output redirection
                outputRedirectionIndex = i;
                break;
            }
            i++;
        }

        if (outputRedirectionIndex != -1) {
            // Open the file for writing, creating it if it doesn't exist, or truncating it if it does
            int fileDescriptor = open(args[outputRedirectionIndex + 1], O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (fileDescriptor == -1) {
                perror("open");
                exit(1);
            }

            // Redirect stdout to the file
            if (dup2(fileDescriptor, STDOUT_FILENO) == -1) {
                perror("dup2");
                exit(1);
            }

            // Close the file descriptor as it's no longer needed
            close(fileDescriptor);

            // Shift the arguments to remove the ">" and its argument
            int j = outputRedirectionIndex;
            while (args[j] != NULL) {
                args[j] = args[j + 2];
                j++;
            }
        }

        // Execute the command
        execvp(args[0], args);
        // If execvp fails, print an error message and exit
        perror("execvp");
        exit(1);
    } else {
        // Parent process
        if (!background) {
            // If it's a foreground process, set foregroundChildPid
            foregroundChildPid = childPid;
            // Wait for the child to finish
            int childStatus;
            waitpid(childPid, &childStatus, 0);

            // Check if the command was test -f and adjust the exit status
            if (WIFEXITED(childStatus) && WEXITSTATUS(childStatus) == 1) {
                printf("exit value 1\n");
            } else {
                lastForegroundStatus = childStatus;
            }

            foregroundChildPid = -1;  // Reset foregroundChildPid
        } else {
            // If it's a background process, print its process ID
            printf("background pid is %d\n", childPid);
        }
        freeTokenizedCommand(args);
    }
}

// free memory for all tokens of the command
void freeTokenizedCommand(char** args) {
    for (int i = 0; args[i] != NULL; ++i) {
        free(args[i]);
    }
    free(args);
}

// driver function for program
void main() {
    int background = 0;
    setupSignalHandlers(background);
    pid_t shellPID = getpid();
    pid_t terminatedChild;
    int childStatus;
    // continue running until "exit" is prompted
    while (1) {
        char* command = prompt();
        if (handleBlankAndComment(command)) {
            continue;
        }
        char** args = tokenizeCommand(command);
        command = expand_pid(command, shellPID);
        // Check for background execution
        if (args != NULL && args[0] != NULL) {
            int i = 0;
            while (args[i] != NULL) {
                i++;
            }
            if (i > 0 && strcmp(args[i - 1], "&") == 0) {
                background = 1;
                args[i - 1] = NULL; // Remove the "&" from arguments
            } else {
                background = 0;
            }
        }
        // handle exit command
        if (strcmp(args[0], "exit") == 0) {
            exit(0);
        // handle cd command
        } else if (strcmp(args[0], "cd") == 0) {
            // Check if the command has an argument for the directory
            if (args[1] == NULL) {
                // Change to the home directory
                chdir(getenv("HOME"));
            } else {
                // Change to the specified directory
                if (chdir(args[1]) != 0) {
                    perror("cd"); // Print an error message if chdir fails
                }
            }
        // handle status command
        } else if (strcmp(args[0], "status") == 0) {
            // Check if any foreground process has been executed
            if (lastForegroundStatus == -1) {
                // No foreground process has been executed
                printf("exit value 0\n");
            } else {
                // Print the status of the last foreground process
                handleStatus(lastForegroundStatus);
            }
        // handle unknown command
        } else {
            pid_t childPid = fork();
            if (childPid == -1) {
                perror("fork");
                exit(1);
            } else if (childPid == 0) {
                // Child process
                // Check for input/output redirection and execute command
                executeCommand(args, background);
                exit(0);
            } else {
                // Parent process
                if (!background) {
                    // If it's a foreground process, set foregroundChildPid
                    foregroundChildPid = childPid;
                    // Wait for the child to finish
                    int childStatus;
                    waitpid(childPid, &childStatus, 0);

                    // Check if the command was test -f and adjust the exit status
                    if (WIFEXITED(childStatus) && WEXITSTATUS(childStatus) == 1) {
                        printf("exit value 1\n");
                    } else {
                        lastForegroundStatus = childStatus;
                    }

                    foregroundChildPid = -1;  // Reset foregroundChildPid
                    } else {
                        // If it's a background process, print its process ID only in the parent process
                        printf("background pid is %d\n", childPid);
                        fflush(stdout);
                        // Continue with the loop to check for terminated background processes
                        while ((terminatedChild = waitpid(-1, &childStatus, WNOHANG)) > 0) {
                            printf("background pid %d is done: ", terminatedChild);
                            handleStatus(childStatus);
                        }
                    }
                backgroundTerminationInitiated = 0;
                freeTokenizedCommand(args);
            }
        }
    }
}