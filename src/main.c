/* 12jul15abu
 * (c) Software Lab. Alexander Burger
 */

#include "pico.h"

/* Globals */
int Chr, Trace;
char **AV, *AV0, *Home;
heap *Heaps;
cell *Avail;
stkEnv Env;
catchFrame *CatchPtr;
FILE *InFile, *OutFile;
any TheKey, TheCls, Thrown;
any Intern[2], Transient[2];
any ApplyArgs, ApplyBody;

/* ROM Data */
any const __attribute__ ((__aligned__(2*WORD))) Rom[] = {
   #include "rom.d"
};

/* RAM Symbols */
any __attribute__ ((__aligned__(2*WORD))) Ram[] = {
   #include "ram.d"
};

static bool Jam;
static jmp_buf ErrRst;


/*** System ***/
void giveup(char *msg) {
   fprintf(stderr, "%s\n", msg);
   exit(1);
}

void bye(int n) {
   static bool b;

   if (!b) {
      b = YES;
      unwind(NULL);
      prog(val(Bye));
   }
   exit(n);
}

void execError(char *s) {
   fprintf(stderr, "%s: Can't exec\n", s);
   exit(127);
}

/* Allocate memory */
void *alloc(void *p, size_t siz) {
   if (!(p = realloc(p,siz)))
      giveup("No memory");
   return p;
}

/* Allocate cell heap */
void heapAlloc(void) {
   heap *h;
   cell *p;

   h = (heap*)((long)alloc(NULL, sizeof(heap) + sizeof(cell)) + (sizeof(cell)-1) & ~(sizeof(cell)-1));
   h->next = Heaps,  Heaps = h;
   p = h->cells + CELLS-1;
   do
      Free(p);
   while (--p >= h->cells);
}

// (heap 'flg) -> num
any doHeap(any x) {
   long n = 0;

   x = cdr(x);
   if (isNil(EVAL(car(x)))) {
      heap *h = Heaps;
      do
         n += CELLS;
      while (h = h->next);
   }
   else
      for (x = Avail;  x;  x = car(x))
         ++n;
   return box(n * sizeof(cell) / 1024);  // kB
}

// (env ['lst] | ['sym 'val] ..) -> lst
any doEnv(any x) {
   int i;
   bindFrame *p;
   cell c1, c2;

   Push(c1,Nil);
   if (!isCell(x = cdr(x))) {
      for (p = Env.bind;  p;  p = p->link) {
         if (p->i == 0) {
            for (i = p->cnt;  --i >= 0;) {
               for (x = data(c1); ; x = cdr(x)) {
                  if (!isCell(x)) {
                     data(c1) = cons(cons(p->bnd[i].sym, val(p->bnd[i].sym)), data(c1));
                     break;
                  }
                  if (caar(x) == p->bnd[i].sym)
                     break;
               }
            }
         }
      }
   }
   else {
      do {
         Push(c2, EVAL(car(x)));
         if (isCell(data(c2))) {
            do
               data(c1) = cons(
                  isCell(car(data(c2)))?
                     cons(caar(data(c2)), cdar(data(c2))) :
                     cons(car(data(c2)), val(car(data(c2)))),
                  data(c1) );
            while (isCell(data(c2) = cdr(data(c2))));
         }
         else if (!isNil(data(c2))) {
            x = cdr(x);
            data(c1) = cons(cons(data(c2), EVAL(car(x))), data(c1));
         }
         drop(c2);
      }
      while (isCell(x = cdr(x)));
   }
   return Pop(c1);
}

// (up [cnt] sym ['val]) -> any
any doUp(any x) {
   any y, *val;
   int cnt, i;
   bindFrame *p;

   x = cdr(x);
   if (!isNum(y = car(x)))
      cnt = 1;
   else
      cnt = (int)unBox(y),  x = cdr(x),  y = car(x);
   for (p = Env.bind, val = &val(y);  p;  p = p->link) {
      if (p->i <= 0) {
         for (i = 0;  i < p->cnt;  ++i)
            if (p->bnd[i].sym == y) {
               if (!--cnt) {
                  if (isCell(x = cdr(x)))
                     return p->bnd[i].val = EVAL(car(x));
                  return p->bnd[i].val;
               }
               val = &p->bnd[i].val;
            }
      }
   }
   if (isCell(x = cdr(x)))
      return *val = EVAL(car(x));
   return *val;
}

