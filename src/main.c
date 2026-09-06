#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <string.h>
#include <stat.h>
#include <signal.h>

#define CACTSOLE_PATH "/bin/cactsole"
#define RESCUE_PATH   "/bin/cactsole-rescue"
#define DAEMON_DIR    "/sbin"
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
    "# services       : opt-in — daemons from /sbin to start before shell.\n"
    "#                  По умолчанию пусто; список выберет пользователь (утилита).\n"
    "restart_policy=always\n"
    "rescue_shell=1\n"
    "crash_limit=4\n"
    "cooldown_sec=8\n"
    "# services=logd devd netd powerd quirkd resolved seatd audiod wifid\n";

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
#define CONFIG_BUF_SIZE       1024
#define IDLE_MS               1000
#define MIN_SLEEP_SLICE_MS    50

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

/* ── Отслеживаемые фоновые демоны (/sbin/<name>) ─────────────────────── */

#define SVC_MAX     12
#define SVC_NAME_SZ 32
#define SVC_PATH_SZ 64

struct svc {
    char  name[SVC_NAME_SZ];
    char  path[SVC_PATH_SZ];
    pid_t pid;          /* 0 = не запущен */
    long  next_start_ms;/* монотонное время, когда можно поднимать снова */
    long  started_ms;   /* монотонное время старта последнего инстанса */
    unsigned burst;     /* подряд быстрых падений */
};

static struct svc services[SVC_MAX];
static int services_n = 0;

/* ── Монотонное время ────────────────────────────────────────────────── */

static long now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
    }
    return 0;
}

/* ── Утилиты ─────────────────────────────────────────────────────────── */

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

static void cfg_defaults(struct supervisor_cfg *cfg) {
    cfg->policy = POLICY_ALWAYS;
    cfg->use_rescue_shell = 1;
    cfg->crash_limit = FAST_CRASH_LIMIT;
    cfg->cooldown_sec = COOLDOWN_SEC;
}

/* ── Список демонов по умолчанию ─────────────────────────────────────── */

/* Список по умолчанию закомментирован: демоны включаются только ключом
 * services в /etc/cgoct.conf — выбор делает пользователь (отдельная утилита). */
#if 0
static const char *default_service_names[] = {
    "logd", "devd", "netd", "powerd", "quirkd",
    "resolved", "seatd", "audiod", "wifid",
};
#endif

static void add_service_name(const char *name) {
    if (services_n >= SVC_MAX) return;
    if (name[0] == '\0') return;

    int i;
    for (i = 0; i < services_n; i++) {
        if (strcmp(services[i].name, name) == 0) return; /* уже есть */
    }

    struct svc *s = &services[services_n];
    memset(s, 0, sizeof(*s));

    if (name[0] == '/') {
        /* Полный путь. */
        strncpy(s->path, name, SVC_PATH_SZ - 1);
        const char *slash = strrchr(name, '/');
        const char *base = slash ? slash + 1 : name;
        strncpy(s->name, base, SVC_NAME_SZ - 1);
    } else {
        strncpy(s->name, name, SVC_NAME_SZ - 1);
        snprintf(s->path, SVC_PATH_SZ, "%s/%s", DAEMON_DIR, name);
    }
    s->name[SVC_NAME_SZ - 1] = '\0';
    s->path[SVC_PATH_SZ - 1] = '\0';
    services_n++;
}

static void services_defaults(void) {
    services_n = 0;
#if 0
    size_t i;
    for (i = 0; i < sizeof(default_service_names) / sizeof(default_service_names[0]); i++) {
        add_service_name(default_service_names[i]);
    }
#endif
}

