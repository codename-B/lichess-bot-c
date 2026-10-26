/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native C adaptation of the Syzygy variant reader in python-chess.
 * Copyright (C) Niklas Fiekas and contributors; adaptation 2026.
 * Original Syzygy format and indexing algorithms by Ronald de Man.
 * See vendor/python-chess-LICENSE. No Python runtime is involved.
 */
#define _POSIX_C_SOURCE 200809L
#include "variant_syzygy.h"
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "variant_syzygy_data.inc"

#define VT_PIECES 7
#define VT_CACHE_SIZE 128
typedef struct {
    size_t index_table,size_table,data,offset,sympat;
    uint64_t size[3],base[65];
    uint32_t *symlen;
    unsigned symbols,blocksize,idxbits,min_len,max_len,flags;
} VPairs;
typedef struct {
    int pieces[VT_PIECES],norm[VT_PIECES];
    uint64_t factor[VT_PIECES],tb_size;
    VPairs pairs;
    size_t map[4];
} VSide;
typedef struct VTable {
    struct VTable *next;
    unsigned char *data;
    size_t length;
    unsigned variant;
    int dtz,num,has_pawns,pawns[2],enc_type,symmetric,split,files,bad;
    char key[16],mirror_key[16],requested_key[16],requested_mirror[16];
    VSide side[4][2];
    uint64_t used;
} VTable;
struct VariantSyzygy {
    char **paths;
    size_t path_count,count;
    VTable *tables;
    uint64_t tick;
};
static unsigned v8(VTable *t,size_t off) {
    if(off>=t->length){t->bad=1;return 0;}return t->data[off];
}
static unsigned v16(VTable *t,size_t off) {
    if(off>t->length || t->length-off<2){t->bad=1;return 0;}
    return t->data[off]|((unsigned)t->data[off+1]<<8);
}
static uint32_t v32(VTable *t,size_t off) {
    if(off>t->length || t->length-off<4){t->bad=1;return 0;}
    return (uint32_t)t->data[off]|((uint32_t)t->data[off+1]<<8)|((uint32_t)t->data[off+2]<<16)|((uint32_t)t->data[off+3]<<24);
}
static uint64_t big(VTable *t,size_t off,unsigned n) {
    if(off>t->length || t->length-off<n){t->bad=1;return 0;}
    uint64_t value=0;for(unsigned i=0;i<n;++i)value=(value<<8)|t->data[off+i];return value;
}
static int advance(VTable *t,size_t *offset,uint64_t amount) {
    if(*offset>t->length || amount>(uint64_t)(t->length-*offset)){t->bad=1;return 0;}
    *offset+=(size_t)amount;return 1;
}
static uint64_t choose_n(int n,int k) {
    if(k<0 || n<k || n<0)return 0;
    if(k>n-k)k=n-k;
    uint64_t v=1;for(int i=1;i<=k;++i)v=v*(uint64_t)(n-k+i)/(unsigned)i;return v;
}
static void swap_square(int *a,int *b){int v=*a;*a=*b;*b=v;}
static int offdiag(int sq){return (sq>>3)-(sq&7);}
static int flipdiag(int sq){return ((sq>>3)|(sq<<3))&63;}
static void mirror_position(int *p,int n,int mask){for(int i=0;i<n;++i)p[i]^=mask;}
static void diagonal_position(int *p,int n){for(int i=0;i<n;++i)p[i]=flipdiag(p[i]);}
static void key_from_counts(const int counts[2][7],int mirror,char key[16]) {
    const char *symbols=" PNBRQK";size_t n=0;
    for(int side=0;side<2;++side) {
        int c=side^mirror;
        for(int pt=6;pt>=1;--pt)for(int i=0;i<counts[c][pt];++i)key[n++]=symbols[pt];
        if(!side)key[n++]='v';
    }
    key[n]=0;
}
static int position_key(const Position *p,char key[16],char mirror[16]) {
    int counts[2][7]={{0}},n=0;
    for(int sq=0;sq<64;++sq)if(p->board[sq]){++counts[p->board[sq]<0][abs(p->board[sq])];++n;}
    if(n>VT_PIECES)return 0;
    key_from_counts((const int (*)[7])counts,0,key);key_from_counts((const int (*)[7])counts,1,mirror);return 1;
}
static void table_keys(VTable *t,const int *pieces) {
    int counts[2][7]={{0}};
    for(int i=0;i<t->num;++i)++counts[pieces[i]>>3][pieces[i]&7];
    key_from_counts((const int (*)[7])counts,0,t->key);key_from_counts((const int (*)[7])counts,1,t->mirror_key);
}
static int normalise(VTable *t,VSide *s) {
    memset(s->norm,0,sizeof(s->norm));
    for(int i=0;i<t->num;++i)if((s->pieces[i]&7)<1 || (s->pieces[i]&7)>6 || s->pieces[i]>14)return 0;
    int i;
    if(t->has_pawns) {
        s->norm[0]=t->pawns[0];if(t->pawns[1])s->norm[t->pawns[0]]=t->pawns[1];
        i=t->pawns[0]+t->pawns[1];
    } else {s->norm[0]=t->enc_type==0?3:(t->enc_type==2?2:t->enc_type-1);i=s->norm[0];}
    if(i>t->num || i<1)return 0;
    while(i<t->num){int j=i;while(j<t->num && s->pieces[j]==s->pieces[i]){++s->norm[i];++j;}i=j;}
    return 1;
}
static int factors(VTable *t,VSide *s,int order,int order2,int file) {
    int i=s->norm[0],n=64-i,k=0;uint64_t factor=1;
    if(t->has_pawns && order2<15){i+=s->norm[i];n=64-i;}
    if(order>7 || (order2!=15 && order2>7))return 0;
    while(i<t->num || k==order || (t->has_pawns && k==order2)) {
        if(k>16)return 0;
        if(k==order) {
            s->factor[0]=factor;
            if(t->has_pawns)factor*=(uint64_t)VT_PFACTOR[s->norm[0]-1][file];
            else if(t->enc_type<4){static const unsigned pivot[4]={31332,0,518,278};factor*=pivot[t->enc_type];}
            else factor*=(uint64_t)VT_MFACTOR[t->enc_type-2];
        } else if(t->has_pawns && k==order2) {
            s->factor[s->norm[0]]=factor;factor*=choose_n(48-s->norm[0],s->norm[s->norm[0]]);
        } else {
            if(i>=t->num || s->norm[i]<=0 || s->norm[i]>n)return 0;
            s->factor[i]=factor;factor*=choose_n(n,s->norm[i]);n-=s->norm[i];i+=s->norm[i];
        }
        ++k;
    }
    s->tb_size=factor;return factor!=0;
}
static int symbol_length(VTable *t,VPairs *d,unsigned symbol,unsigned char *state,unsigned depth) {
    if(symbol>=d->symbols || depth>256 || state[symbol]==1)return 0;
    if(state[symbol]==2)return 1;
    state[symbol]=1;size_t off=d->sympat+3U*symbol;
    unsigned b0=v8(t,off),b1=v8(t,off+1),b2=v8(t,off+2),s2=(b2<<4)|(b1>>4);
    if(s2==4095)d->symlen[symbol]=0;
    else {
        unsigned s1=((b1&15)<<8)|b0;
        if(!symbol_length(t,d,s1,state,depth+1) || !symbol_length(t,d,s2,state,depth+1))return 0;
        uint64_t length=(uint64_t)d->symlen[s1]+d->symlen[s2]+1;
        if(length>UINT32_MAX)return 0;
        d->symlen[symbol]=(uint32_t)length;
    }
    state[symbol]=2;return !t->bad;
}
static int setup_pairs(VTable *t,VSide *s,size_t *offset) {
    VPairs *d=&s->pairs;size_t at=*offset;d->flags=v8(t,at);
    if(d->flags&128) {
        d->min_len=t->dtz?(t->variant==POSITION_ANTICHESS?1U:0U):v8(t,at+1);
        return advance(t,offset,2);
    }
    d->blocksize=v8(t,at+1);d->idxbits=v8(t,at+2);
    uint32_t real_blocks=v32(t,at+4);uint64_t blocks=(uint64_t)real_blocks+v8(t,at+3);
    d->max_len=v8(t,at+8);d->min_len=v8(t,at+9);
    if(d->blocksize>30 || !d->idxbits || d->idxbits>31 || !d->min_len || d->max_len<d->min_len || d->max_len>64)return 0;
    unsigned h=d->max_len-d->min_len+1;
    d->symbols=v16(t,at+10+2U*h);d->offset=at+10;d->sympat=at+12+2U*h;
    if(!d->symbols || d->symbols>4096)return 0;
    if(!advance(t,offset,12U+2U*h+3U*d->symbols+(d->symbols&1)))return 0;
    d->size[0]=6*((s->tb_size+(UINT64_C(1)<<d->idxbits)-1)>>d->idxbits);
    d->size[1]=2*blocks;d->size[2]=(UINT64_C(1)<<d->blocksize)*real_blocks;
    d->symlen=calloc(d->symbols,sizeof(*d->symlen));unsigned char *state=calloc(d->symbols,1);
    if(!d->symlen || !state){free(state);return 0;}
    int ok=1;
    for(unsigned i=0;i<d->symbols && ok;++i)ok=symbol_length(t,d,i,state,0);
    free(state);if(!ok)return 0;
    d->base[h-1]=0;
    for(int i=(int)h-2;i>=0;--i) {
        int64_t x=(int64_t)d->base[i+1]+(int64_t)v16(t,d->offset+(size_t)i*2)-(int64_t)v16(t,d->offset+(size_t)i*2+2);
        if(x<0)return 0;
        d->base[i]=(uint64_t)x/2;
    }
    for(unsigned i=0;i<h;++i)d->base[i]<<=64-(d->min_len+i);
    return !t->bad;
}
static int setup_table(VTable *t) {
    uint32_t magic=v32(t,0);
    uint32_t expected=t->variant==POSITION_ATOMIC?(t->dtz?UINT32_C(0xeb5ea991):UINT32_C(0x49a48d55)):(t->dtz?UINT32_C(0x501bf5d6):UINT32_C(0x21bc55bc));
    uint32_t fallback=t->dtz?UINT32_C(0x23e7cfe4):UINT32_C(0x1593f67b);
    if(magic!=expected && !(t->variant==POSITION_ANTICHESS && !t->has_pawns && magic==fallback))return 0;
    t->split=!t->dtz && (v8(t,4)&1)!=0;t->files=t->has_pawns?((v8(t,4)&2)?4:1):1;
    int header_files=t->has_pawns?4:1,header_sides=t->dtz?1:2;size_t offset=5;
    for(int f=0;f<header_files;++f) {
        int prefix=1+(t->has_pawns && t->pawns[1]>0);
        for(int side=0;side<header_sides;++side) {
            VSide *s=&t->side[f][side];unsigned shift=side?4U:0U;
            int order=(int)((v8(t,offset)>>shift)&15),order2=t->has_pawns && t->pawns[1]?(int)((v8(t,offset+1)>>shift)&15):15;
            for(int i=0;i<t->num;++i)s->pieces[i]=(int)((v8(t,offset+(size_t)prefix+(size_t)i)>>shift)&15);
            if(!normalise(t,s) || !factors(t,s,order,order2,f))return 0;
        }
        if(!advance(t,&offset,(uint64_t)(t->num+prefix)))return 0;
    }
    if(!advance(t,&offset,offset&1))return 0;
    int sides=t->split?2:1;
    for(int f=0;f<t->files;++f)for(int side=0;side<sides;++side)if(!setup_pairs(t,&t->side[f][side],&offset))return 0;
    if(t->dtz) {
        for(int f=0;f<t->files;++f) {
            VSide *s=&t->side[f][0];
            if(s->pairs.flags&2) {
                int wide=(s->pairs.flags&16)!=0;
                if(wide && t->has_pawns && !advance(t,&offset,offset&1))return 0;
                for(int i=0;i<4;++i) {
                    unsigned length=wide?v16(t,offset):v8(t,offset);
                    s->map[i]=offset+(wide?2U:1U);
                    if(!advance(t,&offset,(uint64_t)(wide?2U:1U)*(length+1U)))return 0;
                }
            }
        }
        if(!advance(t,&offset,offset&1))return 0;
    }
    for(int f=0;f<t->files;++f)for(int side=0;side<sides;++side) {
        VPairs *d=&t->side[f][side].pairs;d->index_table=offset;if(!advance(t,&offset,d->size[0]))return 0;
    }
    for(int f=0;f<t->files;++f)for(int side=0;side<sides;++side) {
        VPairs *d=&t->side[f][side].pairs;d->size_table=offset;if(!advance(t,&offset,d->size[1]))return 0;
    }
    for(int f=0;f<t->files;++f)for(int side=0;side<sides;++side) {
        if(!advance(t,&offset,(64-(offset&63))&63))return 0;
        VPairs *d=&t->side[f][side].pairs;d->data=offset;if(!advance(t,&offset,d->size[2]))return 0;
    }
    if(!t->has_pawns)table_keys(t,t->side[0][0].pieces);
    return !t->bad;
}
static void close_table(VTable *t) {
    if(t->data)(void)munmap(t->data,t->length);
    for(int f=0;f<4;++f)for(int s=0;s<2;++s)free(t->side[f][s].pairs.symlen);
    free(t);
}
VariantSyzygy *variant_syzygy_open(const char *const *paths,size_t count) {
    VariantSyzygy *v=calloc(1,sizeof(*v));if(!v)return NULL;
    v->paths=calloc(count?count:1,sizeof(*v->paths));if(!v->paths){free(v);return NULL;}
    for(size_t i=0;i<count;++i){v->paths[i]=strdup(paths[i]);if(!v->paths[i]){variant_syzygy_close(v);return NULL;}++v->path_count;}
    return v;
}
void variant_syzygy_close(VariantSyzygy *v) {
    if(!v)return;
    while(v->tables){VTable *next=v->tables->next;close_table(v->tables);v->tables=next;}
    for(size_t i=0;i<v->path_count;++i)free(v->paths[i]);
    free(v->paths);free(v);
}
static void evict(VariantSyzygy *v) {
    if(v->count<VT_CACHE_SIZE)return;
    VTable **old=&v->tables;
    for(VTable **p=&v->tables;*p;p=&(*p)->next)if((*p)->used<(*old)->used)old=p;
    VTable *victim=*old;*old=victim->next;close_table(victim);--v->count;
}
static VTable *open_table(VariantSyzygy *v,const Position *p,int dtz) {
    char key[16],mirror[16];if(!position_key(p,key,mirror))return NULL;
    for(VTable *t=v->tables;t;t=t->next)if(t->variant==p->variant && t->dtz==dtz && (!strcmp(t->requested_key,key)||!strcmp(t->requested_mirror,key))) {
        t->used=++v->tick;return t->bad?NULL:t;
    }
    int counts[2][7]={{0}},has_pawns=0,n=0;
    for(int sq=0;sq<64;++sq)if(p->board[sq]){int pc=p->board[sq];++counts[pc<0][abs(pc)];has_pawns|=abs(pc)==1;++n;}
    char path[4096];int fd=-1;
    for(size_t i=0;i<v->path_count && fd<0;++i)for(int side=0;side<2 && fd<0;++side)for(int fallback=0;fallback<2 && fd<0;++fallback) {
        if(fallback && (p->variant!=POSITION_ANTICHESS || has_pawns))continue;
        const char *suffix=p->variant==POSITION_ATOMIC?(dtz?".atbz":".atbw"):(fallback?(dtz?".stbz":".stbw"):(dtz?".gtbz":".gtbw"));
        int len=snprintf(path,sizeof(path),"%s/%s%s",v->paths[i],side?mirror:key,suffix);
        if(len<0 || (size_t)len>=sizeof(path))continue;
        fd=open(path,O_RDONLY|O_CLOEXEC);
    }
    if(fd<0)return NULL;
    struct stat st;
    if(fstat(fd,&st)<0 || !S_ISREG(st.st_mode) || st.st_size<16 || st.st_size%64!=16 || (uint64_t)st.st_size>SIZE_MAX){(void)close(fd);return NULL;}
    unsigned char *data=mmap(NULL,(size_t)st.st_size,PROT_READ,MAP_PRIVATE,fd,0);(void)close(fd);
    if(data==MAP_FAILED)return NULL;
    VTable *t=calloc(1,sizeof(*t));if(!t){(void)munmap(data,(size_t)st.st_size);return NULL;}
    t->data=data;t->length=(size_t)st.st_size;t->variant=p->variant;t->num=n;t->dtz=dtz;t->has_pawns=has_pawns;t->symmetric=!strcmp(key,mirror);
    const char *base=strrchr(path,'/');base=base?base+1:path;size_t material=strcspn(base,".");
    if(material>=sizeof(t->key)){close_table(t);return NULL;}
    memcpy(t->key,base,material);t->key[material]=0;
    char *separator=strchr(t->key,'v');
    if(!separator){close_table(t);return NULL;}
    int len=snprintf(t->mirror_key,sizeof(t->mirror_key),"%s",separator+1);size_t tail=(size_t)(len<0?0:len),left=(size_t)(separator-t->key);
    t->mirror_key[tail++]='v';memcpy(t->mirror_key+tail,t->key,left);t->mirror_key[tail+left]=0;
    (void)snprintf(t->requested_key,sizeof(t->requested_key),"%s",key);(void)snprintf(t->requested_mirror,sizeof(t->requested_mirror),"%s",mirror);
    if(has_pawns) {
        t->pawns[0]=counts[1][1];t->pawns[1]=counts[0][1];
        if(t->pawns[1]>0 && (t->pawns[0]==0 || t->pawns[1]<t->pawns[0]))swap_square(&t->pawns[0],&t->pawns[1]);
    } else {
        int singleton=0,min_group=16;
        for(int c=0;c<2;++c)for(int pt=1;pt<=6;++pt){singleton+=counts[c][pt]==1;if(counts[c][pt]>1 && counts[c][pt]<min_group)min_group=counts[c][pt];}
        t->enc_type=singleton>=3?0:(singleton==2?2:1+min_group);
        if(t->enc_type>8){close_table(t);return NULL;}
    }
    if(!setup_table(t)){close_table(t);return NULL;}
    evict(v);t->used=++v->tick;t->next=v->tables;v->tables=t;++v->count;return t;
}

