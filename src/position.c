#include "position.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int from, to, promotion, drop, rook, null; } Move;
static const char symbols[] = " PNBRQK";
static const int knight_dx[8] = {1,2,2,1,-1,-2,-2,-1};
static const int knight_dy[8] = {2,1,-1,-2,-2,-1,1,2};
static const int ray_dx[8] = {1,-1,0,0,1,1,-1,-1};
static const int ray_dy[8] = {0,0,1,-1,1,-1,1,-1};

static int failure(char *err, size_t cap, const char *text) {
    if (err && cap) (void)snprintf(err, cap, "%s", text);
    return 0;
}
static int color_of(int pc) { return pc < 0; }
static int ours(int pc, int color) { return pc && color_of(pc) == color; }
static uint64_t bit(int sq) { return UINT64_C(1) << (unsigned)sq; }
static int coordinate(const char *s) {
    if (s[0] < 'a' || s[0] > 'h' || s[1] < '1' || s[1] > '8') return -1;
    return s[0] - 'a' + (s[1] - '1') * 8;
}
static void square_name(int sq, char *s) {
    s[0] = (char)('a' + (sq & 7)); s[1] = (char)('1' + (sq >> 3));
}
static int piece_type(char ch) {
    ch = (char)toupper((unsigned char)ch);
    for (int pt = 1; pt <= 6; ++pt) if (symbols[pt] == ch) return pt;
    return 0;
}
static int king_square(const Position *p, int color) {
    for (int sq = 63; sq >= 0; --sq)
        if (p->board[sq] == (color ? -6 : 6) && !(p->promoted & bit(sq))) return sq;
    return -1;
}
static int adjacent(int a, int b) {
    return a >= 0 && b >= 0 && abs((a & 7) - (b & 7)) <= 1 && abs((a >> 3) - (b >> 3)) <= 1;
}

unsigned position_variant_from_name(const char *name) {
    if (!name) return POSITION_STANDARD;
    char normalized[64];size_t n=0;
    for (const unsigned char *s=(const unsigned char *)name;*s;++s) {
        if (isspace(*s) || *s=='-' || *s=='_') continue;
        if (n+1>=sizeof(normalized)) return 0;
        normalized[n++]=(char)tolower(*s);
    }
    normalized[n]=0;name=normalized;
    if (!strcmp(name,"standard") || !strcmp(name,"chess") || !strcmp(name,"normal")) return POSITION_STANDARD;
    if (!strcmp(name,"fromposition")) return POSITION_FROM_POSITION;
    if (!strcmp(name,"chess960") || !strcmp(name,"fischerandom") || !strcmp(name,"fischerrandom")) return POSITION_CHESS960;
    if (!strcmp(name,"antichess") || !strcmp(name,"giveaway") || !strcmp(name,"suicide")) return POSITION_ANTICHESS;
    if (!strcmp(name,"atomic")) return POSITION_ATOMIC;
    if (!strcmp(name,"crazyhouse")) return POSITION_CRAZYHOUSE;
    if (!strcmp(name,"horde")) return POSITION_HORDE;
    if (!strcmp(name,"kingofthehill")) return POSITION_KING_OF_THE_HILL;
    if (!strcmp(name,"racingkings")) return POSITION_RACING_KINGS;
    if (!strcmp(name,"threecheck") || !strcmp(name,"3check")) return POSITION_THREE_CHECK;
    return 0;
}
const char *position_variant_name(unsigned variant) {
    switch (variant) {
    case POSITION_STANDARD: return "standard";
    case POSITION_FROM_POSITION: return "fromPosition";
    case POSITION_CHESS960: return "chess960";
    case POSITION_ANTICHESS: return "antichess";
    case POSITION_ATOMIC: return "atomic";
    case POSITION_CRAZYHOUSE: return "crazyhouse";
    case POSITION_HORDE: return "horde";
    case POSITION_KING_OF_THE_HILL: return "kingOfTheHill";
    case POSITION_RACING_KINGS: return "racingKings";
    case POSITION_THREE_CHECK: return "threeCheck";
    default: return "unknown";
    }
}
const char *position_initial_fen(unsigned variant) {
    switch (variant) {
    case POSITION_ANTICHESS: return "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w - - 0 1";
    case POSITION_HORDE: return "rnbqkbnr/pppppppp/8/1PP2PP1/PPPPPPPP/PPPPPPPP/PPPPPPPP/PPPPPPPP w kq - 0 1";
    case POSITION_RACING_KINGS: return "8/8/8/8/8/8/krbnNBRK/qrbnNBRQ w - - 0 1";
    case POSITION_THREE_CHECK: return "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 3+3 0 1";
    case POSITION_CRAZYHOUSE: return "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR[] w KQkq - 0 1";
    default: return POSITION_START_FEN;
    }
}

