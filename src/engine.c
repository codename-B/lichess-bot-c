#define _GNU_SOURCE
#include "engine.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#include <sys/prctl.h>
#endif

static int fail(Engine *engine, int code, const char *operation)
{
    (void)snprintf(engine->error, sizeof(engine->error), "%s: %s",
                   operation, strerror(code));
    errno = code;
    return -1;
}

static void close_fd(int *fd)
{
    if (*fd >= 0)
        (void)close(*fd);
    *fd = -1;
}

static void close_input(Engine *engine)
{
    close_fd(&engine->input_fd);
    engine->input_offset = 0;
    engine->input_length = 0;
}

void engine_init(Engine *engine)
{
    memset(engine, 0, sizeof(*engine));
    engine->pid = -1;
    engine->input_fd = -1;
    engine->output_fd = -1;
    engine->dead = 1;
    engine->wait_status = -1;
}

/* Keep pipe descriptors above stderr, even if a standard descriptor is closed. */
static int make_pipe(int descriptors[2])
{
    int i;
    descriptors[0] = descriptors[1] = -1;
    if (pipe(descriptors) < 0)
        return -1;
    for (i = 0; i < 2; ++i) {
        if (descriptors[i] < 3) {
            int replacement = fcntl(descriptors[i], F_DUPFD_CLOEXEC, 3);
            if (replacement < 0)
                goto error;
            close_fd(&descriptors[i]);
            descriptors[i] = replacement;
        } else if (fcntl(descriptors[i], F_SETFD, FD_CLOEXEC) < 0) {
            goto error;
        }
    }
    return 0;
error:
    {
        int saved = errno;
        close_fd(&descriptors[0]);
        close_fd(&descriptors[1]);
        errno = saved;
        return -1;
    }
}

static int nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* Called in a single-threaded fork child, before exec. Keep the error pipe. */
static void close_child_descriptors(int keep, long limit)
{
#if defined(__linux__) && defined(SYS_close_range)
    int lower = keep == 3 ||
                syscall(SYS_close_range, 3u, (unsigned int)keep - 1u, 0u) == 0;
    int upper = syscall(SYS_close_range, (unsigned int)keep + 1u, UINT_MAX, 0u) == 0;
    if (lower && upper)
        return;
#endif
    for (long fd = 3; fd < limit; ++fd)
        if (fd != keep)
            (void)close((int)fd);
}

enum child_stage { CHILD_DUP, CHILD_CHDIR, CHILD_ENV, CHILD_EXEC, CHILD_GROUP };
struct child_error { int code; int stage; };

static void child_fail(int fd, enum child_stage stage)
{
    struct child_error error = { errno, stage };
    ssize_t result;
    do {
        result = write(fd, &error, sizeof(error));
    } while (result < 0 && errno == EINTR);
    _exit(127);
}

static int spawn_impl(Engine *engine, char *const argv[], const char *workdir,
                       int silence_stderr, int own_group);

int engine_spawn(Engine *engine, char *const argv[], const char *workdir)
{
    return engine_spawn_ex(engine, argv, workdir, 0);
}

int engine_spawn_ex(Engine *engine, char *const argv[], const char *workdir,
                    int silence_stderr)
{
    return spawn_impl(engine, argv, workdir, silence_stderr, 1);
}

int engine_spawn_attached(Engine *engine, char *const argv[], const char *workdir)
{
    return spawn_impl(engine, argv, workdir, 0, 0);
}

