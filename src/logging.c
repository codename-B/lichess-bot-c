#define _POSIX_C_SOURCE 200809L
#include "logging.h"
#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LOG_PATH_CAP 4096U
#define LOG_BASENAME "lichess-bot-c.log"
#define LOG_PREFIX LOG_BASENAME "."
#define LOG_BACKUPS 7U
#define LOG_QUEUE_RECORDS 128U
#define LOG_QUEUE_TEXT 2048U

static struct {
    FILE *explicit_file, *automatic_file;
    char directory[LOG_PATH_CAP], path[LOG_PATH_CAP];
    int64_t day;
    time_t stamp;
    int deferred;
    size_t queued;
    struct { time_t when; char text[LOG_QUEUE_TEXT]; } queue[LOG_QUEUE_RECORDS];
} logger;

static void set_error(char *error, size_t cap, const char *message) {
    if (error && cap) (void)snprintf(error, cap, "%s", message);
}

static int path_join(char *out, size_t cap, const char *directory, const char *name) {
    size_t length = strlen(directory);
    if (lbc_format(out, cap, "%s%s%s", directory,
                   length && directory[length - 1] == '/' ? "" : "/", name)) {
        errno = ENAMETOOLONG; return -1;
    }
    return 0;
}

static int64_t utc_day(time_t stamp) {
    int64_t value = (int64_t)stamp;
    return value / INT64_C(86400) - (value < 0 && value % INT64_C(86400) != 0);
}

static int dated_name(time_t stamp, char *out, size_t cap) {
    struct tm date;
    char suffix[32];
    if (!gmtime_r(&stamp, &date) || strftime(suffix, sizeof(suffix), "%Y-%m-%d", &date) != 10 ||
        lbc_format(out, cap, LOG_PREFIX "%s", suffix)) { errno = EINVAL; return -1; }
    return 0;
}

/* Only regular files with an exact, valid date suffix are log archives. */
static int archive_name(const char *name) {
    size_t prefix = sizeof(LOG_PREFIX) - 1;
    if (strlen(name) != prefix + 10 || memcmp(name, LOG_PREFIX, prefix)) return 0;
    const char *date = name + prefix;
    for (size_t i = 0; i < 10; ++i)
        if (i == 4 || i == 7) { if (date[i] != '-') return 0; }
        else if (date[i] < '0' || date[i] > '9') return 0;
    unsigned year = (unsigned)(date[0] - '0') * 1000U + (unsigned)(date[1] - '0') * 100U +
                    (unsigned)(date[2] - '0') * 10U + (unsigned)(date[3] - '0');
    unsigned month = (unsigned)(date[5] - '0') * 10U + (unsigned)(date[6] - '0');
    unsigned day = (unsigned)(date[8] - '0') * 10U + (unsigned)(date[9] - '0');
    if (!year || month < 1 || month > 12 || day < 1) return 0;
    static const unsigned lengths[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    unsigned days = lengths[month - 1];
    if (month == 2 && (!(year % 400U) || (!(year % 4U) && year % 100U))) ++days;
    return day <= days;
}

static int regular_archive(const char *name, char *path, size_t cap) {
    if (!archive_name(name)) return 0;
    if (path_join(path, cap, logger.directory, name)) return -1;
    struct stat file;
    if (lstat(path, &file)) return errno == ENOENT ? 0 : -1;
    return S_ISREG(file.st_mode) ? 1 : 0;
}

static int prune_archives(void) {
    DIR *directory = opendir(logger.directory);
    if (!directory) return -1;
    char newest[LOG_BACKUPS][64] = {{0}}, path[LOG_PATH_CAP];
    size_t count = 0;
    int result = 0, saved_errno = 0;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (!entry) { if (errno) { result = -1; saved_errno = errno; } break; }
        int regular = regular_archive(entry->d_name, path, sizeof(path));
        if (regular < 0) { result = -1; saved_errno = errno; break; }
        if (!regular) continue;
        size_t at = 0;
        while (at < count && strcmp(entry->d_name, newest[at]) < 0) ++at;
        if (at >= LOG_BACKUPS) continue;
        size_t last = count < LOG_BACKUPS ? count++ : LOG_BACKUPS - 1;
        for (size_t i = last; i > at; --i) memcpy(newest[i], newest[i - 1], sizeof(newest[i]));
        (void)lbc_copy(newest[at], sizeof(newest[at]), entry->d_name);
    }
    if (!result && count == LOG_BACKUPS) {
        rewinddir(directory);
        for (;;) {
            errno = 0;
            struct dirent *entry = readdir(directory);
            if (!entry) { if (errno) { result = -1; saved_errno = errno; } break; }
            if (strcmp(entry->d_name, newest[LOG_BACKUPS - 1]) >= 0) continue;
            int regular = regular_archive(entry->d_name, path, sizeof(path));
            if (regular < 0 || (regular && unlink(path) && errno != ENOENT)) {
                result = -1; saved_errno = errno; break;
            }
        }
    }
    if (closedir(directory) && !result) { result = -1; saved_errno = errno; }
    if (result) errno = saved_errno;
    return result;
}

