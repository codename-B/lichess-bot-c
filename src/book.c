#define _POSIX_C_SOURCE 200809L
#include "book.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static uint64_t be64(const unsigned char *p) {
    uint64_t n=0;for (unsigned i=0;i<8;++i) n=(n<<8)|p[i];return n;
}
static unsigned be16(const unsigned char *p) {return ((unsigned)p[0]<<8)|p[1];}
static uint32_t be32(const unsigned char *p) {return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static uint64_t random_below(uint64_t *state,uint64_t bound) {
    static uint64_t fallback;
    if (!state) state=&fallback;
    if (!*state) *state=(uint64_t)time(NULL)^((uint64_t)(unsigned)getpid()<<32)^UINT64_C(0x9e3779b97f4a7c15);
    uint64_t x,minimum=(UINT64_C(0)-bound)%bound;
    do {x=*state;x^=x>>12;x^=x<<25;x^=x>>27;*state=x;x*=UINT64_C(2685821657736338717);} while(x<minimum);
    return x%bound;
}
int book_selection_from_name(const char *name) {
    if (!name || !strcmp(name,"weighted_random")) return BOOK_WEIGHTED_RANDOM;
    if (!strcmp(name,"uniform_random")) return BOOK_UNIFORM_RANDOM;
    if (!strcmp(name,"best_move")) return BOOK_BEST_MOVE;
    return -1;
}
int book_normalization_from_name(const char *name) {
    if (!name || !strcmp(name,"none")) return BOOK_NORMALIZE_NONE;
    if (!strcmp(name,"sum")) return BOOK_NORMALIZE_SUM;
    if (!strcmp(name,"max")) return BOOK_NORMALIZE_MAX;
    return -1;
}
void book_collection_init(BookCollection *books) {
    memset(books,0,sizeof(*books));
    for (size_t i=0;i<BOOK_MAX_FILES;++i) books->files[i].fd=-1;
}
void book_collection_close(BookCollection *books) {
    for (size_t i=0;i<books->count;++i) {
        BookFile *f=&books->files[i];
        if (f->data && f->bytes) (void)munmap((void *)f->data,f->bytes);
        if (f->fd>=0) (void)close(f->fd);
    }
    book_collection_init(books);
}
int book_collection_add(BookCollection *books,const char *path,char *err,size_t cap) {
    if (!path || strlen(path)>=sizeof(books->files[0].path) || books->count>=BOOK_MAX_FILES) {
        if (err && cap) (void)snprintf(err,cap,"Invalid book path or too many books");
        return 0;
    }
    int fd=open(path,O_RDONLY|O_CLOEXEC);
    if (fd<0) {if(err && cap)(void)snprintf(err,cap,"Cannot open book %s: %s",path,strerror(errno));return 0;}
    struct stat st;
    if (fstat(fd,&st)<0 || st.st_size<0 || (uint64_t)st.st_size>SIZE_MAX || st.st_size%16 || !S_ISREG(st.st_mode)) {
        if(err && cap)(void)snprintf(err,cap,"Book must be a regular file of 16-byte Polyglot records: %s",path);
        (void)close(fd);return 0;
    }
    size_t bytes=(size_t)st.st_size;
    void *data=bytes?mmap(NULL,bytes,PROT_READ,MAP_PRIVATE,fd,0):NULL;
    if (data==MAP_FAILED) {
        if(err && cap)(void)snprintf(err,cap,"Cannot map book %s: %s",path,strerror(errno));
        (void)close(fd);return 0;
    }
    BookFile *f=&books->files[books->count++];
    f->fd=fd;f->bytes=bytes;f->entries=bytes/16;f->data=data;
    (void)snprintf(f->path,sizeof(f->path),"%s",path);
    if(err && cap)*err=0;
    return 1;
}
static int decode(const Position *p,const unsigned char *entry,char out[8]) {
    unsigned raw=be16(entry+8),from=(raw>>6)&63,to=raw&63,promo=(raw>>12)&7;
    char uci[8];
    const char *symbols=" pnbrqk";
    if (promo>5) return 0;
    if (from==to) {
        if (!promo || promo>=5) return 0;
        uci[0]=(char)(symbols[promo+1]-'a'+'A');uci[1]='@';
        uci[2]=(char)('a'+(to&7));uci[3]=(char)('1'+(to>>3));uci[4]=0;
    } else {
        uci[0]=(char)('a'+(from&7));uci[1]=(char)('1'+(from>>3));
        uci[2]=(char)('a'+(to&7));uci[3]=(char)('1'+(to>>3));uci[4]=0;
        if(promo){uci[4]=symbols[promo+1];uci[5]=0;}
    }
    return position_normalize_uci(p,uci,p->chess960,out) && position_is_legal(p,out);
}
int book_choose(const BookCollection *books,const Position *p,const BookOptions *options,uint64_t *rng,BookResult *result) {
    if (!books || !p || !options || !result || !options->max_depth || (uint64_t)p->ply>(uint64_t)options->max_depth*2-1) return 0;
    uint64_t key=position_polyglot_hash(p);
    for(size_t b=0;b<books->count;++b) {
        const BookFile *f=&books->files[b];size_t lo=0,hi=f->entries;
        while(lo<hi){size_t mid=lo+(hi-lo)/2;if(be64(f->data+mid*16)<key)lo=mid+1;else hi=mid;}
        size_t begin=lo,end=begin;
        while(end<f->entries && be64(f->data+end*16)==key)++end;
        if(begin==end)continue;
        uint64_t sum=0,max=0;
        if(options->normalization!=BOOK_NORMALIZE_NONE && options->selection!=BOOK_WEIGHTED_RANDOM) {
            for(size_t i=begin;i<end;++i) {
                const unsigned char *e=f->data+i*16;unsigned w=be16(e+10);char move[8];
                if(w && decode(p,e,move)){sum+=w;if(w>max)max=w;}
            }
        }
        double minimum=options->min_weight;
        if(options->normalization==BOOK_NORMALIZE_SUM && sum) minimum *= (double)sum/100.0;
        if(options->normalization==BOOK_NORMALIZE_MAX && max) minimum *= (double)max/100.0;
        uint64_t mass=0,count=0;unsigned best=0;int found=0;BookResult chosen;
        memset(&chosen,0,sizeof(chosen));
        for(size_t i=begin;i<end;++i) {
            const unsigned char *e=f->data+i*16;unsigned w=be16(e+10);char move[8];
            if(options->selection==BOOK_WEIGHTED_RANDOM ? w==0 : (double)w<minimum)continue;
            if(!decode(p,e,move))continue;
            int select=0;
            if(options->selection==BOOK_WEIGHTED_RANDOM){mass+=w;select=random_below(rng,mass)<w;}
            else if(options->selection==BOOK_UNIFORM_RANDOM){++count;select=random_below(rng,count)==0;}
            else if(options->selection==BOOK_BEST_MOVE){select=!found || w>best;}
            if(select){memcpy(chosen.move,move,sizeof(chosen.move));chosen.weight=w;chosen.learn=be32(e+12);chosen.book_index=b;best=w;found=1;}
        }
        if(found){*result=chosen;return 1;}
    }
    return 0;
}
