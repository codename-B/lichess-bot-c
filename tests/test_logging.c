#define _POSIX_C_SOURCE 200809L
#include "logging.h"
#include "settings.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static unsigned checks;
static int saved_stderr = -1;
static int fixed_time_enabled;
static time_t fixed_time;

/* Override time() for rotation tests. */
time_t time(time_t *destination) {
    time_t value;
    if (fixed_time_enabled) value = fixed_time;
    else {
        struct timespec actual;
        value = clock_gettime(CLOCK_REALTIME, &actual) ? (time_t)-1 : actual.tv_sec;
    }
    if (destination) *destination = value;
    return value;
}

#define CHECK(test) do { ++checks; if (!(test)) { \
    if (saved_stderr >= 0) (void)dup2(saved_stderr, STDERR_FILENO); \
    fprintf(stderr, "FAIL %s:%d: %s (errno=%d)\n", __FILE__, __LINE__, #test, errno); \
    exit(1); } } while (0)

static void emit(const char *format, ...) {
    va_list args;
    va_start(args, format);
    logging_vwrite(format, args);
    va_end(args);
}

static void path_join(char out[4096], const char *directory, const char *name) {
    int length = snprintf(out, 4096, "%s/%s", directory, name);
    CHECK(length >= 0 && length < 4096);
}

static void write_file(const char *path, const char *contents) {
    FILE *file = fopen(path, "wb"); CHECK(file != NULL);
    size_t length = strlen(contents);
    CHECK(fwrite(contents, 1, length, file) == length);
    CHECK(fclose(file) == 0);
}

static char *read_file(const char *path) {
    FILE *file = fopen(path, "rb"); CHECK(file != NULL);
    CHECK(fseek(file, 0, SEEK_END) == 0);
    long length = ftell(file); CHECK(length >= 0);
    CHECK(fseek(file, 0, SEEK_SET) == 0);
    char *contents = malloc((size_t)length + 1); CHECK(contents != NULL);
    CHECK(fread(contents, 1, (size_t)length, file) == (size_t)length);
    contents[length] = 0;
    CHECK(fclose(file) == 0);
    return contents;
}

static int exists(const char *path) {
    struct stat info;
    if (lstat(path, &info) == 0) return 1;
    CHECK(errno == ENOENT || errno == ENOTDIR);
    return 0;
}

static unsigned occurrences(const char *contents, const char *needle) {
    unsigned count = 0;
    while ((contents = strstr(contents, needle)) != NULL) {
        ++count; contents += strlen(needle);
    }
    return count;
}

static void contains_once(const char *path, const char *needle) {
    char *contents = read_file(path);
    CHECK(occurrences(contents, needle) == 1);
    free(contents);
}

static void remove_tree(const char *directory) {
    DIR *dir = opendir(directory); CHECK(dir != NULL);
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char path[4096]; path_join(path, directory, entry->d_name);
        struct stat info; CHECK(lstat(path, &info) == 0);
        if (S_ISDIR(info.st_mode)) remove_tree(path);
        else CHECK(unlink(path) == 0);
    }
    CHECK(closedir(dir) == 0);
    CHECK(rmdir(directory) == 0);
}

static Settings *sample_settings(void) {
    char error[512];
    Settings *settings = settings_load_string(
        "token: synthetic-logging-config-token-08271\n"
        "url: https://example.invalid\n"
        "engine:\n  name: 'engine with spaces'\n  uci_options: {Hash: 128, Threads: 4}\n"
        "greeting: {hello: \"quote \\\" slash \\\\ newline\\nsecond line\"}\n"
        "challenge: {variants: [standard, atomic], accept_bot: true}\n"
        "unicode: \"\\u0080\\u0085\\u2028\\u2029\\u265e\\ufeff\\ufffe\\uffff\\U0001f600\"\n"
        "numbers: [4.0, -0.0, 1.0e+20, 1.0e-9, -.inf, .nan]\n"
        "nullable: null\nratio: 1.25\nlimit: .inf\n'literal.key': [3, false, null]\n",
        error, sizeof(error));
    CHECK(settings != NULL);
    return settings;
}

