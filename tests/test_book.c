#define _POSIX_C_SOURCE 200809L
#include "book.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"book check failed at line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void entry(FILE *f,uint64_t key,const char *move,unsigned weight) {
    unsigned char data[16]={0};for(int i=7;i>=0;--i){data[i]=(unsigned char)(key&255);key>>=8;}
    unsigned from=(unsigned)(move[0]-'a'+8*(move[1]-'1')),to=(unsigned)(move[2]-'a'+8*(move[3]-'1')),raw=(from<<6)|to;
    data[8]=(unsigned char)(raw>>8);data[9]=(unsigned char)raw;data[10]=(unsigned char)(weight>>8);data[11]=(unsigned char)weight;
    CHECK(fwrite(data,1,sizeof(data),f)==sizeof(data));
}
int main(void) {
    char paths[3][64]={"/tmp/lichess-book-a-XXXXXX","/tmp/lichess-book-b-XXXXXX","/tmp/lichess-book-bad-XXXXXX"};
    Position p;CHECK(position_init(&p,POSITION_STANDARD,"startpos",NULL,0));uint64_t key=position_polyglot_hash(&p);
    for(int i=0;i<3;++i) {
        int fd=mkstemp(paths[i]);CHECK(fd>=0);FILE *f=fdopen(fd,"wb");CHECK(f!=NULL);
        if(i==0){entry(f,key,"e2e5",65535);entry(f,key,"d2d4",25);entry(f,key,"e2e4",75);entry(f,key,"g1f3",0);}
        else if(i==1){entry(f,key,"c2c4",100);}
        else CHECK(fputc(1,f)!=EOF);
        CHECK(fclose(f)==0);
    }
    BookCollection books;book_collection_init(&books);char error[256];
    CHECK(book_collection_add(&books,paths[0],error,sizeof(error)));CHECK(book_collection_add(&books,paths[1],error,sizeof(error)));
    CHECK(!book_collection_add(&books,paths[2],error,sizeof(error)));
    BookOptions options={BOOK_BEST_MOVE,BOOK_NORMALIZE_NONE,1,20};BookResult result;uint64_t rng=72813;
    CHECK(book_choose(&books,&p,&options,&rng,&result));CHECK(!strcmp(result.move,"e2e4") && result.weight==75 && result.book_index==0);
    options.min_weight=80;CHECK(book_choose(&books,&p,&options,&rng,&result));CHECK(!strcmp(result.move,"c2c4") && result.book_index==1);
    options.normalization=BOOK_NORMALIZE_SUM;options.min_weight=30;
    CHECK(book_choose(&books,&p,&options,&rng,&result));CHECK(!strcmp(result.move,"e2e4"));
    options.normalization=BOOK_NORMALIZE_MAX;options.min_weight=90;
    CHECK(book_choose(&books,&p,&options,&rng,&result));CHECK(!strcmp(result.move,"e2e4"));
    options.selection=BOOK_WEIGHTED_RANDOM;options.min_weight=65535;options.normalization=BOOK_NORMALIZE_NONE;
    unsigned e4=0,d4=0;
    for(int i=0;i<4000;++i){CHECK(book_choose(&books,&p,&options,&rng,&result));e4+=!strcmp(result.move,"e2e4");d4+=!strcmp(result.move,"d2d4");}
    CHECK(e4+d4==4000 && e4>2800 && e4<3200);
    options.selection=BOOK_UNIFORM_RANDOM;options.min_weight=0;unsigned zero=0;
    for(int i=0;i<3000;++i){CHECK(book_choose(&books,&p,&options,&rng,&result));zero+=!strcmp(result.move,"g1f3");}
    CHECK(zero>850 && zero<1150);
    p.ply=39;CHECK(book_choose(&books,&p,&options,&rng,&result));p.ply=40;CHECK(!book_choose(&books,&p,&options,&rng,&result));
    p.ply=0;options.max_depth=0;CHECK(!book_choose(&books,&p,&options,&rng,&result));
    CHECK(book_selection_from_name("bad")==-1);CHECK(book_normalization_from_name("bad")==-1);
    book_collection_close(&books);CHECK(books.count==0);
    for(int i=0;i<3;++i)CHECK(unlink(paths[i])==0);
    puts("book: mmap/binary-search, legal filtering, ordered fallbacks, all selections, zero weights, normalization, max depth passed");
    return 0;
}
