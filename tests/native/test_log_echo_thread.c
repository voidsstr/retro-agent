/* test_log_echo_thread.c - TRUE-SOURCE: compiles and RUNS the real
 * agent/src/log.c on real threads (stubs/logfake_env.h: pthreads, a real log
 * file, and a console that can FREEZE the way a QuickEdit/Mark selection or a
 * hung conhost/display freezes a real one).
 *
 * THE DEFECT (agent <= 1.89.1, log.c raw_out): every log line was written to
 * the console with WriteFile(stderr) while g_log_cs was held. With the console
 * frozen, that writer kept the lock and every other thread that logs queued
 * behind it - the accept loop included - so the agent stopped answering while
 * the OS was fine (ADMIN-PC, Win7, 2026-09-26: 35 hours, agent.log just stops).
 * old_shape_* below runs that exact shape in this harness and shows the next
 * logger stuck: the OLD-BUGGY value.
 *
 * THE FIX (agent 1.89.2): loggers copy into a ring and one echo thread writes
 * the console with no lock held (agent/shared/conring.h; ring logic in
 * test_conring.c). Here: 4 threads log 6,000 lines into a FROZEN console and
 * all of them finish, agent.log gets every line, the console shows a clean
 * prefix and one gap notice when it thaws, only ONE thread ever touched the
 * console, shutdown does not wait on it, and QuickEdit is cleared on NT only.
 *
 * Each scenario runs in its own forked process (log.c's state is static) under
 * an alarm, so a regression shows up as HUNG instead of hanging the suite.
 */
#define _GNU_SOURCE
#include "munit.h"
#include "stubs/logfake_env.h"

/* log.c's temp-dir fallback formats into MAX_PATH; host gcc's truncation
 * analysis is noise here, not a finding about the code under test. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
#include "../../agent/src/log.c"
#pragma GCC diagnostic pop

#include <signal.h>
#include <sys/wait.h>

static char g_dir[64];
static char g_logpath[128];

static void setup_dir(void)
{
    snprintf(g_dir, sizeof(g_dir), "/tmp/logecho.XXXXXX");
    if (!mkdtemp(g_dir)) { perror("mkdtemp"); _exit(2); }
    snprintf(g_logpath, sizeof(g_logpath), "%s/agent.log", g_dir);
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    char *s;
    if (!f) return calloc(1, 1);
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    s = malloc((size_t)n + 1);
    n = (long)fread(s, 1, (size_t)n, f);
    s[n] = '\0';
    fclose(f);
    return s;
}

static int count(const char *hay, const char *needle)
{
    int c = 0;
    size_t k = strlen(needle);
    for (const char *p = hay; (p = strstr(p, needle)) != NULL; p += k) c++;
    return c;
}

static int wait_console(const char *needle, int ms)
{
    DWORD t0 = GetTickCount();
    for (;;) {
        char *c = fake_console_text();
        int hit = strstr(c, needle) != NULL;
        free(c);
        if (hit) return 1;
        if ((long)(GetTickCount() - t0) > ms) return 0;
        usleep(2000);
    }
}

static int wait_log(const char *needle, int ms)
{
    DWORD t0 = GetTickCount();
    for (;;) {
        char *c = slurp(g_logpath);
        int hit = strstr(c, needle) != NULL;
        free(c);
        if (hit) return 1;
        if ((long)(GetTickCount() - t0) > ms) return 0;
        usleep(2000);
    }
}

static void cleanup(void)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dir);
    if (system(cmd) != 0) { /* best effort */ }
}

/* Run one scenario in a child under an alarm: log.c's state is static, and a
 * logger that waits on the frozen console must fail the test, not hang it. */
static void run_isolated(const char *name, void (*fn)(void), unsigned limit_s)
{
    int st = 0;
    pid_t pid;
    fflush(stdout);
    fflush(stderr);
    pid = fork();
    if (pid == 0) {
        alarm(limit_s);
        munit_fails = 0;
        fn();
        fflush(stdout);
        fflush(stderr);
        _exit(munit_fails ? 1 : 0);
    }
    if (pid < 0 || waitpid(pid, &st, 0) != pid) {
        munit_fails++;
        fprintf(stderr, "    FAIL %s: could not run the scenario\n", name);
    } else if (WIFSIGNALED(st)) {
        munit_fails++;
        fprintf(stderr, "    FAIL %s: HUNG (signal %d) - something waited on the "
                "frozen console\n", name, WTERMSIG(st));
    } else if (WEXITSTATUS(st) != 0) {
        munit_fails++;
        fprintf(stderr, "    FAIL %s (see above)\n", name);
    }
}

/* ======================================================= the OLD shape */
static CRITICAL_SECTION old_cs;
static volatile int old_next_done = 0;

static void *old_raw_out(void *p)            /* 1.89.1: console write under the lock */
{
    DWORD wr;
    (void)p;
    EnterCriticalSection(&old_cs);
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), "x\r\n", 3, &wr, NULL);
    LeaveCriticalSection(&old_cs);
    return NULL;
}