/*** Primitives ***/
any circ(any x) {
   any y;

   if (!isCell(x)  ||  x >= (any)Rom  &&  x < (any)(Rom+ROMS))
      return NULL;
   for (y = x;;) {
      any z = cdr(y);

      *(word*)&cdr(y) |= 1;
      if (!isCell(y = z)) {
         do
            *(word*)&cdr(x) &= ~1;
         while (isCell(x = cdr(x)));
         return NULL;
      }
      if (y >= (any)Rom  &&  y < (any)(Rom+ROMS)) {
         do
            *(word*)&cdr(x) &= ~1;
         while (y != (x = cdr(x)));
         return NULL;
      }
      if (num(cdr(y)) & 1) {
         while (x != y)
            *(word*)&cdr(x) &= ~1,  x = cdr(x);
         do
            *(word*)&cdr(x) &= ~1;
         while (y != (x = cdr(x)));
         return y;
      }
   }
}

/* Comparisons */
bool equal(any x, any y) {
   any a, b;
   bool res;

   if (x == y)
      return YES;
   if (isNum(x))
      return NO;
   if (isSym(x)) {
      if (!isSymb(y))
         return NO;
      if ((x = name(x)) == (y = name(y)))
         return x != txt(0);
      if (isTxt(x) || isTxt(y))
         return NO;
      do {
         if (num(tail(x)) != num(tail(y)))
            return NO;
         x = val(x),  y = val(y);
      } while (!isNum(x) && !isNum(y));
      return x == y;
   }
   if (!isCell(y))
      return NO;
   a = x, b = y;
   res = NO;
   for (;;) {
      if (!equal(car(x), (any)(num(car(y)) & ~1)))
         break;
      if (!isCell(cdr(x))) {
         res = equal(cdr(x), cdr(y));
         break;
      }
      if (!isCell(cdr(y)))
         break;
      if (x < (any)Rom  ||  x >= (any)(Rom+ROMS))
         *(word*)&car(x) |= 1;
      x = cdr(x),  y = cdr(y);
      if (num(car(x)) & 1) {
         for (;;) {
            if (a == x) {
               if (b == y) {
                  for (;;) {
                     a = cdr(a);
                     if ((b = cdr(b)) == y) {
                        res = a == x;
                        break;
                     }
                     if (a == x) {
                        res = YES;
                        break;
                     }
                  }
               }
               break;
            }
            if (b == y) {
               res = NO;
               break;
            }
            *(word*)&car(a) &= ~1,  a = cdr(a),  b = cdr(b);
         }
         do
            *(word*)&car(a) &= ~1,  a = cdr(a);
         while (a != x);
         return res;
      }
   }
   while (a != x  &&  (a < (any)Rom  ||  a >= (any)(Rom+ROMS)))
      *(word*)&car(a) &= ~1,  a = cdr(a);
   return res;
}

long compare(any x, any y) {
   any a, b;

   if (x == y)
      return 0;
   if (isNil(x))
      return -1;
   if (x == T)
      return +1;
   if (isNum(x)) {
      if (!isNum(y))
         return isNil(y)? +1 : -1;
      return num(x) - num(y);
   }
   if (isSym(x)) {
      int c, d, i, j;
      word w, v;

      if (isNum(y) || isNil(y))
         return +1;
      if (isCell(y) || y == T)
         return -1;
      a = name(x),  b = name(y);
      if (a == txt(0) && b == txt(0))
         return (long)x - (long)y;
      if ((c = getByte1(&i, &w, &a)) == (d = getByte1(&j, &v, &b)))
         do
            if (c == 0)
               return 0;
         while ((c = getByte(&i, &w, &a)) == (d = getByte(&j, &v, &b)));
      return c - d;
   }
   if (!isCell(y))
      return y == T? -1 : +1;
   a = x, b = y;
   for (;;) {
      long n;

      if (n = compare(car(x),car(y)))
         return n;
      if (!isCell(x = cdr(x)))
         return compare(x, cdr(y));
      if (!isCell(y = cdr(y)))
         return y == T? -1 : +1;
      if (x == a && y == b)
         return 0;
   }
}