static void compare_setting(const Setting *actual, const Setting *expected) {
    CHECK(actual != NULL && expected != NULL);
    CHECK(setting_type(actual) == setting_type(expected));
    SettingType type = setting_type(expected);
    if (type == SETTING_MAP) {
        CHECK(setting_size(actual) == setting_size(expected));
        for (size_t i = 0; i < setting_size(expected); ++i) {
            const char *key = setting_key(expected, i);
            compare_setting(setting_get(actual, key), setting_get(expected, key));
        }
    } else if (type == SETTING_LIST) {
        CHECK(setting_size(actual) == setting_size(expected));
        for (size_t i = 0; i < setting_size(expected); ++i)
            compare_setting(setting_at(actual, i), setting_at(expected, i));
    } else if (type != SETTING_NULL) {
        CHECK(setting_text(actual) != NULL && setting_text(expected) != NULL);
        CHECK(!strcmp(setting_text(actual), setting_text(expected)));
    }
}

static void test_disabled(const char *root, const Settings *settings) {
    char error[256], path[4096];
    logging_close(); logging_close();
    CHECK(logging_open(NULL, NULL, settings, error, sizeof(error)) == 0);
    emit("disabled-log:%d:%s", 17, "stderr");
    logging_close();
    path_join(path, root, "config.log"); CHECK(!exists(path));
    path_join(path, root, "lichess-bot-c.log"); CHECK(!exists(path));
    DIR *dir = opendir(root); CHECK(dir != NULL);
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL)
        CHECK(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."));
    CHECK(closedir(dir) == 0);
    CHECK(logging_open(NULL, NULL, NULL, NULL, 0) == 0);
    emit("null-disabled-log");
    logging_close();
    emit("closed-logger-stderr");
}

static void test_explicit_append(const char *root, const Settings *settings) {
    char path[4096], snapshot[4096], error[256];
    path_join(path, root, "user.log");
    write_file(path, "original-user-log\n");
    CHECK(logging_open(path, NULL, settings, error, sizeof(error)) == 0);
    emit("explicit-first:%d:%s", 42, "formatted");
    logging_close();
    CHECK(logging_open(path, NULL, NULL, error, sizeof(error)) == 0);
    emit("explicit-second");
    logging_close();
    contains_once(path, "original-user-log\n");
    contains_once(path, "explicit-first:42:formatted");
    contains_once(path, "explicit-second");
    path_join(snapshot, root, "config.log"); CHECK(!exists(snapshot));
}

static void test_automatic_snapshot(const char *root, const Settings *settings) {
    char directory[4096], current[4096], snapshot[4096], explicit_path[4096], error[512];
    path_join(directory, root, "automatic dir/nested/leaf");
    path_join(explicit_path, root, "dual.log");
    CHECK(logging_open(explicit_path, directory, settings, error, sizeof(error)) == 0);
    emit("dual-sink-message:%d", 902);
    logging_close();
    path_join(current, directory, "lichess-bot-c.log");
    path_join(snapshot, directory, "config.log");
    contains_once(current, "dual-sink-message:902");
    contains_once(explicit_path, "dual-sink-message:902");
    char *contents = read_file(snapshot);
    CHECK(strstr(contents, "synthetic-logging-config-token-08271") == NULL);
    free(contents);
    Settings *saved = settings_load_file(snapshot, error, sizeof(error));
    CHECK(saved != NULL);
    CHECK(setting_type(settings_get(saved, "")) == SETTING_MAP);
    CHECK(setting_size(settings_get(saved, "")) == setting_size(settings_get(settings, "")));
    const char *redacted = settings_str(saved, "token", NULL);
    CHECK(redacted != NULL && *redacted);
    CHECK(strcmp(redacted, settings_str(settings, "token", "")) != 0);
    const Setting *original_root = settings_get(settings, "");
    for (size_t i = 0; i < setting_size(original_root); ++i) {
        const char *key = setting_key(original_root, i);
        if (strcmp(key, "token"))
            compare_setting(setting_get(settings_get(saved, ""), key), setting_get(original_root, key));
    }
    Settings *untouched = sample_settings();
    compare_setting(settings_get(settings, ""), settings_get(untouched, ""));
    settings_free(untouched); settings_free(saved);

    path_join(directory, root, "null-settings");
    CHECK(logging_open(NULL, directory, NULL, error, sizeof(error)) == 0);
    emit("null-settings-automatic");
    logging_close();
    path_join(current, directory, "lichess-bot-c.log");
    path_join(snapshot, directory, "config.log");
    contains_once(current, "null-settings-automatic"); CHECK(!exists(snapshot));
    write_file(snapshot, "preserved-existing-snapshot\n");
    CHECK(logging_open(NULL, directory, NULL, error, sizeof(error)) == 0);
    logging_close();
    contains_once(snapshot, "preserved-existing-snapshot\n");
}

