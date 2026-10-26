#define _POSIX_C_SOURCE 200809L
#include "tablebase.h"
#include "variant_syzygy.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x) do {if(!(x)){fprintf(stderr,"tablebase check failed at line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void fields(char *line,char **out,int count) {
    line[strcspn(line,"\r\n")]=0;out[0]=line;
    for(int i=1;i<count;++i){char *tab=strchr(out[i-1],'\t');CHECK(tab!=NULL);*tab=0;out[i]=tab+1;}
}
static int in_moves(const char *list,const char *move) {
    size_t n=strlen(move);
    for(const char *p=list;*p;) {
        size_t len=strcspn(p," ");if(len==n && !memcmp(p,move,n))return 1;
        p+=len;if(*p)++p;
    }
    return 0;
}
static size_t move_count(const char *list) {
    size_t count=*list!=0;for(const char *p=list;*p;++p)count+=*p==' ';return count;
}
static void copy_file(const char *source,const char *destination) {
    FILE *in=fopen(source,"rb"),*out=fopen(destination,"wb");CHECK(in!=NULL && out!=NULL);
    unsigned char buffer[16384];size_t count;
    while((count=fread(buffer,1,sizeof(buffer),in))!=0)CHECK(fwrite(buffer,1,count,out)==count);
    CHECK(!ferror(in));CHECK(fclose(in)==0);CHECK(fclose(out)==0);
}
static unsigned test_values(Tablebases *tb) {
    FILE *file=fopen("tests/fixtures/tablebase_values.tsv","r");CHECK(file!=NULL);
    char line[2048],*f[4];unsigned count=0;
    while(fgets(line,sizeof(line),file)) {
        if(line[0]=='#' || line[0]=='\n')continue;
        fields(line,f,4);Position p;CHECK(position_init(&p,(unsigned)strtoul(f[0],NULL,10),f[1],NULL,0));
        for(int distance=0;distance<2;++distance) {
            int value;CHECK(variant_syzygy_probe(tb->variant_syzygy,&p,distance,&value));
            if(value!=atoi(f[distance+2]))fprintf(stderr,"raw variant %u distance %d FEN %s got %d expected %s\n",p.variant,distance,f[1],value,f[distance+2]);
            CHECK(value==atoi(f[distance+2]));++count;
        }
    }
    CHECK(!ferror(file));CHECK(!fclose(file));CHECK(count>300);return count;
}
static unsigned test_roots(Tablebases *tb) {
    FILE *file=fopen("tests/fixtures/tablebase_roots.tsv","r");CHECK(file!=NULL);
    char line[16384],*f[8];unsigned count=0;uint64_t rng=89134;
    while(fgets(line,sizeof(line),file)) {
        if(line[0]=='#' || line[0]=='\n')continue;
        fields(line,f,8);int source=atoi(f[0]),quality=atoi(f[2]);Position p;
        CHECK(position_init(&p,(unsigned)strtoul(f[1],NULL,10),f[3],NULL,0));
        TablebaseOptions options={source==TABLEBASE_SYZYGY?7:5,quality,120};TablebaseResult result;
        int ok=source==TABLEBASE_SYZYGY?tablebase_probe_syzygy(tb,&p,&options,&rng,&result):tablebase_probe_gaviota(tb,&p,&options,&rng,&result);
        if(!ok || result.wdl!=atoi(f[4]) || result.distance!=strtod(f[6],NULL))
            fprintf(stderr,"root source %d variant %u quality %d FEN %s: found=%d WDL=%d distance=%g\n",source,p.variant,quality,f[3],ok,result.wdl,result.distance);
        CHECK(ok);CHECK(result.source==source);CHECK(result.wdl==atoi(f[4]));CHECK(result.has_distance==atoi(f[5]));CHECK(result.distance==strtod(f[6],NULL));
        CHECK(result.count>0 && result.count<=POSITION_MAX_MOVES);
        if(quality==TABLEBASE_SUGGEST || !result.has_distance)CHECK(result.count==move_count(f[7]));
        else CHECK(result.count==1);
        CHECK(result.suggested==(result.count>1));
        for(size_t i=0;i<result.count;++i) {
            CHECK(position_is_legal(&p,result.moves[i]));CHECK(in_moves(f[7],result.moves[i]));
            for(size_t j=0;j<i;++j)CHECK(strcmp(result.moves[i],result.moves[j]));
        }
        ++count;
    }
    CHECK(!ferror(file));CHECK(!fclose(file));CHECK(count>150);return count;
}
static void test_fallback(void) {
    char temporary[]="/tmp/lbc-wdl-XXXXXX";CHECK(mkdtemp(temporary)!=NULL);
    const char *names[]={"KQvK","KRvK","KBvK","KNvK","KPvK"};char src[512],dst[512];
    for(size_t i=0;i<sizeof(names)/sizeof(names[0]);++i) {
        CHECK(snprintf(src,sizeof(src),"tests/fixtures/syzygy/%s.rtbw",names[i])>0);
        CHECK(snprintf(dst,sizeof(dst),"%s/%s.rtbw",temporary,names[i])>0);copy_file(src,dst);
    }
    const char *paths[]={temporary};Tablebases tb;char error[256];
    CHECK(tablebase_init(&tb,paths,1,NULL,0,error,sizeof(error)));
    Position p;CHECK(position_init(&p,POSITION_STANDARD,"4k3/8/8/8/8/2Q5/8/4K3 w - - 0 1",NULL,0));
    TablebaseOptions options={7,TABLEBASE_BEST,120};TablebaseResult result;uint64_t rng=8913;
    CHECK(tablebase_probe_syzygy(&tb,&p,&options,&rng,&result));
    CHECK(result.wdl==2 && !result.has_distance && result.count>1 && result.suggested);
    for(size_t i=0;i<result.count;++i)CHECK(position_is_legal(&p,result.moves[i]));
    tablebase_close(&tb);
    for(size_t i=0;i<sizeof(names)/sizeof(names[0]);++i){CHECK(snprintf(dst,sizeof(dst),"%s/%s.rtbw",temporary,names[i])>0);CHECK(unlink(dst)==0);}
    CHECK(rmdir(temporary)==0);
}
static void test_bad_variant_file(void) {
    char temporary[]="/tmp/lbc-bad-tb-XXXXXX";CHECK(mkdtemp(temporary)!=NULL);
    char path[512];CHECK(snprintf(path,sizeof(path),"%s/KQvK.atbw",temporary)>0);
    unsigned char bad[80]={0};FILE *file=fopen(path,"wb");CHECK(file!=NULL);
    CHECK(fwrite(bad,1,sizeof(bad),file)==sizeof(bad));CHECK(fclose(file)==0);
    const char *paths[]={temporary};VariantSyzygy *tb=variant_syzygy_open(paths,1);CHECK(tb!=NULL);
    Position p;CHECK(position_init(&p,POSITION_ATOMIC,"4k3/8/8/8/8/2Q5/8/4K3 w - - 0 1",NULL,0));
    int value;CHECK(!variant_syzygy_probe(tb,&p,0,&value));variant_syzygy_close(tb);
    CHECK(unlink(path)==0);CHECK(rmdir(temporary)==0);
}
int main(void) {
    CHECK(tablebase_dtz_to_wdl(-100.5)==-1);CHECK(tablebase_dtz_to_wdl(-100)==-1);
    CHECK(tablebase_dtz_to_wdl(-99.5)==-2);CHECK(tablebase_dtz_to_wdl(-0.5)==-2);
    CHECK(tablebase_dtz_to_wdl(0)==0);CHECK(tablebase_dtz_to_wdl(0.5)==2);
    CHECK(tablebase_dtz_to_wdl(99.5)==2);CHECK(tablebase_dtz_to_wdl(100)==1);
    CHECK(tablebase_dtm_to_wdl(-100,120)==-1);CHECK(tablebase_dtm_to_wdl(-99,120)==-2);
    CHECK(tablebase_dtm_to_wdl(0,120)==0);CHECK(tablebase_dtm_to_wdl(119,120)==2);
    CHECK(tablebase_dtm_to_wdl(120,120)==1);
    const char *syzygy[]={"tests/fixtures/syzygy"},*gaviota[]={"tests/fixtures/gaviota"};
    Tablebases tb;char error[256];CHECK(tablebase_init(&tb,syzygy,1,gaviota,1,error,sizeof(error)));
    CHECK(tb.syzygy_ready && tb.gaviota_ready && tb.variant_syzygy!=NULL);
    unsigned raw=test_values(&tb),roots=test_roots(&tb);
    Position p;TablebaseResult result;TablebaseOptions options={7,TABLEBASE_BEST,120};uint64_t rng=4514;
    CHECK(position_init(&p,POSITION_STANDARD,"7k/5K2/6Q1/8/8/8/8/8 w - - 99 1",NULL,0));
    CHECK(tablebase_probe_syzygy(&tb,&p,&options,&rng,&result));CHECK(result.wdl==2 && result.distance==1);
    Position child=p;CHECK(position_apply_uci(&child,result.moves[0],NULL,0));CHECK(position_outcome(&child)==1);
    CHECK(tablebase_probe_gaviota(&tb,&p,&options,&rng,&result));CHECK(result.wdl==2 && result.distance==1);
    child=p;CHECK(position_apply_uci(&child,result.moves[0],NULL,0));CHECK(position_outcome(&child)==1);
    options.max_pieces=2;CHECK(!tablebase_probe_syzygy(&tb,&p,&options,&rng,&result));CHECK(!tablebase_probe_gaviota(&tb,&p,&options,&rng,&result));
    options.max_pieces=7;CHECK(position_init(&p,POSITION_STANDARD,"4k3/8/8/8/8/8/8/4K2R w K - 0 1",NULL,0));
    CHECK(!tablebase_probe_syzygy(&tb,&p,&options,&rng,&result));CHECK(!tablebase_probe_gaviota(&tb,&p,&options,&rng,&result));
    CHECK(position_init(&p,POSITION_STANDARD,"8/8/8/8/8/8/3K3k/RR6 w - - 0 1",NULL,0));
    CHECK(!tablebase_probe_syzygy(&tb,&p,&options,&rng,&result));CHECK(!tablebase_probe_gaviota(&tb,&p,&options,&rng,&result));
    CHECK(position_init(&p,POSITION_CRAZYHOUSE,"4k3/8/8/8/8/2Q5/8/4K3[] w - - 0 1",NULL,0));
    CHECK(!tablebase_probe_syzygy(&tb,&p,&options,&rng,&result));CHECK(!tablebase_probe_gaviota(&tb,&p,&options,&rng,&result));
    tablebase_close(&tb);tablebase_close(&tb);
    const char *missing[]={"/this/directory/does/not/exist"};CHECK(!tablebase_init(&tb,missing,1,NULL,0,error,sizeof(error)));tablebase_close(&tb);
    test_fallback();test_bad_variant_file();
    printf("tablebase: %u golden raw probes, %u root selections, Syzygy/Gaviota/atomic/antichess/rule50/WDL fallback/malformed tables passed\n",raw,roots);
    return 0;
}