/*** Error handling ***/
void err(any ex, any x, char *fmt, ...) {
   va_list ap;
   char msg[240];
   outFrame f;

   Chr = 0;
   Env.brk = NO;
   f.fp = stderr;
   pushOutFiles(&f);
   while (*AV  &&  strcmp(*AV,"-") != 0)
      ++AV;
   if (ex)
      outString("!? "), print(val(Up) = ex), newline();
   if (x)
      print(x), outString(" -- ");
   va_start(ap,fmt);
   vsnprintf(msg, sizeof(msg), fmt, ap);
   va_end(ap);
   if (msg[0]) {
      outString(msg), newline();
      val(Msg) = mkStr(msg);
      if (!isNil(val(Err)) && !Jam)
         Jam = YES,  prog(val(Err)),  Jam = NO;
      load(NULL, '?', Nil);
   }
   unwind(NULL);
   Env.stack = NULL;
   Env.next = -1;
   Env.make = Env.yoke = NULL;
   Env.parser = NULL;
   Trace = 0;
   Env.put = putStdout;
   Env.get = getStdin;
   longjmp(ErrRst, +1);
}

// (quit ['any ['any]])
any doQuit(any x) {
   any y;

   x = cdr(x),  y = evSym(x);
   {
      char msg[bufSize(y)];

      bufString(y, msg);
      x = isCell(x = cdr(x))?  EVAL(car(x)) : NULL;
      err(NULL, x, "%s", msg);
   }
}

void argError(any ex, any x) {err(ex, x, "Bad argument");}
void numError(any ex, any x) {err(ex, x, "Number expected");}
void symError(any ex, any x) {err(ex, x, "Symbol expected");}
void pairError(any ex, any x) {err(ex, x, "Cons pair expected");}
void atomError(any ex, any x) {err(ex, x, "Atom expected");}
void lstError(any ex, any x) {err(ex, x, "List expected");}
void varError(any ex, any x) {err(ex, x, "Variable expected");}
void protError(any ex, any x) {err(ex, x, "Protected symbol");}

void unwind(catchFrame *catch) {
   any x;
   int i, j, n;
   bindFrame *p;
   catchFrame *q;

   while (q = CatchPtr) {
      while (p = Env.bind) {
         if ((i = p->i) < 0) {
            j = i, n = 0;
            while (++n, ++j && (p = p->link))
               if (p->i >= 0 || p->i < i)
                  --j;
            do {
               for (p = Env.bind, j = n;  --j;  p = p->link);
               if (p->i < 0  &&  ((p->i -= i) > 0? (p->i = 0) : p->i) == 0)
                  for (j = p->cnt;  --j >= 0;) {
                     x = val(p->bnd[j].sym);
                     val(p->bnd[j].sym) = p->bnd[j].val;
                     p->bnd[j].val = x;
                  }
            } while (--n);
         }
         if (Env.bind == q->env.bind)
            break;
         if (Env.bind->i == 0)
            for (i = Env.bind->cnt;  --i >= 0;)
               val(Env.bind->bnd[i].sym) = Env.bind->bnd[i].val;
         Env.bind = Env.bind->link;
      }
      while (Env.inFrames != q->env.inFrames)
         popInFiles();
      while (Env.outFrames != q->env.outFrames)
         popOutFiles();
      Env = q->env;
      EVAL(q->fin);
      CatchPtr = q->link;
      if (q == catch)
         return;
   }
   while (Env.bind) {
      if (Env.bind->i == 0)
         for (i = Env.bind->cnt;  --i >= 0;)
            val(Env.bind->bnd[i].sym) = Env.bind->bnd[i].val;
      Env.bind = Env.bind->link;
   }
   while (Env.inFrames)
      popInFiles();
   while (Env.outFrames)
      popOutFiles();
}

