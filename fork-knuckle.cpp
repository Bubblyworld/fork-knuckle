#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#include "fork-knuckle.hpp"

/***************************************************************************/
/* Move generator based on separate Slider/Leaper/Pawn tables .            */
/***************************************************************************/

#define FAKE 3
#define MAXTIM 0
#if MAXTIM
/* For detailed timings (MAXTIM=20) we need assembly routine to read TSC */
#define TIME(A) times[A] += rdtsc()-3;

int times[MAXTIM];
char *(names[MAXTIM])={"pintest", "contact", "castle", "ep-capt", "capture",
                "pawns", "leapers", "sliders", "filter", "king   ",
                "do    ", "undo   ", "recapt.", "", "", "", "", "total  "};
#else
#define TIME(A)
#endif

/* Zobris key layout */
#define Zobrist(A,B) (*(long long int *) (Zob[(A)-WHITE] + (B)))
#define XSIZE (8*1024)
union _bucket
{
  struct { // one 32-byte entry
    unsigned long long int Signature1;
    unsigned long long int Signature2;
    unsigned long long int longCount;
    unsigned long long int Dummy;
  } l;
  struct { // two packed 16-byte entries
    unsigned long long int Signature[2];
    unsigned int Count[2];
    unsigned int Extension[2];
  } s;
} *Hash, ExtraHash[XSIZE];

struct P {
    
int rseed = 87105015;
unsigned long long int accept[30], reject[30], miss[30];
char *Zob[2*NPCE];
unsigned char
        pc[NPCE*4+1], /* piece list, equivalenced with various piece info  */
        brd[0xBC+2*0xEF+1],      /* contains play board and 2 delta boards  */
        CasRights,               /* one bit per castling, clear if allowed */
        HashFlag,

        /* piece-number assignment of first row, and piece types */
        capts[8]  = {0, C_PPAWN, C_MPAWN, C_KNIGHT, C_BISHOP, C_ROOK, C_QUEEN, C_KING};

/* overlays that interleave other piece info in pos[] */
unsigned char *const kind = (pc+1);
unsigned char *const cstl = (pc+1+NPCE);
unsigned char *const pos  = (pc+1+NPCE*2);
unsigned char *const code = (pc+1+NPCE*3);

/* Piece counts hidden in the unused Pawn section, indexed by color */
unsigned char *const LastKnight  = (code-4);
unsigned char *const FirstSlider = (code-3);
unsigned char *const FirstPawn   = (code-2);

/* offset overlays to allow negative array subscripts      */
/* and avoid cache collisions of these heavily used arrays */
unsigned char *const board      = (brd+1);                /* 12 x 16 board: dbl guard band */
unsigned char *const capt_code  = (brd+1+0xBC+0x77);      /* piece type that can reach this*/
char          *const delta_vec  = ((char *) brd+1+0xBC+0xEF+0x77); /* step to bridge certain vector */

char noUnder = 0;

char Keys[1040];
int path[100];
int stack[1024], msp = 0, ep1, ep2, Kmoves, Promo, Split, epSqr, HashSize, HashSection;
unsigned long long int HashKey=8729767686LL, HighKey=1234567890LL, count, epcnt, xcnt, ckcnt, cascnt, promcnt, nodecount; /* stats */
FILE *f;
clock_t ttt[30];

    void push_move(int from, int to) { stack[msp++] = from + to; }

    /**
     * Make an empty board surrounded by guard band of uncapturable pieces.
     */
    void board_init(unsigned char *b) {
        for(int i= -1; i<0xBC; i++) b[i] = (i-0x22)&0x88 ? GUARD : DUMMY;
    }

    /**
     * Fill 0x88-style attack tables, with capture codes and vectors.
     */
    void delta_init() {
        /* contact captures (cannot be blocked) */
        capt_code[FL] = capt_code[FR] = C_FDIAG;
        capt_code[BL] = capt_code[BR] = C_BDIAG;
        capt_code[LT] = capt_code[RT] = C_SIDE;
        capt_code[FW] = C_FORW;
        capt_code[BW] = C_BACKW;

        for(int i=0; i<8; i++) {
            /* in all directions */
            capt_code[KNIGHT_ROSE[i]] = C_KNIGHT;
            delta_vec[KNIGHT_ROSE[i]] = KNIGHT_ROSE[i];
            /* distant captures (can be blocked) */
            int k = QUEEN_DIR[i];
            int m = i<4 ? C_ORTH : C_DIAG;
            int y = 0;  
            for(int j=0; j<7; j++) {
                /* scan along ray */
                delta_vec[y+=k] = k;
                /* note that first is contact */
                if(j) capt_code[y] = m;
            }
        }

    }

    /** 
     * initialize piece list to initial setup 
     */
    void piece_init(void) {
        /* piece-number assignment of first row, and piece types */
        static const unsigned char array[8]    = {   12,      1,     14,    11,    0,     15,      2,   13 }; // ??? What are these - offsets in kind, pos???
        static const unsigned char BACK_ROW[8] = { ROOK, KNIGHT, BISHOP, QUEEN, KING, BISHOP, KNIGHT, ROOK };

        /* initalize piece type and position, in initial setup */
        for(int i=0; i<8; i++) {
            kind[array[i]+WHITE] = BACK_ROW[i];
            kind[array[i]]       = BACK_ROW[i];
            kind[i+PAWNS]        = 1;
            kind[i+PAWNS+WHITE]  = 2;

            pos[array[i]]        = i+0x22;
            pos[array[i]+WHITE]  = i+0x92;
            pos[i+PAWNS]         = i+0x32;
            pos[i+PAWNS+WHITE]   = i+0x82;
        }

        /* set capture codes for each piece */
        for(int i=0; i<NPCE; i++) { code[i] = capts[kind[i]]; }

        /* set castling spoilers (King and both original Rooks) */
        cstl[0]        = WHITE;
        cstl[12]       = WHITE>>2;
        cstl[13]       = WHITE>>4;
        cstl[0 +WHITE] = BLACK;
        cstl[12+WHITE] = BLACK>>2;
        cstl[13+WHITE] = BLACK>>4;

        /* piece counts (can change when we compactify lists, or promote) */
        LastKnight[WHITE]  =  2;
        FirstSlider[WHITE] = 11;
        FirstPawn[WHITE]   = 16;
        LastKnight[BLACK]  =  2+WHITE;
        FirstSlider[BLACK] = 11+WHITE;
        FirstPawn[BLACK]   = 16+WHITE;

        Zob[DUMMY-WHITE] = Keys-0x22;
    }

    /**
     * Put pieces on the board according to the piece list.
     */
    void setup(void) {
        for(int i=0; i<WHITE-8; i++) {
            if(pos[i]      ) board[pos[i]]       = WHITE + i;
            if(pos[i+WHITE]) board[pos[i+WHITE]] = BLACK + i;
        }
    }


