/* watchdog.c — restart subzeroclaw on crash, backoff on repeated failures */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <time.h>

#define MAX_BACKOFF 60

int main(int argc, char **argv) {
    const char *bin = argc > 1 ? argv[1] : "/usr/local/bin/subzeroclaw";
    int backoff = 1;

    for (;;) {
        time_t start = time(NULL);
        fprintf(stderr, "[watchdog] starting %s\n", bin);

        pid_t pid = fork();
        if (pid == 0) {
            /* child: pass remaining args to subzeroclaw */
            char **args = calloc(argc, sizeof(char *));
            args[0] = (char *)bin;
            for (int i = 2; i < argc; i++) args[i - 1] = argv[i];
            args[argc - 1] = NULL;
            execv(bin, args);
            perror("execv");
            _exit(127);
        }
        if (pid < 0) { perror("fork"); sleep(backoff); continue; }

        int status;
        waitpid(pid, &status, 0);

        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            fprintf(stderr, "[watchdog] clean exit, done\n");
            return 0;
        }

        time_t uptime = time(NULL) - start;
        if (uptime > 30) backoff = 1;  /* ran long enough, reset */
        else if (backoff < MAX_BACKOFF) backoff *= 2;

        if (WIFSIGNALED(status))
            fprintf(stderr, "[watchdog] killed by signal %d", WTERMSIG(status));
        else
            fprintf(stderr, "[watchdog] exit code %d", WEXITSTATUS(status));
        fprintf(stderr, ", restarting in %ds\n", backoff);
        sleep(backoff);
    }
}