static void *old_next_logger(void *p)        /* e.g. the accept loop's log_msg */
{
    (void)p;
    EnterCriticalSection(&old_cs);
    LeaveCriticalSection(&old_cs);
    old_next_done = 1;
    return NULL;
}

static void scen_old_shape(void)
{
    pthread_t a, b;
    InitializeCriticalSection(&old_cs);
    fake_console_freeze(1);
    pthread_create(&a, NULL, old_raw_out, NULL);
    usleep(50000);
    pthread_create(&b, NULL, old_next_logger, NULL);
    usleep(300000);
    /* OLD-BUGGY: the next logger is stuck behind the console writer */
    CHECK_EQ_I(old_next_done, 0);
    fake_console_freeze(0);
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    CHECK_EQ_I(old_next_done, 1);
}

TEST(old_shape_a_console_write_under_the_lock_stalls_the_next_logger)
{
    run_isolated("old_shape", scen_old_shape, 10);
}

/* ====================================================== the fix, live */
#define WORKERS 4
#define PER     1500

static volatile int g_done_workers = 0;

static void *worker(void *p)
{
    int w = (int)(intptr_t)p, i;
    for (i = 0; i < PER; i++) {
        log_msg(LOG_MAIN, "w%d-%05d", w, i);
        if (i % 250 == 0)
            con_printf("progress w%d %d\n", w, i);
    }
    __sync_add_and_fetch(&g_done_workers, 1);
    return NULL;
}

static void scen_frozen_console(void)
{
    pthread_t t[WORKERS];
    int i, k = 0, last[WORKERS], torn = 0, disorder = 0;
    DWORD t0;
    char *log, *con, *notice, *line;
    unsigned long gap;

    setup_dir();
    fake_console_freeze(1);                 /* frozen before the agent starts */
    log_init(g_logpath);
    for (i = 0; i < WORKERS; i++)
        pthread_create(&t[i], NULL, worker, (void *)(intptr_t)i);
    t0 = GetTickCount();
    while (g_done_workers < WORKERS && GetTickCount() - t0 < 5000)
        usleep(1000);

    /* FIXED: every logger finished although the console accepted nothing */
    CHECK_EQ_I(g_done_workers, WORKERS);
    log = slurp(g_logpath);
    CHECK_EQ_I(count(log, "][MAIN ] w"), WORKERS * PER);   /* agent.log: all */
    free(log);
    con = fake_console_text();
    CHECK_EQ_U(strlen(con), 0);                  /* the console got nothing */
    free(con);

    /* the console recovers: a clean prefix, ONE gap notice, then live again */
    fake_console_freeze(0);
    CHECK(wait_console("line(s) not shown here", 3000), "no gap notice after the thaw");
    CHECK(wait_log("were not shown on the console", 3000), "the gap is not in agent.log");
    log_msg(LOG_MAIN, "after-the-thaw");
    CHECK(wait_console("after-the-thaw\r\n", 3000), "the console did not come back");

    con = fake_console_text();
    notice = strstr(con, "[console: ");
    CHECK(notice != NULL, "gap notice");
    if (notice) {
        gap = strtoul(notice + 10, NULL, 10);
        CHECK_EQ_I(count(con, "[console: "), 1);
        for (i = 0; i < WORKERS; i++) last[i] = -1;
        for (line = con; line < notice; ) {
            char *eol = strstr(line, "\r\n");
            int w, n;
            if (!eol || eol > notice) { torn++; break; }
            if (sscanf(line, "[%*d:%*d:%*d][MAIN ] w%d-%d", &w, &n) == 2) {
                if (w < 0 || w >= WORKERS || n <= last[w]) disorder++;
                else last[w] = n;
                k++;
            }
            line = eol + 2;
        }
        CHECK_EQ_I(torn, 0);                     /* never half a line */
        CHECK_EQ_I(disorder, 0);                 /* per-thread order kept */
        CHECK(k > 0 && k < WORKERS * PER, "the ring should have shown a prefix");
        CHECK(gap >= (unsigned long)(WORKERS * PER - k),
              "the notice must count every worker line the console missed");
        CHECK(count(notice, "][MAIN ] w") == 0, "a pre-gap line appeared after the notice");
    }
    free(con);

    /* exactly ONE thread ever touched the console, and it is not a logger */
    CHECK_EQ_I(fake_console_threads(), 1);
    CHECK(!pthread_equal(fake_con_tids[0], pthread_self()), "main thread wrote the console");
    for (i = 0; i < WORKERS; i++)
        CHECK(!pthread_equal(fake_con_tids[0], t[i]), "a logger wrote the console");
    for (i = 0; i < WORKERS; i++)
        pthread_join(t[i], NULL);

    /* NT: QuickEdit cleared (on the echo thread, once the console answered) */
    CHECK(wait_log("console: QuickEdit off", 3000), "QuickEdit result not logged");
    CHECK_EQ_U(fake_con_mode & 0x40, 0);
    CHECK_EQ_U(fake_con_mode & 0x80, 0x80);

    log_shutdown();
    cleanup();
}

TEST(a_frozen_console_stalls_no_logger)
{
    run_isolated("frozen_console", scen_frozen_console, 20);
}