    /**
     * Print board of n x n, in hex (bin=1) or ascii.
     */
    void pboard(unsigned char *b, int n, int bin) {
        static const char* asc = ".+pnbrqkxxxxxxxx.P*NBRQKXXXXXXXX";

        for(int i=n-1; i>=0; i--) {
            for(int j=0; j<n; j++) {
                if(bin) { printf(" %2x", b[16*i+j]&0xFF); }
                else    { printf(" %c", (b[16*i+j]&0xFF)==GUARD ? '-' : asc[kind[(b[16*i+j]&0x7F)-WHITE]+((b[16*i+j]&WHITE)>>1)]); }
            }
            printf("\n");
        }
        printf("\n");
    }

    int checker(int col) {
        for(int i=0; i<8; i++) {
            int v = KING_ROSE[i];
            int x = pos[col-WHITE] + v;
            int piece = board[x];
            if((piece & COLOR) == (col^COLOR)) {
                if(code[piece-WHITE] & capt_code[-v]) return x;
            }
            v = KNIGHT_ROSE[i];
            x = pos[col-WHITE] + v;
            piece = board[x];
            if((piece & COLOR) == (col^COLOR)) {
                if(code[piece-WHITE] & capt_code[-v]) return x;
            }
        }
        return 0;
    }

#ifndef FEN
    int ReadFEN(const char *FEN) {
        int col;
        
        /* remove all pieces */
        for(int i=0; i<NPCE; i++) pos[i] = cstl[i] = 0;
        FirstSlider[WHITE] = 0x10;
        FirstSlider[BLACK] = 0x30;
        LastKnight[WHITE]  = 0x00;
        LastKnight[BLACK]  = 0x20;
        FirstPawn[WHITE]   = 0x18;
        FirstPawn[BLACK]   = 0x38;
        CasRights = 0;
        
        const char *p = FEN;
        char c;
        
        for(int row=7; row>=0; row--) {
            /* read one row of the FEN */
            int file = 0;
            do {
                c = *p++;

                if(c>='1' && c<='8') { file += c - '0'; }
                else {
                    col = WHITE;
                    if(c >= 'a') { c += 'A'-'a'; col = BLACK; }
                    int Piece = BISHOP, cc = 0, nr;
                    switch(c) {
                    case 'K':
                        if(pos[col-WHITE] > 0) return -1;   /* two kings illegal */
                        Piece = KING;
                        nr = col-WHITE;
                        if(0x20*row == 7*(col-WHITE) && file == 4) cc = (col|col>>2|col>>4);
                        
                        break;
                    case 'R': Piece--;
                        if(0x20*row == 7*(col-WHITE)) {
                            /* only Rooks on a1, h1, a8, h8 get castling spoiler */
                            if(file == 0) cc = col>>2;
                            if(file == 7) cc = col>>4;
                        }
                    case 'Q': Piece += 2;
                    case 'B': 
                        if(--FirstSlider[col] <= LastKnight[col]) return(-2);
                        nr = FirstSlider[col];
                        break;
                    case 'P': 
                        if(--FirstPawn[col] < col-WHITE+FW) return(-4);
                        nr = FirstPawn[col];
                        Piece = col>>5;
                        break;
                    case 'N': 
                        if(FirstSlider[col] <= ++LastKnight[col]) return(-3);
                        nr = LastKnight[col];
                        Piece = KNIGHT;
                        break;
                    default:
                        return -15;
                    }
                    pos[nr] = ((file +  16*row) & 0x77) + 0x22;
                    kind[nr] = Piece;
                    code[nr] = capts[Piece];
                    Zob[nr]  = Keys + 128*Piece + (col&BLACK)/8 - 0x22;
                    cstl[nr] = cc;
                    CasRights |= cc;       /* remember K & R on original location */
                    file++;
                }
            } while(file < 8);
            if(file >  8) return -11;
            if(file == 8) {
                c = *p++;
                if(row > 0 && c != '/') return(-10); /* bad format */
                if(row==0  && c != ' ') return -11;
            }
        }
        if(pos[0] == 0 || pos[WHITE] == 0) return -5; /* missing king */

        /* now do castle rights and side to move */
        cstl[DUMMY-WHITE]=0;
        int cc = 0;
        while((c = *p++)) {
            if(c>='0' && c<='9') continue; /* ignore move counts */
            if(c>='a' && c<='h') {
                /* might be e.p. square */
                if(*p == '3' || *p == '6') {
                    epSqr = 0x22 + (*p - '1')*16 + (c - 'a'); 
                    p++;
                    continue;
                } else if(c != 'b') continue;
            }
            switch(c) {
            case 'K': cc |= 0x22; break;
            case 'Q': cc |= 0x28; break;
            case 'k': cc |= 0x44; break;
            case 'q': cc |= 0x50; break;
            case 'w': col = WHITE; break;
            case 'b': col = BLACK; break;
            case ' ':
            case '-': break;
            default: return -12;
            }
        }
        CasRights = (cc & CasRights) ^ 0x7E;
        return col;
    }
#endif


