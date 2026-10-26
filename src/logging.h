#ifndef LBC_LOGGING_H
#define LBC_LOGGING_H

#include "settings.h"
#include <stdarg.h>
#include <stddef.h>

/* Call from the event-loop thread.
 * Reopening closes old files. Empty explicit_path disables the user log;
 * NULL automatic_directory disables automatic logs and config snapshots.
 * Rotate daily in UTC; retain seven dated regular archives.
 * If settings is supplied, write config.log atomically with its token redacted.
 * Return 0 on success, -1 on error. stderr always remains available. */
int logging_open(const char *explicit_path, const char *automatic_directory,
                 const Settings *settings, char *error, size_t error_cap);
void logging_vwrite(const char *format, va_list arguments);
/* Queue logs until logging_flush(), after network/engine output.
 * Keep enqueue timestamps. Overflow flushes without dropping records;
 * close/reopen also flush. Use only from the event-loop thread. */
void logging_defer(int enabled);
void logging_flush(void);
void logging_close(void);

#endif
