#include "position.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"position check failed at line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static int move_compare(const void *a,const void *b){return strcmp(a,b);}
static unsigned long candidate_checks;
static void check_candidate(const Position *p,char (*moves)[8],size_t n,const char *candidate) {
    char normalized[8];
    int expected=position_normalize_uci(p,candidate,p->chess960,normalized) &&
        bsearch(normalized,moves,n,sizeof(moves[0]),move_compare)!=NULL;
    int actual=position_is_legal(p,candidate);
    if(actual!=expected)fprintf(stderr,"candidate mismatch variant=%u move=%s expected=%d actual=%d\n",p->variant,candidate,expected,actual);
    CHECK(actual==expected);++candidate_checks;
}
/* Compare single-move legality with reference move sets, including illegal moves. */
static void check_candidates(const Position *p,char (*moves)[8],size_t n) {
    char move[8];
    for(int from=0;from<64;++from) for(int to=0;to<64;++to) {
        move[0]=(char)('a'+from%8);move[1]=(char)('1'+from/8);
        move[2]=(char)('a'+to%8);move[3]=(char)('1'+to/8);move[4]=0;
        check_candidate(p,moves,n,move);
        if(to<8 || to>=56) for(const char *promo="qrbnk";*promo;++promo) {
            move[4]=*promo;move[5]=0;check_candidate(p,moves,n,move);
        }
    }
    for(const char *piece="PNBRQK";*piece;++piece) for(int to=0;to<64;++to) {
        move[0]=*piece;move[1]='@';move[2]=(char)('a'+to%8);move[3]=(char)('1'+to/8);move[4]=0;
        check_candidate(p,moves,n,move);
    }
}
static uint64_t perft(const Position *p,int depth) {
    if(!depth)return 1;
    char moves[POSITION_MAX_MOVES][8];size_t count=position_legal_moves(p,moves,POSITION_MAX_MOVES);uint64_t nodes=0;
    for(size_t i=0;i<count;++i){Position child=*p;CHECK(position_apply_generated_uci(&child,moves[i]));nodes+=perft(&child,depth-1);}
    return nodes;
}
int main(int argc,char **argv) {
    const char *path=argc>1?argv[1]:"tests/fixtures/positions.tsv";
    FILE *f=fopen(path,"r");CHECK(f!=NULL);char line[16384],error[256];unsigned cases=0;
    while(fgets(line,sizeof(line),f)) {
        if(line[0]=='#' || line[0]=='\n')continue;
        line[strcspn(line,"\r\n")]=0;char *fields[7];fields[0]=line;
        for(int i=1;i<7;++i){char *tab=strchr(fields[i-1],'\t');CHECK(tab!=NULL);*tab=0;fields[i]=tab+1;}
        unsigned variant=(unsigned)strtoul(fields[0],NULL,10);Position p;
        CHECK(position_init(&p,variant,fields[1],error,sizeof(error)));
        char fen[POSITION_FEN_MAX];CHECK(position_fen(&p,fen,sizeof(fen)));CHECK(!strcmp(fen,fields[1]));
        CHECK(position_polyglot_hash(&p)==(uint64_t)strtoull(fields[2],NULL,16));
        char moves[POSITION_MAX_MOVES][8];size_t n=position_legal_moves(&p,moves,POSITION_MAX_MOVES);CHECK(n<=POSITION_MAX_MOVES);
        qsort(moves,n,sizeof(moves[0]),move_compare);char joined[POSITION_MAX_MOVES*8];size_t used=0;
        for(size_t i=0;i<n;++i){if(i)joined[used++]=' ';size_t len=strlen(moves[i]);memcpy(joined+used,moves[i],len);used+=len;}
        if(!used)joined[used++]='-';
        joined[used]=0;CHECK(!strcmp(joined,fields[3]));
        check_candidates(&p,moves,n);
        if(strcmp(fields[4],"-")) {
            char san[64],uci[8];CHECK(position_uci_to_san(&p,fields[4],san,sizeof(san)));CHECK(!strcmp(san,fields[5]));
            CHECK(position_san_to_uci(&p,fields[5],uci));CHECK(!strcmp(uci,fields[4]));
            CHECK(position_apply_uci(&p,fields[4],error,sizeof(error)));CHECK(position_fen(&p,fen,sizeof(fen)));CHECK(!strcmp(fen,fields[6]));
        }
        ++cases;
    }
    CHECK(!ferror(f));CHECK(fclose(f)==0);CHECK(cases>200);
    Position p;CHECK(position_init(&p,POSITION_STANDARD,"startpos",NULL,0));
    CHECK(position_variant_from_name("Chess960")==POSITION_CHESS960);
    CHECK(position_variant_from_name("Three-check")==POSITION_THREE_CHECK);
    CHECK(position_variant_from_name("King of the Hill")==POSITION_KING_OF_THE_HILL);
    CHECK(position_variant_from_name("Racing Kings")==POSITION_RACING_KINGS);
    CHECK(position_variant_from_name("Fischer Random")==POSITION_CHESS960);
    CHECK(position_variant_from_name("Not a variant")==0);
    CHECK(perft(&p,1)==20);CHECK(perft(&p,2)==400);CHECK(perft(&p,3)==8902);CHECK(perft(&p,4)==197281);
    char uci[8];CHECK(position_san_to_uci(&p,"Ng1-f3",uci));CHECK(!strcmp(uci,"g1f3"));
    CHECK(position_san_to_uci(&p,"e2-e4",uci));CHECK(!strcmp(uci,"e2e4"));
    CHECK(!position_is_legal(&p,"e2e5"));CHECK(!position_is_legal(&p,"P@e4"));CHECK(!position_is_legal(&p,"0000"));
    CHECK(position_apply_uci(&p,"0000",NULL,0));CHECK(p.turn==1 && p.ply==1 && p.halfmove_clock==1);
    CHECK(position_apply_uci(&p,"@@@@",NULL,0));CHECK(p.turn==0 && p.fullmove_number==2);
    CHECK(!position_init(&p,POSITION_STANDARD,"8/8 w - - 0 1",NULL,0));
    CHECK(!position_init(&p,POSITION_STANDARD,"8/8/8/8/8/8/8/8 x - - 0 1",NULL,0));
    CHECK(position_from_history(&p,POSITION_STANDARD,"startpos","e2e4 e7e5 g1f3 b8c6 f1c4 g8f6 e1h1",NULL,0));
    CHECK(p.board[6]==POSITION_KING && p.board[5]==POSITION_ROOK);
    CHECK(position_init(&p,POSITION_CHESS960,"4k3/8/8/8/8/8/8/R1K1R3 w KQ - 0 1",NULL,0));
    CHECK(position_normalize_uci(&p,"c1a1",0,uci));CHECK(!strcmp(uci,"c1c1"));
    CHECK(position_normalize_uci(&p,"c1e1",0,uci));CHECK(!strcmp(uci,"c1g1"));
    CHECK(position_apply_uci(&p,"c1a1",NULL,0));CHECK(p.board[2]==6 && p.board[3]==4 && p.board[4]==4);
    printf("position: %u independent golden positions, 206603 standard perft nodes, variants/SAN/FEN/Polyglot/Chess960/LAN passed\n",cases);
    printf("single-move legality: %lu golden-set candidate checks passed\n",candidate_checks);
    return 0;
}