static time_t days_ago(time_t now, unsigned days) {
    return now - (time_t)days * (time_t)86400;
}

static void archive_path(char out[4096], const char *directory, time_t when) {
    struct tm utc; CHECK(gmtime_r(&when, &utc) != NULL);
    char name[64];
    CHECK(strftime(name, sizeof(name), "lichess-bot-c.log.%Y-%m-%d", &utc) == 28);
    path_join(out, directory, name);
}

static void set_mtime(const char *path, time_t when) {
    struct timespec times[2] = {{when, 0}, {when, 0}};
    CHECK(utimensat(AT_FDCWD, path, times, 0) == 0);
}

static void test_rotation_and_retention(const char *root) {
    char directory[4096], current[4096], path[4096], collision[4096], error[512];
    path_join(directory, root, "rotation"); CHECK(mkdir(directory, 0700) == 0);
    path_join(current, directory, "lichess-bot-c.log");
    time_t now = time(NULL); CHECK(now != (time_t)-1);
    /* Identical mtimes; retain by filename. */
    const unsigned ages[] = {40, 500, 2, 120, 9, 800, 17, 70, 4, 300, 23, 10};
    for (size_t i = 0; i < sizeof(ages) / sizeof(ages[0]); ++i) {
        archive_path(path, directory, days_ago(now, ages[i]));
        char contents[80];
        int length = snprintf(contents, sizeof(contents), "archive-age-%u\n", ages[i]);
        CHECK(length > 0 && (size_t)length < sizeof(contents));
        write_file(path, contents); set_mtime(path, days_ago(now, 1));
    }
    const char *unrelated[] = {"notes.log", "lichess-bot-c.log.backup",
        "lichess-bot-c.log.2000-01-01.extra", "lichess-bot-c.log.2000-99-77"};
    for (size_t i = 0; i < sizeof(unrelated) / sizeof(unrelated[0]); ++i) {
        path_join(path, directory, unrelated[i]); write_file(path, "unrelated-preserved\n");
    }
    path_join(path, directory, "lichess-bot-c.log.1990-01-01");
    CHECK(mkdir(path, 0700) == 0);
    path_join(path, directory, "lichess-bot-c.log.1989-01-01");
    CHECK(symlink("notes.log", path) == 0);
    write_file(current, "old-current-preserved\n");
    set_mtime(current, days_ago(now, 2));
    archive_path(collision, directory, days_ago(now, 2));
    CHECK(logging_open(NULL, directory, NULL, error, sizeof(error)) == 0);
    emit("new-current-message");
    logging_close();
    char *contents = read_file(collision);
    CHECK(occurrences(contents, "archive-age-2\n") == 1);
    CHECK(occurrences(contents, "old-current-preserved\n") == 1);
    CHECK(strstr(contents, "archive-age-2\n") < strstr(contents, "old-current-preserved\n"));
    free(contents);
    contents = read_file(current);
    CHECK(strstr(contents, "old-current-preserved") == NULL);
    CHECK(occurrences(contents, "new-current-message") == 1); free(contents);
    for (size_t i = 0; i < sizeof(ages) / sizeof(ages[0]); ++i) {
        archive_path(path, directory, days_ago(now, ages[i]));
        CHECK(exists(path) == (ages[i] <= 40));
    }
    for (size_t i = 0; i < sizeof(unrelated) / sizeof(unrelated[0]); ++i) {
        path_join(path, directory, unrelated[i]); contains_once(path, "unrelated-preserved\n");
    }
    struct stat info;
    path_join(path, directory, "lichess-bot-c.log.1990-01-01");
    CHECK(lstat(path, &info) == 0 && S_ISDIR(info.st_mode));
    path_join(path, directory, "lichess-bot-c.log.1989-01-01");
    CHECK(lstat(path, &info) == 0 && S_ISLNK(info.st_mode));

    /* Same-day restarts prune without rotating. */
    archive_path(path, directory, days_ago(now, 900)); write_file(path, "late-old-archive\n");
    CHECK(logging_open(NULL, directory, NULL, error, sizeof(error)) == 0);
    emit("same-day-restart");
    logging_close();
    CHECK(!exists(path));
    contains_once(current, "new-current-message");
    contains_once(current, "same-day-restart");
    contains_once(collision, "old-current-preserved\n");
}