static int attacked(const Position *p, int sq, int by) {
    int x = sq & 7, y = sq >> 3;
    int pawn_y = y + (by ? 1 : -1);
    if (pawn_y >= 0 && pawn_y < 8) for (int dx = -1; dx <= 1; dx += 2) {
        int xx = x + dx;
        if (xx >= 0 && xx < 8 && p->board[pawn_y * 8 + xx] == (by ? -1 : 1)) return 1;
    }
    for (int i = 0; i < 8; ++i) {
        int xx = x + knight_dx[i], yy = y + knight_dy[i];
        if (xx >= 0 && xx < 8 && yy >= 0 && yy < 8 && p->board[yy * 8 + xx] == (by ? -2 : 2)) return 1;
    }
    for (int i = 0; i < 8; ++i) {
        int xx = x + ray_dx[i], yy = y + ray_dy[i], distance = 1;
        while (xx >= 0 && xx < 8 && yy >= 0 && yy < 8) {
            int pc = p->board[yy * 8 + xx];
            if (pc) {
                if (ours(pc, by)) {
                    int pt = abs(pc);
                    if (pt == 5 || (i < 4 ? pt == 4 : pt == 3) || (distance == 1 && pt == 6)) return 1;
                }
                break;
            }
            xx += ray_dx[i]; yy += ray_dy[i]; ++distance;
        }
    }
    return 0;
}
int position_in_check(const Position *p, int color) {
    if (p->variant == POSITION_ANTICHESS) return 0;
    int king = king_square(p,color);
    if (king < 0) return 0;
    if (p->variant == POSITION_ATOMIC && adjacent(king,king_square(p,!color))) return 0;
    return attacked(p,king,!color);
}
int position_piece_count(const Position *p) {
    int n = 0;
    for (int i = 0; i < 64; ++i) n += p->board[i] != 0;
    return n;
}
static int has_pieces(const Position *p, int color) {
    for (int i = 0; i < 64; ++i) if (ours(p->board[i],color)) return 1;
    return 0;
}
int position_variant_end(const Position *p) {
    if (p->variant == POSITION_THREE_CHECK) return p->remaining_checks[0] <= 0 || p->remaining_checks[1] <= 0;
    if (p->variant == POSITION_ANTICHESS || p->variant == POSITION_HORDE)
        return !has_pieces(p,0) || !has_pieces(p,1);
    if (p->variant == POSITION_ATOMIC) return king_square(p,0) < 0 || king_square(p,1) < 0;
    if (p->variant == POSITION_KING_OF_THE_HILL) {
        for (int i = 27; i <= 36; ++i) if ((i==27 || i==28 || i==35 || i==36) && abs(p->board[i]) == 6) return 1;
    }
    if (p->variant == POSITION_RACING_KINGS) {
        int wk=king_square(p,0), bk=king_square(p,1);
        if (wk < 56 && bk < 56) return 0;
        if (p->turn == 0 || bk >= 56 || bk < 0) return 1;
        for (int x=0; x<8; ++x) {
            int to=56+x;
            if (adjacent(bk,to) && !ours(p->board[to],1) && !attacked(p,to,0)) return 0;
        }
        return 1;
    }
    return 0;
}

