#include "tablebase.h"
#include "variant_syzygy.h"
#include "../vendor/fathom/tbprobe.h"
#define tb_init gtb_init
#include "../vendor/gaviota/gtb-probe.h"
#undef tb_init
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static int error_text(char *err,size_t cap,const char *message) {
    if(err && cap)(void)snprintf(err,cap,"%s",message);
    return 0;
}
int tablebase_init(Tablebases *tb,const char *const *syzygy,size_t ns,const char *const *gaviota,size_t ng,char *err,size_t cap) {
    memset(tb,0,sizeof(*tb));size_t used=0;
    for(size_t i=0;i<ns;++i) {
        struct stat st;
        if(!syzygy[i] || stat(syzygy[i],&st)<0 || !S_ISDIR(st.st_mode)) return error_text(err,cap,"Syzygy path is not a readable directory");
        size_t n=strlen(syzygy[i]);
        if(used+n+2>sizeof(tb->syzygy_paths)) return error_text(err,cap,"Syzygy path list is too long");
        if(i)tb->syzygy_paths[used++]=':';
        memcpy(tb->syzygy_paths+used,syzygy[i],n+1);used+=n;
    }
    if(ns) {
        if(!tb_init(tb->syzygy_paths)) return error_text(err,cap,"Could not initialize Syzygy tables");
        tb->syzygy_ready=1;
        tb->variant_syzygy=variant_syzygy_open(syzygy,ns);
        if(!tb->variant_syzygy){tablebase_close(tb);return error_text(err,cap,"Could not initialize variant table reader");}
    }
    if(ng) {
        tb->gaviota_paths=tbpaths_init();
        for(size_t i=0;i<ng;++i) {
            struct stat st;
            if(!gaviota[i] || stat(gaviota[i],&st)<0 || !S_ISDIR(st.st_mode)) {
                tablebase_close(tb);return error_text(err,cap,"Gaviota path is not a readable directory");
            }
            tb->gaviota_paths=tbpaths_add(tb->gaviota_paths,gaviota[i]);
        }
        (void)gtb_init(0,tb_CP4,tb->gaviota_paths);
        if(!tb_is_initialized()) {tablebase_close(tb);return error_text(err,cap,"Could not initialize Gaviota tables");}
        tb->gaviota_ready=1;
        if(!tbcache_init(32U*1024U*1024U,25)) {tablebase_close(tb);return error_text(err,cap,"Could not allocate Gaviota cache");}
    }
    if(err && cap)*err=0;
    return 1;
}
void tablebase_close(Tablebases *tb) {
    variant_syzygy_close(tb->variant_syzygy);
    if(tb->gaviota_ready){tbcache_done();tb_done();}
    if(tb->gaviota_paths)tb->gaviota_paths=tbpaths_done(tb->gaviota_paths);
    if(tb->syzygy_ready)tb_free();
    memset(tb,0,sizeof(*tb));
}
int tablebase_dtz_to_wdl(double dtz) {
    if(dtz<=-100)return -1;
    if(dtz<0)return -2;
    if(dtz==0)return 0;
    if(dtz<100)return 2;
    return 1;
}
int tablebase_dtm_to_wdl(int dtm,int threshold) {
    if(dtm<=-100)return -1;
    if(dtm<0)return -2;
    if(!dtm)return 0;
    if(dtm<threshold)return 2;
    return 1;
}
static int standard(const Position *p) {
    return p->variant==POSITION_STANDARD || p->variant==POSITION_FROM_POSITION || p->variant==POSITION_CHESS960;
}
static int kings_valid(const Position *p) {
    int w=0,b=0;
    for(int i=0;i<64;++i){w+=p->board[i]==6;b+=p->board[i]==-6;}
    return w==1 && b==1;
}
static size_t random_index(uint64_t *rng,size_t count) {
    static uint64_t fallback;
    if(!rng)rng=&fallback;
    if(!*rng)*rng=(uint64_t)time(NULL)^UINT64_C(0x9e3779b97f4a7c15);
    uint64_t x=*rng;x^=x>>12;x^=x<<25;x^=x>>27;*rng=x;
    return (size_t)((x*UINT64_C(2685821657736338717))%(uint64_t)count);
}
static int fathom_probe(const Position *p,int distance,int *out) {
    uint64_t colors[2]={0,0},pieces[7]={0,0,0,0,0,0,0};
    for(int sq=0;sq<64;++sq) {
        int pc=p->board[sq];if(!pc)continue;
        uint64_t b=UINT64_C(1)<<(unsigned)sq;colors[pc<0]|=b;pieces[abs(pc)]|=b;
    }
    unsigned ep=p->ep_square<0?0U:(unsigned)p->ep_square;
    if(distance) {
        unsigned r=tb_probe_root(colors[0],colors[1],pieces[6],pieces[5],pieces[4],pieces[3],pieces[2],pieces[1],0,0,ep,!p->turn,NULL);
        if(r==TB_RESULT_FAILED)return 0;
        if(r==TB_RESULT_CHECKMATE){*out=-1;return 1;}
        if(r==TB_RESULT_STALEMATE){*out=0;return 1;}
        int wdl=(int)TB_GET_WDL(r)-2,dtz=(int)TB_GET_DTZ(r);
        *out=wdl<0?-dtz:dtz;return 1;
    }
    unsigned r=tb_probe_wdl(colors[0],colors[1],pieces[6],pieces[5],pieces[4],pieces[3],pieces[2],pieces[1],0,0,ep,!p->turn);
    if(r==TB_RESULT_FAILED)return 0;
    *out=(int)r-2;return 1;
}
static int gaviota_probe(const Position *p,int *out) {
    unsigned squares[2][17];unsigned char pcs[2][17];int counts[2]={0,0};
    for(int sq=0;sq<64;++sq) {
        int pc=p->board[sq];if(!pc)continue;
        int c=pc<0,index=counts[c]++;squares[c][index]=(unsigned)sq;pcs[c][index]=(unsigned char)abs(pc);
    }
    for(int c=0;c<2;++c){squares[c][counts[c]]=tb_NOSQUARE;pcs[c][counts[c]]=tb_NOPIECE;}
    unsigned info=tb_UNKNOWN,plies=0;
    int ok=tb_probe_hard((unsigned)p->turn,p->ep_square<0?tb_NOSQUARE:(unsigned)p->ep_square,0,squares[0],squares[1],pcs[0],pcs[1],&info,&plies);
    if(!ok || info==tb_UNKNOWN || info==tb_FORBID || plies>100000)return 0;
    if(info==tb_DRAW){*out=0;return 1;}
    int winner=info==tb_WMATE?0:1;
    *out=winner==p->turn?(int)plies:-(int)plies;
    return 1;
}
static int syzygy_probe_value(Tablebases *tb,const Position *p,int distance,int *out) {
    int outcome=position_outcome(p);
    if(outcome>=0){*out=outcome==0?0:(outcome==p->turn+1?1:-1)*(distance?1:2);return 1;}
    return standard(p)?fathom_probe(p,distance,out):variant_syzygy_probe(tb->variant_syzygy,p,distance,out);
}
static void result_init(TablebaseResult *result,int source) {
    memset(result,0,sizeof(*result));result->wdl=-3;result->source=source;
}
int tablebase_probe_syzygy(Tablebases *tb,const Position *p,const TablebaseOptions *options,uint64_t *rng,TablebaseResult *result) {
    result_init(result,TABLEBASE_SYZYGY);
    int is_standard=standard(p);
    if(!tb->syzygy_ready || (!is_standard && p->variant!=POSITION_ATOMIC && p->variant!=POSITION_ANTICHESS) ||
       p->castling || (is_standard && !kings_valid(p)) || position_piece_count(p)>options->max_pieces || position_piece_count(p)>7)return 0;
    char moves[POSITION_MAX_MOVES][8];size_t n=position_legal_moves(p,moves,POSITION_MAX_MOVES);
    if(!n)return 0;
    double scores[POSITION_MAX_MOVES];int has_dtz=1;
    for(size_t i=0;i<n;++i) {
        Position q=*p;int dtz;
        if(!position_apply_generated_uci(&q,moves[i]) || !syzygy_probe_value(tb,&q,1,&dtz)){has_dtz=0;break;}
        double value=-(double)dtz;
        if(position_outcome(&q)>=0)scores[i]=value;
        else {
            if(!q.halfmove_clock && value!=0)value=copysign(0.5,value);
            scores[i]=value+(value!=0?copysign(q.halfmove_clock,value):0);
        }
    }
    if(!has_dtz)for(size_t i=0;i<n;++i) {
        Position q=*p;int wdl;
        if(!position_apply_generated_uci(&q,moves[i]) || !syzygy_probe_value(tb,&q,0,&wdl))return 0;
        scores[i]=-wdl;
    }
    int best=-3;double best_score=1000000;
    for(size_t i=0;i<n;++i) {
        int wdl=has_dtz?tablebase_dtz_to_wdl(scores[i]):(int)scores[i];
        if(wdl>best){best=wdl;best_score=scores[i];}
        else if(wdl==best && scores[i]<best_score)best_score=scores[i];
    }
    size_t candidates[POSITION_MAX_MOVES],count=0;int suggest=options->quality==TABLEBASE_SUGGEST || !has_dtz;
    for(size_t i=0;i<n;++i) {
        int wdl=has_dtz?tablebase_dtz_to_wdl(scores[i]):(int)scores[i];
        if(wdl==best && (suggest || scores[i]==best_score))candidates[count++]=i;
    }
    if(!count)return 0;
    if(suggest) {
        for(size_t j=0;j<count;++j)memcpy(result->moves[j],moves[candidates[j]],8);
        result->count=count;
    } else {memcpy(result->moves[0],moves[candidates[random_index(rng,count)]],8);result->count=1;}
    result->wdl=best;result->has_distance=has_dtz;result->distance=best_score;result->suggested=suggest && result->count>1;
    return 1;
}
int tablebase_probe_gaviota(Tablebases *tb,const Position *p,const TablebaseOptions *options,uint64_t *rng,TablebaseResult *result) {
    result_init(result,TABLEBASE_GAVIOTA);
    if(!tb->gaviota_ready || !standard(p) || p->castling || !kings_valid(p) || position_piece_count(p)>options->max_pieces || position_piece_count(p)>5)return 0;
    char moves[POSITION_MAX_MOVES][8];size_t n=position_legal_moves(p,moves,POSITION_MAX_MOVES);
    if(!n)return 0;
    int scores[POSITION_MAX_MOVES],best=-2,best_score=1000000;
    for(size_t i=0;i<n;++i) {
        Position q=*p;int dtm;
        if(!position_apply_generated_uci(&q,moves[i]) || !gaviota_probe(&q,&dtm))return 0;
        dtm=-dtm;
        /* A checkmated child has DTM zero: prefer the immediate win. */
        if(!dtm && position_outcome(&q)==p->turn+1)scores[i]=1;
        else scores[i]=dtm+(dtm>0?q.halfmove_clock:(dtm<0?-q.halfmove_clock:0));
        int wdl=(scores[i]>0)-(scores[i]<0);
        if(wdl>best){best=wdl;best_score=scores[i];}
        else if(wdl==best && scores[i]<best_score)best_score=scores[i];
    }
    int threshold=options->min_dtm_to_consider_as_wdl_1,cutoff=threshold;
    if(threshold<100)threshold=100;
    if(best_score<100)cutoff=100;
    size_t candidates[POSITION_MAX_MOVES],count=0;
    for(size_t i=0;i<n;++i) {
        int wdl=(scores[i]>0)-(scores[i]<0);
        if(wdl!=best)continue;
        if(options->quality==TABLEBASE_SUGGEST ? (best_score<threshold && scores[i]>=cutoff) : scores[i]!=best_score)continue;
        candidates[count++]=i;
    }
    if(!count)return 0;
    if(options->quality==TABLEBASE_SUGGEST) {
        for(size_t j=0;j<count;++j)memcpy(result->moves[j],moves[candidates[j]],8);
        result->count=count;
    } else {memcpy(result->moves[0],moves[candidates[random_index(rng,count)]],8);result->count=1;}
    result->wdl=tablebase_dtm_to_wdl(best_score,threshold);result->distance=best_score;result->has_distance=1;result->suggested=result->count>1;
    return 1;
}
