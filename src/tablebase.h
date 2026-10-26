#ifndef LICHESS_TABLEBASE_H
#define LICHESS_TABLEBASE_H
#include "position.h"

enum { TABLEBASE_SYZYGY=1, TABLEBASE_GAVIOTA=2 };
enum { TABLEBASE_BEST=0, TABLEBASE_SUGGEST=1 };
typedef struct {
    int syzygy_ready;
    int gaviota_ready;
    const char **gaviota_paths;
    char syzygy_paths[16384];
    struct VariantSyzygy *variant_syzygy;
} Tablebases;
typedef struct {
    int max_pieces;
    int quality;
    int min_dtm_to_consider_as_wdl_1;
} TablebaseOptions;
typedef struct {
    char moves[POSITION_MAX_MOVES][8];
    size_t count;
    int wdl; /* -2..2, side-to-move perspective; -3 means unknown. */
    int source;
    int has_distance;
    double distance; /* Signed DTZ/DTM, including halfmove-clock adjustment. */
    int suggested; /* WDL-only probes may require engine filtering even in best mode. */
} TablebaseResult;
/* Keep one context on the event loop to reuse mappings and caches.
 * Missing tables are a probe miss. */
int tablebase_init(Tablebases *tb,const char *const *syzygy_paths,size_t syzygy_count,
                   const char *const *gaviota_paths,size_t gaviota_count,char *err,size_t cap);
void tablebase_close(Tablebases *tb);
int tablebase_probe_syzygy(Tablebases *tb,const Position *p,const TablebaseOptions *options,
                           uint64_t *rng,TablebaseResult *result);
int tablebase_probe_gaviota(Tablebases *tb,const Position *p,const TablebaseOptions *options,
                            uint64_t *rng,TablebaseResult *result);
/* Convert tablebase distances to WDL scores. */
int tablebase_dtz_to_wdl(double dtz);
int tablebase_dtm_to_wdl(int dtm,int threshold);
#endif