static uint64_t clean_castling(const Position *p) {
    uint64_t rights = 0;
    for (int c=0; c<2; ++c) {
        int k=king_square(p,c), rank=c?56:0;
        if (k < rank || k >= rank+8 || (!p->chess960 && (k&7)!=4)) continue;
        for (int f=0; f<8; ++f) {
            int sq=rank+f;
            if (p->castling & bit(sq)) {
                if (p->board[sq] == (c?-4:4) && !(p->promoted & bit(sq)) && (p->chess960 || f==0 || f==7)) rights |= bit(sq);
            }
        }
        /* X-FEN cannot retain two rights on the same side of the king. */
        if (p->chess960) {
            int left=-1,right=-1;
            for (int f=0; f<8; ++f) if (rights & bit(rank+f)) {
                if (rank+f<k && left<0) left=rank+f;
                if (rank+f>k) right=rank+f;
            }
            rights &= ~(UINT64_C(255) << (unsigned)rank);
            if (left>=0) rights |= bit(left);
            if (right>=0) rights |= bit(right);
        }
    }
    return rights;
}
static int integer_field(const char *s, int min, int max, int *out) {
    if (!s || !*s || *s=='-') return 0;
    char *end=NULL;
    long n=strtol(s,&end,10);
    if (!end || *end || n<min || n>max) return 0;
    *out=(int)n; return 1;
}
int position_init(Position *p, unsigned variant, const char *fen, char *err, size_t cap) {
    if (!p || !position_variant_from_name(position_variant_name(variant))) return failure(err,cap,"Unknown chess variant");
    if (!fen || !*fen || !strcmp(fen,"startpos")) fen=position_initial_fen(variant);
    if (strlen(fen)>=POSITION_FEN_MAX) return failure(err,cap,"FEN is too long");
    Position q;
    memset(&q,0,sizeof(q)); q.variant=variant; q.ep_square=-1; q.fullmove_number=1;
    q.chess960=variant==POSITION_CHESS960; q.remaining_checks[0]=q.remaining_checks[1]=3;
    char copy[POSITION_FEN_MAX]; (void)snprintf(copy,sizeof(copy),"%s",fen);
    char *fields[8]; int nf=0; char *s=copy;
    while (*s) {
        while (isspace((unsigned char)*s)) ++s;
        if (!*s) break;
        if (nf>=8) return failure(err,cap,"Too many FEN fields");
        fields[nf++]=s;
        while (*s && !isspace((unsigned char)*s)) ++s;
        if (*s) *s++=0;
    }
    if (nf<2) return failure(err,cap,"FEN requires board and side to move");
    int rank=7,file=0,last=-1; s=fields[0];
    while (*s && *s!='[') {
        char ch=*s++;
        if (ch=='/' && rank==0 && file==8 && variant==POSITION_CRAZYHOUSE) break;
        if (ch=='/') {
            if (file!=8 || rank==0) return failure(err,cap,"Invalid FEN rank");
            --rank; file=0; last=-1;
        } else if (ch>='1' && ch<='8') {
            file+=ch-'0'; last=-1;
            if (file>8) return failure(err,cap,"Too many squares in FEN rank");
        } else if (ch=='~') {
            if (last<0) return failure(err,cap,"Invalid promoted marker");
            q.promoted |= bit(last); last=-1;
        } else {
            int pt=piece_type(ch);
            if (!pt || file>=8) return failure(err,cap,"Invalid FEN piece");
            last=rank*8+file++; q.board[last]=(int8_t)(islower((unsigned char)ch)?-pt:pt);
        }
    }
    if (rank!=0 || file!=8) return failure(err,cap,"FEN must contain 8 complete ranks");
    if (*s=='[') ++s;
    if (*s) {
        if (variant!=POSITION_CRAZYHOUSE) return failure(err,cap,"Pocket requires crazyhouse");
        while (*s && *s!=']') {
            char ch=*s++; int pt=piece_type(ch), c=islower((unsigned char)ch)!=0;
            if (!pt || pt==6 || q.pockets[c][pt]>=64) return failure(err,cap,"Invalid crazyhouse pocket");
            ++q.pockets[c][pt];
        }
        if (*s==']') ++s;
        if (*s) return failure(err,cap,"Invalid text after pocket");
    }
    if (!strcmp(fields[1],"w")) q.turn=0;
    else if (!strcmp(fields[1],"b")) q.turn=1;
    else return failure(err,cap,"Invalid FEN side to move");
    if (nf>=3 && strcmp(fields[2],"-")) for (s=fields[2]; *s; ++s) {
        int c=islower((unsigned char)*s)!=0, base=c?56:0, k=king_square(&q,c), f=-1;
        char ch=(char)toupper((unsigned char)*s);
        if (ch=='K') {
            for (int j=7;j>=0;--j) if (q.board[base+j]==(c?-4:4) && base+j>k) {f=j;break;}
            if (f<0) f=7;
        } else if (ch=='Q') {
            for (int j=0;j<8;++j) if (q.board[base+j]==(c?-4:4) && base+j<k) {f=j;break;}
            if (f<0) f=0;
        } else if (ch>='A' && ch<='H') f=ch-'A';
        else return failure(err,cap,"Invalid FEN castling right");
        q.castling |= bit(base+f);
        if (variant==POSITION_FROM_POSITION && (k!=base+4 || (f!=0 && f!=7))) q.chess960=1;
    }
    if (nf>=4 && strcmp(fields[3],"-")) {
        if (strlen(fields[3])!=2 || (q.ep_square=coordinate(fields[3]))<0 || (q.ep_square/8!=2 && q.ep_square/8!=5))
            return failure(err,cap,"Invalid FEN en passant square");
    }
    int clocks=4;
    if (variant==POSITION_THREE_CHECK && nf>=5 && strchr(fields[4],'+')) {
        int w,b; char tail;
        if (sscanf(fields[4],"%d+%d%c",&w,&b,&tail)!=2 || w<0 || w>3 || b<0 || b>3) return failure(err,cap,"Invalid remaining checks");
        q.remaining_checks[0]=w; q.remaining_checks[1]=b; clocks=5;
    }
    if (nf>clocks && !integer_field(fields[clocks],0,INT_MAX-1,&q.halfmove_clock)) return failure(err,cap,"Invalid halfmove clock");
    if (nf>clocks+1 && !integer_field(fields[clocks+1],0,INT_MAX-1,&q.fullmove_number)) return failure(err,cap,"Invalid fullmove number");
    if (!q.fullmove_number) q.fullmove_number=1;
    if (variant==POSITION_THREE_CHECK && nf==7 && clocks==4) {
        int w,b; char tail;
        if (sscanf(fields[6],"+%d+%d%c",&w,&b,&tail)!=2 || w<0 || w>3 || b<0 || b>3) return failure(err,cap,"Invalid delivered checks");
        q.remaining_checks[0]=3-w; q.remaining_checks[1]=3-b;
    } else if (nf>clocks+2) return failure(err,cap,"Unexpected FEN fields");
    q.castling=clean_castling(&q);
    *p=q; if (err && cap) *err=0; return 1;
}

