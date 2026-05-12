#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <string.h>
#include <stat.h>

#define CACTSOLE_PATH "/bin/cactsole"
#define RESCUE_PATH   "/bin/cactsole-rescue"
#define CONFIG_PATH   "/etc/cgoct.conf"
#define LOG_PATH      "/var/log/cgoct.log"
#define TTY_PATH      "/dev/tty"

/* Default config written on first boot when /etc/cgoct.conf is missing. */
static const char default_config[] =
    "# cgoct supervisor config — auto-generated on first boot.\n"
    "# restart_policy : always | on-failure | once\n"
    "# rescue_shell   : 0 | 1   (try /bin/cactsole-rescue on crash-loop)\n"
    "# crash_limit    : 1..20   (fast-crash bursts before cooldown)\n"
    "# cooldown_sec   : 1..120  (pause after crash-loop)\n"
    "restart_policy=always\n"
    "rescue_shell=1\n"
    "crash_limit=4\n"
    "cooldown_sec=8\n";

static char *cactsole_argv[] = { "cactsole", NULL };
static char *rescue_argv[]   = { "cactsole-rescue", "--safe-mode", NULL };
static char *cactsole_envp[] = { "PATH=/bin:/sbin", "HOME=/", NULL };
static int log_fd = -1;

/* Supervisor tuning to avoid fast crash loops. */
#define FAST_CRASH_SEC        3
#define FAST_CRASH_LIMIT      4
#define RESTART_DELAY_MIN_SEC 1
#define RESTART_DELAY_MAX_SEC 10
#define COOLDOWN_SEC          8
#define CONFIG_BUF_SIZE       512

enum restart_policy {
    POLICY_ALWAYS = 0,
    POLICY_ON_FAILURE = 1,
    POLICY_ONCE = 2,
};

struct supervisor_cfg {
    enum restart_policy policy;
    int use_rescue_shell;
    int crash_limit;
    int cooldown_sec;
};

static long now_sec(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return ts.tv_sec;
    }
    return 0;
}

static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static int write_all(int fd, const char *data, int len) {
    int total = 0;
    while (total < len) {
        int n = (int)write(fd, data + total, (size_t)(len - total));
        if (n <= 0) return -1;
        total += n;
    }
    return 0;
}

static void prepare_files(void) {
    if (!file_exists(CONFIG_PATH)) {
        int fd = open(CONFIG_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            (void)write_all(fd, default_config, (int)sizeof(default_config) - 1);
            close(fd);
        }
    }
}

static int setup_console(void) {
    int fd = open(TTY_PATH, O_RDWR);
    if (fd < 0) {
        kprint("cgoct: warning: open(/dev/tty) failed\n");
        return -1;
    }
    if (fd != 0) { dup2(fd, 0); }
    dup2(fd, 1);
    dup2(fd, 2);
    if (fd > 2) close(fd);
    return 0;
}

static void setup_log(void) {
    log_fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log_fd < 0) {
        kprint("cgoct: warning: log file unavailable\n");
    }
}

static void log_text(const char *s) {
    if (log_fd >= 0) {
        write(log_fd, s, (size_t)strlen(s));
    }
}

static const char *policy_name(enum restart_policy policy) {
    if (policy == POLICY_ON_FAILURE) return "on-failure";
    if (policy == POLICY_ONCE) return "once";
    return "always";
}

static const char *enabled_name(int enabled) {
    return enabled ? "enabled" : "disabled";
}

static int status_is_success(int status) {
    return status == 0;
}

static int status_exit_code(int status) {
    return status;
}

static void cfg_defaults(struct supervisor_cfg *cfg) {
    cfg->policy = POLICY_ALWAYS;
    cfg->use_rescue_shell = 1;
    cfg->crash_limit = FAST_CRASH_LIMIT;
    cfg->cooldown_sec = COOLDOWN_SEC;
}

static void parse_config_line(struct supervisor_cfg *cfg, char *line) {
    int i = 0;
    int eq = -1;

    while (line[i] != '\0') {
        if (line[i] == '=') {
            eq = i;
            break;
        }
        i++;
    }

    if (eq <= 0) return;

    line[eq] = '\0';
    char *key = line;
    char *value = &line[eq + 1];

    if (strcmp(key, "restart_policy") == 0) {
        if (strcmp(value, "always") == 0) cfg->policy = POLICY_ALWAYS;
        else if (strcmp(value, "on-failure") == 0) cfg->policy = POLICY_ON_FAILURE;
        else if (strcmp(value, "once") == 0) cfg->policy = POLICY_ONCE;
    } else if (strcmp(key, "rescue_shell") == 0) {
        cfg->use_rescue_shell = atoi(value) ? 1 : 0;
    } else if (strcmp(key, "crash_limit") == 0) {
        int n = atoi(value);
        if (n > 0 && n <= 20) cfg->crash_limit = n;
    } else if (strcmp(key, "cooldown_sec") == 0) {
        int n = atoi(value);
        if (n >= 1 && n <= 120) cfg->cooldown_sec = n;
    }
}