static uint64_t encode_tail(const VTable *t,const VSide *s,int *p,int start,uint64_t index,int pawn_second) {
    int i=start;
    while(i<t->num) {
        int group=pawn_second?t->pawns[1]:s->norm[i];
        if(group<=0 || i+group>t->num)return UINT64_MAX;
        for(int j=i;j<i+group;++j)for(int k=j+1;k<i+group;++k)if(p[j]>p[k])swap_square(&p[j],&p[k]);
        uint64_t value=0;
        for(int m=i;m<i+group;++m) {
            int previous=0;for(int k=0;k<i;++k)previous+=p[m]>p[k];
            value+=choose_n(p[m]-previous-(pawn_second?8:0),m-i+1);
        }
        index+=value*s->factor[i];i+=group;pawn_second=0;
    }
    return index;
}
static uint64_t encode_piece(const VTable *t,const VSide *s,int *p) {
    int n=t->num,i,j;int64_t index;
    if(t->enc_type<3) {
        if(p[0]&4)mirror_position(p,n,7);
        if(p[0]&32)mirror_position(p,n,56);
        for(i=0;i<n && !offdiag(p[i]);++i){}
        if(i<(t->enc_type==0?3:2) && offdiag(p[i])>0)diagonal_position(p,n);
    }
    if(t->enc_type==0) {
        i=p[1]>p[0];j=(p[2]>p[0])+(p[2]>p[1]);
        if(offdiag(p[0]))index=VT_TRIANGLE[p[0]]*63*62+(p[1]-i)*62+p[2]-j;
        else if(offdiag(p[1]))index=6*63*62+VT_DIAG[p[0]]*28*62+VT_LOWER[p[1]]*62+p[2]-j;
        else if(offdiag(p[2]))index=6*63*62+4*28*62+VT_DIAG[p[0]]*7*28+(VT_DIAG[p[1]]-i)*28+VT_LOWER[p[2]];
        else index=6*63*62+4*28*62+4*7*28+VT_DIAG[p[0]]*7*6+(VT_DIAG[p[1]]-i)*6+VT_DIAG[p[2]]-j;
        i=3;
    } else if(t->enc_type==2) {
        int correction=p[1]>p[0];
        if(offdiag(p[0]))index=VT_TRIANGLE[p[0]]*63+p[1]-correction;
        else if(offdiag(p[1]))index=6*63+VT_DIAG[p[0]]*28+VT_LOWER[p[1]];
        else index=6*63+4*28+VT_DIAG[p[0]]*7+VT_DIAG[p[1]]-correction;
        i=2;
    } else if(t->enc_type==3) {
        if(VT_TRIANGLE[p[0]]>VT_TRIANGLE[p[1]])swap_square(&p[0],&p[1]);
        if(p[0]&4)mirror_position(p,n,7);
        if(p[0]&32)mirror_position(p,n,56);
        if(offdiag(p[0])>0 || (!offdiag(p[0]) && offdiag(p[1])>0))diagonal_position(p,n);
        int test45=p[1]==32 || p[1]==40 || p[1]==48 || p[1]==33 || p[1]==41 || p[1]==34;
        if(test45 && VT_TRIANGLE[p[0]]==VT_TRIANGLE[p[1]]){swap_square(&p[0],&p[1]);mirror_position(p,n,56);diagonal_position(p,n);}
        index=VT_PP_IDX[VT_TRIANGLE[p[0]]][p[1]];i=2;
    } else {
        for(i=1;i<s->norm[0];++i)if(VT_TRIANGLE[p[0]]>VT_TRIANGLE[p[i]])swap_square(&p[0],&p[i]);
        if(p[0]&4)mirror_position(p,n,7);
        if(p[0]&32)mirror_position(p,n,56);
        if(offdiag(p[0])>0)diagonal_position(p,n);
        for(i=1;i<s->norm[0];++i)for(j=i+1;j<s->norm[0];++j)if(VT_MTWIST[p[i]]>VT_MTWIST[p[j]])swap_square(&p[i],&p[j]);
        index=VT_MULTIDX[s->norm[0]-1][VT_TRIANGLE[p[0]]];
        for(i=1;i<s->norm[0];++i)index+=(int64_t)choose_n(VT_MTWIST[p[i]],i);
    }
    if(index<0)return UINT64_MAX;
    return encode_tail(t,s,p,i,(uint64_t)index*s->factor[0],0);
}
static int pawn_file(const VTable *t,int *p) {
    for(int i=1;i<t->pawns[0];++i)if(VT_FLAP[p[0]]>VT_FLAP[p[i]])swap_square(&p[0],&p[i]);
    return VT_FILE_TO_FILE[p[0]&7];
}
static uint64_t encode_pawn(const VTable *t,const VSide *s,int *p) {
    if(p[0]&4)mirror_position(p,t->num,7);
    for(int i=1;i<t->pawns[0];++i)for(int j=i+1;j<t->pawns[0];++j)if(VT_PTWIST[p[i]]<VT_PTWIST[p[j]])swap_square(&p[i],&p[j]);
    int first=t->pawns[0]-1;uint64_t index=(uint64_t)VT_PAWNIDX[first][VT_FLAP[p[0]]];
    for(int i=first;i>0;--i)index+=choose_n(VT_PTWIST[p[i]],first-i+1);
    return encode_tail(t,s,p,t->pawns[0],index*s->factor[0],t->pawns[1]>0);
}
static int decompress(VTable *t,VSide *s,uint64_t index,int *result) {
    VPairs *d=&s->pairs;if(index>=s->tb_size)return 0;
    if(!d->idxbits){*result=(int)d->min_len;return 1;}
    uint64_t main=index>>d->idxbits;int64_t literal=(int64_t)(index&((UINT64_C(1)<<d->idxbits)-1))-(INT64_C(1)<<(d->idxbits-1));
    if(main>d->size[0]/6 || main*6+6>d->size[0])return 0;
    int64_t block=(int64_t)v32(t,d->index_table+(size_t)main*6);
    literal+=(int64_t)v16(t,d->index_table+(size_t)main*6+4);
    while(literal<0) {
        if(--block<0 || (uint64_t)block>=d->size[1]/2)return 0;
        literal+=(int64_t)v16(t,d->size_table+(size_t)block*2)+1;
    }
    while((uint64_t)block<d->size[1]/2 && literal>(int64_t)v16(t,d->size_table+(size_t)block*2)) {
        literal-=(int64_t)v16(t,d->size_table+(size_t)block*2)+1;++block;
    }
    if((uint64_t)block>=d->size[1]/2 || ((uint64_t)block<<d->blocksize)>=d->size[2])return 0;
    size_t ptr=d->data+((size_t)block<<d->blocksize);uint64_t code=big(t,ptr,8);ptr+=8;unsigned bitcount=0,symbol=0;
    for(unsigned steps=0;steps<1000000;++steps) {
        unsigned length=d->min_len;
        while(length<=d->max_len && code<d->base[length-d->min_len])++length;
        if(length>d->max_len)return 0;
        uint64_t value=(code-d->base[length-d->min_len])>>(64-length);
        value+=v16(t,d->offset+2U*(length-d->min_len));
        if(value>=d->symbols)return 0;
        symbol=(unsigned)value;
        if(literal<(int64_t)d->symlen[symbol]+1)break;
        literal-=(int64_t)d->symlen[symbol]+1;code=length==64?0:code<<length;bitcount+=length;
        while(bitcount>=32){bitcount-=32;code|=big(t,ptr,4)<<bitcount;ptr+=4;}
        if(t->bad || steps==999999)return 0;
    }
    for(unsigned depth=0;d->symlen[symbol];++depth) {
        if(depth>256)return 0;
        size_t at=d->sympat+3U*symbol;unsigned b0=v8(t,at),b1=v8(t,at+1),b2=v8(t,at+2),left=((b1&15)<<8)|b0,right=(b2<<4)|(b1>>4);
        if(left>=d->symbols || right>=d->symbols)return 0;
        if(literal<(int64_t)d->symlen[left]+1)symbol=left;
        else {literal-=(int64_t)d->symlen[left]+1;symbol=right;}
    }
    size_t at=d->sympat+3U*symbol;unsigned value=v8(t,at);
    if(t->dtz)value|=(v8(t,at+1)&15)<<8;
    if(t->bad)return 0;
    *result=(int)value;return 1;
}
/* Return 1 on a hit; 2 requests DTZ reconstruction from the opposite side. */
static int raw_probe(VariantSyzygy *v,const Position *p,int dtz,int wdl,int *result) {
    VTable *t=open_table(v,p,dtz);if(!t)return 0;
    char key[16],mirror[16];if(!position_key(p,key,mirror))return 0;
    int cmirror=0,sqmirror=0,bside=0;
    if(!t->symmetric){if(strcmp(key,t->key)){cmirror=8;sqmirror=56;bside=!p->turn;}else bside=p->turn;}
    else if(p->turn){cmirror=8;sqmirror=56;}
    int squares[VT_PIECES]={0},i=0,file=0;
    if(t->has_pawns) {
        int pc=t->side[0][0].pieces[0]^cmirror;
        for(int sq=0;sq<64;++sq)if(p->board[sq]==((pc&8)?-(pc&7):(pc&7))) {
            if(i>=t->num || sq<8 || sq>=56)return 0;
            squares[i++]=sq^sqmirror;
        }
        if(i!=t->pawns[0])return 0;
        file=pawn_file(t,squares);if(file>=t->files)return 0;
    }
    VSide *s=&t->side[file][dtz?0:bside];
    if(dtz && (int)(s->pairs.flags&1)!=bside && (t->has_pawns || !t->symmetric))return 2;
    if(!dtz && bside && !t->split)return 0;
    while(i<t->num) {
        int pc=s->pieces[i]^cmirror,begin=i;
        for(int sq=0;sq<64;++sq)if(p->board[sq]==((pc&8)?-(pc&7):(pc&7))) {
            if(i>=t->num)return 0;
            squares[i++]=sq^(t->has_pawns?sqmirror:0);
        }
        if(i==begin)return 0;
    }
    uint64_t index=t->has_pawns?encode_pawn(t,s,squares):encode_piece(t,s,squares);
    int value;if(!decompress(t,s,index,&value))return 0;
    if(dtz) {
        if(wdl<-2 || wdl>2)return 0;
        if(s->pairs.flags&2) {
            int wide=(s->pairs.flags&16)!=0;
            size_t offset=s->map[VT_WDL_TO_MAP[wdl+2]]+(size_t)value*(wide?2U:1U);
            value=(int)(wide?v16(t,offset):v8(t,offset));
        }
        if(!(s->pairs.flags&(unsigned)VT_PA_FLAGS[wdl+2]) || (wdl&1))value*=2;
    } else value-=2;
    if(t->bad || (!dtz && (value<-2 || value>2)))return 0;
    *result=value;return 1;
}