static int capture_move(const Position *p, const Move *m) {
    if (m->null || m->drop || m->rook>=0) return 0;
    return p->board[m->to]!=0 || (abs(p->board[m->from])==1 && m->to==p->ep_square && (m->from&7)!=(m->to&7));
}
static void remove_piece(Position *p,int sq) {
    int pc=p->board[sq];
    if (abs(pc)==6 && !(p->promoted & bit(sq))) p->castling &= ~(UINT64_C(255) << (pc<0?56U:0U));
    p->board[sq]=0; p->promoted &= ~bit(sq); p->castling &= ~bit(sq);
}
static void push_move(Position *p,const Move *m) {
    int color=p->turn, sign=color?-1:1, old_ep=p->ep_square;
    p->ep_square=-1;
    if (p->halfmove_clock<INT_MAX) ++p->halfmove_clock;
    if (color && p->fullmove_number<INT_MAX) ++p->fullmove_number;
    ++p->ply;
    if (m->null) {p->turn=!color; return;}
    if (m->drop) {
        p->board[m->to]=(int8_t)(sign*m->drop); p->promoted &= ~bit(m->to);
        --p->pockets[color][m->drop];
    } else if (m->rook>=0) {
        int base=color?56:0, ks=m->rook>m->from;
        remove_piece(p,m->from); remove_piece(p,m->rook);
        p->board[base+(ks?6:2)]=(int8_t)(sign*6);
        p->board[base+(ks?5:3)]=(int8_t)(sign*4);
    } else {
        int pc=p->board[m->from], was_promoted=(p->promoted & bit(m->from))!=0;
        int capture_sq=m->to;
        if (abs(pc)==1 && m->to==old_ep && !p->board[m->to] && (m->from&7)!=(m->to&7)) capture_sq=m->to+(color?8:-8);
        int captured=p->board[capture_sq], captured_promoted=(p->promoted & bit(capture_sq))!=0;
        if (abs(pc)==1 || captured) p->halfmove_clock=0;
        if (abs(pc)==1 && abs(m->to-m->from)==16 && (m->from/8==1 || m->from/8==6)) p->ep_square=(m->from+m->to)/2;
        remove_piece(p,m->from); remove_piece(p,capture_sq);
        p->board[m->to]=(int8_t)(m->promotion?sign*m->promotion:pc);
        if (m->promotion || was_promoted) p->promoted |= bit(m->to);
        if (captured && p->variant==POSITION_CRAZYHOUSE) ++p->pockets[color][captured_promoted?1:abs(captured)];
        if (captured && p->variant==POSITION_ATOMIC) {
            remove_piece(p,m->to);
            for (int i=0;i<8;++i) {
                int x=(m->to&7)+ray_dx[i], y=(m->to>>3)+ray_dy[i];
                if (x>=0 && x<8 && y>=0 && y<8 && abs(p->board[y*8+x])!=1) remove_piece(p,y*8+x);
            }
        }
    }
    p->turn=!color;
    if (p->variant==POSITION_THREE_CHECK && position_in_check(p,p->turn)) --p->remaining_checks[color];
}
static int legal_after(const Position *p,const Move *m) {
    if (p->variant==POSITION_ANTICHESS) return 1;
    Position q=*p; push_move(&q,m);
    if (p->variant==POSITION_ATOMIC) {
        if (king_square(&q,p->turn)<0) return 0;
        if (king_square(&q,!p->turn)<0) return 1;
    }
    if (position_in_check(&q,p->turn)) return 0;
    if (p->variant==POSITION_RACING_KINGS && position_in_check(&q,!p->turn)) return 0;
    return 1;
}
static int castle_attacked(const Position *p,int sq,int by) {
    if (p->variant==POSITION_ANTICHESS) return 0;
    if (p->variant==POSITION_ATOMIC && adjacent(sq,king_square(p,by))) return 0;
    return attacked(p,sq,by);
}
static int castle_possible(const Position *p,int k,int r) {
    int color=p->turn, base=color?56:0, ks=r>k, kt=base+(ks?6:2), rt=base+(ks?5:3);
    if (!(clean_castling(p)&bit(r)) || k/8!=base/8) return 0;
    int lo=k<kt?k:kt, hi=k>kt?k:kt;
    for (int sq=lo;sq<=hi;++sq) if (sq!=k && sq!=r && p->board[sq]) return 0;
    lo=r<rt?r:rt; hi=r>rt?r:rt;
    for (int sq=lo;sq<=hi;++sq) if (sq!=k && sq!=r && p->board[sq]) return 0;
    Position q=*p; q.board[k]=0;
    if (castle_attacked(&q,k,!color)) return 0;
    int step=kt>k?1:-1;
    for (int sq=k;sq!=kt;sq+=step) if (castle_attacked(&q,sq,!color)) return 0;
    q.board[r]=0; q.board[rt]=(int8_t)(color?-4:4);
    if (castle_attacked(&q,kt,!color)) return 0;
    return 1;
}
static int same_move(const Move *a,const Move *b);
static void add_move(const Position *p,Move *moves,size_t *n,Move m,const Move *wanted) {
    if (wanted && !same_move(&m,wanted)) return;
    if (*n<POSITION_MAX_MOVES && legal_after(p,&m)) moves[(*n)++]=m;
}
static void pawn_move(const Position *p,Move *moves,size_t *n,int from,int to,const Move *wanted) {
    Move m={from,to,0,0,-1,0};
    if (to<8 || to>=56) {
        for (int pt=2;pt<=5;++pt) {m.promotion=pt;add_move(p,moves,n,m,wanted);}
        if (p->variant==POSITION_ANTICHESS) {m.promotion=6;add_move(p,moves,n,m,wanted);}
    } else add_move(p,moves,n,m,wanted);
}
/* source=-1 scans all pieces; antichess always checks for compulsory captures.
 * drop_square limits crazyhouse drops. */