/*** Evaluation ***/
any evExpr(any expr, any x) {
   any y = car(expr);
   symval bnd[length(y)+2];
   struct {  // bindFrame
      struct bindFrame *link;
      int i, cnt;
   } f;

   f.link = Env.bind,  Env.bind = (bindFrame*)&f;
   f.i = sizeof(bnd) / (2*sizeof(any)) - 1;
   f.cnt = 1,  bnd[0].sym = At,  bnd[0].val = val(At);
   while (isCell(y)) {
      bnd[f.cnt].sym = car(y);
      bnd[f.cnt].val = EVAL(car(x));
      ++f.cnt, x = cdr(x), y = cdr(y);
   }
   if (isNil(y)) {
      do {
         x = val(bnd[--f.i].sym);
         val(bnd[f.i].sym) = bnd[f.i].val;
         bnd[f.i].val = x;
      } while (f.i);
      x = prog(cdr(expr));
   }
   else if (y != At) {
      bnd[f.cnt].sym = y,  bnd[f.cnt++].val = val(y),  val(y) = x;
      do {
         x = val(bnd[--f.i].sym);
         val(bnd[f.i].sym) = bnd[f.i].val;
         bnd[f.i].val = x;
      } while (f.i);
      x = prog(cdr(expr));
   }
   else {
      int n, cnt;
      cell *arg;
      cell c[n = cnt = length(x)];

      while (--n >= 0)
         Push(c[n], EVAL(car(x))),  x = cdr(x);
      do {
         x = val(bnd[--f.i].sym);
         val(bnd[f.i].sym) = bnd[f.i].val;
         bnd[f.i].val = x;
      } while (f.i);
      n = Env.next,  Env.next = cnt;
      arg = Env.arg,  Env.arg = c;
      x = prog(cdr(expr));
      if (cnt)
         drop(c[cnt-1]);
      Env.arg = arg,  Env.next = n;
   }
   while (--f.cnt >= 0)
      val(bnd[f.cnt].sym) = bnd[f.cnt].val;
   Env.bind = f.link;
   return x;
}

void undefined(any x, any ex) {err(ex, x, "Undefined");}

static any evList2(any foo, any ex) {
   cell c1;

   Push(c1, foo);
   if (isCell(foo)) {
      foo = evExpr(foo, cdr(ex));
      drop(c1);
      return foo;
   }
   for (;;) {
      if (isNil(val(foo)))
         undefined(foo,ex);
      if (isNum(foo = val(foo))) {
         foo = evSubr(foo,ex);
         drop(c1);
         return foo;
      }
      if (isCell(foo)) {
         foo = evExpr(foo, cdr(ex));
         drop(c1);
         return foo;
      }
   }
}

/* Evaluate a list */
any evList(any ex) {
   any foo;

   if (isNum(foo = car(ex)))
      return ex;
   if (isCell(foo)) {
      if (isNum(foo = evList(foo)))
         return evSubr(foo,ex);
      return evList2(foo,ex);
   }
   for (;;) {
      if (isNil(val(foo)))
         undefined(foo,ex);
      if (isNum(foo = val(foo)))
         return evSubr(foo,ex);
      if (isCell(foo))
         return evExpr(foo, cdr(ex));
   }
}

/* Evaluate number */
long evNum(any ex, any x) {return xNum(ex, EVAL(car(x)));}

long xNum(any ex, any x) {
   NeedNum(ex,x);
   return unBox(x);
}

/* Evaluate any to sym */
any evSym(any x) {return xSym(EVAL(car(x)));}

any xSym(any x) {
   int i;
   word w;
   any y;
   cell c1, c2;

   if (isSymb(x))
      return x;
   Push(c1,x);
   putByte0(&i, &w, &y);
   i = 0,  pack(x, &i, &w, &y, &c2);
   y = popSym(i, w, y, &c2);
   drop(c1);
   return i? y : Nil;
}

// (args) -> flg
any doArgs(any ex __attribute__((unused))) {
   return Env.next > 0? T : Nil;
}

// (next) -> any
any doNext(any ex __attribute__((unused))) {
   if (Env.next > 0)
      return data(Env.arg[--Env.next]);
   if (Env.next == 0)
      Env.next = -1;
   return Nil;
}

// (arg ['cnt]) -> any
any doArg(any ex) {
   long n;

   if (Env.next < 0)
      return Nil;
   if (!isCell(cdr(ex)))
      return data(Env.arg[Env.next]);
   if ((n = evNum(ex,cdr(ex))) > 0  &&  n <= Env.next)
      return data(Env.arg[Env.next - n]);
   return Nil;
}

// (rest) -> lst
any doRest(any x) {
   int i;
   cell c1;

   if ((i = Env.next) <= 0)
      return Nil;
   Push(c1, x = cons(data(Env.arg[--i]), Nil));
   while (i)
      x = cdr(x) = cons(data(Env.arg[--i]), Nil);
   return Pop(c1);
}

any mkDat(int y, int m, int d) {
   int n;
   static char mon[13] = {31,31,28,31,30,31,30,31,31,30,31,30,31};

   if (y<0 || m<1 || m>12 || d<1 || d>mon[m] && (m!=2 || d!=29 || y%4 || !(y%100) && y%400))
      return Nil;
   n = (12*y + m - 3) / 12;
   return box((4404*y+367*m-1094)/12 - 2*n + n/4 - n/100 + n/400 + d);
}