static int from_square(const char *move){return move[0]-'a'+8*(move[1]-'1');}
static int to_square(const char *move){return move[2]-'a'+8*(move[3]-'1');}
static int en_passant(const Position *p,const char *move) {
    int from=from_square(move),to=to_square(move);
    return abs(p->board[from])==1 && to==p->ep_square && !p->board[to] && ((from&7)!=(to&7));
}
static int terminal_wdl(const Position *p,int *value) {
    int outcome=position_outcome(p);
    if(outcome>=0){*value=outcome==0?0:(outcome==p->turn+1?2:-2);return 1;}
    if(p->variant==POSITION_ATOMIC) {
        int nonking=0;for(int sq=0;sq<64;++sq)nonking|=p->board[sq]!=0 && abs(p->board[sq])!=6;
        if(!nonking){*value=0;return 1;}
    }
    return 0;
}
static int probe_ab(VariantSyzygy *v,const Position *p,int alpha,int beta,int threats,unsigned depth,int *value,int *success);
static int suicide_ab(VariantSyzygy *v,const Position *p,int alpha,int beta,int threats,unsigned depth,int *value,int *success);
static int probe_wdl(VariantSyzygy *v,const Position *p,unsigned depth,int *value);
static int probe_dtz(VariantSyzygy *v,const Position *p,unsigned depth,int *value);