static size_t generate_filtered(const Position *p,Move *moves,int source,int drops,int drop_square,const Move *wanted) {
    if (position_variant_end(p)) return 0;
    size_t n=0;
    for (int from=source<0?0:source;from<(source<0?64:source+1);++from) {
        int pc=p->board[from]; if (!ours(pc,p->turn)) continue;
        int pt=abs(pc), x=from&7, y=from>>3;
        if (pt==1) {
            int dy=p->turn?-1:1, yy=y+dy;
            if (yy<0 || yy>=8) continue;
            int to=yy*8+x;
            if (!p->board[to]) {
                pawn_move(p,moves,&n,from,to,wanted);
                int initial=p->turn?y==6:(y==1 || (p->variant==POSITION_HORDE && y==0));
                if (initial && !p->board[(y+2*dy)*8+x]) pawn_move(p,moves,&n,from,(y+2*dy)*8+x,wanted);
            }
            for (int dx=-1;dx<=1;dx+=2) if (x+dx>=0 && x+dx<8) {
                to=yy*8+x+dx;
                int target=p->board[to];
                if ((target && !ours(target,p->turn)) || (to==p->ep_square && !target && p->board[to-dy*8]==(p->turn?1:-1)))
                    pawn_move(p,moves,&n,from,to,wanted);
            }
        } else if (pt==2) {
            for (int i=0;i<8;++i) {
                int xx=x+knight_dx[i],yy=y+knight_dy[i];
                if (xx<0 || xx>=8 || yy<0 || yy>=8 || ours(p->board[yy*8+xx],p->turn)) continue;
                Move m={from,yy*8+xx,0,0,-1,0};add_move(p,moves,&n,m,wanted);
            }
        } else {
            int begin=pt==3?4:0,end=pt==4?4:8;
            for (int i=begin;i<end;++i) {
                int xx=x+ray_dx[i], yy=y+ray_dy[i];
                while (xx>=0 && xx<8 && yy>=0 && yy<8) {
                    int to=yy*8+xx, target=p->board[to];
                    if (ours(target,p->turn)) break;
                    Move m={from,to,0,0,-1,0};add_move(p,moves,&n,m,wanted);
                    if (target || pt==6) break;
                    xx+=ray_dx[i];yy+=ray_dy[i];
                }
            }
            if (pt==6 && !(p->promoted&bit(from))) {
                int base=p->turn?56:0;
                for (int f=0;f<8;++f) if ((p->castling&bit(base+f)) && castle_possible(p,from,base+f)) {
                    Move m={from,base+f,0,0,base+f,0};add_move(p,moves,&n,m,wanted);
                }
            }
        }
    }
    if (p->variant==POSITION_ANTICHESS) {
        size_t captures=0;
        for (size_t i=0;i<n;++i) if (capture_move(p,&moves[i])) moves[captures++]=moves[i];
        if (captures) n=captures;
    }
    if (drops && p->variant==POSITION_CRAZYHOUSE) {
        for (int pt=1;pt<=5;++pt) if (p->pockets[p->turn][pt]) for (int to=drop_square<0?0:drop_square;to<(drop_square<0?64:drop_square+1);++to) {
            if (p->board[to] || (pt==1 && (to<8 || to>=56))) continue;
            Move m={to,to,0,pt,-1,0};add_move(p,moves,&n,m,wanted);
        }
    }
    return n;
}
static size_t generate(const Position *p,Move *moves) {
    return generate_filtered(p,moves,-1,1,-1,NULL);
}
static void move_uci(const Position *p,const Move *m,int mode,char out[8]) {
    if (m->null) {memcpy(out,"0000",5);return;}
    if (m->drop) {out[0]=symbols[m->drop];out[1]='@';square_name(m->to,out+2);out[4]=0;return;}
    int to=m->to;
    if (m->rook>=0 && !mode) to=(p->turn?56:0)+(m->rook>m->from?6:2);
    square_name(m->from,out);square_name(to,out+2);
    if (m->promotion) {out[4]=(char)tolower((unsigned char)symbols[m->promotion]);out[5]=0;}
    else out[4]=0;
}
static int read_move(const Position *p,const char *uci,Move *m) {
    if (!uci) return 0;
    size_t len=strlen(uci);
    *m=(Move){0,0,0,0,-1,0};
    if (!strcmp(uci,"0000") || !strcmp(uci,"@@@@")) {m->null=1;return 1;}
    if (len==4 && uci[1]=='@') {
        m->drop=piece_type(uci[0]); m->to=coordinate(uci+2); m->from=m->to;
        return m->drop>0 && m->drop<6 && m->to>=0;
    }
    if (len!=4 && len!=5) return 0;
    m->from=coordinate(uci);m->to=coordinate(uci+2);
    if (m->from<0 || m->to<0) return 0;
    if (len==5) {
        m->promotion=piece_type(uci[4]);
        if (m->promotion<2 || (m->promotion>5 && p->variant!=POSITION_ANTICHESS)) return 0;
    }
    int pc=p->board[m->from];
    if (abs(pc)==6 && ours(pc,p->turn) && !m->promotion) {
        if (p->board[m->to]==(p->turn?-4:4) && (p->castling&bit(m->to))) m->rook=m->to;
        else if ((m->to/8)==(p->turn?7:0) && ((m->to&7)==2 || (m->to&7)==6) &&
                 (!p->chess960 || abs(m->to-m->from)>1 || m->to==m->from)) {
            int side=(m->to&7)==6, base=p->turn?56:0;
            for (int f=0;f<8;++f) if ((p->castling&bit(base+f)) && ((base+f>m->from)==side)) m->rook=base+f;
            if (m->rook>=0) m->to=m->rook;
        }
    }
    return m->from!=m->to || m->rook>=0;
}
static int same_move(const Move *a,const Move *b) {
    return a->from==b->from && a->to==b->to && a->promotion==b->promotion && a->drop==b->drop && a->rook==b->rook && a->null==b->null;
}
static int find_legal(const Position *p,const char *uci,Move *out) {
    Move want; if (!read_move(p,uci,&want) || want.null) return 0;
    Move moves[POSITION_MAX_MOVES];
    size_t n=p->variant==POSITION_ANTICHESS ? generate(p,moves) :
        generate_filtered(p,moves,want.from,want.drop!=0,want.to,&want);
    for (size_t i=0;i<n;++i) if (same_move(&want,&moves[i])) {*out=moves[i];return 1;}
    return 0;
}
int position_normalize_uci(const Position *p,const char *uci,int mode,char out[8]) {
    Move m;if (!read_move(p,uci,&m)) return 0;
    move_uci(p,&m,mode,out);return 1;
}
int position_is_legal(const Position *p,const char *uci) {Move m;return find_legal(p,uci,&m);}
int position_apply_generated_uci(Position *p,const char *uci) {
    Move m;if(!read_move(p,uci,&m))return 0;
    push_move(p,&m);return 1;
}
int position_apply_uci(Position *p,const char *uci,char *err,size_t cap) {
    Move m;
    if (!read_move(p,uci,&m)) return failure(err,cap,"Invalid coordinate move");
    if (!m.null && !find_legal(p,uci,&m)) return failure(err,cap,"Illegal move for this position or variant");
    push_move(p,&m);if (err && cap) *err=0;return 1;
}
int position_from_history(Position *p,unsigned variant,const char *fen,const char *history,char *err,size_t cap) {
    Position q;if (!position_init(&q,variant,fen,err,cap)) return 0;
    const char *s=history?history:"";
    while (*s) {
        while (isspace((unsigned char)*s)) ++s;
        if (!*s) break;
        const char *start=s;while (*s && !isspace((unsigned char)*s)) ++s;
        size_t n=(size_t)(s-start);if (n>=8) return failure(err,cap,"History contains an oversized move");
        char uci[8];memcpy(uci,start,n);uci[n]=0;
        if (!position_apply_uci(&q,uci,err,cap)) return 0;
    }
    *p=q;return 1;
}
size_t position_legal_moves(const Position *p,char (*out)[8],size_t cap) {
    Move moves[POSITION_MAX_MOVES];size_t n=generate(p,moves);
    if (out) for (size_t i=0;i<n && i<cap;++i) move_uci(p,&moves[i],p->chess960,out[i]);
    return n;
}
int position_is_capture(const Position *p,const char *uci) {
    Move m;return read_move(p,uci,&m) && capture_move(p,&m);
}
int position_is_zeroing(const Position *p,const char *uci) {
    Move m;if (!read_move(p,uci,&m) || m.null || m.drop || m.rook>=0) return 0;
    return abs(p->board[m.from])==1 || capture_move(p,&m);
}
static int legal_ep(const Position *p) {
    if (p->ep_square<0) return 0;
    Move moves[POSITION_MAX_MOVES];size_t n=generate(p,moves);
    for (size_t i=0;i<n;++i) if (!moves[i].drop && abs(p->board[moves[i].from])==1 && moves[i].to==p->ep_square && capture_move(p,&moves[i])) return 1;
    return 0;
}
int position_fen_ex(const Position *p,char *out,size_t cap,int raw_ep,int shredder) {
    char buf[POSITION_FEN_MAX];size_t n=0;
    for (int rank=7;rank>=0;--rank) {
        int empty=0;
        for (int file=0;file<8;++file) {
            int sq=rank*8+file,pc=p->board[sq];
            if (!pc) {++empty;continue;}
            if (empty) {buf[n++]=(char)('0'+empty);empty=0;}
            char ch=symbols[abs(pc)];buf[n++]=pc<0?(char)tolower((unsigned char)ch):ch;
            if (p->variant==POSITION_CRAZYHOUSE && (p->promoted&bit(sq))) buf[n++]='~';
        }
        if (empty) buf[n++]=(char)('0'+empty);
        if (rank) buf[n++]='/';
    }
    if (p->variant==POSITION_CRAZYHOUSE) {
        buf[n++]='[';
        for (int c=0;c<2;++c) for (int pt=5;pt>=1;--pt) {
            size_t count=p->pockets[c][pt];
            if (n>sizeof(buf)-64 || count>sizeof(buf)-64-n) return 0;
            memset(buf+n,c?tolower((unsigned char)symbols[pt]):symbols[pt],count);
            n+=count;
        }
        buf[n++]=']';
    }
    buf[n++]=' ';buf[n++]=p->turn?'b':'w';buf[n++]=' ';
    uint64_t rights=clean_castling(p);size_t rights_start=n;
    for (int c=0;c<2;++c) {
        int k=king_square(p,c),base=c?56:0;
        for (int f=7;f>=0;--f) if (rights&bit(base+f)) {
            char ch;
            if (shredder) ch=(char)('A'+f);
            else {
                int kingside=base+f>k,other=0;
                for (int j=0;j<8;++j) if (j!=f && p->board[base+j]==(c?-4:4) && ((base+j>k)==kingside) && (kingside?j>f:j<f)) other=1;
                ch=other?(char)('A'+f):(kingside?'K':'Q');
            }
            buf[n++]=c?(char)tolower((unsigned char)ch):ch;
        }
    }
    if (n==rights_start) buf[n++]='-';
    buf[n++]=' ';
    if (p->ep_square>=0 && (raw_ep || legal_ep(p))) {square_name(p->ep_square,buf+n);n+=2;}
    else buf[n++]='-';
    int len;
    if (p->variant==POSITION_THREE_CHECK)
        len=snprintf(buf+n,sizeof(buf)-n," %d+%d %d %d",p->remaining_checks[0]>0?p->remaining_checks[0]:0,p->remaining_checks[1]>0?p->remaining_checks[1]:0,p->halfmove_clock,p->fullmove_number);
    else len=snprintf(buf+n,sizeof(buf)-n," %d %d",p->halfmove_clock,p->fullmove_number);
    if (len<0 || (size_t)len>=sizeof(buf)-n) return 0;
    n+=(size_t)len;
    if (n>=cap) return 0;
    memcpy(out,buf,n+1);return 1;
}
int position_fen(const Position *p,char *out,size_t cap) {return position_fen_ex(p,out,cap,0,0);}