static void load_config(struct supervisor_cfg *cfg) {
    char buf[CONFIG_BUF_SIZE];
    int fd = open(CONFIG_PATH, O_RDONLY);
    int nread, i, line_start;

    cfg_defaults(cfg);

    if (fd < 0) {
        kprint("cgoct: config missing, using defaults\n");
        return;
    }

    nread = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (nread <= 0) {
        kprint("cgoct: config empty, using defaults\n");
        return;
    }

    buf[nread] = '\0';
    line_start = 0;
    for (i = 0; i <= nread; i++) {
        if (buf[i] == '\n' || buf[i] == '\0') {
            buf[i] = '\0';
            if (buf[line_start] != '\0' && buf[line_start] != '#') {
                parse_config_line(cfg, &buf[line_start]);
            }
            line_start = i + 1;
        }
    }
}

static pid_t spawn_process(const char *path, char *argv[]) {
    pid_t pid = fork();
    if (pid == 0) {
        execve(path, (char *const *)argv, (char *const *)cactsole_envp);
        log_text("cgoct: execve failed\n");
        _exit(1);
    }
    if (pid < 0) {
        log_text("cgoct: fork failed\n");
    }
    return pid;
}

int main(void) {
    struct supervisor_cfg cfg;
    int restart_delay = RESTART_DELAY_MIN_SEC;
    int fast_crash_count = 0;
    int try_rescue_next = 0;

    prepare_files();
    load_config(&cfg);
    setup_log();
    if (setup_console() < 0) {
        log_text("cgoct: booting without tty, will retry later\n");
    }
    printf("cgoct: supervisor online\n");
    printf("  restart policy : %s\n", policy_name(cfg.policy));
    printf("  rescue shell   : %s\n", enabled_name(cfg.use_rescue_shell));
    printf("  crash limit    : %d\n", cfg.crash_limit);
    printf("  cooldown       : %d sec\n", cfg.cooldown_sec);
    log_text("cgoct: supervisor online\n");

    while (1) {
        const char *spawn_path = CACTSOLE_PATH;
        char **spawn_argv = cactsole_argv;
        char *spawn_name = cactsole_argv[0];

        /* Re-bind stdio to tty every supervisor loop iteration. */
        setup_console();

        if (try_rescue_next && cfg.use_rescue_shell) {
            if (file_exists(RESCUE_PATH)) {
                spawn_path = RESCUE_PATH;
                spawn_argv = rescue_argv;
                spawn_name = rescue_argv[0];
            } else {
                log_text("cgoct: rescue shell missing, fallback to /bin/cactsole\n");
                try_rescue_next = 0;
            }
        }

        long started_at = now_sec();
        pid_t pid = spawn_process(spawn_path, spawn_argv);

        if (pid <= 0) {
            if (setup_console() == 0) {
                log_text("cgoct: tty recovered\n");
            }
            if (restart_delay < RESTART_DELAY_MAX_SEC) {
                restart_delay <<= 1;
                if (restart_delay > RESTART_DELAY_MAX_SEC) {
                    restart_delay = RESTART_DELAY_MAX_SEC;
                }
            }
            printf("cgoct: spawn failed: %s (retry in %d sec)\n", spawn_name, restart_delay);
            try_rescue_next = 1;
            sleep((unsigned int)restart_delay);
            continue;
        }

        if (pid > 0) {
            int status;
            waitpid(pid, &status, 0);
            int exit_code = status_exit_code(status);

            long lived_sec = now_sec() - started_at;
            if (status_is_success(status)) {
                /* User requested shell exit: do not treat as crash-loop. */
                fast_crash_count = 0;
                restart_delay = 0;
                try_rescue_next = 0;
                printf("cgoct: %s exited cleanly after %d sec (exit=%d)\n",
                       spawn_name, (int)lived_sec, exit_code);
            } else if (lived_sec <= FAST_CRASH_SEC) {
                fast_crash_count++;
                printf("cgoct: %s crashed after %d sec (burst=%d, exit=%d)\n",
                       spawn_name, (int)lived_sec, fast_crash_count, exit_code);
            } else {
                fast_crash_count = 0;
                restart_delay = RESTART_DELAY_MIN_SEC;
                try_rescue_next = 0;
                printf("cgoct: %s exited after %d sec (exit=%d)\n",
                       spawn_name, (int)lived_sec, exit_code);
            }

            if (cfg.policy == POLICY_ONCE) {
                log_text("cgoct: restart_policy=once, stopping restarts\n");
                break;
            }

            if (cfg.policy == POLICY_ON_FAILURE && status_is_success(status)) {
                log_text("cgoct: clean exit with restart_policy=on-failure\n");
                break;
            }

            if (fast_crash_count >= cfg.crash_limit) {
                printf("cgoct: crash-loop detected (cooldown=%d sec)\n", cfg.cooldown_sec);
                sleep((unsigned int)cfg.cooldown_sec);
                fast_crash_count = 0;
                restart_delay = RESTART_DELAY_MAX_SEC / 2;
                try_rescue_next = 1;
            } else {
                if (restart_delay <= 0) {
                    printf("cgoct: restarting now\n");
                } else {
                    printf("cgoct: restarting in %d sec\n", restart_delay);
                    sleep((unsigned int)restart_delay);
                    if (restart_delay < RESTART_DELAY_MAX_SEC) {
                        restart_delay++;
                    }
                }
            }
        }
    }

    if (log_fd >= 0) {
        close(log_fd);
    }
    return 0;
}