static void test_rotation_without_collision(const char *root) {
    char directory[4096], current[4096], archive[4096], error[256];
    path_join(directory, root, "first-rotation"); CHECK(mkdir(directory, 0700) == 0);
    path_join(current, directory, "lichess-bot-c.log");
    time_t now = time(NULL); CHECK(now != (time_t)-1);
    write_file(current, "first-old-current\n");
    set_mtime(current, days_ago(now, 3));
    archive_path(archive, directory, days_ago(now, 3)); CHECK(!exists(archive));
    CHECK(logging_open(NULL, directory, NULL, error, sizeof(error)) == 0);
    emit("first-rotation-new-current");
    logging_close();
    char *contents = read_file(archive);
    CHECK(!strcmp(contents, "first-old-current\n")); free(contents);
    contents = read_file(current);
    CHECK(strstr(contents, "first-old-current") == NULL);
    CHECK(occurrences(contents, "first-rotation-new-current") == 1); free(contents);
}

static void test_live_midnight(const char *root) {
    char directory[4096], current[4096], archive[4096], path[4096], explicit_path[4096], error[256];
    path_join(directory, root, "live-midnight"); CHECK(mkdir(directory, 0700) == 0);
    path_join(current, directory, "lichess-bot-c.log");
    path_join(explicit_path, root, "live-midnight-explicit.log");
    time_t actual = time(NULL); CHECK(actual != (time_t)-1);
    time_t day = actual - actual % (time_t)86400;
    fixed_time = day + (time_t)43200; fixed_time_enabled = 1;
    for (unsigned age = 1; age <= 7; ++age) {
        archive_path(path, directory, days_ago(fixed_time, age));
        write_file(path, "prior-live-archive\n");
    }
    CHECK(logging_open(explicit_path, directory, NULL, error, sizeof(error)) == 0);
    emit("live-before-midnight");
    fixed_time = day + (time_t)86400;
    emit("live-after-midnight");
    archive_path(archive, directory, day);
    char *contents = read_file(archive);
    CHECK(occurrences(contents, "live-before-midnight") == 1);
    CHECK(strstr(contents, "live-after-midnight") == NULL); free(contents);
    for (unsigned age = 1; age <= 7; ++age) {
        archive_path(path, directory, days_ago(day, age));
        CHECK(exists(path) == (age < 7));
    }
    /* Clock corrections must not overwrite yesterday's log. */
    fixed_time = day + (time_t)86399;
    emit("live-backwards-clock");
    fixed_time = day + (time_t)86410;
    emit("live-caught-up");
    logging_close(); fixed_time_enabled = 0;
    contents = read_file(current);
    CHECK(strstr(contents, "live-before-midnight") == NULL);
    CHECK(occurrences(contents, "live-after-midnight") == 1);
    CHECK(occurrences(contents, "live-backwards-clock") == 1);
    CHECK(occurrences(contents, "live-caught-up") == 1); free(contents);
    contents = read_file(archive);
    CHECK(occurrences(contents, "live-before-midnight") == 1);
    CHECK(strstr(contents, "live-backwards-clock") == NULL); free(contents);
    contents = read_file(explicit_path);
    const char *messages[] = {"live-before-midnight", "live-after-midnight", "live-backwards-clock", "live-caught-up"};
    const char *previous = contents;
    for (size_t i = 0; i < sizeof(messages) / sizeof(messages[0]); ++i) {
        const char *found = strstr(contents, messages[i]);
        CHECK(found != NULL && occurrences(contents, messages[i]) == 1);
        CHECK(found >= previous); previous = found;
    }
    free(contents);

    path_join(directory, root, "same-log-destination");
    path_join(current, directory, "lichess-bot-c.log");
    CHECK(logging_open(current, directory, NULL, error, sizeof(error)) == 0);
    emit("same-destination-once"); logging_close();
    contains_once(current, "same-destination-once");
}