int position_outcome(const Position *p) {
    if (position_variant_end(p)) {
        if (p->variant==POSITION_THREE_CHECK) {
            if (p->remaining_checks[0]<=0 && p->remaining_checks[1]<=0) return 0;
            return p->remaining_checks[0]<=0?1:2;
        }
        if (p->variant==POSITION_ANTICHESS) return !has_pieces(p,0)?1:2;
        if (p->variant==POSITION_HORDE) return !has_pieces(p,0)?(!has_pieces(p,1)?0:2):1;
        if (p->variant==POSITION_ATOMIC) return king_square(p,0)<0?2:1;
        if (p->variant==POSITION_KING_OF_THE_HILL) {
            for (int sq=27;sq<=36;++sq) if ((sq==27||sq==28||sq==35||sq==36) && abs(p->board[sq])==6) return p->board[sq]>0?1:2;
        }
        if (p->variant==POSITION_RACING_KINGS) {
            int w=king_square(p,0)>=56,b=king_square(p,1)>=56;
            return w&&b?0:(w?1:2);
        }
    }
    Move moves[POSITION_MAX_MOVES];if (generate(p,moves)) return -1;
    if (p->variant==POSITION_ANTICHESS) return p->turn+1;
    if (position_in_check(p,p->turn)) return (!p->turn)+1;
    return 0;
}
static int format_san(const Position *p,const Move *m,char *out,size_t cap,int suffix) {
    char buf[64];size_t n=0;
    if (m->null) {(void)snprintf(buf,sizeof(buf),"--");n=2;}
    else if (m->rook>=0) {const char *castle=m->rook>m->from?"O-O":"O-O-O";n=strlen(castle);memcpy(buf,castle,n);}
    else if (m->drop) {
        if (m->drop!=1) buf[n++]=symbols[m->drop];
        buf[n++]='@';square_name(m->to,buf+n);n+=2;
    } else {
        int pt=abs(p->board[m->from]),capture=capture_move(p,m);
        if (pt!=1) {
            buf[n++]=symbols[pt];Move moves[POSITION_MAX_MOVES];size_t count=generate(p,moves);
            int other=0,samefile=0,samerank=0;
            for (size_t i=0;i<count;++i) if (!moves[i].drop && moves[i].rook<0 && moves[i].from!=m->from && moves[i].to==m->to && abs(p->board[moves[i].from])==pt) {
                other=1;samefile |= (moves[i].from&7)==(m->from&7);samerank |= (moves[i].from>>3)==(m->from>>3);
            }
            if (other) {
                if (!samefile) buf[n++]=(char)('a'+(m->from&7));
                else if (!samerank) buf[n++]=(char)('1'+(m->from>>3));
                else {square_name(m->from,buf+n);n+=2;}
            }
        } else if (capture) buf[n++]=(char)('a'+(m->from&7));
        if (capture) buf[n++]='x';
        square_name(m->to,buf+n);n+=2;
        if (m->promotion) {buf[n++]='=';buf[n++]=symbols[m->promotion];}
    }
    if (suffix && !m->null) {
        Position q=*p;push_move(&q,m);int outcome=position_outcome(&q);
        if (outcome>0) buf[n++]='#';
        else if (position_in_check(&q,q.turn)) buf[n++]='+';
    }
    if (n>=cap) return 0;
    buf[n]=0;memcpy(out,buf,n+1);return 1;
}
int position_uci_to_san(const Position *p,const char *uci,char *out,size_t cap) {
    Move m;
    if (!read_move(p,uci,&m) || (!m.null && !find_legal(p,uci,&m))) return 0;
    return format_san(p,&m,out,cap,1);
}
static void normalize_san(const char *in,char *out,size_t cap) {
    size_t n=0;
    while (*in && n+1<cap) {
        char ch=*in++;
        if (ch=='+' || ch=='#' || ch=='!' || ch=='?' || ch=='=' || isspace((unsigned char)ch)) continue;
        if (ch=='0') ch='O';
        out[n++]=ch;
    }
    out[n]=0;
}
int position_san_to_uci(const Position *p,const char *san,char out[8]) {
    if (!san) return 0;
    if (!strcmp(san,"--") || !strcmp(san,"0000") || !strcmp(san,"@@@@") || !strcmp(san,"Z0")) {memcpy(out,"0000",5);return 1;}
    Move m;
    if (find_legal(p,san,&m)) {move_uci(p,&m,p->chess960,out);return 1;}
    char wanted[64];normalize_san(san,wanted,sizeof(wanted));
    if (wanted[0]=='P' && wanted[1]=='@') memmove(wanted,wanted+1,strlen(wanted));
    Move moves[POSITION_MAX_MOVES];size_t n=generate(p,moves);int found=0;
    for (size_t i=0;i<n;++i) {
        char text[64],normalized[64];
        if (!format_san(p,&moves[i],text,sizeof(text),0)) continue;
        normalize_san(text,normalized,sizeof(normalized));
        if (!strcmp(wanted,normalized)) {
            if (found) return 0;
            m=moves[i];found=1;
        }
    }
    /* Resolve CECP long algebraic moves (Ng1-f3, Nb1xc3, e8Q) against legal moves. */
    if (!found) {
        size_t len=strlen(wanted),start=0,end=len;
        int pt=1,promotion=0,fromfile=-1,fromrank=-1,bad=0;
        if (len && wanted[len-1]>='A' && wanted[len-1]<='Z' && piece_type(wanted[len-1])>=2) {
            promotion=piece_type(wanted[--end]);
        }
        if (end>=2) {
            int target=coordinate(wanted+end-2);
            if (wanted[0]>='A' && wanted[0]<='Z' && piece_type(wanted[0])) {pt=piece_type(wanted[0]);start=1;}
            for (size_t i=start;i+2<end;++i) {
                char ch=wanted[i];
                if (ch=='x' || ch=='-') continue;
                if (ch>='a' && ch<='h' && fromfile<0) fromfile=ch-'a';
                else if (ch>='1' && ch<='8' && fromrank<0) fromrank=ch-'1';
                else bad=1;
            }
            if (target>=0 && !bad) for (size_t i=0;i<n;++i) {
                Move *candidate=&moves[i];
                int to=candidate->rook>=0?(p->turn?56:0)+(candidate->rook>candidate->from?6:2):candidate->to;
                if (candidate->drop || abs(p->board[candidate->from])!=pt || candidate->promotion!=promotion ||
                    (candidate->to!=target && to!=target) ||
                    (fromfile>=0 && (candidate->from&7)!=fromfile) || (fromrank>=0 && (candidate->from>>3)!=fromrank)) continue;
                if (found) return 0;
                m=*candidate;found=1;
            }
        }
    }
    if (found) move_uci(p,&m,p->chess960,out);
    return found;
}

#include "polyglot_random.inc"
uint64_t position_polyglot_hash(const Position *p) {
    uint64_t key=0;
    for (int sq=0;sq<64;++sq) if (p->board[sq]) {
        int pc=p->board[sq],index=(abs(pc)-1)*2+(pc>0);
        key ^= position_polyglot_random[index*64+sq];
    }
    uint64_t rights=clean_castling(p);
    for (int c=0;c<2;++c) {
        int k=king_square(p,c),base=c?56:0;int ks=0,qs=0;
        for (int f=0;f<8;++f) if (rights&bit(base+f)) {ks|=base+f>k;qs|=base+f<k;}
        if (ks) key ^= position_polyglot_random[768+c*2];
        if (qs) key ^= position_polyglot_random[769+c*2];
    }
    if (p->ep_square>=0) {
        int rank=(p->ep_square>>3)+(p->turn?1:-1),file=p->ep_square&7,can=0;
        if (rank>=0 && rank<8) for (int d=-1;d<=1;d+=2) if (file+d>=0 && file+d<8 && p->board[rank*8+file+d]==(p->turn?-1:1)) can=1;
        if (can) key ^= position_polyglot_random[772+file];
    }
    if (!p->turn) key ^= position_polyglot_random[780];
    return key;
}
