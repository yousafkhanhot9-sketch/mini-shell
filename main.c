#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <limits.h>

#define MAX_LINE 1024
#define MAX_ARGS 100
#define HISTORY_SIZE 100

static char history[HISTORY_SIZE][MAX_LINE];
static int history_count = 0;

static void reap_children(int sig) {
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0) {}
}

static void add_history(const char *line) {
    if (!line[0]) return;
    if (history_count < HISTORY_SIZE) {
        snprintf(history[history_count++], MAX_LINE, "%s", line);
    } else {
        memmove(history, history + 1, (HISTORY_SIZE - 1) * MAX_LINE);
        snprintf(history[HISTORY_SIZE - 1], MAX_LINE, "%s", line);
    }
}

static void show_help(void) {
    puts("Built-ins: cd [dir], pwd, help, history, exit");
    puts("Features: commands, command > file, command >> file, command < file");
    puts("          command1 | command2, command &");
}

static int builtin(char **args) {
    if (!args[0]) return 1;
    if (strcmp(args[0], "cd") == 0) {
        const char *dir = args[1] ? args[1] : getenv("HOME");
        if (!dir || chdir(dir) < 0) perror("cd");
        return 1;
    }
    if (strcmp(args[0], "pwd") == 0) {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd))) puts(cwd);
        else perror("pwd");
        return 1;
    }
    if (strcmp(args[0], "help") == 0) { show_help(); return 1; }
    if (strcmp(args[0], "history") == 0) {
        for (int i = 0; i < history_count; i++)
            printf("%d  %s\n", i + 1, history[i]);
        return 1;
    }
    if (strcmp(args[0], "exit") == 0) exit(0);
    return 0;
}

/* Splits a command into arguments and handles <, > and >>. */
static int parse_command(char *text, char **args,
                         char **input_file, char **output_file,
                         int *append) {
    int n = 0;
    *input_file = NULL;
    *output_file = NULL;
    *append = 0;

    char *save = NULL;
    for (char *tok = strtok_r(text, " \t\r\n", &save);
         tok && n < MAX_ARGS - 1;
         tok = strtok_r(NULL, " \t\r\n", &save)) {
        if (strcmp(tok, "<") == 0) {
            tok = strtok_r(NULL, " \t\r\n", &save);
            if (!tok) { fprintf(stderr, "Missing input filename\n"); return -1; }
            *input_file = tok;
        } else if (strcmp(tok, ">") == 0 || strcmp(tok, ">>") == 0) {
            *append = (strcmp(tok, ">>") == 0);
            tok = strtok_r(NULL, " \t\r\n", &save);
            if (!tok) { fprintf(stderr, "Missing output filename\n"); return -1; }
            *output_file = tok;
        } else {
            args[n++] = tok;
        }
    }
    args[n] = NULL;
    return n;
}

static int apply_redirection(char *in, char *out, int append) {
    if (in) {
        int fd = open(in, O_RDONLY);
        if (fd < 0) { perror(in); return -1; }
        if (dup2(fd, STDIN_FILENO) < 0) { perror("dup2"); close(fd); return -1; }
        close(fd);
    }
    if (out) {
        int flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
        int fd = open(out, flags, 0644);
        if (fd < 0) { perror(out); return -1; }
        if (dup2(fd, STDOUT_FILENO) < 0) { perror("dup2"); close(fd); return -1; }
        close(fd);
    }
    return 0;
}

static void execute_child(char *command, int background,
                          int input_fd, int output_fd) {
    char *args[MAX_ARGS], *in, *out;
    int append;
    int count = parse_command(command, args, &in, &out, &append);
    if (count <= 0) _exit(count < 0 ? 1 : 0);

    signal(SIGINT, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);

    if (input_fd != STDIN_FILENO && dup2(input_fd, STDIN_FILENO) < 0) {
        perror("dup2"); _exit(1);
    }
    if (output_fd != STDOUT_FILENO && dup2(output_fd, STDOUT_FILENO) < 0) {
        perror("dup2"); _exit(1);
    }
    if (apply_redirection(in, out, append) < 0) _exit(1);

    if (builtin(args)) _exit(0);
    execvp(args[0], args);
    fprintf(stderr, "myshell: %s: %s\n", args[0], strerror(errno));
    _exit(127);
}

static void run_line(char *line) {
    int background = 0;
    size_t len = strlen(line);
    while (len && (line[len-1] == '\n' || line[len-1] == ' ' || line[len-1] == '\t'))
        line[--len] = '\0';
    if (!len) return;

    if (line[len-1] == '&') {
        background = 1;
        line[--len] = '\0';
    }

    char *pipe_pos = strchr(line, '|');
    if (pipe_pos) {
        *pipe_pos = '\0';
        char *left = line;
        char *right = pipe_pos + 1;
        int fds[2];
        if (pipe(fds) < 0) { perror("pipe"); return; }

        pid_t p1 = fork();
        if (p1 == 0) {
            close(fds[0]);
            execute_child(left, background, STDIN_FILENO, fds[1]);
        }
        if (p1 < 0) { perror("fork"); close(fds[0]); close(fds[1]); return; }

        pid_t p2 = fork();
        if (p2 == 0) {
            close(fds[1]);
            execute_child(right, background, fds[0], STDOUT_FILENO);
        }
        close(fds[0]); close(fds[1]);
        if (p2 < 0) { perror("fork"); return; }
        if (!background) { waitpid(p1, NULL, 0); waitpid(p2, NULL, 0); }
        return;
    }

    char copy[MAX_LINE];
    snprintf(copy, sizeof(copy), "%s", line);
    char *args[MAX_ARGS], *in, *out;
    int append;
    int count = parse_command(copy, args, &in, &out, &append);
    if (count <= 0) return;

    /* Run simple built-ins in the parent so cd changes this shell. */
    if (!background && !in && !out && builtin(args)) return;

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return; }
    if (pid == 0) execute_child(line, background, STDIN_FILENO, STDOUT_FILENO);
    if (!background) waitpid(pid, NULL, 0);
}

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = reap_children;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    signal(SIGINT, SIG_IGN); /* Ctrl+C should not terminate the shell. */

    char line[MAX_LINE];
    while (1) {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, "?");
        printf("myshell:%s$ ", cwd);
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) {
            putchar('\n');
            break;
        }
        line[strcspn(line, "\n")] = '\0';
        if (!line[0]) continue;
        add_history(line);
        run_line(line);
    }
    return 0;
}
