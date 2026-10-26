#ifndef LBC_POLICY_H
#define LBC_POLICY_H
#include "config.h"
#include "json.h"
#include <stddef.h>
#include <stdint.h>

#define POLICY_QUEUE_CAP 256
#define POLICY_RECENT_CAP 4096
#define POLICY_FILTER_CAP 4096
#define POLICY_LIST_CAP 32
typedef struct {
    int used, rated, bot, titled, rating, variant, admitted;
    int64_t initial, increment, days, created_us;
    char id[33], opponent[65], name[129], speed[32], perf[32], fen[512];
} Challenge;
typedef struct {
    char name[65], aspect[32];
    int64_t until_us;
} PolicyFilter;
typedef struct {
    const char *url;
    int owners, attempts, initialized;
    int64_t due_us;
    char (*names)[65];
    char etag[512];
    size_t count;
} OnlineBlockList;
typedef struct {
    char id[65], name[129];
    struct { char name[32]; int rating; } perfs[24];
    size_t count;
} BotProfile;
typedef struct {
    char opponent[65], variant[32], speed[32], mode[8], preference[8], override_name[129];
    int initial, increment, days, min_rating, max_rating;
    uint64_t all_weight, ready_weight;
    char fallback[65];
} MatchSpec;
typedef struct {
    BotProfile profile;
    Challenge queue[POLICY_QUEUE_CAP];
    struct { char name[65]; int64_t until_us; } recent[POLICY_RECENT_CAP];
    PolicyFilter filters[POLICY_FILTER_CAP];
    OnlineBlockList lists[POLICY_LIST_CAP];
    size_t list_count;
    uint64_t random;
    MatchSpec match;
} Policy;

enum PolicyRateLimitKind {
    POLICY_RATE_LIMIT_NONE, POLICY_RATE_LIMIT_BOT, POLICY_RATE_LIMIT_OPPONENT
};
typedef struct {
    enum PolicyRateLimitKind kind;
    int64_t delay_us;
    int daily;
} PolicyRateLimit;

/* Classify a response without I/O. response may be NULL;
 * retry_after_us is -1 when absent or invalid. Initialize backoff_step to zero;
 * generic 429s without Retry-After advance it; non-429s reset it.
 * Durations saturate at INT64_MAX; use saturating addition with a clock. */
PolicyRateLimit policy_challenge_rate_limit(const Json *response, long status,
                                             int64_t retry_after_us, unsigned *backoff_step);

int policy_init(Policy *policy, const Config *cfg, uint64_t seed, char *error, size_t cap);
void policy_free(Policy *policy);
int policy_profile(Policy *policy, const Json *json, int object);
int policy_rating(const Policy *policy, const char *perf);
int policy_challenge_parse(Challenge *out, const Json *json, int object, int64_t now);
const char *policy_challenge_reason(Policy *policy, const Config *cfg, const Challenge *challenge,
                                    int engagements, int64_t now);
int policy_queue_add(Policy *policy, const Challenge *challenge);
void policy_queue_remove(Policy *policy, const char *id);
Challenge *policy_queue_best(Policy *policy, const Config *cfg, int bot_slots_available, int64_t now);
void policy_accepted(Policy *policy, const Config *cfg, const Challenge *challenge, int64_t now);
int policy_online_due(const Policy *policy, int64_t now);
int policy_online_update(Policy *policy, size_t list, const char *text, size_t length, int64_t now);
int policy_is_blocked(const Policy *policy, const Config *cfg, const char *name, int matchmaking, int64_t now);
int policy_match_begin(Policy *policy, const Config *cfg);
void policy_match_consider(Policy *policy, const Config *cfg, const Json *json, int object, int engaged, int64_t now);
const char *policy_match_finish(Policy *policy);
int policy_match_body(const Policy *policy, char *body, size_t cap);
int policy_filter(Policy *policy, const Config *cfg, const char *name, const char *aspect,
                  int64_t until, int persistent);
void policy_match_declined(Policy *policy, const Config *cfg, const Json *json, int challenge, int64_t now);
const char *policy_game_category(int variant, int64_t initial, int64_t increment, int days);
#endif