static FILE *open_append(const char *path, int automatic) {
    int flags = O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC;
    if (automatic) flags |= O_NOFOLLOW | O_NONBLOCK;
    int fd = open(path, flags, automatic ? 0600 : 0666);
    if (fd < 0) return NULL;
    struct stat file;
    if (automatic && (fstat(fd, &file) || !S_ISREG(file.st_mode))) {
        (void)close(fd); errno = EINVAL; return NULL;
    }
    FILE *stream = fdopen(fd, "a");
    if (!stream) { int saved = errno; (void)close(fd); errno = saved; }
    return stream;
}

static int copy_append(int source, int destination) {
    char data[16384];
    for (;;) {
        ssize_t count = read(source, data, sizeof(data));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return -1;
        if (!count) return 0;
        size_t at = 0;
        while (at < (size_t)count) {
            ssize_t written = write(destination, data + at, (size_t)count - at);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) { if (!written) errno = EIO; return -1; }
            at += (size_t)written;
        }
    }
}

static int rotate_current(time_t stamp) {
    char name[64], archive[LOG_PATH_CAP];
    if (dated_name(stamp, name, sizeof(name)) ||
        path_join(archive, sizeof(archive), logger.directory, name)) return -1;
    int source = open(logger.path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (source < 0) return errno == ENOENT ? 0 : -1;
    struct stat original;
    if (fstat(source, &original) || !S_ISREG(original.st_mode)) {
        (void)close(source); errno = EINVAL; return -1;
    }
    /* link/unlink avoids overwriting an existing archive; copy if links fail. */
    if (!link(logger.path, archive)) {
        (void)close(source); return unlink(logger.path);
    }
    int destination = open(archive, O_WRONLY | O_APPEND | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (destination < 0 && errno == EEXIST)
        destination = open(archive, O_WRONLY | O_APPEND | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (destination < 0) { int saved = errno; (void)close(source); errno = saved; return -1; }
    struct stat target;
    if (fstat(destination, &target) || !S_ISREG(target.st_mode)) {
        (void)close(source); (void)close(destination); errno = EINVAL; return -1;
    }
    if (original.st_dev == target.st_dev && original.st_ino == target.st_ino) {
        (void)close(source); (void)close(destination); return unlink(logger.path);
    }
    int result = copy_append(source, destination);
    if (!result && fsync(destination)) result = -1;
    int saved = errno;
    if (result && ftruncate(destination, target.st_size)) saved = errno;
    if (close(destination) && !result) { result = -1; saved = errno; }
    (void)close(source);
    if (!result) return unlink(logger.path);
    errno = saved; return -1;
}

static int open_automatic(time_t now) {
    struct stat previous;
    if (!lstat(logger.path, &previous)) {
        if (!S_ISREG(previous.st_mode)) { errno = EINVAL; return -1; }
        if (utc_day(previous.st_mtime) < utc_day(now) && rotate_current(previous.st_mtime)) return -1;
    } else if (errno != ENOENT) return -1;
    if (prune_archives()) return -1;
    logger.automatic_file = open_append(logger.path, 1);
    if (!logger.automatic_file) return -1;
    logger.day = utc_day(now); logger.stamp = now;
    return 0;
}

static int quote_yaml(FILE *stream, const char *text) {
    if (fputc('"', stream) == EOF) return -1;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p == '"' || *p == '\\') {
            if (fputc('\\', stream) == EOF || fputc((int)*p, stream) == EOF) return -1;
        } else if (*p < 32 || *p == 127) {
            if (fprintf(stream, "\\u%04x", (unsigned)*p) < 0) return -1;
        } else if (p[0] == 0xc2 && p[1] >= 0x80 && p[1] <= 0x9f) {
            if (fprintf(stream, "\\u%04x", (unsigned)p[1]) < 0) return -1;
            ++p;
        } else if (p[0] == 0xe2 && p[1] == 0x80 && (p[2] == 0xa8 || p[2] == 0xa9)) {
            if (fprintf(stream, "\\u%04x", 0x2000U + (unsigned)p[2] - 0x80U) < 0) return -1;
            p += 2;
        } else if (p[0] == 0xef && ((p[1] == 0xbb && p[2] == 0xbf) ||
                                  (p[1] == 0xbf && (p[2] == 0xbe || p[2] == 0xbf)))) {
            unsigned code = 0xf000U + ((unsigned)p[1] & 0x3fU) * 64U + ((unsigned)p[2] & 0x3fU);
            if (fprintf(stream, "\\u%04x", code) < 0) return -1;
            p += 2;
        } else if (fputc((int)*p, stream) == EOF) return -1;
    }
    return fputc('"', stream) == EOF ? -1 : 0;
}

static int indent_yaml(FILE *stream, unsigned depth) {
    for (unsigned i = 0; i < depth * 2U; ++i) if (fputc(' ', stream) == EOF) return -1;
    return 0;
}

/* Use YAML for infinite defaults. Redact the token before writing. */
static int write_setting(FILE *stream, const Setting *value, unsigned depth) {
    if (depth > 64) { errno = EINVAL; return -1; }
    SettingType type = setting_type(value);
    if (type == SETTING_STRING) return quote_yaml(stream, setting_str(value, ""));
    if (type == SETTING_NULL) return fputs("null", stream) == EOF ? -1 : 0;
    if (type == SETTING_BOOL) return fputs(setting_bool(value, 0) ? "true" : "false", stream) == EOF ? -1 : 0;
    if (type == SETTING_INT) return fprintf(stream, "%" PRId64, setting_int(value, 0)) < 0 ? -1 : 0;
    if (type == SETTING_DOUBLE) {
        double number = setting_double(value, 0);
        if (isnan(number)) return fputs(".nan", stream) == EOF ? -1 : 0;
        if (isinf(number)) return fputs(number < 0 ? "-.inf" : ".inf", stream) == EOF ? -1 : 0;
        char text[64];
        (void)snprintf(text, sizeof(text), "%.17g", number);
        /* Preserve a floating-point scalar even when its value is integral. */
        if (!strchr(text, '.')) {
            char *exponent = strchr(text, 'e');
            if (!exponent) exponent = strchr(text, 'E');
            if (!exponent) exponent = text + strlen(text);
            memmove(exponent + 2, exponent, strlen(exponent) + 1);
            exponent[0] = '.'; exponent[1] = '0';
        }
        return fputs(text, stream) == EOF ? -1 : 0;
    }
    int map = type == SETTING_MAP;
    size_t count = setting_size(value);
    if (fputc(map ? '{' : '[', stream) == EOF) return -1;
    for (size_t i = 0; i < count; ++i) {
        if (fputs(i ? ",\n" : "\n", stream) == EOF || indent_yaml(stream, depth + 1)) return -1;
        const char *key = map ? setting_key(value, i) : NULL;
        if (map && (quote_yaml(stream, key) || fputs(": ", stream) == EOF)) return -1;
        if (depth == 0 && key && !strcmp(key, "token")) {
            if (quote_yaml(stream, "[redacted]")) return -1;
        } else if (write_setting(stream, setting_at(value, i), depth + 1)) return -1;
    }
    if (count && (fputc('\n', stream) == EOF || indent_yaml(stream, depth))) return -1;
    return fputc(map ? '}' : ']', stream) == EOF ? -1 : 0;
}

static int write_snapshot(const Settings *settings) {
    if (!settings) return 0;
    char path[LOG_PATH_CAP], temporary[LOG_PATH_CAP];
    if (path_join(path, sizeof(path), logger.directory, "config.log") ||
        lbc_format(temporary, sizeof(temporary), "%s.tmp.XXXXXX", path)) { errno = ENAMETOOLONG; return -1; }
    int fd = mkstemp(temporary);
    if (fd < 0) return -1;
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    FILE *stream = fdopen(fd, "w");
    if (!stream) { int saved = errno; (void)close(fd); (void)unlink(temporary); errno = saved; return -1; }
    int result = fputs("# Normalized configuration; authentication token redacted.\n", stream) == EOF ? -1 : 0;
    if (!result) result = write_setting(stream, settings_get(settings, ""), 0);
    if (!result && (fputc('\n', stream) == EOF || fflush(stream) || fsync(fileno(stream)))) result = -1;
    int saved = errno;
    if (fclose(stream) && !result) { result = -1; saved = errno; }
    if (!result && rename(temporary, path)) { result = -1; saved = errno; }
    if (result) { (void)unlink(temporary); errno = saved; }
    return result;
}

void logging_close(void) {
    logging_flush();
    if (logger.explicit_file) (void)fclose(logger.explicit_file);
    if (logger.automatic_file) (void)fclose(logger.automatic_file);
    memset(&logger, 0, sizeof(logger));
}

int logging_open(const char *explicit_path, const char *automatic_directory,
                 const Settings *settings, char *error, size_t error_cap) {
    logging_close(); set_error(error, error_cap, "");
    const char *failure = NULL;
    if (automatic_directory) {
        if (!*automatic_directory || lbc_copy(logger.directory, sizeof(logger.directory), automatic_directory) ||
            path_join(logger.path, sizeof(logger.path), automatic_directory, LOG_BASENAME)) {
            failure = "Invalid or oversized automatic log directory"; goto failed;
        }
        if (lbc_mkdirs(logger.directory) || open_automatic(time(NULL))) {
            failure = "Cannot create, rotate or open automatic logs"; goto failed;
        }
    }
    if (explicit_path && *explicit_path) {
        logger.explicit_file = open_append(explicit_path, 0);
        if (!logger.explicit_file) { failure = "Cannot open requested log file"; goto failed; }
        /* Avoid duplicate writes when --logfile is also the automatic log. */
        if (logger.automatic_file) {
            struct stat explicit_stat, automatic_stat;
            if (!fstat(fileno(logger.explicit_file), &explicit_stat) &&
                !fstat(fileno(logger.automatic_file), &automatic_stat) &&
                explicit_stat.st_dev == automatic_stat.st_dev && explicit_stat.st_ino == automatic_stat.st_ino) {
                (void)fclose(logger.explicit_file); logger.explicit_file = NULL;
            }
        }
    }
    if (automatic_directory && write_snapshot(settings)) {
        failure = "Cannot write redacted configuration snapshot"; goto failed;
    }
    return 0;
failed:
    set_error(error, error_cap, failure);
    logging_close(); return -1;
}

static void write_line(FILE *stream, const char *timestamp, const char *format, va_list arguments) {
    if (!format) return;
    va_list copy; va_copy(copy, arguments);
    if (timestamp && *timestamp) (void)fprintf(stream, "%s ", timestamp);
    (void)vfprintf(stream, format, copy); va_end(copy);
    (void)fputc('\n', stream); (void)fflush(stream);
}

/* Flush logs after pending moves. */
static void logging_emit(time_t now, const char *format, va_list arguments) {
    if (logger.automatic_file && utc_day(now) > logger.day) {
        (void)fclose(logger.automatic_file); logger.automatic_file = NULL;
        if (rotate_current(logger.stamp) || prune_archives() || !(logger.automatic_file = open_append(logger.path, 1))) {
            (void)fprintf(stderr, "Automatic log rotation failed; stderr and explicit logging remain available.\n");
        } else { logger.day = utc_day(now); logger.stamp = now; }
    }
    char timestamp[32] = "";
    if (logger.explicit_file || logger.automatic_file) {
        struct tm date;
        if (gmtime_r(&now, &date)) (void)strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &date);
    }
    if (logger.explicit_file) write_line(logger.explicit_file, timestamp, format, arguments);
    if (logger.automatic_file) write_line(logger.automatic_file, timestamp, format, arguments);
    write_line(stderr, NULL, format, arguments);
}

static void emit_record(time_t when, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    logging_emit(when, format, arguments);
    va_end(arguments);
}

void logging_flush(void) {
    size_t count = logger.queued;
    logger.queued = 0;
    for (size_t i = 0; i < count; ++i)
        emit_record(logger.queue[i].when, "%s", logger.queue[i].text);
}

void logging_defer(int enabled) {
    if (!enabled) logging_flush();
    logger.deferred = enabled != 0;
}

void logging_vwrite(const char *format, va_list arguments) {
    time_t now = time(NULL);
    if (!logger.deferred) { logging_emit(now, format, arguments); return; }
    if (logger.queued == LOG_QUEUE_RECORDS) logging_flush();
    size_t at = logger.queued;
    va_list copy;
    va_copy(copy, arguments);
    int n = vsnprintf(logger.queue[at].text, LOG_QUEUE_TEXT, format, copy);
    va_end(copy);
    if (n < 0 || (size_t)n >= LOG_QUEUE_TEXT) {
        /* Write oversized lines in full. */
        logging_flush(); logging_emit(now, format, arguments); return;
    }
    logger.queue[at].when = now;
    ++logger.queued;
}
