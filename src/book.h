#ifndef LICHESS_BOOK_H
#define LICHESS_BOOK_H
#include "position.h"

enum { BOOK_WEIGHTED_RANDOM, BOOK_UNIFORM_RANDOM, BOOK_BEST_MOVE };
enum { BOOK_NORMALIZE_NONE, BOOK_NORMALIZE_SUM, BOOK_NORMALIZE_MAX };
#define BOOK_MAX_FILES 64
typedef struct {
    const unsigned char *data;
    size_t bytes;
    size_t entries;
    int fd;
    char path[1024];
} BookFile;
typedef struct { BookFile files[BOOK_MAX_FILES]; size_t count; } BookCollection;
typedef struct {
    int selection;
    int normalization;
    double min_weight;
    unsigned max_depth; /* Full moves from the initial position. */
} BookOptions;
typedef struct {
    char move[8];
    unsigned weight;
    uint32_t learn;
    size_t book_index;
} BookResult;
void book_collection_init(BookCollection *books);
void book_collection_close(BookCollection *books);
int book_collection_add(BookCollection *books,const char *path,char *err,size_t cap);
/* 1 if found, otherwise 0.
 * NULL rng uses a private generator. */
int book_choose(const BookCollection *books,const Position *p,const BookOptions *options,
                uint64_t *rng,BookResult *result);
int book_selection_from_name(const char *name);
int book_normalization_from_name(const char *name);
#endif
