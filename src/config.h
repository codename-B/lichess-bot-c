#ifndef LBC_CONFIG_H
#define LBC_CONFIG_H

#include <stdint.h>
#include <stdio.h>
#include "settings.h"

#define LBC_VERSION "0.3.0"
#define MAX_GAMES 16
#define MAX_ENGINE_ARGS 64
#define MAX_OPTIONS 64
#define TOKEN_CAP 4096
#define VAR_STANDARD 1
#define VAR_FROM_POSITION 2
#define VAR_CHESS960 4
#define VAR_ANTICHESS 8
#define VAR_ATOMIC 16
#define VAR_CRAZYHOUSE 32
#define VAR_HORDE 64
#define VAR_KING_OF_THE_HILL 128
#define VAR_RACING_KINGS 256
#define VAR_THREE_CHECK 512
#define VAR_ALL 1023
#define MAX_CONFIG_STRINGS 256

typedef struct {
    char *engine_argv[MAX_ENGINE_ARGS + 1];
    const char *engine_dir;
    const char *options[MAX_OPTIONS];
    int option_count;
    char base_url[512];
    char token[TOKEN_CAP];
    int concurrency, variants, accept, only_bots, casual_only, ponder, trace;
    int min_initial, max_initial, min_increment, max_increment;
    int overhead_ms, movetime_ms, depth, http_timeout_ms, engine_timeout_ms;
    int64_t nodes;
    Settings *settings;
    const char *protocol, *config_file, *logfile;
    int upgrade, verbose, disable_auto_logging, engine_debug, silence_stderr;
    char *owned_strings[MAX_CONFIG_STRINGS];
    size_t owned_count;
} Config;

/* 0 = run; 1 = help/version printed; -1 = invalid configuration. */
int config_parse(Config *config, int argc, char **argv);
void config_free(Config *config);
void config_help(FILE *out);
int config_variant(const char *name);
const char *config_variant_name(int variant);
const char *config_engine_variant_name(int variant);

#endif