    void move_gen(int color, int lastply, int d) {
        int i, j, k, p, v, x, z, y, /*r,*/ m, h;
        int /*savsp,*/ mask, /*new,*/ forward, rank, prank, /*xcolor,*/ ep_flag;
        int in_check=0, checker= -1, check_dir = 20;
        int pstack[12], ppos[12], psp=0, first_move=msp;
        
        /* Some general preparation */
        k = pos[color-WHITE];           /* position of my King */
        forward = 48- color;            /* forward step */
        rank = 0x58 - (forward>>1);     /* 4th/5th rank */
        prank = 0xD0 - 5*(color>>1);    /* 2nd/7th rank */
        ep_flag = lastply>>24&0xFF;
        ep1 = ep2 = msp; Promo = 0;
        
        /* Pintest, starting from possible pinners in enemy slider list   */
        /* if aiming at King & 1 piece of us in between, park this piece  */
        /* on pin stack for rest of move generation, after generating its */
        /* moves along the pin line.                                      */
        for(p=FirstSlider[COLOR-color]; p<COLOR-WHITE+FW-color; p++)
        {   /* run through enemy slider list */
            j = pos[p]; /* enemy slider */
            if(j==0) continue;  /* currently captured */
            if(capt_code[j-k]&code[p]&C_DISTANT)
            {   /* slider aimed at our king */
                v = delta_vec[j-k];
                x = k;     /* trace ray from our King */
                while((m=board[x+=v]) == DUMMY);
                if(x==j)
                {   /* distant check detected         */
                    in_check += 2;
                    checker = j;
                    check_dir = v;
                } else
                if(m&color)
                {   /* first on ray from King is ours */
                    y = x;
                    while(board[y+=v] == DUMMY);
                    if(y==j)
                    {   /* our piece at x is pinned!  */
                        /* remove from piece list     */
                        /* and put on pin stack       */
                        m -= WHITE;
                        ppos[psp] = pos[m];
                        pos[m] = 0;
                        pstack[psp++] = m;
                        z = x<<8;

                        if(kind[m]<3)
                        {   /* flag promotions */
                            if(!((prank^x)&0xF0)) z |= 0xA1000000;
                            y = x + forward; 
                            if(!(v&7)) /* Pawn along file */
                            {   /* generate non-captures  */
                                if(!(board[y]&COLOR))
                                {   push_move(z,y);
                                    y += forward;Promo++;
                                    if(!((board[y]&COLOR) | ((rank^y)&0xF0)))
                                        push_move(z,y|y<<24);
                                }
                            } else
                            {   /* diagonal pin       */
                                /* try capture pinner */
                                if(y+RT==j) { Promo++; push_move(z,y+RT); }
                                if(y+LT==j) { Promo++; push_move(z,y+LT); }
                            }
                        } else
                        if(code[m]&capt_code[j-k]&C_DISTANT)
                        {   /* slider moves along pin ray */
                            y = x;
                            do{ /* moves upto capt. pinner*/
                                y += v;
                                push_move(z,y);
                            } while(y != j);
                            y = x;
                            while((y-=v) != k)
                            {   /* moves towards King     */
                                push_move(z,y);
                            }
                        }
                    }
                }
            }
        }

        /* all pinned pieces are now removed from lists */
        /* all their remaining legal moves are generated*/
        /* all distant checks are detected              */

    /* determine if opponent's move put us in contact check */
        y = lastply&0xFF;
        if(capt_code[k-y] & code[board[y]-WHITE] & C_CONTACT)
        {   checker = y; in_check++;
            check_dir = delta_vec[checker-k];
        }

    /* determine how to proceed based on check situation    */
        if(in_check)
        {   /* purge moves with pinned pieces if in check   */
            msp = first_move;

            if(in_check > 2) goto King_Moves; /* double check */
            if(checker == ep_flag) { ep1 = msp; goto ep_Captures; }
            goto Regular_Moves;
        }
    /* generate castlings */
        ep1 = msp;
        if(!(color&CasRights))
        {
            if(!((board[k+RT]^DUMMY)|(board[k+RT+RT]^DUMMY)|
                 (CasRights&color>>4)))
                push_move(k<<8,k+2+0xB0000000+0x3000000);
            if(!((board[k+LT]^DUMMY)|(board[k+LT+LT]^DUMMY)|(board[k+LT+LT+LT]^DUMMY)|
                 (CasRights&color>>2)))
                push_move(k<<8,k-2+0xB0000000-0x4000000);
        }

    /* generate e.p. captures (at most two)                  */
    /* branches are almost always take, e.p. capture is rare */
    ep_Captures:
        mask = color | PAWNS;

        x = ep_flag+1;
        if((board[x]&mask)==mask) push_move(x<<8,(ep_flag^0x10)|0xA0000000);

        x = ep_flag-1;
        if((board[x]&mask)==mask) push_move(x<<8,(ep_flag^0x10)|0xA0000000);
        ep2 = msp;

    /* On contact check only King retreat or capture helps   */
    /* Use in that case specialized recapture generator      */
    Regular_Moves:
      if(in_check & 1)
      {
          //xcolor = color^COLOR;
        /* check for pawns, through 2 squares on board */
        m = color | PAWNS;
        z = x = checker; y = x - forward;
        
        if(!((prank^y)&0xF0)) Promo++,z |= 0xA1000000;
        if((board[y+RT]&m)==m && pos[board[y+RT]-WHITE]) push_move((y+RT)<<8,z);
        if((board[y+LT]&m)==m && pos[board[y+LT]-WHITE]) push_move((y+LT)<<8,z);

        for(p=LastKnight[color]; p>color-WHITE; p--)
        {
            k = pos[p];
            if(k==0) continue;
            m = code[p];
            i = capt_code[k-x];
            if(i&m) push_move(k<<8,x);
        }

        for(p=color-WHITE+FL; p>=FirstSlider[color]; p--)
        {
            k = pos[p];
            if(k==0) continue;
            m = code[p];
            i = capt_code[k-x];
            if(i&m)
            {
                v = delta_vec[k-x];
                y = x;
                while(board[y+=v]==DUMMY);
                if(y==k) push_move(k<<8,x);
            }
        }

      } else
    /* Basic move generator for generating all moves */
      {
        /* First do pawns, from pawn list hanging from list head    */

        mask = color|0x80;  /* matches own color, empty square, or guard  */

        for(p=FirstPawn[color]; p<color-WHITE+PAWNS+8; p++)
        {
            x = pos[p]; z = x<<8;
            if(x==0) continue;

            /* flag promotions */
            if(!((prank^x)&0xF0)) Promo++,z |= 0xA1000000;

            /* capture moves */
            y = x + forward;
            if(!(board[y+LT]&mask)) push_move(z,y+LT);
            if(!(board[y+RT]&mask)) push_move(z,y+RT);
            
            /* non-capture moves */
            if(!(board[y]&COLOR))
            {   push_move(z,y);
                y += forward;
                if(!((board[y]&COLOR) | ((rank^y)&0xF0)))
                    push_move(z,y|y<<24);        /* e.p. flag */
            }
        }

        /* Next do Knights */

        for(p=LastKnight[color]; p>color-WHITE; p--)
        {
            x = pos[p]; z = x<<8;
            if(x==0) continue;

            /* always 8 direction, unroll loop to avoid branches */
            if(!(board[x+14]&color)) push_move(z,x+14);
            if(!(board[x+31]&color)) push_move(z,x+31);
            if(!(board[x+33]&color)) push_move(z,x+33);
            if(!(board[x+18]&color)) push_move(z,x+18);
            if(!(board[x-14]&color)) push_move(z,x-14);
            if(!(board[x-31]&color)) push_move(z,x-31);
            if(!(board[x-33]&color)) push_move(z,x-33);
            if(!(board[x-18]&color)) push_move(z,x-18);
        }

        /* now do sliding pieces */
        /* for each ray, do ray scan, and goto next ray when blocked */

        for(p=color-WHITE+FL; p>=FirstSlider[color]; p--)
        {   
          x = pos[p]; z = x<<8;
          if(x==0) continue;

          if((kind[p]-3)&2)
          { /* scan 4 rook rays for R and Q */
            y = x;
            do{ if(!((h=board[y+=RT])&color)) push_move(z,y); } while(!(h&COLOR));
            y = x;
            do{ if(!((h=board[y+=LT])&color)) push_move(z,y); } while(!(h&COLOR));
            y = x;
            do{ if(!((h=board[y+=FW])&color)) push_move(z,y); } while(!(h&COLOR));
            y = x;
            do{ if(!((h=board[y+=BW])&color)) push_move(z,y); } while(!(h&COLOR));
          }
          if((kind[p]-3)&1)
          { /* scan 4 bishop rays for B and Q */
            y = x;
            do{ if(!((h=board[y+=FL])&color)) push_move(z,y); } while(!(h&COLOR));
            y = x;
            do{ if(!((h=board[y+=BR])&color)) push_move(z,y); } while(!(h&COLOR));
            y = x;
            do{ if(!((h=board[y+=FR])&color)) push_move(z,y); } while(!(h&COLOR));
            y = x;
            do{ if(!((h=board[y+=BL])&color)) push_move(z,y); } while(!(h&COLOR));
          }
        }

    /* remove moves that don't solve distant check by          */
    /* capturing checker or interposing on check ray           */
        if(in_check)
        for(i=first_move; i<msp; i++)  /* go through all moves */
        {
            if(delta_vec[(j=stack[i]&0xFF)-k] != check_dir)
                stack[i--] = stack[--msp];
            else
            {   /* on check ray, could block or capture checker*/
                x = k;
                do{
                    x += check_dir;
                    if(x==j) break;
                } while(x != checker);
               if(x!=j) {  stack[i--] = stack[--msp]; }
            }
        }
      }

    /* Generate moves with the always present King */
    King_Moves:
        x = pos[color-WHITE]; z = x<<8;
        Kmoves = msp;

        /* always 8 direction, unroll loop to avoid branches */
        if(!(board[x+RT]&color)) push_move(z,x+RT);
        if(!(board[x+FR]&color)) push_move(z,x+FR);
        if(!(board[x+FW]&color)) push_move(z,x+FW);
        if(!(board[x+FL]&color)) push_move(z,x+FL);
        if(!(board[x+LT]&color)) push_move(z,x+LT);
        if(!(board[x+BL]&color)) push_move(z,x+BL);
        if(!(board[x+BW]&color)) push_move(z,x+BW);
        if(!(board[x+BR]&color)) push_move(z,x+BR);

    /* Put pieces that were parked onto pin stack back in lists */
       while(psp>0)
        {   /* pop pinned piece and link in old place it remembers*/
            m = pstack[--psp];
            pos[m] = ppos[psp];
        }

}

#if 1
int move_count(int color, int lastply, int d)
{
    int i, j, k, p, v, x, z, y, /*r,*/ m, h;
    int /*savsp,*/ mask, /*new,*/ forward, rank, prank, /*xcolor,*/ ep_flag;
    int in_check=0, checker= -1, check_dir = 20;
    int pstack[12], ppos[12], psp=0, first_move=msp, myCount=0;

    /* Some general preparation */
        k = pos[color-WHITE];           /* position of my King */
        forward = 48- color;            /* forward step */
        rank = 0x58 - (forward>>1);     /* 4th/5th rank */
        prank = 0xD0 - 5*(color>>1);    /* 2nd/7th rank */
        ep_flag = lastply>>24&0xFF;

    /* Pintest, starting from possible pinners in enemy slider list   */
    /* if aiming at King & 1 piece of us in between, park this piece  */
    /* on pin stack for rest of move generation, after generating its */
    /* moves along the pin line.                                      */
        for(p=FirstSlider[COLOR-color]; p<COLOR-WHITE+FW-color; p++)
        {   /* run through enemy slider list */
            j = pos[p]; /* enemy slider */
            if(j==0) continue;  /* currently captured */
            if(capt_code[j-k]&code[p]&C_DISTANT)
            {   /* slider aimed at our king */
                v = delta_vec[j-k];
                x = k;     /* trace ray from our King */
                while((m=board[x+=v]) == DUMMY);
                if(x==j)
                {   /* distant check detected         */
                    in_check += 2;
                    checker = j;
                    check_dir = v;
                } else
                if(m&color)
                {   /* first on ray from King is ours */
                    y = x;
                    while(board[y+=v] == DUMMY);
                    if(y==j)
                    {   /* our piece at x is pinned!  */
                        /* remove from piece list     */
                        /* and put on pin stack       */
                        m -= WHITE;
                        ppos[psp] = pos[m];
                        pos[m] = 0;
                        pstack[psp++] = m;
                        z = x<<8;

                        if(kind[m]<3)
                        {   /* flag promotions */
                           if(!((prank^x)&0xF0)) {
			      z |= 0xA1000000;
                              y = x + forward; 
                              if(!(v&7)) /* Pawn along file */
                              { /* generate non-captures  */
                                if(!(board[y]&COLOR))
                                {   push_move(z,y);
                                    y += forward;
                                    if(!((board[y]&COLOR) | ((rank^y)&0xF0)))
                                        push_move(z,y);
                                }
                              } else
                              { /* diagonal pin       */
                                /* try capture pinner */
                                if(y+RT==j) push_move(z,y+RT);
                                if(y+LT==j) push_move(z,y+LT);
                              }
                           } else {
                              y = x + forward; 
                              if(!(v&7)) /* Pawn along file */
                              { /* generate non-captures  */
                                if(!(board[y]&COLOR))
                                {   myCount++;
                                    y += forward;
                                    myCount += !((board[y]&COLOR) | ((rank^y)&0xF0));
                                }
                              } else
                              { /* diagonal pin       */
                                /* try capture pinner */
                                myCount += (y+RT==j);
                                myCount += (y+LT==j);
                              }
                           }
                        } else
                        if(code[m]&capt_code[j-k]&C_DISTANT)
                        {   /* slider moves along pin ray */
                            y = x;
                            do{ /* moves upto capt. pinner*/
                                y += v;
                                myCount++;
                            } while(y != j);
                            y = x;
                            while((y-=v) != k)
                            {   /* moves towards King     */
                                myCount++;
                            }
                        }
                    }
                }
            }
        }

        /* all pinned pieces are now removed from lists */
        /* all their remaining legal moves are generated*/
        /* all distant checks are detected              */

    /* determine if opponent's move put us in contact check */
        y = lastply&0xFF;
        if(capt_code[k-y] & code[board[y]-WHITE] & C_CONTACT)
        {   checker = y; in_check++;
            check_dir = delta_vec[checker-k];
        }

    /* determine how to proceed based on check situation    */
        if(in_check)
        {   /* purge moves with pinned pieces if in check   */
            msp = first_move; myCount = 0;

            if(in_check > 2) goto King_Moves2; /* double check */
            if(checker == ep_flag) goto ep_Captures_in_Check;
            goto Regular_Moves_in_Check;
        }
    /* generate castlings */
        if(!(color&CasRights))
        {
            if(!((board[k+RT]^DUMMY)|(board[k+RT+RT]^DUMMY)|
                 (CasRights&color>>4)))
                push_move(k<<8,k+2+0xB0000000+0x3000000);
            if(!((board[k+LT]^DUMMY)|(board[k+LT+LT]^DUMMY)|(board[k+LT+LT+LT]^DUMMY)|
                 (CasRights&color>>2)))
                push_move(k<<8,k-2+0xB0000000-0x4000000);
        }

    /* generate e.p. captures (at most two)                  */
    /* branches are almost always take, e.p. capture is rare */
    //ep_Captures2:
        mask = color | PAWNS;

        x = ep_flag+RT;
        if((board[x]&mask)==mask) push_move(x<<8,(ep_flag^0x10)|0xA0000000);

        x = ep_flag+LT;
        if((board[x]&mask)==mask) push_move(x<<8,(ep_flag^0x10)|0xA0000000);
        ep2 = msp;

    /* On contact check only King retreat or capture helps   */
    /* Use in that case specialized recapture generator      */
    //Regular_Moves2:
    /* Basic move generator for generating all moves */
      {
        /* First do pawns, from pawn list hanging from list head    */

        mask = color|0x80;  /* matches own color, empty square, or guard  */

        for(p=FirstPawn[color]; p<color-WHITE+PAWNS+8; p++)
        {
          x = pos[p]; z = x<<8;
          if(x==0) continue;

            /* flag promotions */
          if(!((prank^x)&0xF0)) {
            z |= 0xA1000000;

            /* capture moves */
            y = x + forward;
            if(!(board[y+LT]&mask)) push_move(z,y+LT);
            if(!(board[y+RT]&mask)) push_move(z,y+RT);
            
            /* non-capture moves (no double-push if promotion!) */
            if(!(board[y]&COLOR)) push_move(z,y);
          } else {
            /* capture moves */
            y = x + forward;
            myCount += !(board[y+LT]&mask);
            myCount += !(board[y+RT]&mask);
            
            /* non-capture moves */
            if(!(board[y]&COLOR))
            {   myCount++;
                y += forward;
                myCount += !((board[y]&COLOR) | ((rank^y)&0xF0));
            }
          }
        }

        /* Next do Knights */

        for(p=LastKnight[color]; p>color-WHITE; p--)
        {
            x = pos[p]; z = x<<8;
            if(x==0) continue;

            /* always 8 direction, unroll loop to avoid branches */
            myCount += !(board[x+14]&color);
            myCount += !(board[x+31]&color);
            myCount += !(board[x+33]&color);
            myCount += !(board[x+18]&color);
            myCount += !(board[x-14]&color);
            myCount += !(board[x-31]&color);
            myCount += !(board[x-33]&color);
            myCount += !(board[x-18]&color);
        }

        /* now do sliding pieces */
        /* for each ray, do ray scan, and goto next ray when blocked */

        for(p=color-WHITE+FL; p>=FirstSlider[color]; p--)
        {   
          x = pos[p]; z = x<<8;
          if(x==0) continue;

          if((kind[p]-3)&2)
          { /* scan 4 rook rays for R and Q */
            register int h, y;
            y = x; while((h=board[y+=RT]) == DUMMY) myCount++; myCount += !(board[y]&color);
            y = x; while((h=board[y+=LT]) == DUMMY) myCount++; myCount += !(board[y]&color);
            y = x; while((h=board[y+=FW]) == DUMMY) myCount++; myCount += !(board[y]&color);
            y = x; while((h=board[y+=BW]) == DUMMY) myCount++; myCount += !(board[y]&color);
          }
          if((kind[p]-3)&1)
          { /* scan 4 bishop rays for B and Q */
            register int h, y;
            y = x; while((h=board[y+=FL]) == DUMMY) myCount++; myCount += !(board[y]&color);
            y = x; while((h=board[y+=BR]) == DUMMY) myCount++; myCount += !(board[y]&color);
            y = x; while((h=board[y+=FR]) == DUMMY) myCount++; myCount += !(board[y]&color);
            y = x; while((h=board[y+=BL]) == DUMMY) myCount++; myCount += !(board[y]&color);
          }
        }
      }
      goto King_Moves2;

    /* generate e.p. captures (at most two)                  */
    /* branches are almost always take, e.p. capture is rare */
    ep_Captures_in_Check:
        mask = color | PAWNS;

        x = ep_flag+RT;
        if((board[x]&mask)==mask) push_move(x<<8,(ep_flag^0x10)|0xA0000000);

        x = ep_flag+LT;
        if((board[x]&mask)==mask) push_move(x<<8,(ep_flag^0x10)|0xA0000000);

    /* On contact check only King retreat or capture helps   */
    /* Use in that case specialized recapture generator      */
    Regular_Moves_in_Check:
      if(in_check & 1)
      {
          //xcolor = color^COLOR;
        /* check for pawns, through 2 squares on board */
        m = color | PAWNS;
        z = x = checker; y = x - forward;
        
        if(!((prank^y)&0xF0)) {
          z |= 0xA1000000;
          if((board[y+RT]&m)==m && pos[board[y+RT]-WHITE]) push_move((y+RT)<<8,z);
          if((board[y+LT]&m)==m && pos[board[y+LT]-WHITE]) push_move((y+LT)<<8,z);
        } else {
          myCount += (board[y+RT]&m)==m && pos[board[y+RT]-WHITE];
          myCount += (board[y+LT]&m)==m && pos[board[y+LT]-WHITE];
        }

        for(p=LastKnight[color]; p>color-WHITE; p--)
        {
            k = pos[p];
            if(k==0) continue;
            m = code[p];
            i = capt_code[k-x];
            myCount += (i&m) != 0;
        }

        for(p=color-WHITE+FL; p>=FirstSlider[color]; p--)
        {
            k = pos[p];
            if(k==0) continue;
            m = code[p];
            i = capt_code[k-x];
            if(i&m)
            {
                v = delta_vec[k-x];
                y = x;
                while(board[y+=v]==DUMMY);
                myCount += (y==k);
            }
        }

      } else
    /* Basic move generator for generating all moves */
      {
        /* First do pawns, from pawn list hanging from list head    */

        mask = color|0x80;  /* matches own color, empty square, or guard  */

        for(p=FirstPawn[color]; p<color-WHITE+PAWNS+8; p++)
        {
            x = pos[p]; z = x<<8;
            if(x==0) continue;

            /* flag promotions */
            if(!((prank^x)&0xF0)) Promo++,z |= 0xA1000000;

            /* capture moves */
            y = x + forward;
            if(!(board[y+LT]&mask)) push_move(z,y+LT);
            if(!(board[y+RT]&mask)) push_move(z,y+RT);
            
            /* non-capture moves */
            if(!(board[y]&COLOR))
            {   push_move(z,y);
                y += forward;
                if(!((board[y]&COLOR) | ((rank^y)&0xF0)))
                    push_move(z,y);        /* forget e.p. flag */
            }
        }

        /* Next do Knights */

        for(p=LastKnight[color]; p>color-WHITE; p--)
        {
            x = pos[p]; z = x<<8;
            if(x==0) continue;

            /* always 8 direction, unroll loop to avoid branches */
            if(!(board[x+14]&color)) push_move(z,x+14);
            if(!(board[x+31]&color)) push_move(z,x+31);
            if(!(board[x+33]&color)) push_move(z,x+33);
            if(!(board[x+18]&color)) push_move(z,x+18);
            if(!(board[x-14]&color)) push_move(z,x-14);
            if(!(board[x-31]&color)) push_move(z,x-31);
            if(!(board[x-33]&color)) push_move(z,x-33);
            if(!(board[x-18]&color)) push_move(z,x-18);
        }

        /* now do sliding pieces */
        /* for each ray, do ray scan, and goto next ray when blocked */

        for(p=color-WHITE+FL; p>=FirstSlider[color]; p--)
        {   
          x = pos[p]; z = x<<8;
          if(x==0) continue;

          if((kind[p]-3)&2)
          { /* scan 4 rook rays for R and Q */
            y = x;
            do{ if(!((h=board[y+=RT])&color)) push_move(z,y); } while(!(h&COLOR));
            y = x;
            do{ if(!((h=board[y+=LT])&color)) push_move(z,y); } while(!(h&COLOR));
            y = x;
            do{ if(!((h=board[y+=FW])&color)) push_move(z,y); } while(!(h&COLOR));
            y = x;
            do{ if(!((h=board[y+=BW])&color)) push_move(z,y); } while(!(h&COLOR));
          }
          if((kind[p]-3)&1)
          { /* scan 4 bishop rays for B and Q */
            y = x;
            do{ if(!((h=board[y+=FL])&color)) push_move(z,y); } while(!(h&COLOR));
            y = x;
            do{ if(!((h=board[y+=BR])&color)) push_move(z,y); } while(!(h&COLOR));
            y = x;
            do{ if(!((h=board[y+=FR])&color)) push_move(z,y); } while(!(h&COLOR));
            y = x;
            do{ if(!((h=board[y+=BL])&color)) push_move(z,y); } while(!(h&COLOR));
          }
        }

    /* remove moves that don't solve distant check by          */
    /* capturing checker or interposing on check ray           */
        if(in_check)
        for(i=first_move; i<msp; i++)  /* go through all moves */
        {
            if(delta_vec[(j=stack[i]&0xFF)-k] != check_dir)
                stack[i--] = stack[--msp];
            else
            {   /* on check ray, could block or capture checker*/
                x = k;
                do{
                    x += check_dir;
                    if(x==j) break;
                } while(x != checker);
               if(x!=j) {  stack[i--] = stack[--msp]; }
            }
        }
      }

    /* Generate moves with the always present King */
    King_Moves2:
        x = pos[color-WHITE]; z = x<<8;
        Kmoves = msp;

        /* always 8 direction, unroll loop to avoid branches */
        if(!(board[x+RT]&color)) push_move(z,x+RT);
        if(!(board[x+FR]&color)) push_move(z,x+FR);
        if(!(board[x+FW]&color)) push_move(z,x+FW);
        if(!(board[x+FL]&color)) push_move(z,x+FL);
        if(!(board[x+LT]&color)) push_move(z,x+LT);
        if(!(board[x+BL]&color)) push_move(z,x+BL);
        if(!(board[x+BW]&color)) push_move(z,x+BW);
        if(!(board[x+BR]&color)) push_move(z,x+BR);

        /* Put pieces that were parked onto pin stack back in lists */
        while(psp>0)
        {   /* pop pinned piece and link in old place it remembers*/
            m = pstack[--psp];
            pos[m] = ppos[psp];
        }
      return myCount;
}
#endif

int capturable(int color, int x)
{   /* do full check for captures on square x by all opponen't pieces */
    int i, /*j,*/ k, v, y, m, p;

    /* check for pawns, through 2 squares on board */
    v = color - 48; m = color | PAWNS;
    if((board[x+v+RT]&m)==m) return 1; 
    if((board[x+v+LT]&m)==m) return 2;

    for(p=LastKnight[color]; p>=color-WHITE; p--)
    {
        k = pos[p];
        if(k==0) continue;
        m = code[p];
        i = capt_code[k-x];
        if(i&m) return p+256;
    }

    for(p=color-WHITE+FL; p>=FirstSlider[color]; p--)
    {
        k = pos[p];
        if(k==0) continue;
        m = code[p];
        i = capt_code[k-x];
        if(i&m)
        {
            v = delta_vec[k-x];
            y = x;
            while(board[y+=v]==DUMMY);
            if(y==k) return p+512;
        }
    }

    return 0;
}

void leaf_perft(int color, int lastply, int depth, int d)
{   /* recursive perft, with in-lined make/unmake */
    int i, j, /*k,*/ /*m,*/ /*x, y, v,*/ h, /*p,*/ oldpiece/*, store, myCount*/;
    int first_move, piece, victim, /*pred, succ,*/ from, to, capt, mode;//,
        /*pcnr, vicnr, in_check=0, checker= -1, check_dir = 20, legal*/;
    int SavRights = CasRights;//, lep1, lep2, lkm, flag;
    //unsigned long long int ocnt=count, SavCnt;
    //union _bucket *Bucket;

    TIME(17)
    first_move = msp; /* new area on move stack */
    //legal = 0;
    count += move_count(color, lastply, d); /* bulk count, but generate moves that need legality checking */

    for(i = first_move; i<msp; i++)  /* go through all moves */
    {
      /* fetch move from move stack */
        from = (stack[i]>>8)&0xFF;
        to = capt = stack[i]&0xFF;
        mode = (unsigned int)stack[i]>>24;
        piece  = board[from];

        if(mode)
        {   /* e.p. or castling, usually skipped  */
            /* e.p.:shift capture square one rank */
            if(mode == 0xA0) capt ^= 0x10; else
            if(mode == 0xA1)
            {   /* Promotion. Shuffle piece list :( */
                oldpiece = piece; pos[piece-WHITE]=0;
                /* set up for new Queen first */
                piece = --FirstSlider[color]+WHITE;
                kind[piece-WHITE] = QUEEN;
                code[piece-WHITE] = C_QUEEN;
                Zob[piece-WHITE]  = Keys + 128*QUEEN + (color&BLACK)/8 - 0x22;
                pos[piece-WHITE]  = from;
            }else
            {   /* castling, determine Rook move  */
                j = mode - 0xB0 + from;
                h = (from+to) >> 1;
                /* abort if Rook in check         */
                if(capturable(color^COLOR, h)) continue;
                /* move Rook                      */
                board[h] = board[j];
                board[j] = DUMMY;
                pos[board[h]-WHITE] = h;
            }
        }

        victim = board[capt];
        CasRights |= cstl[piece-WHITE] | cstl[victim-WHITE];

minor:
      /* perform move, in piece list and on board */
        /* update board position */
        board[capt] = board[from] = DUMMY;
        board[to] = piece;

        /* update piece location in piece list    */
        pos[piece-WHITE] = to;


        /* remove captured piece from piece list  */
        pos[victim-WHITE] = 0;

        count += !capturable(color^COLOR, pos[color-WHITE]);
      /* retract move */

        /* restore piece list */
        pos[piece-WHITE] = from;
        pos[victim-WHITE] = capt;

        /* restore board */
        board[to] = DUMMY;      /* restore board  */
        board[capt] = victim;
        board[from] = piece;

        if((unsigned int) stack[i]>=0xA1000000)   /* was castling or prom */
        {   if(mode==0xA1)
            {
                if(noUnder) {
                    FirstSlider[color]++;
                    piece =oldpiece;
                    pos[piece-WHITE] = from;
                    board[from] = piece;
                } else
                if(--kind[piece-WHITE] >= KNIGHT)
                {
                    if(kind[piece-WHITE] == KNIGHT)
                    {   /* Knight must be put in Knight list */
                        FirstSlider[color]++;
                        piece = ++LastKnight[color]+WHITE;
                        pos[piece-WHITE]  = from;
                        kind[piece-WHITE] = KNIGHT;
                        Zob[piece-WHITE]  = Keys + 128*KNIGHT
                                                 + (color&BLACK)/8 - 0x22;
                    } else Zob[piece-WHITE] -= 128;
                    code[piece-WHITE] = capts[kind[piece-WHITE]];
                    goto minor; /* try minor promotion */
                } else
                {   /* All promotions tried, demote to Pawn again */
                    kind[piece-WHITE] = QUEEN; /* put Q back for hash store */
                    piece = oldpiece; LastKnight[color]--;
                    pos[piece-WHITE] = from;
                    board[from] = piece;
                }
            } else
            {   /* undo Rook move */
                board[j] = board[h];
                board[h] = DUMMY;
                pos[board[j]-WHITE] = j;
            }
        }

        CasRights = SavRights;

    }

    msp = first_move; /* throw away moves */
}

void perft(int color, int lastply, int depth, int d)
{   /* recursive perft, with in-lined make/unmake */
    int i, j, /*k, m, x, y, v,*/ h, /*p,*/ oldpiece, store;
    int first_move, piece, victim, /*pred, succ,*/ from, to, capt, mode;//,
        //pcnr, vicnr, in_check=0, checker= -1, check_dir = 20, legal;
    int SavRights = CasRights, /*lep1,*/ lep2, lkm, flag, Index;
    unsigned long long int ocnt=count, OldKey = HashKey, OldHKey = HighKey, SavCnt;
    union _bucket *Bucket;

    TIME(17)
    first_move = msp; /* new area on move stack */
    //legal = 0;
    move_gen(color, lastply, d); /* generate moves */
    nodecount++;
    /*lep1 = ep1;*/ lep2 = ep2; lkm = Kmoves; flag = depth == 1 && !Promo;

    if(flag)
        count += Kmoves - first_move - ep2 + ep1; /* bulk count */

    for(i = flag ? ep1 : first_move; i<msp; i++)  /* go through all moves */
    {
        if((i == lep2) & flag) { i = lkm; if(i >= msp) break; }

      /* fetch move from move stack */
        from = (stack[i]>>8)&0xFF;
        to = capt = stack[i]&0xFF;
        mode = (unsigned int)stack[i]>>24;
path[d] = stack[i];
        piece  = board[from];

        Index = 0;
        if(mode)
        {   /* e.p. or castling, usually skipped  */
            /* e.p.:shift capture square one rank */
            if(mode < 0xA0)
            {   if(((board[to+RT]^piece) & (COLOR|PAWNS)) == COLOR ||
                   ((board[to+LT]^piece) & (COLOR|PAWNS)) == COLOR)
                    Index = mode * 76265;
            } else
            if(mode == 0xA0) capt ^= 0x10; else
            if(mode == 0xA1)
            {   /* Promotion. Shuffle piece list :( */
                oldpiece = piece; pos[piece-WHITE]=0;
                /* set up for new Queen first */
                piece = --FirstSlider[color]+WHITE;
                kind[piece-WHITE] = QUEEN;
                code[piece-WHITE] = C_QUEEN;
                Zob[piece-WHITE]  = Keys + 128*QUEEN + (color&BLACK)/8 - 0x22;
                pos[piece-WHITE]  = from;
                HashKey ^= Zobrist(piece, from) ^ Zobrist(oldpiece, from);
                HighKey ^= Zobrist(piece, from+8) ^ Zobrist(oldpiece, from+8);
                Index += 14457159; /* prevent hits by non-promotion moves */
            }else
            {   /* castling, determine Rook move  */
                j = mode - 0xB0 + from;
                h = (from+to) >> 1;
                /* abort if Rook in check         */
                if(capturable(color^COLOR, h)) continue;
                /* move Rook                      */
                board[h] = board[j];
                board[j] = DUMMY;
                pos[board[h]-WHITE] = h;
                HashKey ^= Zobrist(board[h],h) ^ Zobrist(board[h],j);
                HighKey ^= Zobrist(board[h],h+8) ^ Zobrist(board[h],j+8);
            }
        }

        victim = board[capt];
        CasRights |= cstl[piece-WHITE] | cstl[victim-WHITE];

        if(depth==1)
        {   if(piece != color && mode < 0xA0)
            {   count++; goto quick; }
        } else if(HashFlag)
        {
            SavCnt = count;
            HashKey ^= Zobrist(piece,from)  /* key update for normal move */
                     ^ Zobrist(piece,to)
                     ^ Zobrist(victim,capt);
            HighKey ^= Zobrist(piece,from+8)  /* key update for normal move */
                     ^ Zobrist(piece,to+8)
                     ^ Zobrist(victim,capt+8);
            Index += (CasRights << 4) +color*919581;
            if(depth>2) {
              path[d] = stack[i];
              if(depth > 7) { // the count will not fit in 32 bits
                 if(depth > 9) {
                   int i = HashSection, j = depth-9;
                   while(j--) i >>= 1;
                   Bucket =      Hash + ((Index + (int)HashKey) & i) + 7 * HashSection + i;
                 } else
                     Bucket =      Hash + ((Index + (int)HashKey) & HashSection) + (depth-3) * HashSection;
                 if(Bucket->l.Signature1 == HighKey && Bucket->l.Signature2 == HashKey)
                 {   count += Bucket->l.longCount; accept[depth]++;
                     goto quick;
                 }
                 reject[depth]++;
                 goto minor;
              }
              Bucket =      Hash + ((Index + (int)HashKey) & HashSection) + (depth-3) * HashSection;
            } else Bucket = ExtraHash + ((Index + (int)HashKey) & (XSIZE-1));

            store = (HashKey>>32) & 1;
            if(Bucket->s.Signature[store] == HighKey && (Bucket->s.Extension[store] ^ (HashKey>>32)) < 2)
             {   count += Bucket->s.Count[store]; accept[depth]++;
                Bucket->s.Extension[store] &= ~1;
                Bucket->s.Extension[store^1] |= 1;
                goto quick;
             }
            if(Bucket->s.Signature[store^1] == HighKey && (Bucket->s.Extension[store^1] ^ (HashKey>>32)) < 2)
            {   count += Bucket->s.Count[store^1]; accept[depth]++;
                Bucket->s.Extension[store^1] &= ~1;
                Bucket->s.Extension[store] |= 1;
                goto quick;
            }
            reject[depth]++; // miss;
            if(Bucket->s.Extension[store^1] & 1) store ^= 1;
        }
minor:
      /* perform move, in piece list and on board */
        /* update board position */
        board[capt] = board[from] = DUMMY;
        board[to] = piece;

        /* update piece location in piece list    */
        pos[piece-WHITE] = to;


        /* remove captured piece from piece list  */
        pos[victim-WHITE] = 0;

        if((piece != color && mode != 0xA0) ||
                 !capturable(color^COLOR, pos[color-WHITE]))
        {
      /* recursion or count end leaf */
                if(depth == 2) leaf_perft(COLOR-color, stack[i], depth-1, d+1);
                else perft(COLOR-color, stack[i], depth-1, d+1);
                if(HashFlag)
                {
                    if(depth > 7) { //large entry
                        Bucket->l.Signature1 = HighKey;
                        Bucket->l.Signature2 = HashKey;
                        Bucket->l.longCount  = count - SavCnt;
                    } else { // packed entry
                        Bucket->s.Signature[store] = HighKey;
                        Bucket->s.Extension[store] = (HashKey>>32) & ~1; // erase low bit
                        Bucket->s.Count[store]     = count - SavCnt;
                    }
                }
        }
      /* retract move */

        /* restore piece list */
        pos[piece-WHITE] = from;
        pos[victim-WHITE] = capt;

        /* restore board */
        board[to] = DUMMY;      /* restore board  */
        board[capt] = victim;
        board[from] = piece;

quick:
        if((unsigned int) stack[i]>=0xA1000000)   /* was castling or prom */
        {   if(mode==0xA1)
            {
                if(noUnder) {
                    FirstSlider[color]++;
                    piece =oldpiece;
                    pos[piece-WHITE] = from;
                    board[from] = piece;
                } else
                if(--kind[piece-WHITE] >= KNIGHT)
                {
                    HashKey ^= Zobrist(piece, to);
                    HighKey ^= Zobrist(piece, to+8);
                    if(kind[piece-WHITE] == KNIGHT)
                    {   /* Knight must be put in Knight list */
                        FirstSlider[color]++;
                        piece = ++LastKnight[color]+WHITE;
                        pos[piece-WHITE]  = from;
                        kind[piece-WHITE] = KNIGHT;
                        Zob[piece-WHITE]  = Keys + 128*KNIGHT
                                                 + (color&BLACK)/8 - 0x22;
                    } else Zob[piece-WHITE] -= 128;
                    code[piece-WHITE] = capts[kind[piece-WHITE]];
                    HashKey ^= Zobrist(piece, to);
                    HighKey ^= Zobrist(piece, to+8);
                    goto minor; /* try minor promotion */
                } else
                {   /* All promotions tried, demote to Pawn again */
                    kind[piece-WHITE] = QUEEN; /* put Q back for hash store */
                    piece = oldpiece; LastKnight[color]--;
                    pos[piece-WHITE] = from;
                    board[from] = piece;
                }
            } else
            {   /* undo Rook move */
                board[j] = board[h];
                board[h] = DUMMY;
                pos[board[j]-WHITE] = j;
            }
        }

        HashKey = OldKey;
        HighKey = OldHKey;
        CasRights = SavRights;

    }

    msp = first_move; /* throw away moves */

    /* For split perft uncomment the following line */
    if(d <= Split && d > 1)
    {
        int i; clock_t t = clock();
        printf("%d. ", d);
        fprintf(f, "%d. ", d);
        for(i=1; i<d; i++) {
                   printf("%c%c%c%c ",
                   'a'+((path[i]-0x2222)>> 8&7),
                   '1'+((path[i]-0x2222)>>12&7),
                   'a'+((path[i]-0x2222)    &7),
                   '1'+((path[i]-0x2222)>> 4&7) );
                   fprintf(f, "%c%c%c%c ",
                   'a'+((path[i]-0x2222)>> 8&7),
                   '1'+((path[i]-0x2222)>>12&7),
                   'a'+((path[i]-0x2222)    &7),
                   '1'+((path[i]-0x2222)>> 4&7) );
        }
        printf("moves = %10lld (%6.3f sec)\n", count-ocnt, (t - ttt[d])*(1./CLOCKS_PER_SEC));
        fprintf(f, "moves = %10lld (%6.3f sec)\n", count-ocnt, (t - ttt[d])*(1./CLOCKS_PER_SEC)); fflush(f);
        ttt[d] = t;
    }
}

void doit(int Dep, int Col, int split) {

    Split = split;
    
    printf("Quick Perft by H.G. Muller\n");
    printf("Perft mode: ");
    if(HashFlag) printf("Hash-table size = %d%cB",
                 (HashSize+2) >> (HashSize<64*1024 ? 6: 16),
                 HashSize<64*1024 ? 'k' : 'M' );
    else         printf("No hashing");
    printf(", bulk counting in horizon nodes\n\n");
    f = fopen("log.txt", "a");
    fprintf(f, "perft %d -%d\n", Dep, Split);

    for(int i=1; i<=Dep; i++)
    {
        int lastPly = ((epSqr^16)<<24) + checker(Col);
        clock_t t = clock();
        count = epcnt = xcnt = ckcnt = cascnt = promcnt = 0;
        for(int j=0; j<10; j++) accept[j] = reject[j] = 0, ttt[j] = t;
        if(i == 1) leaf_perft(Col, lastPly, i, 1); else
        perft(Col, lastPly, i, 1);
        t = clock()-t;
        printf("perft(%2d)= %12lld (%6.3f sec)\n", i, count, t*(1./CLOCKS_PER_SEC));
    }
    fclose(f);
}

void setup_hash(int size) {
    HashSize = size;

    {    HashSection = (1<<(HashSize-3)) - 1; HashSize = (1<<HashSize) - 2;
         Hash = (union _bucket *) calloc(HashSize+4, sizeof(union _bucket) );
         Hash = (union _bucket *) (((long)Hash + 63) & ~63);
         printf("Hash-table size = %x, Starts at %lx,section = %x\n", HashSize+1, (long)Hash, HashSection);
         HashFlag++;
         for(int i=128; i<1040; i++) Keys[i] = rand()>>6;
    }
}

/**
 * @return color
 */
int setup_board(const char* FEN) {
    noUnder = 0; // for strict perft adherence
    
    delta_init();

    piece_init();

    board_init(board);

    int color = ReadFEN(FEN);

    setup();
                                          
    pboard(board, 12, 0);

    return color;
}

}; //class P

