#define _POSIX_C_SOURCE 200809L
#include "engine.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(test) do { if (!(test)) { \
    fprintf(stderr, "%s:%d: %s failed (errno=%d)\n", __FILE__, __LINE__, #test, errno); \
    exit(1); \
} } while (0)

typedef struct {
    size_t count;
    size_t bytes;
    size_t length[16];
    uint32_t hash[16];
    char prefix[16][160];
} Lines;

static double now(void)
{
    struct timespec value;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &value) == 0);
    return (double)value.tv_sec + (double)value.tv_nsec / 1e9;
}

static uint32_t hash_bytes(const char *data, size_t len)
{
    uint32_t hash = UINT32_C(2166136261);
    for (size_t i = 0; i < len; ++i)
        hash = (hash ^ (unsigned char)data[i]) * UINT32_C(16777619);
    return hash;
}

static void got_line(void *context, const char *line, size_t len)
{
    Lines *lines = context;
    CHECK(line[len] == '\0');
    CHECK(!len || line[len - 1] != '\r');
    if (lines->count < 16) {
        size_t i = lines->count;
        size_t copy = len < sizeof(lines->prefix[i]) - 1 ? len : sizeof(lines->prefix[i]) - 1;
        memcpy(lines->prefix[i], line, copy);
        lines->prefix[i][copy] = '\0';
        lines->length[i] = len;
        lines->hash[i] = hash_bytes(line, len);
    }
    ++lines->count;
    lines->bytes += len + 1;
}

static void finish_child(Engine *engine)
{
    double deadline = now() + 3.0;
    engine_terminate(engine);
    while (engine_reap(engine) == 0 && now() < deadline)
        (void)poll(NULL, 0, 1);
    CHECK(engine->pid == -1);
    CHECK(engine->input_fd == -1);
    CHECK(engine->output_fd == -1);
    CHECK(engine->dead);
    CHECK(!engine_wants_write(engine));
}

/* Pump both pipes until the deadline. */
static void pump(Engine *engine, Lines *lines, size_t target, int until_eof)
{
    double deadline = now() + 5.0;
    while (engine->output_fd >= 0 && now() < deadline &&
           (until_eof || lines->count < target)) {
        struct pollfd fds[2] = {
            { engine->output_fd, POLLIN, 0 },
            { engine_wants_write(engine) ? engine->input_fd : -1, POLLOUT, 0 }
        };
        int ready = poll(fds, 2, 100);
        CHECK(ready >= 0 || errno == EINTR);
        if (ready < 0)
            continue;
        if (fds[1].revents & (POLLOUT | POLLERR | POLLHUP))
            CHECK(engine_flush(engine) == 0);
        if (fds[0].revents & (POLLIN | POLLERR | POLLHUP)) {
            int result = engine_read(engine, got_line, lines);
            CHECK(result == 0 || engine->dead);
        }
    }
    CHECK(now() < deadline);
    if (until_eof)
        CHECK(engine->output_fd == -1 && engine->dead);
    else
        CHECK(lines->count >= target);
}

static void spawn_mode(Engine *engine, const char *program, const char *mode)
{
    char *argv[] = { (char *)program, "--fixture", (char *)mode, NULL };
    engine_init(engine);
    if (engine_spawn(engine, argv, NULL) < 0) {
        fprintf(stderr, "spawn failed: %s\n", engine->error);
        exit(1);
    }
    CHECK((fcntl(engine->input_fd, F_GETFL) & O_NONBLOCK) != 0);
    CHECK((fcntl(engine->output_fd, F_GETFL) & O_NONBLOCK) != 0);
    CHECK((fcntl(engine->input_fd, F_GETFD) & FD_CLOEXEC) != 0);
    CHECK((fcntl(engine->output_fd, F_GETFD) & FD_CLOEXEC) != 0);
}