static int spawn_impl(Engine *engine, char *const argv[], const char *workdir,
                       int silence_stderr, int own_group)
{
    int input[2] = { -1, -1 }, output[2] = { -1, -1 };
    int report[2] = { -1, -1 };
    struct child_error error = { 0, 0 };
    size_t received = 0;
    struct rlimit files;
    long limit;
    pid_t pid;
#if defined(__linux__)
    pid_t parent_pid = getpid();
#endif
    const char *operation = "create engine pipes";

    if (!argv || !argv[0] || !argv[0][0])
        return fail(engine, EINVAL, "empty engine command");
    if (engine->pid > 0) {
        if (engine_reap(engine) < 0)
            return -1;
        if (engine->pid > 0)
            return fail(engine, EBUSY, "previous engine has not exited");
    }
    engine_terminate(engine);
    engine_init(engine);

    if (make_pipe(input) < 0 || make_pipe(output) < 0 || make_pipe(report) < 0)
        goto error_before_fork;
    operation = "make engine pipes nonblocking";
    if (nonblocking(input[1]) < 0 || nonblocking(output[0]) < 0)
        goto error_before_fork;

    limit = sysconf(_SC_OPEN_MAX);
    if (getrlimit(RLIMIT_NOFILE, &files) == 0 && files.rlim_max != RLIM_INFINITY &&
        files.rlim_max <= (rlim_t)INT_MAX && (long)files.rlim_max > limit)
        limit = (long)files.rlim_max;
    if (limit < 0)
        limit = 1048576;

    operation = "fork engine";
    pid = fork();
    if (pid < 0)
        goto error_before_fork;
    if (pid == 0) {
        if (own_group && setpgid(0, 0) < 0)
            child_fail(report[1], CHILD_GROUP);
#if defined(__linux__)
        /* Terminate the engine if its adapter dies. */
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) < 0)
            child_fail(report[1], CHILD_GROUP);
        if (getppid() != parent_pid)
            _exit(127);
#endif
        if (dup2(input[0], STDIN_FILENO) < 0 ||
            dup2(output[1], STDOUT_FILENO) < 0)
            child_fail(report[1], CHILD_DUP);
        if (silence_stderr) {
            int quiet = open("/dev/null", O_WRONLY);
            if (quiet < 0 || dup2(quiet, STDERR_FILENO) < 0)
                child_fail(report[1], CHILD_DUP);
            if (quiet != STDERR_FILENO)
                (void)close(quiet);
        }
        /* stderr is inherited even if the caller marked it close-on-exec. */
        (void)fcntl(STDERR_FILENO, F_SETFD, 0);
        close_child_descriptors(report[1], limit);
        (void)signal(SIGPIPE, SIG_DFL);
        if (workdir && chdir(workdir) < 0)
            child_fail(report[1], CHILD_CHDIR);
        if (unsetenv("LICHESS_BOT_TOKEN") < 0)
            child_fail(report[1], CHILD_ENV);
        execvp(argv[0], argv);
        child_fail(report[1], CHILD_EXEC);
    }

    engine->pid = pid;
    engine->process_group = own_group ? (int)pid : 0;
    engine->input_fd = input[1];
    engine->output_fd = output[0];
    engine->dead = 0;
    close_fd(&input[0]);
    close_fd(&output[1]);
    close_fd(&report[1]);
    /* Child closes on exec or writes an error record. */
    while (received < sizeof(error)) {
        ssize_t count = read(report[0], (char *)&error + received,
                             sizeof(error) - received);
        if (count > 0) {
            received += (size_t)count;
        } else if (count == 0) {
            break;
        } else if (errno != EINTR) {
            error.code = errno;
            error.stage = CHILD_EXEC;
            received = sizeof(error);
            break;
        }
    }
    close_fd(&report[0]);
    if (received) {
        static const char *const stages[] = {
            "connect engine pipes", "change engine directory",
            "clear engine token environment", "exec engine", "isolate engine group"
        };
        if (received != sizeof(error)) {
            error.code = EIO;
            error.stage = CHILD_EXEC;
        }
        engine_terminate(engine);
        return fail(engine, error.code, stages[error.stage]);
    }
    return 0;

error_before_fork:
    {
        int saved = errno;
        close_fd(&input[0]);
        close_fd(&input[1]);
        close_fd(&output[0]);
        close_fd(&output[1]);
        close_fd(&report[0]);
        close_fd(&report[1]);
        return fail(engine, saved, operation);
    }
}

int engine_queue(Engine *engine, const char *data, size_t len)
{
    size_t pending = engine->input_length - engine->input_offset;
    if (engine->dead || engine->input_fd < 0)
        return fail(engine, EPIPE, "engine input is closed");
    if (len && !data)
        return fail(engine, EINVAL, "null engine command");
    if (len > ENGINE_BUFFER_SIZE - pending)
        return fail(engine, EMSGSIZE, "engine command queue is full");
    if (len > ENGINE_BUFFER_SIZE - engine->input_length) {
        memmove(engine->input_buffer, engine->input_buffer + engine->input_offset,
                 pending);
        engine->input_offset = 0;
        engine->input_length = pending;
    }
    if (len) {
        memcpy(engine->input_buffer + engine->input_length, data, len);
        engine->input_length += len;
    }
    return 0;
}