static void test_failures(const char *root, const Settings *settings) {
    char blocker[4096], bad_directory[4096], good_path[4096], path[4096], error[256];
    path_join(blocker, root, "directory-blocker"); write_file(blocker, "keep-blocker\n");
    path_join(bad_directory, blocker, "cannot-create");
    path_join(good_path, root, "before-failure.log");
    CHECK(logging_open(good_path, NULL, NULL, error, sizeof(error)) == 0);
    emit("before-reopen-failure");
    error[0] = 0;
    CHECK(logging_open(NULL, bad_directory, settings, error, sizeof(error)) == -1);
    CHECK(*error != 0);
    emit("after-reopen-failure-stderr-only");
    logging_close();
    char *contents = read_file(good_path);
    CHECK(strstr(contents, "before-reopen-failure") != NULL);
    CHECK(strstr(contents, "after-reopen-failure") == NULL); free(contents);
    contains_once(blocker, "keep-blocker\n");

    error[0] = 0;
    CHECK(logging_open(root, NULL, NULL, error, sizeof(error)) == -1);
    CHECK(*error != 0); emit("explicit-directory-failure-stderr"); logging_close();
    path_join(bad_directory, root, "bad-current"); CHECK(mkdir(bad_directory, 0700) == 0);
    path_join(path, bad_directory, "lichess-bot-c.log"); CHECK(mkdir(path, 0700) == 0);
    error[0] = 0;
    CHECK(logging_open(NULL, bad_directory, NULL, error, sizeof(error)) == -1);
    CHECK(*error != 0); logging_close();

    path_join(bad_directory, root, "bad-snapshot"); CHECK(mkdir(bad_directory, 0700) == 0);
    path_join(path, bad_directory, "config.log"); CHECK(mkdir(path, 0700) == 0);
    error[0] = 0;
    CHECK(logging_open(NULL, bad_directory, settings, error, sizeof(error)) == -1);
    CHECK(*error != 0); emit("snapshot-failure-stderr"); logging_close();
    CHECK(logging_open(NULL, bad_directory, NULL, error, sizeof(error)) == 0);
    logging_close();

    char guarded[8]; memset(guarded, 'X', sizeof(guarded));
    CHECK(logging_open(root, NULL, NULL, guarded + 2, 3) == -1);
    CHECK(guarded[0] == 'X' && guarded[1] == 'X');
    CHECK(guarded[5] == 'X' && guarded[6] == 'X' && guarded[7] == 'X');
    CHECK(memchr(guarded + 2, 0, 3) != NULL);
    CHECK(logging_open(root, NULL, NULL, NULL, 0) == -1);
    logging_close(); logging_close();
}

