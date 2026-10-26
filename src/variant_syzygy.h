#ifndef LICHESS_VARIANT_SYZYGY_H
#define LICHESS_VARIANT_SYZYGY_H
#include "position.h"
typedef struct VariantSyzygy VariantSyzygy;
VariantSyzygy *variant_syzygy_open(const char *const *paths,size_t count);
void variant_syzygy_close(VariantSyzygy *tables);
/* Probe Atomic/Antichess Syzygy tables. Return 0 on a miss. */
int variant_syzygy_probe(VariantSyzygy *tables,const Position *p,int distance,int *value);
#endif