static void test_exec_isolation(const char *program)
{
    Engine engine;
    Lines lines = { 0 };
    char directory[] = "/tmp/lichess-engine-test-XXXXXX";
    char descriptor[24], cwd[180];
    char argument[] = "a b; $(printf injected) * \"literal\"";
    int original = open("/dev/null", O_RDONLY);
    int extra;
    char *argv[] = { (char *)program, "--fixture", "probe", descriptor, argument, NULL };
    CHECK(original >= 0);
    extra = fcntl(original, F_DUPFD, 100);
    CHECK(extra >= 100);
    CHECK(fcntl(extra, F_SETFD, 0) == 0);
    (void)close(original);
    CHECK(mkdtemp(directory) != NULL);
    CHECK(snprintf(descriptor, sizeof(descriptor), "%d", extra) > 0);
    CHECK(setenv("LICHESS_BOT_TOKEN", "transport-test-token", 1) == 0);
    engine_init(&engine);
    CHECK(engine_spawn(&engine, argv, directory) == 0);
    pump(&engine, &lines, 0, 1);
    CHECK(lines.count == 5);
    CHECK(strcmp(lines.prefix[0], "token=unset") == 0);
    CHECK(strcmp(lines.prefix[1], "fd=closed") == 0);
    CHECK(strncmp(lines.prefix[2], "arg=", 4) == 0);
    CHECK(strcmp(lines.prefix[2] + 4, argument) == 0);
    CHECK(snprintf(cwd, sizeof(cwd), "cwd=%s", directory) > 0);
    CHECK(strcmp(lines.prefix[3], cwd) == 0);
    CHECK(strcmp(lines.prefix[4], "stdin=blocking") == 0);
    CHECK(strcmp(getenv("LICHESS_BOT_TOKEN"), "transport-test-token") == 0);
    CHECK(fcntl(extra, F_GETFD) >= 0);
    finish_child(&engine);
    CHECK(unsetenv("LICHESS_BOT_TOKEN") == 0);
    (void)close(extra);
    CHECK(rmdir(directory) == 0);
}

static void test_long_command(const char *program)
{
    Engine engine;
    Lines lines = { 0 };
    const size_t payload = 96u * 1024u;
    char *command = malloc(payload + 1);
    CHECK(command != NULL);
    for (size_t i = 0; i < payload; ++i)
        command[i] = (char)('a' + i % 26);
    command[payload] = '\n';
    spawn_mode(&engine, program, "echo");
    CHECK(engine_queue(&engine, command, payload + 1) == 0);
    CHECK(engine_queue(&engine, "tail\nquit\n", 10) == 0);
    CHECK(engine_wants_write(&engine));
    pump(&engine, &lines, 0, 1);
    CHECK(lines.count == 4);
    CHECK(strcmp(lines.prefix[0], "first") == 0);
    CHECK(strcmp(lines.prefix[1], "second") == 0);
    CHECK(lines.length[2] == payload);
    CHECK(lines.hash[2] == hash_bytes(command, payload));
    CHECK(strcmp(lines.prefix[3], "tail") == 0);
    CHECK(!engine_wants_write(&engine));
    finish_child(&engine);
    free(command);
}

static void test_queue_pressure(const char *program)
{
    Engine engine;
    Lines lines = { 0 };
    char *command = malloc(ENGINE_BUFFER_SIZE);
    size_t sent, pending;
    double start;
    CHECK(command != NULL);
    memset(command, 'z', ENGINE_BUFFER_SIZE);
    spawn_mode(&engine, program, "wait");
    pump(&engine, &lines, 1, 0);
    CHECK(engine_reap(&engine) == 0);
    CHECK(engine_queue(&engine, command, ENGINE_BUFFER_SIZE) == 0);
    CHECK(engine_queue(&engine, "x", 1) == -1 && errno == EMSGSIZE);
    CHECK(engine.input_length == ENGINE_BUFFER_SIZE && engine.input_offset == 0);
    start = now();
    CHECK(engine_flush(&engine) == 0);
    CHECK(now() - start < 1.0);
    sent = engine.input_offset;
    CHECK(sent > 0 && sent < ENGINE_BUFFER_SIZE);
    CHECK(engine_wants_write(&engine));
    pending = ENGINE_BUFFER_SIZE - sent;
    /* Compaction after a partial write must preserve all bytes. */
    CHECK(engine_queue(&engine, "x", 1) == 0);
    CHECK(engine.input_offset == 0 && engine.input_length == pending + 1);
    CHECK(memcmp(engine.input_buffer, command, pending) == 0);
    CHECK(engine.input_buffer[pending] == 'x');
    CHECK(engine_queue(&engine, command, ENGINE_BUFFER_SIZE) == -1 && errno == EMSGSIZE);
    CHECK(engine.input_length == pending + 1);
    CHECK(engine_queue(&engine, NULL, 1) == -1 && errno == EINVAL);
    CHECK(engine_queue(&engine, NULL, 0) == 0);
    finish_child(&engine);
    CHECK(WIFSIGNALED(engine.wait_status) && WTERMSIG(engine.wait_status) == SIGKILL);
    CHECK(engine_queue(&engine, "x", 1) == -1 && errno == EPIPE);
    free(command);
}