static void test_deferred(const char *root) {
    char path[4096],second[4096],error[256];
    path_join(path,root,"deferred.log");path_join(second,root,"deferred-second.log");
    CHECK(logging_open(path,NULL,NULL,error,sizeof(error))==0);
    logging_defer(1);
    emit("queued-not-yet-written:%d",17);
    char *data=read_file(path);CHECK(!*data);free(data);
    logging_flush();contains_once(path,"queued-not-yet-written:17");
    for(int i=0;i<300;++i)emit("bounded-queue-%03d",i);
    char large[8193];memset(large,'x',sizeof(large)-1);large[sizeof(large)-1]=0;
    emit("large-prefix:%s:large-suffix",large);
    emit("drained-on-reopen");
    CHECK(logging_open(second,NULL,NULL,error,sizeof(error))==0);
    contains_once(path,"drained-on-reopen");
    data=read_file(path);
    CHECK(occurrences(data,"bounded-queue-")==300);
    const char *begin=strstr(data,"large-prefix:"),*end=strstr(data,":large-suffix");
    CHECK(begin!=NULL && end!=NULL && end-begin==(ptrdiff_t)(strlen("large-prefix:")+8192));
    free(data);
    logging_defer(1);emit("drained-on-disable");logging_defer(0);
    contains_once(second,"drained-on-disable");
    logging_defer(1);emit("drained-on-close");logging_close();
    contains_once(second,"drained-on-close");
    /* Deferred logs retain enqueue timestamps. */
    char directory[4096],current[4096],archive[4096];
    path_join(directory,root,"deferred-midnight");
    time_t actual=time(NULL),day=actual-actual%(time_t)86400;
    fixed_time_enabled=1;fixed_time=day+(time_t)86399;
    CHECK(logging_open(NULL,directory,NULL,error,sizeof(error))==0);
    logging_defer(1);emit("deferred-before-midnight");
    fixed_time=day+(time_t)86401;emit("deferred-after-midnight");logging_close();fixed_time_enabled=0;
    path_join(current,directory,"lichess-bot-c.log");archive_path(archive,directory,day);
    contains_once(archive,"deferred-before-midnight");contains_once(current,"deferred-after-midnight");
}

int main(void) {
    char root[] = "/tmp/lbc-logging-test-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    int previous_directory = open(".", O_RDONLY); CHECK(previous_directory >= 0);
    CHECK(chdir(root) == 0);
    FILE *stderr_capture = tmpfile(); CHECK(stderr_capture != NULL);
    CHECK(fflush(stderr) == 0);
    saved_stderr = dup(STDERR_FILENO); CHECK(saved_stderr >= 0);
    CHECK(dup2(fileno(stderr_capture), STDERR_FILENO) >= 0);
    /* Different local date detects localtime() use. */
    time_t now = time(NULL); struct tm utc;
    CHECK(now != (time_t)-1 && gmtime_r(&now, &utc) != NULL);
    CHECK(setenv("TZ", utc.tm_hour < 12 ? "LBC+12" : "LBC-14", 1) == 0);
    tzset();
    Settings *settings = sample_settings();
    test_disabled(root, settings);
    test_explicit_append(root, settings);
    test_automatic_snapshot(root, settings);
    test_rotation_and_retention(root);
    test_rotation_without_collision(root);
    test_live_midnight(root);
    test_failures(root, settings);
    test_deferred(root);
    settings_free(settings);
    logging_close();
    CHECK(fflush(stderr) == 0);
    CHECK(dup2(saved_stderr, STDERR_FILENO) >= 0);
    CHECK(close(saved_stderr) == 0); saved_stderr = -1;
    CHECK(fseek(stderr_capture, 0, SEEK_END) == 0);
    long length = ftell(stderr_capture); CHECK(length >= 0);
    CHECK(fseek(stderr_capture, 0, SEEK_SET) == 0);
    char *captured = malloc((size_t)length + 1); CHECK(captured != NULL);
    CHECK(fread(captured, 1, (size_t)length, stderr_capture) == (size_t)length);
    captured[length] = 0;
    const char *expected[] = {"disabled-log:17:stderr", "null-disabled-log",
        "closed-logger-stderr", "explicit-first:42:formatted", "explicit-second",
        "dual-sink-message:902", "null-settings-automatic", "new-current-message",
        "same-day-restart", "first-rotation-new-current", "before-reopen-failure", "after-reopen-failure-stderr-only",
        "explicit-directory-failure-stderr", "snapshot-failure-stderr", "live-before-midnight",
        "live-after-midnight", "live-backwards-clock", "live-caught-up", "same-destination-once"};
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i)
        CHECK(occurrences(captured, expected[i]) == 1);
    free(captured); CHECK(fclose(stderr_capture) == 0);
    CHECK(fchdir(previous_directory) == 0); CHECK(close(previous_directory) == 0);
    remove_tree(root);
    printf("logging: %u checks passed\n", checks);
    return 0;
}