static void scen_shutdown_frozen(void)
{
    DWORD t0, took;
    char *log;
    size_t n;
    const char *mark = "--- log closed (clean shutdown) ---\r\n";

    setup_dir();
    fake_console_freeze(1);
    log_init(g_logpath);
    log_msg(LOG_MAIN, "shutdown complete; exiting process");
    t0 = GetTickCount();
    log_shutdown();                         /* must not wait on the console */
    took = GetTickCount() - t0;
    CHECK(took < 2500, "log_shutdown waited on the frozen console");
    log = slurp(g_logpath);
    n = strlen(log);
    CHECK(n >= strlen(mark) && strcmp(log + n - strlen(mark), mark) == 0,
          "the clean-exit marker must still end the file");
    free(log);
    cleanup();
}

TEST(shutdown_is_bounded_when_the_console_is_frozen)
{
    run_isolated("shutdown_frozen", scen_shutdown_frozen, 10);
}

static void scen_shutdown_drains(void)
{
    char *con;
    setup_dir();
    log_init(g_logpath);
    log_console_title("Retro Remote Agent Version T");
    con_printf("a\nb\r\n");
    con_printf("Shutting down...\n");
    log_shutdown();                         /* drains the echo before closing */
    con = fake_console_text();
    /* the whole marker, "\r\n" included: a hand-counted 31 cut its "\n" */
    CHECK(strncmp(con, "--- log opened (raw win32) ---\r\n", 32) == 0, "first line");
    CHECK(strstr(con, "a\r\nb\r\n") != NULL, "\\n must become \\r\\n, \\r\\n stays");
    CHECK(strstr(con, "Shutting down...\r\n") != NULL, "the last line was not drained");
    CHECK(strcmp(fake_con_title, "Retro Remote Agent Version T") == 0, "title");
    CHECK_EQ_I(fake_console_threads(), 1);
    free(con);
    cleanup();
}

TEST(clean_shutdown_drains_the_echo_and_the_title_is_set_off_thread)
{
    run_isolated("shutdown_drains", scen_shutdown_drains, 10);
}

static void scen_no_thread(void)
{
    int i;
    char *con;
    setup_dir();
    fake_fail_create_thread = 1;            /* the 31 MB Deskpro */
    fake_console_freeze(1);
    log_init(g_logpath);
    for (i = 0; i < 200; i++) {             /* must return: echo is OFF, */
        log_msg(LOG_MAIN, "line %d", i);    /* never done inline         */
        con_printf("con %d\n", i);
    }
    log_console_title("t");
    CHECK(wait_log("no console echo thread", 1000), "the fallback must say so");
    CHECK(wait_log("line 199", 1000), "agent.log still gets every line");
    fake_console_freeze(0);
    usleep(100000);
    con = fake_console_text();
    CHECK_EQ_U(strlen(con), 0);
    CHECK_EQ_I(fake_console_threads(), 0);
    free(con);
    log_shutdown();
    cleanup();
}

TEST(no_echo_thread_means_no_echo_never_inline_echo)
{
    run_isolated("no_thread", scen_no_thread, 10);
}

static void scen_win9x(void)
{
    setup_dir();
    fake_version = 0xC0000A04ul;            /* Windows 98 SE */
    log_init(g_logpath);
    CHECK(wait_log("Win9x - no QuickEdit to clear", 2000), "9x result not logged");
    CHECK(wait_log("log: console echo on its own thread", 2000), "echo thread on 9x");
    CHECK_EQ_I(fake_con_setmode_calls, 0);
    CHECK_EQ_U(fake_con_mode & 0x40, 0x40);
    log_shutdown();
    cleanup();
}

TEST(win9x_runs_the_echo_thread_and_leaves_the_console_mode_alone)
{
    run_isolated("win9x", scen_win9x, 10);
}

static void scen_no_console(void)
{
    char *log;
    setup_dir();
    fake_con_present = 0;                   /* an NT service: no std handles */
    log_init(g_logpath);
    log_msg(LOG_MAIN, "service line");
    con_printf("nobody sees this\n");
    log_console_title("t");
    log_shutdown();
    log = slurp(g_logpath);
    CHECK(strstr(log, "service line") != NULL, "file logging unaffected");
    CHECK(strstr(log, "console echo on its own thread") == NULL, "no console, no thread");
    CHECK_EQ_I(fake_console_threads(), 0);
    free(log);
    cleanup();
}

TEST(no_console_means_no_echo_thread_and_file_logging_unchanged)
{
    run_isolated("no_console", scen_no_console, 10);
}

MUNIT_MAIN("log.c console echo on real threads (agent 1.89.2)",
    RUN(old_shape_a_console_write_under_the_lock_stalls_the_next_logger);
    RUN(a_frozen_console_stalls_no_logger);
    RUN(shutdown_is_bounded_when_the_console_is_frozen);
    RUN(clean_shutdown_drains_the_echo_and_the_title_is_set_off_thread);
    RUN(no_echo_thread_means_no_echo_never_inline_echo);
    RUN(win9x_runs_the_echo_thread_and_leaves_the_console_mode_alone);
    RUN(no_console_means_no_echo_thread_and_file_logging_unchanged);
)