static void test_lines_and_eof(const char *program)
{
    Engine engine;
    Lines lines = { 0 };
    char limit[24];
    char *argv[] = { (char *)program, "--fixture", "lines", limit, NULL };
    CHECK(snprintf(limit, sizeof(limit), "%u", ENGINE_BUFFER_SIZE) > 0);
    engine_init(&engine);
    CHECK(engine_spawn(&engine, argv, NULL) == 0);
    pump(&engine, &lines, 0, 1);
    CHECK(lines.count == 4);
    CHECK(lines.length[0] == ENGINE_BUFFER_SIZE && lines.prefix[0][0] == 'a');
    CHECK(engine.dropped_lines == 1);
    CHECK(strcmp(lines.prefix[1], "bestmove e2e4") == 0);
    CHECK(lines.length[2] == 0);
    CHECK(strcmp(lines.prefix[3], "last-without-newline") == 0);
    CHECK(engine.output_length == 0 && !engine.output_dropping);
    finish_child(&engine);
}

static void test_fairness(const char *program)
{
    Engine engine;
    Lines lines = { 0 };
    struct pollfd fd;
    double start;
    spawn_mode(&engine, program, "flood");
    fd = (struct pollfd){ engine.output_fd, POLLIN, 0 };
    CHECK(poll(&fd, 1, 2000) == 1);
    start = now();
    CHECK(engine_read(&engine, got_line, &lines) == 0);
    CHECK(now() - start < 1.0);
    CHECK(lines.count > 0);
    CHECK(lines.bytes <= ENGINE_READ_BUDGET);
    CHECK(!engine.dead);
    finish_child(&engine);
}

static void test_broken_input(const char *program)
{
    Engine engine;
    Lines lines = { 0 };
    spawn_mode(&engine, program, "stdin_closed");
    pump(&engine, &lines, 1, 0);
    CHECK(strcmp(lines.prefix[0], "stdin-closed") == 0);
    CHECK(engine_queue(&engine, "isready\n", 8) == 0);
    CHECK(engine_flush(&engine) == -1 && errno == EPIPE);
    CHECK(engine.dead && !engine_wants_write(&engine));
    finish_child(&engine);
}

static void test_reap_before_drain(const char *program)
{
    Engine engine;
    Lines lines = { 0 };
    double deadline;
    spawn_mode(&engine, program, "exit");
    deadline = now() + 3.0;
    while (engine_reap(&engine) == 0 && now() < deadline)
        (void)poll(NULL, 0, 1);
    CHECK(engine.pid == -1 && engine.dead);
    CHECK(engine.output_fd >= 0);
    CHECK(WIFEXITED(engine.wait_status) && WEXITSTATUS(engine.wait_status) == 0);
    pump(&engine, &lines, 0, 1);
    CHECK(lines.count == 1 && strcmp(lines.prefix[0], "goodbye") == 0);
    finish_child(&engine);
}