int engine_wants_write(const Engine *engine)
{
    return !engine->dead && engine->input_fd >= 0 &&
           engine->input_offset < engine->input_length;
}

int engine_flush(Engine *engine)
{
    if (engine->dead || engine->input_fd < 0)
        return fail(engine, EPIPE, "engine input is closed");
    while (engine->input_offset < engine->input_length) {
        ssize_t count = write(engine->input_fd,
                              engine->input_buffer + engine->input_offset,
                              engine->input_length - engine->input_offset);
        if (count > 0) {
            engine->input_offset += (size_t)count;
        } else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return 0;
        } else if (count < 0 && errno == EINTR) {
            return 0;
        } else {
            int saved = count == 0 ? EPIPE : errno;
            engine->dead = 1;
            close_input(engine);
            return fail(engine, saved, "write engine input");
        }
    }
    engine->input_length = engine->input_offset = 0;
    return 0;
}

static void dispatch(Engine *engine,
                     void (*on_line)(void *, const char *, size_t), void *context)
{
    size_t len = engine->output_length;
    if (len && engine->output_buffer[len - 1] == '\r')
        --len;
    engine->output_buffer[len] = '\0';
    engine->output_length = 0;
    if (on_line)
        on_line(context, engine->output_buffer, len);
}

static void accept_output(Engine *engine, const char *data, size_t len,
                          void (*on_line)(void *, const char *, size_t), void *context)
{
    const char *end = data + len;
    while (data < end) {
        const char *newline = memchr(data, '\n', (size_t)(end - data));
        size_t part = (size_t)((newline ? newline : end) - data);
        if (!engine->output_dropping) {
            if (part > ENGINE_BUFFER_SIZE - engine->output_length) {
                engine->output_dropping = 1;
                engine->output_length = 0;
                ++engine->dropped_lines;
            } else {
                memcpy(engine->output_buffer + engine->output_length, data, part);
                engine->output_length += part;
            }
        }
        if (!newline)
            break;
        if (!engine->output_dropping)
            dispatch(engine, on_line, context);
        engine->output_dropping = 0;
        data = newline + 1;
    }
}

int engine_read(Engine *engine,
                void (*on_line)(void *, const char *, size_t), void *context)
{
    char data[8192];
    size_t budget = ENGINE_READ_BUDGET;
    if (engine->output_fd < 0)
        return fail(engine, EPIPE, "engine output is closed");
    while (budget) {
        size_t request = budget < sizeof(data) ? budget : sizeof(data);
        ssize_t count = read(engine->output_fd, data, request);
        if (count > 0) {
            budget -= (size_t)count;
            accept_output(engine, data, (size_t)count, on_line, context);
        } else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return 0;
        } else if (count < 0 && errno == EINTR) {
            return 0;
        } else {
            int saved = count == 0 ? EPIPE : errno;
            if (count == 0 && !engine->output_dropping && engine->output_length)
                dispatch(engine, on_line, context);
            engine->output_length = 0;
            engine->output_dropping = 0;
            engine->dead = 1;
            close_fd(&engine->output_fd);
            close_input(engine);
            return fail(engine, saved,
                         count == 0 ? "engine closed stdout" : "read engine output");
        }
    }
    return 0;
}

int engine_reap(Engine *engine)
{
    int status;
    pid_t result;
    if (engine->pid <= 0)
        return 1;
    result = waitpid(engine->pid, &status, WNOHANG);
    if (result == 0 || (result < 0 && errno == EINTR))
        return 0;
    if (result < 0 && errno != ECHILD)
        return fail(engine, errno, "reap engine");
    engine->pid = -1;
    engine->dead = 1;
    engine->wait_status = result > 0 ? status : -1;
    close_input(engine);
    return 1;
}

void engine_terminate(Engine *engine)
{
    close_input(engine);
    close_fd(&engine->output_fd);
    engine->output_length = 0;
    engine->output_dropping = 0;
    engine->dead = 1;
    if (engine->process_group > 0) {
        if (kill((pid_t)-engine->process_group, SIGKILL) < 0 && errno != ESRCH)
            (void)fail(engine, errno, "kill engine");
        engine->process_group = 0;
    }
    if (engine->pid > 0) {
        if (kill(engine->pid, SIGKILL) < 0 && errno != ESRCH)
            (void)fail(engine, errno, "kill engine");
        (void)engine_reap(engine);
    }
}