static int suicide_captures(VariantSyzygy *v,const Position *p,int alpha,int beta,unsigned depth,int *value,int *found) {
    if(depth>64)return 0;
    char moves[POSITION_MAX_MOVES][8];size_t n=position_legal_moves(p,moves,POSITION_MAX_MOVES);*found=0;
    for(size_t i=0;i<n;++i)if(position_is_capture(p,moves[i])) {
        *found=1;Position child=*p;int score,success;
        if(!position_apply_generated_uci(&child,moves[i]) || !suicide_ab(v,&child,-beta,-alpha,0,depth+1,&score,&success))return 0;
        score=-score;if(score>alpha)alpha=score;if(alpha>=beta)break;
    }
    *value=alpha;return 1;
}
static int suicide_ab(VariantSyzygy *v,const Position *p,int alpha,int beta,int threats,unsigned depth,int *value,int *success) {
    if(depth>64)return 0;
    if(terminal_wdl(p,value)){*success=2;return 1;}
    int opponent=0;
    for(int sq=0;sq<64;++sq)opponent+=p->board[sq]!=0 && (p->board[sq]<0)!=p->turn;
    if(opponent>1) {
        int found;
        if(!suicide_captures(v,p,alpha,beta,depth,value,&found))return 0;
        if(found){*success=2;return 1;}
    } else {
        char moves[POSITION_MAX_MOVES][8];size_t n=position_legal_moves(p,moves,POSITION_MAX_MOVES);
        for(size_t i=0;i<n;++i)if(position_is_capture(p,moves[i])){*value=-2;*success=2;return 1;}
    }
    int threat_found=0;
    if(threats || position_piece_count(p)>=6) {
        char moves[POSITION_MAX_MOVES][8];size_t n=position_legal_moves(p,moves,POSITION_MAX_MOVES);
        for(size_t i=0;i<n;++i)if(abs(p->board[from_square(moves[i])])!=1) {
            Position child=*p;int score,found;
            if(!position_apply_generated_uci(&child,moves[i]) || !suicide_captures(v,&child,-beta,-alpha,depth+1,&score,&found))return 0;
            score=-score;
            if(found && score>alpha){threat_found=1;alpha=score;if(alpha>=beta){*value=score;*success=3;return 1;}}
        }
    }
    int score;if(!raw_probe(v,p,0,0,&score))return 0;
    if(score>alpha){*value=score;*success=1;}
    else {*value=alpha;*success=threat_found?3:1;}
    return 1;
}
static int probe_ab(VariantSyzygy *v,const Position *p,int alpha,int beta,int threats,unsigned depth,int *value,int *success) {
    if(depth>64 || p->castling || position_piece_count(p)>VT_PIECES+1)return 0;
    if(p->variant==POSITION_ANTICHESS)return suicide_ab(v,p,alpha,beta,threats,depth,value,success);
    if(terminal_wdl(p,value)){*success=2;return 1;}
    char moves[POSITION_MAX_MOVES][8];size_t n=position_legal_moves(p,moves,POSITION_MAX_MOVES);
    for(size_t i=0;i<n;++i)if(p->board[to_square(moves[i])] && position_is_capture(p,moves[i])) {
        Position child=*p;int score,child_success;
        if(!position_apply_generated_uci(&child,moves[i]) || !probe_ab(v,&child,-beta,-alpha,0,depth+1,&score,&child_success))return 0;
        score=-score;
        if(score>alpha){if(score>=beta){*value=score;*success=2;return 1;}alpha=score;}
    }
    int score;if(!raw_probe(v,p,0,0,&score))return 0;
    if(alpha>=score){*value=alpha;*success=1+(alpha>0);}
    else {*value=score;*success=1;}
    return 1;
}
static int probe_wdl(VariantSyzygy *v,const Position *p,unsigned depth,int *value) {
    int score,success;
    if(!probe_ab(v,p,-2,2,0,depth,&score,&success))return 0;
    if(p->ep_square<0 || p->variant==POSITION_ANTICHESS){*value=score;return 1;}
    char moves[POSITION_MAX_MOVES][8];size_t n=position_legal_moves(p,moves,POSITION_MAX_MOVES);int ep_best=-3,non_ep=0;
    for(size_t i=0;i<n;++i) {
        if(!en_passant(p,moves[i])){non_ep=1;continue;}
        Position child=*p;int child_score;
        if(!position_apply_generated_uci(&child,moves[i]) || !probe_ab(v,&child,-2,2,0,depth+1,&child_score,&success))return 0;
        if(-child_score>ep_best)ep_best=-child_score;
    }
    if(ep_best>-3 && (ep_best>=score || (score==0 && !non_ep)))score=ep_best;
    *value=score;return 1;
}
static int before_zeroing(int wdl){return (wdl>0?1:-1)*(abs(wdl)==2?1:101);}
static int probe_dtz_no_ep(VariantSyzygy *v,const Position *p,unsigned depth,int *value) {
    int wdl,success;
    if(depth>64 || !probe_ab(v,p,-2,2,1,depth,&wdl,&success))return 0;
    if(!wdl){*value=0;return 1;}
    int non_pawn=0;
    for(int sq=0;sq<64;++sq)non_pawn|=p->board[sq]!=0 && (p->board[sq]<0)==p->turn && abs(p->board[sq])!=1;
    if(success==2 || !non_pawn){*value=before_zeroing(wdl);return 1;}
    char moves[POSITION_MAX_MOVES][8];size_t n=position_legal_moves(p,moves,POSITION_MAX_MOVES);
    if(wdl>0) {
        if(success==3){*value=wdl==2?2:102;return 1;}
        int missing_pawn=0;
        for(size_t i=0;i<n;++i)if(abs(p->board[from_square(moves[i])])==1 && !position_is_capture(p,moves[i])) {
            Position child=*p;int child_value;
            if(!position_apply_generated_uci(&child,moves[i]) || !probe_wdl(v,&child,depth+1,&child_value)){missing_pawn=1;continue;}
            if(-child_value==wdl){*value=wdl==2?1:101;return 1;}
        }
        if(missing_pawn)return 0;
    }
    int distance,status=raw_probe(v,p,1,wdl,&distance);
    if(!status)return 0;
    if(status==1){*value=before_zeroing(wdl)+(wdl>0?distance:-distance);return 1;}
    if(wdl>0) {
        int best=65535;
        for(size_t i=0;i<n;++i)if(abs(p->board[from_square(moves[i])])!=1 && !p->board[to_square(moves[i])]) {
            Position child=*p;int child_value;
            if(!position_apply_generated_uci(&child,moves[i]) || !probe_dtz(v,&child,depth+1,&child_value))return 0;
            child_value=-child_value;
            if(child_value==1 && position_in_check(&child,child.turn) && !position_legal_moves(&child,NULL,0))best=1;
            else if(child_value>0 && child_value+1<best)best=child_value+1;
        }
        if(best==65535)return 0;
        *value=best;return 1;
    }
    int best=-1;
    for(size_t i=0;i<n;++i) {
        Position child=*p;int child_value;
        if(!position_apply_generated_uci(&child,moves[i]))return 0;
        if(!child.halfmove_clock) {
            if(wdl==-2)child_value=-1;
            else {if(!probe_ab(v,&child,1,2,1,depth+1,&child_value,&success))return 0;child_value=child_value==2?0:-101;}
        } else {if(!probe_dtz(v,&child,depth+1,&child_value))return 0;child_value=-child_value-1;}
        if(child_value<best)best=child_value;
    }
    *value=best;return 1;
}
static int probe_dtz(VariantSyzygy *v,const Position *p,unsigned depth,int *value) {
    int distance;
    if(!probe_dtz_no_ep(v,p,depth,&distance))return 0;
    if(p->ep_square<0 || p->variant==POSITION_ANTICHESS){*value=distance;return 1;}
    char moves[POSITION_MAX_MOVES][8];size_t n=position_legal_moves(p,moves,POSITION_MAX_MOVES);int ep_best=-3,non_ep=0;
    for(size_t i=0;i<n;++i) {
        if(!en_passant(p,moves[i])){non_ep=1;continue;}
        Position child=*p;int score,success;
        if(!position_apply_generated_uci(&child,moves[i]) || !probe_ab(v,&child,-2,2,0,depth+1,&score,&success))return 0;
        if(-score>ep_best)ep_best=-score;
    }
    if(ep_best>-3) {
        int e=VT_WDL_TO_DTZ[ep_best+2];
        if(distance<-100){if(e>=0)distance=e;}
        else if(distance<0){if(e>=0 || e<-100)distance=e;}
        else if(distance>100){if(e>0)distance=e;}
        else if(distance>0){if(e==1)distance=e;}
        else if(e>=0 || !non_ep)distance=e;
    }
    *value=distance;return 1;
}
int variant_syzygy_probe(VariantSyzygy *v,const Position *p,int distance,int *value) {
    if(!v || !p || !value || (p->variant!=POSITION_ATOMIC && p->variant!=POSITION_ANTICHESS) || p->castling)return 0;
    return distance?probe_dtz(v,p,0,value):probe_wdl(v,p,0,value);
}