static void test_spawn_errors(const char *program)
{
    Engine engine;
    char *missing[] = { "/no/such/lichess-c-test-engine", NULL };
    char *valid[] = { (char *)program, "--fixture", "exit", NULL };
    engine_init(&engine);
    CHECK(engine_spawn(&engine, NULL, NULL) == -1 && errno == EINVAL);
    CHECK(engine_spawn(&engine, missing, NULL) == -1 && errno == ENOENT);
    CHECK(strstr(engine.error, "exec engine") != NULL);
    finish_child(&engine);
    CHECK(engine_spawn(&engine, valid, "/no/such/lichess-c-test-directory") == -1);
    CHECK(strstr(engine.error, "change engine directory") != NULL);
    finish_child(&engine);
    /* Never replace a live child, even with an empty queue. */
    spawn_mode(&engine, program, "wait");
    CHECK(engine_spawn(&engine, valid, NULL) == -1 && errno == EBUSY);
    finish_child(&engine);
    /* Reuse storage after reaping. */
    CHECK(engine_spawn(&engine, valid, NULL) == 0);
    finish_child(&engine);
}

/* Run this executable as the engine fixture. */
static int fixture(int argc, char **argv)
{
    CHECK(argc >= 1);
    CHECK(setvbuf(stdout, NULL, _IONBF, 0) == 0);
    const char *mode = argv[0];
    if (!strcmp(mode, "probe")) {
        CHECK(argc == 3);
        char cwd[4096];
        CHECK(getcwd(cwd, sizeof(cwd)) != NULL);
        printf("token=%s\r\nfd=%s\narg=%s\ncwd=%s\nstdin=%s\n",
               getenv("LICHESS_BOT_TOKEN") ? "set" : "unset",
               fcntl(atoi(argv[1]), F_GETFD) >= 0 ? "open" : "closed", argv[2], cwd,
               fcntl(STDIN_FILENO, F_GETFL) & O_NONBLOCK ? "nonblocking" : "blocking");
    } else if (!strcmp(mode, "echo")) {
        fputs("first\r\nsecond\n", stdout);
        char *line = NULL;
        size_t capacity = 0;
        ssize_t length;
        while ((length = getline(&line, &capacity, stdin)) >= 0) {
            if (!strcmp(line, "quit\n")) break;
            CHECK(fwrite(line, 1, (size_t)length, stdout) == (size_t)length);
        }
        free(line);
    } else if (!strcmp(mode, "lines")) {
        CHECK(argc == 2);
        size_t limit = (size_t)strtoul(argv[1], NULL, 10);
        CHECK(limit == ENGINE_BUFFER_SIZE);
        char *line = malloc(limit * 3);
        CHECK(line != NULL);
        memset(line, 'a', limit);
        CHECK(fwrite(line, 1, limit, stdout) == limit);
        fputs("\ninfo ", stdout);
        memset(line, 'x', limit * 3);
        CHECK(fwrite(line, 1, limit * 3, stdout) == limit * 3);
        fputs("\nbestmove e2e4\r\n\nlast-without-newline", stdout);
        free(line);
    } else if (!strcmp(mode, "flood")) {
        const char line[] = "info depth 1 nodes 123456789\n";
        char chunk[(sizeof(line) - 1) * 4096];
        for (size_t i = 0; i < sizeof(chunk); i += sizeof(line) - 1)
            memcpy(chunk + i, line, sizeof(line) - 1);
        for (;;)
            CHECK(fwrite(chunk, 1, sizeof(chunk), stdout) == sizeof(chunk));
    } else if (!strcmp(mode, "stdin_closed")) {
        CHECK(close(STDIN_FILENO) == 0);
        puts("stdin-closed");
        for (;;) pause();
    } else if (!strcmp(mode, "wait")) {
        CHECK(signal(SIGTERM, SIG_IGN) != SIG_ERR);
        puts("waiting");
        for (;;) pause();
    } else if (!strcmp(mode, "exit")) {
        puts("goodbye");
    } else {
        CHECK(0);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "--fixture"))
        return fixture(argc - 2, argv + 2);
    CHECK(argc == 2);
    CHECK(argv[1][0] == '/');
    CHECK(signal(SIGPIPE, SIG_IGN) != SIG_ERR);
    test_exec_isolation(argv[1]);
    test_long_command(argv[1]);
    test_queue_pressure(argv[1]);
    test_lines_and_eof(argv[1]);
    test_fairness(argv[1]);
    test_broken_input(argv[1]);
    test_reap_before_drain(argv[1]);
    test_spawn_errors(argv[1]);
    puts("engine transport: 8 test groups passed");
    return 0;
}
