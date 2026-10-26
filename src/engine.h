#ifndef LICHESS_BOT_C_ENGINE_H
#define LICHESS_BOT_C_ENGINE_H

#include <stddef.h>
#include <sys/types.h>

#define ENGINE_BUFFER_SIZE (128u * 1024u)
#define ENGINE_READ_BUDGET (256u * 1024u)

typedef struct Engine {
    pid_t pid;
    int input_fd;              /* Parent writes engine stdin. */
    int output_fd;             /* Parent reads engine stdout. */
    int dead;                  /* No further commands may be queued. */
    int wait_status;           /* waitpid status, or -1 when unavailable. */
    int process_group;         /* Owned group also contains engine descendants. */
    char error[160];

    size_t input_offset;
    size_t input_length;
    size_t output_length;
    unsigned long dropped_lines;
    int output_dropping;
    char input_buffer[ENGINE_BUFFER_SIZE];
    char output_buffer[ENGINE_BUFFER_SIZE + 1u];
} Engine;

/* Initialize before use; never reinitialize a live child. */
void engine_init(Engine *engine);

/* Spawn with execvp; NULL workdir inherits cwd. No shell, token, or extra fds.
 * Wait for exec confirmation, then use nonblocking pipes. stderr is inherited.
 * Failures return -1 and set errno and error. */
int engine_spawn(Engine *engine, char *const argv[], const char *workdir);
/* Optionally suppress stderr. */
int engine_spawn_ex(Engine *engine, char *const argv[], const char *workdir,
                    int silence_stderr);
/* Share the adapter's process group so shutdown also kills its engine. */
int engine_spawn_attached(Engine *engine, char *const argv[], const char *workdir);

/* Queue exactly len bytes, adding no newline. A failed append queues nothing. */
int engine_queue(Engine *engine, const char *data, size_t len);

/* Flush until empty, interrupted, or the pipe would block. All return 0. */
int engine_flush(Engine *engine);

/* Read up to ENGINE_READ_BUDGET bytes; return -1 on EOF/error after delivery.
 * Lines are NUL-terminated without CR/LF and borrowed until callback return.
 * Deliver an unfinished line at EOF; discard oversized lines and count them.
 * May drain output after reaping. Callbacks may queue commands but not change
 * engine lifecycle. */
int engine_read(Engine *engine,
                void (*on_line)(void *, const char *, size_t), void *context);

int engine_wants_write(const Engine *engine);

/* 0: child still running; 1: reaped/already gone; -1: waitpid failed. Never waits. */
int engine_reap(Engine *engine);

/* Close pipes, SIGKILL the child group, and reap without waiting.
 * If pid stays positive, keep calling engine_reap. Safe to call repeatedly. */
void engine_terminate(Engine *engine);

#endif