// (date 'dat) -> (y m d)
// (date 'y 'm 'd) -> dat | NIL
// (date '(y m d)) -> dat | NIL
any doDate(any ex) {
   any x, z;
   int y, m, d, n;
   cell c1;

   x = cdr(ex);
   if (isNil(z = EVAL(car(x))))
      return Nil;
   if (isCell(z))
      return mkDat(xNum(ex, car(z)),  xNum(ex, cadr(z)),  xNum(ex, caddr(z)));
   if (!isCell(x = cdr(x))) {
      if ((n = xNum(ex,z)) < 0)
         return Nil;
      y = (100*n - 20) / 3652425;
      n += (y - y/4);
      y = (100*n - 20) / 36525;
      n -= 36525*y / 100;
      m = (10*n - 5) / 306;
      d = (10*n - 306*m + 5) / 10;
      if (m < 10)
         m += 3;
      else
         ++y,  m -= 9;
      Push(c1, cons(box(d), Nil));
      data(c1) = cons(box(m), data(c1));
      data(c1) = cons(box(y), data(c1));
      return Pop(c1);
   }
   y = xNum(ex,z);
   m = evNum(ex,x);
   return mkDat(y, m, evNum(ex,cdr(x)));
}

// (cmd ['any]) -> sym
any doCmd(any x) {
   if (isNil(x = evSym(cdr(x))))
      return mkStr(AV0);
   bufString(x, AV0);
   return x;
}

// (argv [var ..] [. sym]) -> lst|sym
any doArgv(any ex) {
   any x, y;
   char **p;
   cell c1;

   if (*(p = AV) && strcmp(*p,"-") == 0)
      ++p;
   if (isNil(x = cdr(ex))) {
      if (!*p)
         return Nil;
      Push(c1, x = cons(mkStr(*p++), Nil));
      while (*p)
         x = cdr(x) = cons(mkStr(*p++), Nil);
      return Pop(c1);
   }
   do {
      if (!isCell(x)) {
         NeedSymb(ex,x);
         CheckVar(ex,x);
         if (!*p)
            return val(x) = Nil;
         Push(c1, y = cons(mkStr(*p++), Nil));
         while (*p)
            y = cdr(y) = cons(mkStr(*p++), Nil);
         return val(x) = Pop(c1);
      }
      y = car(x);
      NeedVar(ex,y);
      CheckVar(ex,y);
      val(y) = *p? mkStr(*p++) : Nil;
   } while (!isNil(x = cdr(x)));
   return val(y);
}

// (opt) -> sym
any doOpt(any ex __attribute__((unused))) {
   return *AV && strcmp(*AV,"-")? mkStr(*AV++) : Nil;
}

any loadAll(any ex) {
   any x = Nil;

   while (*AV  &&  strcmp(*AV,"-") != 0)
      x = load(ex, 0, mkStr(*AV++));
   return x;
}

/*** Main ***/
int main(int ac, char *av[]) {
   int i;
   char *p;

   AV0 = *av++;
   AV = av;
   heapAlloc();
   Intern[0] = Intern[1] = Transient[0] = Transient[1] = Nil;
   intern(Nil, Intern);
   intern(T, Intern);
   intern(Meth, Intern);
   intern(Quote, Intern);  // Last protected symbol
   for (i = 1; i < RAMS; i += 2)
      if (Ram[i] != (any)(Ram + i))
         intern((any)(Ram + i), Intern);
   if (ac >= 2 && strcmp(av[ac-2], "+") == 0)
      val(Dbg) = T,  av[ac-2] = NULL;
   if (av[0] && *av[0] != '-' && (p = strrchr(av[0], '/')) && !(p == av[0]+1 && *av[0] == '.')) {
      Home = malloc(p - av[0] + 2);
      memcpy(Home, av[0], p - av[0] + 1);
      Home[p - av[0] + 1] = '\0';
   }
   InFile = stdin,  Env.get = getStdin;
   OutFile = stdout,  Env.put = putStdout;
   ApplyArgs = cons(cons(consSym(Nil,0), Nil), Nil);
   ApplyBody = cons(Nil,Nil);
   if (!setjmp(ErrRst))
      prog(val(Main)),  loadAll(NULL);
   while (!feof(stdin))
      load(NULL, ':', Nil);
   bye(0);
}