/* Добавить имена из строки-значения (разделители: пробел/запятая/таб). */
static void parse_service_names(const char *val) {
    char buf[CONFIG_BUF_SIZE];
    strncpy(buf, val, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *save = NULL;
    char *tok = strtok_r(buf, " ,\t", &save);
    while (tok) {
        add_service_name(tok);
        tok = strtok_r(NULL, " ,\t", &save);
    }
}

/* ── Конфиг ──────────────────────────────────────────────────────────── */

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
    } else if (strcmp(key, "services") == 0) {
        /* Полная замена списка по умолчанию. */
        services_n = 0;
        parse_service_names(value);
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

/* ── Запуск процессов ────────────────────────────────────────────────── */

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

/* Ожидание до deadline (или до idle_ms), но короткими порциями, чтобы
 * оперативно реагировать на падения детей. */
static void sleep_slices(long ms) {
    if (ms <= 0) return;
    if (ms > IDLE_MS) ms = IDLE_MS;
    while (ms > 0) {
        long slice = ms;
        if (slice > 200) slice = 200;
        usleep((unsigned int)(slice * 1000L));
        ms -= slice;
    }
}

int main(void) {
    struct supervisor_cfg cfg;
    long now = now_ms();

    /* Состояние шелла. */
    pid_t shell_pid = 0;
    long  shell_started_ms = 0;
    long  shell_next_start = 0;
    int   fast_crash_count = 0;
    int   restart_delay = RESTART_DELAY_MIN_SEC;
    int   try_rescue_next = 0;

    prepare_files();
    services_defaults();
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
    if (services_n > 0) {
        printf("  services       :");
        {
            int i;
            for (i = 0; i < services_n; i++) printf(" %s", services[i].name);
        }
        printf("\n");
    }
    log_text("cgoct: supervisor online\n");

    while (1) {
        now = now_ms();

        /* Re-bind stdio to tty every supervisor loop iteration. */
        setup_console();

        /* ── 1. Поднять всех демонов, чей срок подошёл ── */
        {
            int i;
            for (i = 0; i < services_n; i++) {
                struct svc *s = &services[i];
                char *args[2];

                if (s->pid > 0) continue;
                if (now < s->next_start_ms) continue;
                if (!file_exists(s->path)) {
                    /* Бинарь не установлен — молча пропускаем, не спамя. */
                    s->next_start_ms = now + 30000;
                    continue;
                }

                args[0] = s->name;
                args[1] = NULL;
                pid_t pid = spawn_process(s->path, args);
                if (pid <= 0) {
                    printf("cgoct: spawn failed: %s (retry in 3s)\n", s->name);
                    s->next_start_ms = now_ms() + 3000;
                } else {
                    s->pid = pid;
                    s->started_ms = now_ms();
                    printf("cgoct: started %s (pid=%d)\n", s->name, (int)pid);
                    log_text("cgoct: started service\n");
                }
            }
        }

        /* ── 2. Поднять шелл, если пора ── */
        if (shell_pid <= 0 && now >= shell_next_start) {
            const char *spawn_path = CACTSOLE_PATH;
            char **spawn_argv = cactsole_argv;
            char *spawn_name = cactsole_argv[0];

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

            pid_t pid = spawn_process(spawn_path, spawn_argv);
            if (pid > 0) {
                shell_pid = pid;
                shell_started_ms = now_ms();
                if (try_rescue_next) {
                    try_rescue_next = 0;
                    printf("cgoct: rescue shell started (pid=%d)\n", (int)pid);
                } else {
                    printf("cgoct: shell started (pid=%d)\n", (int)pid);
                }
            } else {
                if (setup_console() == 0) {
                    log_text("cgoct: tty recovered\n");
                }
                if (restart_delay < RESTART_DELAY_MAX_SEC) restart_delay++;
                printf("cgoct: spawn failed: %s (retry in %d sec)\n",
                       spawn_name, restart_delay);
                try_rescue_next = 1;
                shell_next_start = now_ms() + (long)restart_delay * 1000L;
            }
        }

        /* ── 3. Забрать завершившихся детей (shell и демоны) ── */
        for (;;) {
            int status = 0;
            pid_t done = waitpid(-1, &status, WNOHANG);
            if (done <= 0) break;

            long lived_ms = now_ms() - shell_started_ms;
            int  matched_shell = (shell_pid > 0 && done == shell_pid);

            if (matched_shell) {
                int exit_code = status;
                shell_pid = 0;

                if (status_is_success(status)) {
                    fast_crash_count = 0;
                    restart_delay = 0;
                    printf("cgoct: shell exited cleanly after %d ms (exit=%d)\n",
                           (int)lived_ms, exit_code);
                } else if (lived_ms <= FAST_CRASH_SEC * 1000L) {
                    fast_crash_count++;
                    printf("cgoct: shell crashed after %d ms (burst=%d, exit=%d)\n",
                           (int)lived_ms, fast_crash_count, exit_code);
                } else {
                    fast_crash_count = 0;
                    restart_delay = RESTART_DELAY_MIN_SEC;
                    printf("cgoct: shell exited after %d ms (exit=%d)\n",
                           (int)lived_ms, exit_code);
                }

                if (cfg.policy == POLICY_ONCE) {
                    log_text("cgoct: restart_policy=once, stopping restarts\n");
                    goto supervisor_exit;
                }

                if (cfg.policy == POLICY_ON_FAILURE && status_is_success(status)) {
                    log_text("cgoct: clean exit with restart_policy=on-failure\n");
                    goto supervisor_exit;
                }

                if (fast_crash_count >= cfg.crash_limit) {
                    printf("cgoct: shell crash-loop detected (cooldown=%d sec)\n",
                           cfg.cooldown_sec);
                    log_text("cgoct: shell crash-loop cooldown\n");
                    fast_crash_count = 0;
                    restart_delay = RESTART_DELAY_MAX_SEC / 2;
                    try_rescue_next = 1;
                    shell_next_start = now_ms() + (long)cfg.cooldown_sec * 1000L;
                } else if (restart_delay <= 0) {
                    printf("cgoct: restarting shell now\n");
                    shell_next_start = now_ms();
                } else {
                    printf("cgoct: restarting shell in %d sec\n", restart_delay);
                    shell_next_start = now_ms() + (long)restart_delay * 1000L;
                    if (restart_delay < RESTART_DELAY_MAX_SEC) restart_delay++;
                }
            } else {
                /* Завершился демон. */
                int i;
                for (i = 0; i < services_n; i++) {
                    struct svc *s = &services[i];
                    if (s->pid <= 0 || s->pid != done) continue;
                    s->pid = 0;
                    lived_ms = now_ms() - s->started_ms;

                    if (status_is_success(status)) {
                        s->burst = 0;
                        printf("cgoct: %s exited cleanly after %d ms\n",
                               s->name, (int)lived_ms);
                    } else if (lived_ms <= FAST_CRASH_SEC * 1000L) {
                        s->burst++;
                        printf("cgoct: %s crashed after %d ms (burst=%u)\n",
                               s->name, (int)lived_ms, s->burst);
                    } else {
                        s->burst = 0;
                        printf("cgoct: %s exited after %d ms\n",
                               s->name, (int)lived_ms);
                    }

                    if (s->burst >= (unsigned)cfg.crash_limit) {
                        printf("cgoct: %s crash-loop, cooldown %d sec\n",
                               s->name, cfg.cooldown_sec);
                        s->burst = 0;
                        s->next_start_ms = now_ms() + (long)cfg.cooldown_sec * 1000L;
                    } else {
                        s->next_start_ms = now_ms() + 1000;
                    }
                    break;
                }
            }
        }

        /* ── 4. Подсчитать, сколько можно поспать ── */
        now = now_ms();
        {
            long deadline = 0;
            int i;

            if (shell_pid <= 0) {
                deadline = shell_next_start - now;
            }
            for (i = 0; i < services_n; i++) {
                if (services[i].pid > 0) continue;
                long d = services[i].next_start_ms - now;
                if (deadline == 0 || (d > 0 && d < deadline)) deadline = d;
            }
            if (deadline < MIN_SLEEP_SLICE_MS) deadline = MIN_SLEEP_SLICE_MS;
            sleep_slices(deadline);
        }
    }

supervisor_exit:
    /* Политика "once"/"on-failure": погасить демонов и завершиться. */
    {
        int i;
        for (i = 0; i < services_n; i++) {
            if (services[i].pid > 0) {
                kill(services[i].pid, SIGTERM);
            }
        }
    }
    if (log_fd >= 0) {
        close(log_fd);
    }
    return 0;
}