int main(int argc, char **argv)
{
    const char *FEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w QKqk",
               *Fritz = "r3r1k1/1pq2pp1/2p2n2/1PNn4/2QN2b1/6P1/3RPP2/2R3KB b - -";
    int depth = 6;

    if(argc > 1 && !strcmp(argv[1], "-u")) {
	argc--; argv++;
    }

    if(argc > 1 && sscanf(argv[1], "%d", &depth)==1 && depth > 0)
    {   argc--; argv++; } else
    {   printf("Usage is: perft <depth> [H<hash size>] [-<split depth>] [<FEN string>]\n");
        printf("          <hash size> = 20 gives you 2^20 = 1M entries (16MB)\n");
        exit(0);
    }

    int hash_size = 0;
    if(argc > 1 && argv[1][0] == 'H' && sscanf(argv[1]+1, "%d", &hash_size) == 1) {
        argc--; argv++;
    }

    int split = 0;
    if(argc > 1 && argv[1][0] == '-' && sscanf(argv[1]+1, "%d", &split) == 1) {
        argc--; argv++;
    }

    if(argc > 1) FEN = argv[1];

    class P p;

    if(hash_size > 0) { p.setup_hash(hash_size); }

    int color = p.setup_board(FEN);

    if(color < 0) {
        printf("Bad FEN '%s', error code = %d\n", FEN, color);
        exit(0);
    }

    p.doit(depth, color, split);
}

