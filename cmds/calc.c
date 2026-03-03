#include "cmdargs.h"
#include "stdio.h"
#include <stdint.h>

typedef struct { const char* s; int ok; } p_t;
static void ws(p_t* p){ while (*p->s==' '||*p->s=='\t') p->s++; }
static int expr(p_t* p);
static int num(p_t* p){ ws(p); int sign=1; if(*p->s=='-'){sign=-1;p->s++;} else if(*p->s=='+')p->s++; ws(p); if(*p->s<'0'||*p->s>'9'){p->ok=0;return 0;} int v=0; while(*p->s>='0'&&*p->s<='9'){v=v*10+(*p->s-'0');p->s++;} return sign*v; }
static int fac(p_t* p){ ws(p); if(*p->s=='('){p->s++; int v=expr(p); ws(p); if(*p->s!=')'){p->ok=0;return 0;} p->s++; return v;} return num(p);} 
static int term(p_t* p){ int v=fac(p); while(p->ok){ ws(p); char o=*p->s; if(o!='*'&&o!='/') break; p->s++; int r=fac(p); if(!p->ok) break; if(o=='*') v*=r; else { if(r==0){p->ok=0;return 0;} v/=r; } } return v; }
static int expr(p_t* p){ int v=term(p); while(p->ok){ ws(p); char o=*p->s; if(o!='+'&&o!='-') break; p->s++; int r=term(p); if(!p->ok) break; if(o=='+') v+=r; else v-=r; } return v; }

int main(void){
    cmd_args_t a; cmd_load_args(&a);
    if (a.argc < 2) { eprint("Usage: calc <expr>\n"); return 1; }
    char e[256]; int pos=0; e[0]='\0';
    for (int i=1;i<a.argc && pos < (int)sizeof(e)-1;i++) {
        for (int j=0;a.argv[i][j] && pos < (int)sizeof(e)-1;j++) e[pos++]=a.argv[i][j];
        if (i+1<a.argc && pos < (int)sizeof(e)-1) e[pos++]=' ';
    }
    e[pos]='\0';
    p_t p={e,1}; int v=expr(&p); ws(&p);
    if (!p.ok || *p.s!='\0') { eprint("calc: invalid expression\n"); return 1; }
    printf("%d\n", v);
    return 0;
}
void _start(void){ sys_exit((uint32_t)main()); }
