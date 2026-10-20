#include "all.h"

/*
* 6502 backend emitter.
*
* Memory layout (zero page):
*   $00-$01  software stack pointer (16-bit, LE)
*   $02-$09  return value / scratch (ZpRet, 8 bytes)
*   $0A-$11  first argument scratch (ZpArg0, 8 bytes)
*   $12-$19  second argument scratch (ZpArg1, 8 bytes)
*   $1A-$1B  pointer scratch (ZpPtr, 2 bytes)
*   $1C-$1D  base/frame pointer (ZpBp, 2 bytes)
*   $1E+     QBE temporaries (4 bytes per Kw, 8 per Kl)
*
* Variants:
*   MosNmos  - NMOS 6502: no BRA, no STZ,
*              (zp),Y only, safe JMP indirect
*   Mos2a07  - Ricoh 2A03/2A07 (NES):
*              6502 sans BCD (D flag ignored)
*   Mos6510  - MOS 6510 (C64): I/O port at $00-$01,
*              ZP shifted up by 2 bytes
*   Mos65c02 - WDC 65C02: BRA, STZ, PHX/PLX,
*              (zp) indirect without Y
*   Mos65816 - WDC 65816: 16-bit A/X/Y via
*              REP/SEP, much faster 32-bit math
*/

int ZpSp, ZpRet, ZpArg0, ZpArg1, ZpPtr, ZpBp, ZpTmp;
static int ZpMax = 0xF0;

static int needmul, needdiv, needudiv;
static int needmod, needumod;
static int lbl;

static char *
sname(char *n)
{
	if (n[0] == '$')
		return n + 1;
	return n;
}

static int *zpoff;
static int call_stk_usage;
static int va_base_offset;
static int zpuse;

static int
tmpoff(int t)
{
	return zpoff[t - Tmp0];
}

/* store zero to zp byte */
static void
stostz(FILE *f, int zp)
{
	if (mosvar >= Mos65c02) {
		fprintf(f, "\tstz $%02X\n", zp);
	} else {
		fprintf(f, "\tlda #0\n");
		fprintf(f, "\tsta $%02X\n", zp);
	}
}

/* emit unconditional branch */
static void
emitjmpn(FILE *f, int n)
{
	if (mosvar >= Mos65c02)
		fprintf(f, "\tbra @done_%d\n", n);
	else
		fprintf(f, "\tjmp @done_%d\n", n);
}

/* emit constant */
static void
emitcon(FILE *f, int zp, int64_t val, int sz)
{
	int i;
	int64_t v = val;

	for (i=0; i<sz; i++) {
		if ((v & 0xff) == 0)
			stostz(f, zp + i);
		else {
			fprintf(f, "\tlda #%d\n", (int)(v & 0xff));
			fprintf(f, "\tsta $%02X\n", zp + i);
		}
		v >>= 8;
	}
}

/* copy bytes */
static void
emitcopy(FILE *f, int dst, int src, int sz)
{
	int i;

	if (dst == src)
		return;
	if (mosvar == Mos65816) {
		fprintf(f, "\trep #$20\n");
		fprintf(f, "\t.a16\n");
		for (i=0; i<sz; i+=2) {
			fprintf(f, "\tlda $%02X\n", src + i);
			fprintf(f, "\tsta $%02X\n", dst + i);
		}
		fprintf(f, "\tsep #$20\n");
		fprintf(f, "\t.a8\n");
		return;
	}
	for (i=0; i<sz; i++) {
		fprintf(f, "\tlda $%02X\n", src + i);
		fprintf(f, "\tsta $%02X\n", dst + i);
	}
}

/* emit loading a Ref into zp destination */
static void
emitref(Ref r, int dst, int sz, Fn *fn, FILE *f)
{
	Con *c;
	int64_t v;
	int i;

	switch (rtype(r)) {
	case RTmp:
		emitcopy(f, dst, tmpoff((int)r.val), sz);
		break;
	case RCon:
		c = &fn->con[r.val];
		switch (c->type) {
		case CBits:
			emitcon(f, dst, c->bits.i, sz);
			break;
		case CAddr:
			fprintf(f, "\tlda #<_%s\n", sname(str(c->sym.id)));
			fprintf(f, "\tsta $%02X\n", dst);
			fprintf(f, "\tlda #>_%s\n", sname(str(c->sym.id)));
			fprintf(f, "\tsta $%02X\n", dst + 1);
			if (c->bits.i) {
				fprintf(f, "\tclc\n");
				fprintf(f, "\tlda $%02X\n", dst);
				fprintf(f, "\tadc #%d\n", (int)(c->bits.i & 0xff));
				fprintf(f, "\tsta $%02X\n", dst);
				fprintf(f, "\tlda $%02X\n", dst + 1);
				fprintf(f, "\tadc #%d\n", (int)((c->bits.i >> 8) & 0xff));
				fprintf(f, "\tsta $%02X\n", dst + 1);
			}
			for (i=2; i<sz; i++)
				stostz(f, dst + i);
			break;
		default:
			emitcon(f, dst, 0, sz);
			break;
		}
		break;
	case RInt:
		v = r.val;
		if (v >= (1 << 28))
			v -= (1 << 29);
		emitcon(f, dst, v, sz);
		break;
	case RSlot:
		v = (int)r.val & 0x1fffffff;
		fprintf(f, "\tclc\n");
		fprintf(f, "\tlda $%02X\n", ZpSp);
		fprintf(f, "\tadc #%d\n", (int)(v & 0xff));
		fprintf(f, "\tsta $%02X\n", dst);
		fprintf(f, "\tlda $%02X\n", ZpSp + 1);
		fprintf(f, "\tadc #%d\n", (int)((v >> 8) & 0xff));
		fprintf(f, "\tsta $%02X\n", dst + 1);
		for (i=2; i<sz; i++)
			stostz(f, dst + i);
		break;
	default:
		break;
	}
}

/* n-byte add: dst = a + b */
static void
emitadd(FILE *f, int dst, int a, int b, int sz)
{
	int i;

	if (mosvar == Mos65816) {
		fprintf(f, "\trep #$20\n");
		fprintf(f, "\t.a16\n");
		fprintf(f, "\tclc\n");
		for (i=0; i<sz; i+=2) {
			fprintf(f, "\tlda $%02X\n", a + i);
			fprintf(f, "\tadc $%02X\n", b + i);
			fprintf(f, "\tsta $%02X\n", dst + i);
		}
		fprintf(f, "\tsep #$20\n");
		fprintf(f, "\t.a8\n");
		return;
	}
	fprintf(f, "\tclc\n");
	for (i=0; i<sz; i++) {
		fprintf(f, "\tlda $%02X\n", a + i);
		fprintf(f, "\tadc $%02X\n", b + i);
		fprintf(f, "\tsta $%02X\n", dst + i);
	}
}

/* n-byte sub: dst = a - b */
static void
emitsub(FILE *f, int dst, int a, int b, int sz)
{
	int i;

	if (mosvar == Mos65816) {
		fprintf(f, "\trep #$20\n");
		fprintf(f, "\t.a16\n");
		fprintf(f, "\tsec\n");
		for (i=0; i<sz; i+=2) {
			fprintf(f, "\tlda $%02X\n", a + i);
			fprintf(f, "\tsbc $%02X\n", b + i);
			fprintf(f, "\tsta $%02X\n", dst + i);
		}
		fprintf(f, "\tsep #$20\n");
		fprintf(f, "\t.a8\n");
		return;
	}
	fprintf(f, "\tsec\n");
	for (i=0; i<sz; i++) {
		fprintf(f, "\tlda $%02X\n", a + i);
		fprintf(f, "\tsbc $%02X\n", b + i);
		fprintf(f, "\tsta $%02X\n", dst + i);
	}
}

/* n-byte neg: dst = 0 - a */
static void
emitneg(FILE *f, int dst, int a, int sz)
{
	int i;

	if (mosvar == Mos65816) {
		fprintf(f, "\trep #$20\n");
		fprintf(f, "\t.a16\n");
		fprintf(f, "\tsec\n");
		for (i=0; i<sz; i+=2) {
			fprintf(f, "\tlda #0\n");
			fprintf(f, "\tsbc $%02X\n", a + i);
			fprintf(f, "\tsta $%02X\n", dst + i);
		}
		fprintf(f, "\tsep #$20\n");
		fprintf(f, "\t.a8\n");
		return;
	}
	fprintf(f, "\tsec\n");
	for (i=0; i<sz; i++) {
		fprintf(f, "\tlda #0\n");
		fprintf(f, "\tsbc $%02X\n", a + i);
		fprintf(f, "\tsta $%02X\n", dst + i);
	}
}

/* n-bit bitwise op */
static void
emitbitop(FILE *f, int dst, int a, int b, char *op, int sz)
{
	int i;

	if (mosvar == Mos65816) {
		fprintf(f, "\trep #$20\n");
		fprintf(f, "\t.a16\n");
		for (i=0; i<sz; i+=2) {
			fprintf(f, "\tlda $%02X\n", a + i);
			fprintf(f, "\t%s $%02X\n", op, b + i);
			fprintf(f, "\tsta $%02X\n", dst + i);
		}
		fprintf(f, "\tsep #$20\n");
		fprintf(f, "\t.a8\n");
		return;
	}
	for (i=0; i<sz; i++) {
		fprintf(f, "\tlda $%02X\n", a + i);
		fprintf(f, "\t%s $%02X\n", op, b + i);
		fprintf(f, "\tsta $%02X\n", dst + i);
	}
}

/* n-bit compare, result 0/1 in dst (dst is always 32 or 64 bit but output is
* 1/0) */
static void
emitcmp(FILE *f, int dst, int a, int b, int op, int sz)
{
	int ll, i;

	ll = lbl++;

	switch (op) {
	case Oceql:
	case Oceqw:
		for (i=0; i<sz; i++) {
			fprintf(f, "\tlda $%02X\n", a + i);
			fprintf(f, "\tcmp $%02X\n", b + i);
			fprintf(f, "\tbne @ne_%d\n", ll);
		}
		fprintf(f, "\tlda #1\n");
		emitjmpn(f, ll);
		fprintf(f, "@ne_%d:\n", ll);
		fprintf(f, "\tlda #0\n");
		fprintf(f, "@done_%d:\n", ll);
		break;
	case Ocnel:
	case Ocnew:
		for (i=0; i<sz; i++) {
			fprintf(f, "\tlda $%02X\n", a + i);
			fprintf(f, "\tcmp $%02X\n", b + i);
			fprintf(f, "\tbne @ne_%d\n", ll);
		}
		fprintf(f, "\tlda #0\n");
		emitjmpn(f, ll);
		fprintf(f, "@ne_%d:\n", ll);
		fprintf(f, "\tlda #1\n");
		fprintf(f, "@done_%d:\n", ll);
		break;
	case Ocsltl:
	case Ocslel:
	case Ocsgtl:
	case Ocsgel:
	case Ocsltw:
	case Ocslew:
	case Ocsgtw:
	case Ocsgew:
		if (op == Ocsgtw || op == Ocsgew || op == Ocsgtl || op == Ocsgel) {
			int t = a;
			a = b;
			b = t;
			op = (op == Ocsgtw || op == Ocsgtl) ? Ocsltw : Ocslew;
		}
		/* signed a < b: sub a-b, check N^V (handles overflow) */
		fprintf(f, "\tsec\n");
		for (i=0; i<sz; i++) {
			fprintf(f, "\tlda $%02X\n", a + i);
			fprintf(f, "\tsbc $%02X\n", b + i);
		}
		fprintf(f, "\tbvc @nv_%d\n", ll);
		fprintf(f, "\teor #$80\n");
		fprintf(f, "@nv_%d:\n", ll);
		fprintf(f, "\tbmi @true_%d\n", ll);
		if (op == Ocslew || op == Ocslel) {
			/* also true if equal */
			for (i=0; i<sz; i++) {
				fprintf(f, "\tlda $%02X\n", a + i);
				fprintf(f, "\tcmp $%02X\n", b + i);
				fprintf(f, "\tbne @false_%d\n", ll);
			}
			fprintf(f, "\tjmp @true_%d\n", ll);
		}
		fprintf(f, "@false_%d:\n", ll);
		fprintf(f, "\tlda #0\n");
		fprintf(f, "\tjmp @done_%d\n", ll);
		fprintf(f, "@true_%d:\n", ll);
		fprintf(f, "\tlda #1\n");
		fprintf(f, "@done_%d:\n", ll);
		break;
	case Ocultl:
	case Oculel:
	case Ocugtl:
	case Ocugel:
	case Ocultw:
	case Oculew:
	case Ocugtw:
	case Ocugew:
		if (op == Ocugtw || op == Ocugew || op == Ocugtl || op == Ocugel) {
			int t = a;
			a = b;
			b = t;
			op = (op == Ocugtw || op == Ocugtl) ? Ocultw : Oculew;
		}
		/* unsigned compare high to low */
		for (i=sz-1; i>=0; i--) {
			fprintf(f, "\tlda $%02X\n", a + i);
			fprintf(f, "\tcmp $%02X\n", b + i);
			fprintf(f, "\tbcc @true_%d\n", ll);
			fprintf(f, "\tbne @false_%d\n", ll);
		}
		if (op == Oculew || op == Oculel)
			fprintf(f, "\tbeq @true_%d\n", ll);
		fprintf(f, "@false_%d:\n", ll);
		fprintf(f, "\tlda #0\n");
		fprintf(f, "\tjmp @done_%d\n", ll);
		fprintf(f, "@true_%d:\n", ll);
		fprintf(f, "\tlda #1\n");
		fprintf(f, "@done_%d:\n", ll);
		break;
	default:
		die("mos: unhandled cmp %d", op);
	}
	fprintf(f, "\tsta $%02X\n", dst);
	/* result is always a word (0 or 1), regardless of comparison width */
	for (i=1; i<4; i++) {
		stostz(f, dst + i);
	}
}
static void
emitshift(FILE *f, int dst, int src, int amt, char *dir, int arith,
	int sz)
{
	int ll, i;

	ll = lbl++;
	fprintf(f, "\tldx $%02X\n", amt);
	fprintf(f, "\tbeq @sdone_%d\n", ll);
	if (src != dst)
		emitcopy(f, dst, src, sz);
	fprintf(f, "@sloop_%d:\n", ll);
	if (strcmp(dir, "left") == 0) {
		fprintf(f, "\tasl $%02X\n", dst);
		for (i=1; i<sz; i++)
			fprintf(f, "\trol $%02X\n", dst + i);
	} else if (arith) {
		fprintf(f, "\tlda $%02X\n", dst + sz - 1);
		fprintf(f, "\tcmp #$80\n");
		for (i=sz-1; i>=0; i--)
			fprintf(f, "\tror $%02X\n", dst + i);
	} else {
		fprintf(f, "\tlsr $%02X\n", dst + sz - 1);
		for (i=sz-2; i>=0; i--)
			fprintf(f, "\tror $%02X\n", dst + i);
	}
	fprintf(f, "\tdex\n");
	fprintf(f, "\tbne @sloop_%d\n", ll);
	fprintf(f, "@sdone_%d:\n", ll);
}
static void
emitstore(Ins *ins, Fn *fn, FILE *f)
{
	int n, j, sz, val_sz;

	n = ins->op - Ostoreb;
	val_sz = (ins->op == Ostorel || ins->op == Ostored) ? 8 : 4;
	emitref(ins->arg[1], ZpPtr, 2, fn, f);
	emitref(ins->arg[0], ZpArg0, val_sz, fn, f);

	switch (n) {
	case 0:
		sz = 1;
		break; /* storeb */
	case 1:
		sz = 2;
		break; /* storeh */
	case 2:
		sz = 4;
		break; /* storew */
	case 3:
		sz = 8;
		break; /* storel */
	case 4:
		sz = 4;
		break; /* stores */
	case 5:
		sz = 8;
		break; /* stored */
	default:
		sz = 4;
		break;
	}

	if (mosvar >= Mos65c02 && sz == 1) {
		/* 65C02: (zp) without Y */
		fprintf(f, "\tlda $%02X\n", ZpArg0);
		fprintf(f, "\tsta ($%02X)\n", ZpPtr);
		return;
	}
	for (j=0; j<sz; j++) {
		fprintf(f, "\tldy #%d\n", j);
		fprintf(f, "\tlda $%02X\n", ZpArg0 + j);
		fprintf(f, "\tsta ($%02X),y\n", ZpPtr);
	}
}

static void
emitload(Ins *ins, int dst, Fn *fn, FILE *f)
{
	int j, sz;

	emitref(ins->arg[0], ZpPtr, 2, fn, f);

	switch (ins->op) {
	case Oloadsb:
		if (mosvar >= Mos65c02) {
			fprintf(f, "\tlda ($%02X)\n", ZpPtr);
		} else {
			fprintf(f, "\tldy #0\n");
			fprintf(f, "\tlda ($%02X),y\n", ZpPtr);
		}
		fprintf(f, "\tsta $%02X\n", dst);
		/* sign extend byte */
		fprintf(f, "\tand #$80\n");
		fprintf(f, "\tbeq @lp_%d\n", lbl);
		fprintf(f, "\tlda #$FF\n");
		fprintf(f, "\tjmp @ln_%d\n", lbl);
		fprintf(f, "@lp_%d:\n", lbl);
		fprintf(f, "\tlda #0\n");
		fprintf(f, "@ln_%d:\n", lbl);
		fprintf(f, "\tsta $%02X\n", dst + 1);
		fprintf(f, "\tsta $%02X\n", dst + 2);
		fprintf(f, "\tsta $%02X\n", dst + 3);
		lbl++;
		break;
	case Oloadub:
		if (mosvar >= Mos65c02) {
			fprintf(f, "\tlda ($%02X)\n", ZpPtr);
		} else {
			fprintf(f, "\tldy #0\n");
			fprintf(f, "\tlda ($%02X),y\n", ZpPtr);
		}
		fprintf(f, "\tsta $%02X\n", dst);
		stostz(f, dst + 1);
		stostz(f, dst + 2);
		stostz(f, dst + 3);
		break;
	case Oloadsh:
		fprintf(f, "\tldy #0\n");
		fprintf(f, "\tlda ($%02X),y\n", ZpPtr);
		fprintf(f, "\tsta $%02X\n", dst);
		fprintf(f, "\tiny\n");
		fprintf(f, "\tlda ($%02X),y\n", ZpPtr);
		fprintf(f, "\tsta $%02X\n", dst + 1);
		fprintf(f, "\tand #$80\n");
		fprintf(f, "\tbeq @lp_%d\n", lbl);
		fprintf(f, "\tlda #$FF\n");
		fprintf(f, "\tjmp @ln_%d\n", lbl);
		fprintf(f, "@lp_%d:\n", lbl);
		fprintf(f, "\tlda #0\n");
		fprintf(f, "@ln_%d:\n", lbl);
		fprintf(f, "\tsta $%02X\n", dst + 2);
		fprintf(f, "\tsta $%02X\n", dst + 3);
		lbl++;
		break;
	case Oloaduh:
		fprintf(f, "\tldy #0\n");
		fprintf(f, "\tlda ($%02X),y\n", ZpPtr);
		fprintf(f, "\tsta $%02X\n", dst);
		fprintf(f, "\tiny\n");
		fprintf(f, "\tlda ($%02X),y\n", ZpPtr);
		fprintf(f, "\tsta $%02X\n", dst + 1);
		stostz(f, dst + 2);
		stostz(f, dst + 3);
		break;
	case Oloadsw:
		/* load 4 bytes, sign-extend to 8 */
		for (j=0; j<4; j++) {
			fprintf(f, "\tldy #%d\n", j);
			fprintf(f, "\tlda ($%02X),y\n", ZpPtr);
			fprintf(f, "\tsta $%02X\n", dst + j);
		}
		fprintf(f, "\tand #$80\n");
		fprintf(f, "\tbeq @lp_%d\n", lbl);
		fprintf(f, "\tlda #$FF\n");
		fprintf(f, "\tjmp @ln_%d\n", lbl);
		fprintf(f, "@lp_%d:\n", lbl);
		fprintf(f, "\tlda #0\n");
		fprintf(f, "@ln_%d:\n", lbl);
		for (j=4; j<8; j++)
			fprintf(f, "\tsta $%02X\n", dst + j);
		lbl++;
		break;
	case Oloaduw:
		/* load 4 bytes, zero-extend to 8 */
		for (j=0; j<4; j++) {
			fprintf(f, "\tldy #%d\n", j);
			fprintf(f, "\tlda ($%02X),y\n", ZpPtr);
			fprintf(f, "\tsta $%02X\n", dst + j);
		}
		for (j=4; j<8; j++)
			stostz(f, dst + j);
		break;
	case Oload:
		sz = (ins->cls == Kl || ins->cls == Kd) ? 8 : 4;
		for (j=0; j<sz; j++) {
			fprintf(f, "\tldy #%d\n", j);
			fprintf(f, "\tlda ($%02X),y\n", ZpPtr);
			fprintf(f, "\tsta $%02X\n", dst + j);
		}
		break;
	default:
		die("mos: unhandled load %d", ins->op);
	}
}

/* emit result copy from ZpRet to dst */
static void
emitretcpy(FILE *f, int dst, int sz)
{
	if (dst != ZpRet)
		emitcopy(f, dst, ZpRet, sz);
}

static void
emitins(Ins *i, Fn *fn, FILE *f)
{
	int dst, sz, al, r0, r1, k, opsz;

	if (i->op == Onop || i->op == Odbgloc)
		return;
	if (ispar(i->op))
		return;
	sz = (i->cls == Kl || i->cls == Kd) ? 8 : 4;
	if (isarg(i->op)) {
		return;
	}

	dst = req(i->to, R) ? ZpRet : tmpoff((int)i->to.val);

	if (i->op == Ocopy) {
		if (!req(i->arg[0], R))
			emitref(i->arg[0], dst, sz, fn, f);
		return;
	}

	if (i->op == Ocall) {
		Con *c;
		c = &fn->con[i->arg[0].val];
		fprintf(f, "\tjsr _%s\n", sname(str(c->sym.id)));
		emitretcpy(f, dst, sz);
		if (call_stk_usage > 0) {
			fprintf(f, "\tclc\n");
			fprintf(f, "\tlda $%02X\n", ZpSp);
			fprintf(f, "\tadc #%d\n", call_stk_usage & 0xff);
			fprintf(f, "\tsta $%02X\n", ZpSp);
			fprintf(f, "\tlda $%02X\n", ZpSp + 1);
			fprintf(f, "\tadc #%d\n", (call_stk_usage >> 8) & 0xff);
			fprintf(f, "\tsta $%02X\n", ZpSp + 1);
		}
		return;
	}

	if (isalloc(i->op)) {
		al = i->op == Oalloc4 ? 4 : i->op == Oalloc8 ? 8 : 16;
		emitref(i->arg[0], ZpArg0, sz, fn, f);
		fprintf(f, "\tsec\n");
		fprintf(f, "\tlda $%02X\n", ZpSp);
		fprintf(f, "\tsbc $%02X\n", ZpArg0);
		fprintf(f, "\tsta $%02X\n", ZpSp);
		fprintf(f, "\tlda $%02X\n", ZpSp + 1);
		fprintf(f, "\tsbc $%02X\n", ZpArg0 + 1);
		fprintf(f, "\tsta $%02X\n", ZpSp + 1);
		if (al > 1) {
			fprintf(f, "\tlda $%02X\n", ZpSp);
			fprintf(f, "\tand #%d\n", ~(al - 1) & 0xff);
			fprintf(f, "\tsta $%02X\n", ZpSp);
			fprintf(f, "\tsta $%02X\n", dst);
		} else {
			fprintf(f, "\tlda $%02X\n", ZpSp);
			fprintf(f, "\tsta $%02X\n", dst);
		}
		fprintf(f, "\tlda $%02X\n", ZpSp + 1);
		fprintf(f, "\tsta $%02X\n", dst + 1);
		stostz(f, dst + 2);
		stostz(f, dst + 3);
		return;
	}

	if (isstore(i->op)) {
		emitstore(i, fn, f);
		return;
	}

	if (isload(i->op)) {
		emitload(i, dst, fn, f);
		return;
	}

	/* extensions */
	switch (i->op) {
	case Oextsb:
		emitref(i->arg[0], dst, sz, fn, f);
		fprintf(f, "\tlda $%02X\n", dst);
		fprintf(f, "\tand #$80\n");
		fprintf(f, "\tbeq @ep_%d\n", lbl);
		fprintf(f, "\tlda #$FF\n");
		fprintf(f, "\tjmp @en_%d\n", lbl);
		fprintf(f, "@ep_%d:\n", lbl);
		fprintf(f, "\tlda #0\n");
		fprintf(f, "@en_%d:\n", lbl);
		fprintf(f, "\tsta $%02X\n", dst + 1);
		fprintf(f, "\tsta $%02X\n", dst + 2);
		fprintf(f, "\tsta $%02X\n", dst + 3);
		lbl++;
		return;
	case Oextub:
		emitref(i->arg[0], dst, sz, fn, f);
		stostz(f, dst + 1);
		stostz(f, dst + 2);
		stostz(f, dst + 3);
		return;
	case Oextsh:
		emitref(i->arg[0], dst, sz, fn, f);
		fprintf(f, "\tlda $%02X\n", dst + 1);
		fprintf(f, "\tand #$80\n");
		fprintf(f, "\tbeq @ep_%d\n", lbl);
		fprintf(f, "\tlda #$FF\n");
		fprintf(f, "\tjmp @en_%d\n", lbl);
		fprintf(f, "@ep_%d:\n", lbl);
		fprintf(f, "\tlda #0\n");
		fprintf(f, "@en_%d:\n", lbl);
		fprintf(f, "\tsta $%02X\n", dst + 2);
		fprintf(f, "\tsta $%02X\n", dst + 3);
		lbl++;
		return;
	case Oextuh:
		emitref(i->arg[0], dst, 2, fn, f);
		stostz(f, dst + 2);
		stostz(f, dst + 3);
		if (sz == 8) {
			stostz(f, dst + 4);
			stostz(f, dst + 5);
			stostz(f, dst + 6);
			stostz(f, dst + 7);
		}
		return;
	case Oextuw:
		emitref(i->arg[0], dst, 4, fn, f);
		if (sz == 8) {
			stostz(f, dst + 4);
			stostz(f, dst + 5);
			stostz(f, dst + 6);
			stostz(f, dst + 7);
		}
		return;
	case Oextsw:
		emitref(i->arg[0], dst, 4, fn, f);
		if (sz == 8) {
			fprintf(f, "\tlda $%02X\n", dst + 3);
			fprintf(f, "\tand #$80\n");
			fprintf(f, "\tbeq @lp_%d\n", lbl);
			fprintf(f, "\tlda #$FF\n");
			fprintf(f, "\tjmp @ln_%d\n", lbl);
			fprintf(f, "@lp_%d:\n", lbl);
			fprintf(f, "\tlda #0\n");
			fprintf(f, "@ln_%d:\n", lbl);
			fprintf(f, "\tsta $%02X\n", dst + 4);
			fprintf(f, "\tsta $%02X\n", dst + 5);
			fprintf(f, "\tsta $%02X\n", dst + 6);
			fprintf(f, "\tsta $%02X\n", dst + 7);
			lbl++;
		}
		return;
	default:
		break;
	}

	/* two-operand: load args */
	r0 = ZpArg0;
	r1 = ZpArg1;
	/* 64-bit comparisons: result is Kw but operands are 8 bytes */
	opsz = sz;
	if (i->op >= Oceql && i->op <= Ocultl)
		opsz = 8;
	if (i->op != Omul && i->op != Odiv && i->op != Oudiv && i->op != Orem &&
			i->op != Ourem) {
		if (!req(i->arg[0], R) && rtype(i->arg[0]) == RTmp)
			r0 = tmpoff((int)i->arg[0].val);
		if (!req(i->arg[1], R) && rtype(i->arg[1]) == RTmp)
			r1 = tmpoff((int)i->arg[1].val);
	}

	if (!req(i->arg[0], R) && r0 == ZpArg0)
		emitref(i->arg[0], r0, opsz, fn, f);
	if (!req(i->arg[1], R) && r1 == ZpArg1)
		emitref(i->arg[1], r1, opsz, fn, f);

	/* comparisons */
	if ((Oceqw <= i->op && i->op <= Ocultw) ||
			(Oceql <= i->op && i->op <= Ocultl)) {
		emitcmp(f, dst, r0, r1, i->op, opsz);
		return;
	}

	switch (i->op) {
	case Oadd:
		emitadd(f, dst, r0, r1, sz);
		break;
	case Osub:
		emitsub(f, dst, r0, r1, sz);
		break;
	case Oneg:
		emitneg(f, dst, r0, sz);
		break;
	case Omul:
		if (sz == 8)
			die("mos: 64-bit multiply not supported");
		needmul = 1;
		fprintf(f, "\tjsr __mul32\n");
		emitretcpy(f, dst, sz);
		break;
	case Odiv:
		if (sz == 8)
			die("mos: 64-bit divide not supported");
		needdiv = 1;
		fprintf(f, "\tjsr __div32\n");
		emitretcpy(f, dst, sz);
		break;
	case Oudiv:
		if (sz == 8)
			die("mos: 64-bit divide not supported");
		needudiv = 1;
		fprintf(f, "\tjsr __udiv32\n");
		emitretcpy(f, dst, sz);
		break;
	case Orem:
		if (sz == 8)
			die("mos: 64-bit remainder not supported");
		needmod = 1;
		fprintf(f, "\tjsr __mod32\n");
		emitretcpy(f, dst, sz);
		break;
	case Ourem:
		if (sz == 8)
			die("mos: 64-bit remainder not supported");
		needumod = 1;
		fprintf(f, "\tjsr __umod32\n");
		emitretcpy(f, dst, sz);
		break;
	case Oand:
		emitbitop(f, dst, r0, r1, "and", sz);
		break;
	case Oor:
		emitbitop(f, dst, r0, r1, "ora", sz);
		break;
	case Oxor:
		emitbitop(f, dst, r0, r1, "eor", sz);
		break;
	case Oshl:
		emitshift(f, dst, r0, r1, "left", 0, sz);
		break;
	case Oshr:
		emitshift(f, dst, r0, r1, "right", 0, sz);
		break;
	case Osar:
		emitshift(f, dst, r0, r1, "right", 1, sz);
		break;
	case Oblit1:
		break;
	case Oblit0:
		if ((i + 1)->op != Oblit1)
			die("mos: Oblit0 not followed by Oblit1");
		al = (i + 1)->arg[0].val;
		if (al > 255)
			die("mos: blit size %d exceeds "
					"8-bit Y register limit", al);
		fprintf(f, "\tldy #0\n");
		fprintf(f, "@blit_loop_%d:\n", lbl);
		fprintf(f, "\tcpy #%d\n", al);
		fprintf(f, "\tbeq @blit_done_%d\n", lbl);
		fprintf(f, "\tlda ($%02X),y\n", r0);
		fprintf(f, "\tsta ($%02X),y\n", r1);
		fprintf(f, "\tiny\n\tjmp @blit_loop_%d\n", lbl);
		fprintf(f, "@blit_done_%d:\n", lbl);
		lbl++;
		break;
	case Ovastart:
		fprintf(f, "\tldy #0\n");
		fprintf(f, "\tlda $%02X\n\tsta ($%02X),y\n", va_base_offset, r0);
		fprintf(f, "\tlda $%02X\n", va_base_offset + 1);
		fprintf(f, "\tiny\n\tsta ($%02X),y\n", r0);
		break;
	case Ovaarg:
		/* Load va_list pointer into ZpArg0 */
		fprintf(f, "\tldy #0\n");
		fprintf(f, "\tlda ($%02X),y\n", r0);
		fprintf(f, "\tsta $%02X\n", ZpArg0);
		fprintf(f, "\tiny\n");
		fprintf(f, "\tlda ($%02X),y\n", r0);
		fprintf(f, "\tsta $%02X\n", ZpArg0 + 1);

		/* Dereference va_list pointer to read 'sz' bytes into dst */
		fprintf(f, "\tldy #0\n");
		for (k=0; k<sz; k++) {
			fprintf(f, "\tlda ($%02X),y\n", ZpArg0);
			fprintf(f, "\tsta $%02X\n", dst + k);
			fprintf(f, "\tiny\n");
		}

		/* Increment va_list pointer and store back to r0 */
		fprintf(f, "\tclc\n");
		fprintf(f, "\tlda $%02X\n", ZpArg0);
		fprintf(f, "\tadc #%d\n", sz);
		fprintf(f, "\tsta $%02X\n", ZpArg0);
		fprintf(f, "\tlda $%02X\n", ZpArg0 + 1);
		fprintf(f, "\tadc #0\n");
		fprintf(f, "\tsta $%02X\n", ZpArg0 + 1);

		fprintf(f, "\tldy #0\n");
		fprintf(f, "\tlda $%02X\n", ZpArg0);
		fprintf(f, "\tsta ($%02X),y\n", r0);
		fprintf(f, "\tiny\n");
		fprintf(f, "\tlda $%02X\n", ZpArg0 + 1);
		fprintf(f, "\tsta ($%02X),y\n", r0);
		break;
	default:
		die("mos: no match for %s(%c)", optab[i->op].name, "wlsd"[i->cls]);
	}
}

static void
emitcallargs(Blk *b, Ins *call, Fn *fn, FILE *f)
{
	Ins *i, *first_arg;
	int k, sz, off;
	call_stk_usage = 0;

	for (i=call-1; i>=b->ins; i--)
		if (!isarg(i->op))
			break;

	/* Calculate total stack space for arguments */
	off = 0;
	first_arg = i + 1;
	for (i=first_arg; i<call; i++) {
		if (i->op == Oargv)
			continue;
		call_stk_usage += (i->cls == Kl || i->cls == Kd) ? 8 : 4;
	}
	if (call_stk_usage > 255)
		die("mos: call args exceed 8-bit Y register limit");

	if (call_stk_usage > 0) {
		fprintf(f, "\tsec\n");
		fprintf(f, "\tlda $%02X\n", ZpSp);
		fprintf(f, "\tsbc #%d\n", call_stk_usage & 0xff);
		fprintf(f, "\tsta $%02X\n", ZpSp);
		fprintf(f, "\tlda $%02X\n", ZpSp + 1);
		fprintf(f, "\tsbc #%d\n", (call_stk_usage >> 8) & 0xff);
		fprintf(f, "\tsta $%02X\n", ZpSp + 1);
	}

	/* Store parameters into the stack incrementally */
	for (i=first_arg; i<call; i++) {
		if (i->op == Oargv)
			continue;
		sz = (i->cls == Kl || i->cls == Kd) ? 8 : 4;
		/* Emit reference to tmp storage */
		emitref(i->arg[0], ZpRet, sz, fn, f);
		fprintf(f, "\tldy #%d\n", off);
		for (k=0; k<sz; k++) {
			fprintf(f, "\tlda $%02X\n", ZpRet + k);
			fprintf(f, "\tsta ($%02X),y\n", ZpSp);
			if (k < sz - 1)
				fprintf(f, "\tiny\n");
		}
		off += sz;
	}
}

static void
emitphis(Blk *src, Blk *dst, Fn *fn, FILE *f)
{
	Phi *p, *q;
	uint a, b;
	int sz, clobber;

	/* pass 1: save destinations that will be clobbered
	* (a later phi reads from this phi's destination) */
	for (p=dst->phi; p; p=p->link)
		for (a=0; a<p->narg; a++)
			if (p->blk[a] == src) {
				clobber = 0;
				for (q=p->link; q; q=q->link)
					for (b=0; b<q->narg; b++)
						if (q->blk[b] == src
						&& rtype(q->arg[b]) == RTmp
						&& q->arg[b].val == p->to.val)
							clobber = 1;
				if (clobber) {
					sz = (p->cls == Kl || p->cls == Kd) ? 8 : 4;
					emitcopy(f, ZpArg1, tmpoff((int)p->to.val), sz);
				}
			}

	/* pass 2: assign phi values, reading from ZpArg1 when
	* an earlier phi already overwrote the source */
	for (p=dst->phi; p; p=p->link)
		for (a=0; a<p->narg; a++)
			if (p->blk[a] == src) {
				sz = (p->cls == Kl || p->cls == Kd) ? 8 : 4;
				clobber = 0;
				if (rtype(p->arg[a]) == RTmp)
					for (q=dst->phi; q!=p; q=q->link)
						for (b=0; b<q->narg; b++)
							if (q->blk[b] == src
							&& q->to.val == p->arg[a].val)
								clobber = 1;
				if (clobber)
					emitcopy(f, tmpoff((int)p->to.val), ZpArg1, sz);
				else
					emitref(p->arg[a], tmpoff((int)p->to.val), sz, fn, f);
			}
}

static void
emit_zp_restore(Fn *fn, FILE *out, uint blk_id)
{
	int zp_sz = zpuse - ZpTmp;
	int tmp_off = 0;

	if (fn->dynalloc) {
		zp_sz += 2;
	}

	if (zp_sz > 0) {
		if (fn->dynalloc) {
			fprintf(out, "\tlda $%02X\n\tsta $%02X\n", ZpBp, ZpSp);
			fprintf(out, "\tlda $%02X\n\tsta $%02X\n", ZpBp + 1, ZpSp + 1);
			tmp_off += 2;
		}

		if (tmp_off < zp_sz) {
			fprintf(out, "\tldy #%d\n", tmp_off);
			fprintf(out, "@restore_zp_blk_%u:\n", blk_id);
			fprintf(out, "\tlda ($%02X),y\n", ZpSp);
			fprintf(out, "\tsta $%02X,y\n", ZpTmp - tmp_off);
			fprintf(out, "\tiny\n\tcpy #%d\n\tbne @restore_zp_blk_%u\n", zp_sz,
							blk_id);
		}

		if (fn->dynalloc) {
			fprintf(out, "\tldy #0\n");
			fprintf(out, "\tlda ($%02X),y\n", ZpSp);
			fprintf(out, "\tsta $%02X\n", ZpBp);
			fprintf(out, "\tiny\n");
			fprintf(out, "\tlda ($%02X),y\n", ZpSp);
			fprintf(out, "\tsta $%02X\n", ZpBp + 1);
		}

		fprintf(out, "\tclc\n\tlda $%02X\n\tadc #%d\n\tsta $%02X\n", ZpSp,
						zp_sz & 0xff, ZpSp);
		fprintf(out, "\tlda $%02X\n\tadc #%d\n\tsta $%02X\n", ZpSp + 1,
						(zp_sz >> 8) & 0xff, ZpSp + 1);
	}
}

void
mos_emitfn(Fn *fn, FILE *out)
{
	Blk *b;
	Ins *i;
	int n, t, j, k, curr_zp;
	int base, zp_sz, tmp_off, off;
	char *name;

	for (b=fn->start; b; b=b->link) {
		for (i=b->ins; i<&b->ins[b->nins]; i++) {
			if (isalloc(i->op)) {
				fn->dynalloc = 1;
				break;
			}
		}
	}

	name = fn->name;
	if (name[0] == '$')
		name++;

	base = (mosvar == Mos6510) ? 2 : 0;
	ZpSp = 0x00 + base;
	ZpRet = 0x02 + base;
	ZpArg0 = 0x0A + base;
	ZpArg1 = 0x12 + base;
	ZpPtr = 0x1A + base;
	ZpBp = 0x1C + base;
	ZpTmp = 0x1E + base;

	/* dynamically allocate zero-page slots based on size */
	zpoff = calloc(fn->ntmp, sizeof(int));
	curr_zp = ZpTmp;
	for (t=Tmp0; t<fn->ntmp; t++) {
		zpoff[t - Tmp0] = curr_zp;
		if (fn->tmp[t].cls == Kl || fn->tmp[t].cls == Kd)
			curr_zp += 8;
		else
			curr_zp += 4;
	}
	zpuse = curr_zp;

	if (fn->vararg) {
		va_base_offset = curr_zp;
		curr_zp += 2;
		zpuse = curr_zp;
	}

	if (zpuse > ZpMax)
		die("mos: too many temporaries "
				"(zero page exhausted)");

	if (fn->lnk.export)
		fprintf(out, ".export _%s\n", name);
	fprintf(out, ".proc _%s\n", name);

	if (mosvar == Mos2a07)
		fprintf(out, "\tcld\n");

	zp_sz = zpuse - ZpTmp;
	tmp_off = 0;

	if (fn->dynalloc) {
		zp_sz += 2;
	}

	if (zp_sz > 0) {
		fprintf(out, "\tsec\n\tlda $%02X\n\tsbc #%d\n\tsta $%02X\n", ZpSp,
						zp_sz & 0xff, ZpSp);
		fprintf(out, "\tlda $%02X\n\tsbc #%d\n\tsta $%02X\n", ZpSp + 1,
						(zp_sz >> 8) & 0xff, ZpSp + 1);

		if (fn->dynalloc) {
			fprintf(out, "\tldy #0\n");
			fprintf(out, "\tlda $%02X\n", ZpBp);
			fprintf(out, "\tsta ($%02X),y\n", ZpSp);
			fprintf(out, "\tiny\n");
			fprintf(out, "\tlda $%02X\n", ZpBp + 1);
			fprintf(out, "\tsta ($%02X),y\n", ZpSp);
			tmp_off += 2;

			fprintf(out, "\tlda $%02X\n\tsta $%02X\n", ZpSp, ZpBp);
			fprintf(out, "\tlda $%02X\n\tsta $%02X\n", ZpSp + 1, ZpBp + 1);
		}

		if (tmp_off < zp_sz) {
			fprintf(out, "\tldy #%d\n", tmp_off);
			fprintf(out, "@save_zp_%s:\n", name);
			fprintf(out, "\tlda $%02X,y\n", ZpTmp - tmp_off);
			fprintf(out, "\tsta ($%02X),y\n", ZpSp);
			fprintf(out, "\tiny\n\tcpy #%d\n\tbne @save_zp_%s\n", zp_sz, name);
		}
	}

	/* copy params from stack ZpSp to tmps */
	off = zp_sz;
	for (i=fn->start->ins; i<&fn->start->ins[fn->start->nins]; i++) {
		if (i->op == Odbgloc)
			continue;
		if (!ispar(i->op))
			break;
		n = tmpoff((int)i->to.val);
		j = (i->cls == Kl || i->cls == Kd) ? 8 : 4;
		fprintf(out, "\tldy #%d\n", off);
		for (k=0; k<j; k++) {
			fprintf(out, "\tlda ($%02X),y\n", ZpSp);
			fprintf(out, "\tsta $%02X\n", n + k);
			if (k < j - 1)
				fprintf(out, "\tiny\n");
		}
		off += j;
	}

	if (fn->vararg) {
		fprintf(out, "\tclc\n\tlda $%02X\n\tadc #%d\n\tsta $%02X\n", ZpSp, off,
						va_base_offset);
		fprintf(out, "\tlda $%02X\n\tadc #0\n\tsta $%02X\n", ZpSp + 1,
						va_base_offset + 1);
	}

	if (fn->nblk == 1) {
		b = fn->rpo[0];
		for (i=b->ins; i<&b->ins[b->nins]; i++) {
			if (i->op == Ocall)
				emitcallargs(b, i, fn, out);
			emitins(i, fn, out);
		}
		j = b->jmp.type;
		if (isret(j) && j != Jret0 && !req(b->jmp.arg, R))
			emitref(b->jmp.arg, ZpRet,
							(b->jmp.type == Jretl || b->jmp.type == Jretd) ? 8 : 4, fn, out);
		emit_zp_restore(fn, out, (uint)b->id);
		fprintf(out, "\trts\n");
	} else {
		for (n=0; n<(int)fn->nblk; n++) {
			b = fn->rpo[n];
			fprintf(out, "@blk_%d: ; @%s\n", n, b->name);
			for (i=b->ins; i<&b->ins[b->nins]; i++) {
				if (i->op == Ocall)
					emitcallargs(b, i, fn, out);
				emitins(i, fn, out);
			}
			switch (b->jmp.type) {
			case Jjmp:
				emitphis(b, b->s1, fn, out);
				fprintf(out, "\tjmp @blk_%d\n", (int)b->s1->id);
				break;
			case Jjnz:
				j = 4;
				if (rtype(b->jmp.arg) == RTmp
				&& fn->tmp[b->jmp.arg.val].cls == Kl)
					j = 8;
				emitref(b->jmp.arg, ZpArg0, j, fn, out);
				fprintf(out, "\tlda $%02X\n", ZpArg0);
				for (t=1; t<j; t++)
					fprintf(out, "\tora $%02X\n", ZpArg0 + t);
				fprintf(out, "\tbeq @jnz_f_%d\n", (int)b->id);
				emitphis(b, b->s1, fn, out);
				fprintf(out, "\tjmp @blk_%d\n", (int)b->s1->id);
				fprintf(out, "@jnz_f_%d:\n", (int)b->id);
				emitphis(b, b->s2, fn, out);
				fprintf(out, "\tjmp @blk_%d\n", (int)b->s2->id);
				break;
			case Jhlt:
				fprintf(out, "@hlt_%d:\n", (int)b->id);
				fprintf(out, "\tjmp @hlt_%d\n", (int)b->id);
				break;
			default:
				if (isret(b->jmp.type)) {
					if (b->jmp.type != Jret0 && !req(b->jmp.arg, R))
						emitref(b->jmp.arg, ZpRet,
										(b->jmp.type == Jretl || b->jmp.type == Jretd) ? 8 : 4, fn,
										out);
					emit_zp_restore(fn, out, (uint)b->id);
					fprintf(out, "\trts\n");
				} else {
					die("mos: "
							"unhandled jmp %d",
							b->jmp.type);
				}
				break;
			}
		}
	}
	fprintf(out, ".endproc\n\n");
	free(zpoff);
	zpoff = NULL;
}

void
mos_emitdat(Dat *d, FILE *out)
{
	char *s;
	int i, len;

	switch (d->type) {
	case DStart:
		if (d->lnk->export)
			fprintf(out, ".export _%s\n", sname(d->name));
		fprintf(out, "_%s:\n", sname(d->name));
		break;
	case DEnd:
		fprintf(out, "\n");
		break;
	case DZ:
		for (i=0; i<(int)d->u.num; i++)
			fprintf(out, "\t.byte 0\n");
		break;
	case DB:
		if (d->isstr) {
			s = d->u.str;
			len = (int)strlen(s);
			if (len >= 2 && s[0] == '"' && s[len - 1] == '"')
				for (i=1; i<len-1; i++) {
					if (s[i] != '\\' || i + 1 >= len - 1) {
						fprintf(out, "\t.byte %d\n", (uchar)s[i]);
						continue;
					}
					i++;
					switch (s[i]) {
					case 'n':
						fputs("\t.byte 10\n", out);
						break;
					case 't':
						fputs("\t.byte 9\n", out);
						break;
					case '0':
						fputs("\t.byte 0\n", out);
						break;
					case '\\':
						fputs("\t.byte 92\n", out);
						break;
					default:
						fprintf(out, "\t.byte %d\n", (uchar)s[i]);
					}
				}
		} else {
			fprintf(out, "\t.byte %d\n", (int)(d->u.num & 0xff));
		}
		break;
	case DH:
		fprintf(out, "\t.word %d\n", (int)(d->u.num & 0xffff));
		break;
	case DW:
		fprintf(out, "\t.dword %u\n", (unsigned)(d->u.num & 0xffffffffLL));
		break;
	case DL:
		fprintf(out, "\t.dword %u\n", (unsigned)(d->u.num & 0xffffffffLL));
		fprintf(out, "\t.dword %u\n", (unsigned)((d->u.num >> 32) & 0xffffffffLL));
		break;
	}
}

void
mos_emitfin(FILE *out)
{

	/* resolve dependencies */
	if (needmod) {
		needdiv = 1;
		needmul = 1;
	}
	if (needumod) {
		needudiv = 1;
		needmul = 1;
	}
	if (needdiv)
		needudiv = 1;

	if (needmul) {
		fprintf(out, ".proc __mul32\n");
		fprintf(out, "\tlda #0\n");
		fprintf(out, "\tsta $%02X\n", ZpRet);
		fprintf(out, "\tsta $%02X\n", ZpRet + 1);
		fprintf(out, "\tsta $%02X\n", ZpRet + 2);
		fprintf(out, "\tsta $%02X\n", ZpRet + 3);
		fprintf(out, "\tldx #32\n");
		fprintf(out, "@loop:\n");
		fprintf(out, "\tlsr $%02X\n", ZpArg1 + 3);
		fprintf(out, "\tror $%02X\n", ZpArg1 + 2);
		fprintf(out, "\tror $%02X\n", ZpArg1 + 1);
		fprintf(out, "\tror $%02X\n", ZpArg1);
		fprintf(out, "\tbcc @skip\n");
		fprintf(out, "\tclc\n");
		fprintf(out, "\tlda $%02X\n", ZpRet);
		fprintf(out, "\tadc $%02X\n", ZpArg0);
		fprintf(out, "\tsta $%02X\n", ZpRet);
		fprintf(out, "\tlda $%02X\n", ZpRet + 1);
		fprintf(out, "\tadc $%02X\n", ZpArg0 + 1);
		fprintf(out, "\tsta $%02X\n", ZpRet + 1);
		fprintf(out, "\tlda $%02X\n", ZpRet + 2);
		fprintf(out, "\tadc $%02X\n", ZpArg0 + 2);
		fprintf(out, "\tsta $%02X\n", ZpRet + 2);
		fprintf(out, "\tlda $%02X\n", ZpRet + 3);
		fprintf(out, "\tadc $%02X\n", ZpArg0 + 3);
		fprintf(out, "\tsta $%02X\n", ZpRet + 3);
		fprintf(out, "@skip:\n");
		fprintf(out, "\tasl $%02X\n", ZpArg0);
		fprintf(out, "\trol $%02X\n", ZpArg0 + 1);
		fprintf(out, "\trol $%02X\n", ZpArg0 + 2);
		fprintf(out, "\trol $%02X\n", ZpArg0 + 3);
		fprintf(out, "\tdex\n");
		fprintf(out, "\tbne @loop\n");
		fprintf(out, "\trts\n");
		fprintf(out, ".endproc\n\n");
	}

	if (needdiv || needmod) {
		if (needdiv) {
			fprintf(out, ".proc __div32\n");
			fprintf(out, "\tlda $%02X\n", ZpArg0 + 3);
			fprintf(out, "\teor $%02X\n", ZpArg1 + 3);
			fprintf(out, "\tpha\n");
			fprintf(out, "\tlda $%02X\n", ZpArg0 + 3);
			fprintf(out, "\tbpl @pa\n");
			fprintf(out, "\tsec\n");
			fprintf(out, "\tlda #0\n");
			fprintf(out, "\tsbc $%02X\n", ZpArg0);
			fprintf(out, "\tsta $%02X\n", ZpArg0);
			fprintf(out, "\tlda #0\n");
			fprintf(out, "\tsbc $%02X\n", ZpArg0 + 1);
			fprintf(out, "\tsta $%02X\n", ZpArg0 + 1);
			fprintf(out, "\tlda #0\n");
			fprintf(out, "\tsbc $%02X\n", ZpArg0 + 2);
			fprintf(out, "\tsta $%02X\n", ZpArg0 + 2);
			fprintf(out, "\tlda #0\n");
			fprintf(out, "\tsbc $%02X\n", ZpArg0 + 3);
			fprintf(out, "\tsta $%02X\n", ZpArg0 + 3);
			fprintf(out, "@pa:\n");
			fprintf(out, "\tlda $%02X\n", ZpArg1 + 3);
			fprintf(out, "\tbpl @pb\n");
			fprintf(out, "\tsec\n");
			fprintf(out, "\tlda #0\n");
			fprintf(out, "\tsbc $%02X\n", ZpArg1);
			fprintf(out, "\tsta $%02X\n", ZpArg1);
			fprintf(out, "\tlda #0\n");
			fprintf(out, "\tsbc $%02X\n", ZpArg1 + 1);
			fprintf(out, "\tsta $%02X\n", ZpArg1 + 1);
			fprintf(out, "\tlda #0\n");
			fprintf(out, "\tsbc $%02X\n", ZpArg1 + 2);
			fprintf(out, "\tsta $%02X\n", ZpArg1 + 2);
			fprintf(out, "\tlda #0\n");
			fprintf(out, "\tsbc $%02X\n", ZpArg1 + 3);
			fprintf(out, "\tsta $%02X\n", ZpArg1 + 3);
			fprintf(out, "@pb:\n");
			fprintf(out, "\tjsr __udiv32\n");
			fprintf(out, "\tpla\n");
			fprintf(out, "\tbpl @done\n");
			fprintf(out, "\tsec\n");
			fprintf(out, "\tlda #0\n");
			fprintf(out, "\tsbc $%02X\n", ZpRet);
			fprintf(out, "\tsta $%02X\n", ZpRet);
			fprintf(out, "\tlda #0\n");
			fprintf(out, "\tsbc $%02X\n", ZpRet + 1);
			fprintf(out, "\tsta $%02X\n", ZpRet + 1);
			fprintf(out, "\tlda #0\n");
			fprintf(out, "\tsbc $%02X\n", ZpRet + 2);
			fprintf(out, "\tsta $%02X\n", ZpRet + 2);
			fprintf(out, "\tlda #0\n");
			fprintf(out, "\tsbc $%02X\n", ZpRet + 3);
			fprintf(out, "\tsta $%02X\n", ZpRet + 3);
			fprintf(out, "@done:\n");
			fprintf(out, "\trts\n");
			fprintf(out, ".endproc\n\n");
		}

		if (needmod) {
			fprintf(out, ".proc __mod32\n");
			fprintf(out, "\tlda $%02X\n", ZpArg0);
			fprintf(out, "\tpha\n");
			fprintf(out, "\tlda $%02X\n", ZpArg0 + 1);
			fprintf(out, "\tpha\n");
			fprintf(out, "\tlda $%02X\n", ZpArg0 + 2);
			fprintf(out, "\tpha\n");
			fprintf(out, "\tlda $%02X\n", ZpArg0 + 3);
			fprintf(out, "\tpha\n");
			fprintf(out, "\tlda $%02X\n", ZpArg1);
			fprintf(out, "\tpha\n");
			fprintf(out, "\tlda $%02X\n", ZpArg1 + 1);
			fprintf(out, "\tpha\n");
			fprintf(out, "\tlda $%02X\n", ZpArg1 + 2);
			fprintf(out, "\tpha\n");
			fprintf(out, "\tlda $%02X\n", ZpArg1 + 3);
			fprintf(out, "\tpha\n");
			fprintf(out, "\tjsr __div32\n");
			fprintf(out, "\tpla\n");
			fprintf(out, "\tsta $%02X\n", ZpArg1 + 3);
			fprintf(out, "\tpla\n");
			fprintf(out, "\tsta $%02X\n", ZpArg1 + 2);
			fprintf(out, "\tpla\n");
			fprintf(out, "\tsta $%02X\n", ZpArg1 + 1);
			fprintf(out, "\tpla\n");
			fprintf(out, "\tsta $%02X\n", ZpArg1);
			fprintf(out, "\tlda $%02X\n", ZpRet);
			fprintf(out, "\tsta $%02X\n", ZpArg0);
			fprintf(out, "\tlda $%02X\n", ZpRet + 1);
			fprintf(out, "\tsta $%02X\n", ZpArg0 + 1);
			fprintf(out, "\tlda $%02X\n", ZpRet + 2);
			fprintf(out, "\tsta $%02X\n", ZpArg0 + 2);
			fprintf(out, "\tlda $%02X\n", ZpRet + 3);
			fprintf(out, "\tsta $%02X\n", ZpArg0 + 3);
			fprintf(out, "\tjsr __mul32\n");
			fprintf(out, "\tpla\n");
			fprintf(out, "\tsta $%02X\n", ZpArg0 + 3);
			fprintf(out, "\tpla\n");
			fprintf(out, "\tsta $%02X\n", ZpArg0 + 2);
			fprintf(out, "\tpla\n");
			fprintf(out, "\tsta $%02X\n", ZpArg0 + 1);
			fprintf(out, "\tpla\n");
			fprintf(out, "\tsta $%02X\n", ZpArg0);
			fprintf(out, "\tsec\n");
			fprintf(out, "\tlda $%02X\n", ZpArg0);
			fprintf(out, "\tsbc $%02X\n", ZpRet);
			fprintf(out, "\tsta $%02X\n", ZpRet);
			fprintf(out, "\tlda $%02X\n", ZpArg0 + 1);
			fprintf(out, "\tsbc $%02X\n", ZpRet + 1);
			fprintf(out, "\tsta $%02X\n", ZpRet + 1);
			fprintf(out, "\tlda $%02X\n", ZpArg0 + 2);
			fprintf(out, "\tsbc $%02X\n", ZpRet + 2);
			fprintf(out, "\tsta $%02X\n", ZpRet + 2);
			fprintf(out, "\tlda $%02X\n", ZpArg0 + 3);
			fprintf(out, "\tsbc $%02X\n", ZpRet + 3);
			fprintf(out, "\tsta $%02X\n", ZpRet + 3);
			fprintf(out, "\trts\n");
			fprintf(out, ".endproc\n\n");
		}
	}

	if (needumod) {
		fprintf(out, ".proc __umod32\n");
		fprintf(out, "\tlda $%02X\n", ZpArg0);
		fprintf(out, "\tpha\n");
		fprintf(out, "\tlda $%02X\n", ZpArg0 + 1);
		fprintf(out, "\tpha\n");
		fprintf(out, "\tlda $%02X\n", ZpArg0 + 2);
		fprintf(out, "\tpha\n");
		fprintf(out, "\tlda $%02X\n", ZpArg0 + 3);
		fprintf(out, "\tpha\n");
		fprintf(out, "\tlda $%02X\n", ZpArg1);
		fprintf(out, "\tpha\n");
		fprintf(out, "\tlda $%02X\n", ZpArg1 + 1);
		fprintf(out, "\tpha\n");
		fprintf(out, "\tlda $%02X\n", ZpArg1 + 2);
		fprintf(out, "\tpha\n");
		fprintf(out, "\tlda $%02X\n", ZpArg1 + 3);
		fprintf(out, "\tpha\n");
		fprintf(out, "\tjsr __udiv32\n");
		fprintf(out, "\tpla\n");
		fprintf(out, "\tsta $%02X\n", ZpArg1 + 3);
		fprintf(out, "\tpla\n");
		fprintf(out, "\tsta $%02X\n", ZpArg1 + 2);
		fprintf(out, "\tpla\n");
		fprintf(out, "\tsta $%02X\n", ZpArg1 + 1);
		fprintf(out, "\tpla\n");
		fprintf(out, "\tsta $%02X\n", ZpArg1);
		fprintf(out, "\tlda $%02X\n", ZpRet);
		fprintf(out, "\tsta $%02X\n", ZpArg0);
		fprintf(out, "\tlda $%02X\n", ZpRet + 1);
		fprintf(out, "\tsta $%02X\n", ZpArg0 + 1);
		fprintf(out, "\tlda $%02X\n", ZpRet + 2);
		fprintf(out, "\tsta $%02X\n", ZpArg0 + 2);
		fprintf(out, "\tlda $%02X\n", ZpRet + 3);
		fprintf(out, "\tsta $%02X\n", ZpArg0 + 3);
		fprintf(out, "\tjsr __mul32\n");
		fprintf(out, "\tpla\n");
		fprintf(out, "\tsta $%02X\n", ZpArg0 + 3);
		fprintf(out, "\tpla\n");
		fprintf(out, "\tsta $%02X\n", ZpArg0 + 2);
		fprintf(out, "\tpla\n");
		fprintf(out, "\tsta $%02X\n", ZpArg0 + 1);
		fprintf(out, "\tpla\n");
		fprintf(out, "\tsta $%02X\n", ZpArg0);
		fprintf(out, "\tsec\n");
		fprintf(out, "\tlda $%02X\n", ZpArg0);
		fprintf(out, "\tsbc $%02X\n", ZpRet);
		fprintf(out, "\tsta $%02X\n", ZpRet);
		fprintf(out, "\tlda $%02X\n", ZpArg0 + 1);
		fprintf(out, "\tsbc $%02X\n", ZpRet + 1);
		fprintf(out, "\tsta $%02X\n", ZpRet + 1);
		fprintf(out, "\tlda $%02X\n", ZpArg0 + 2);
		fprintf(out, "\tsbc $%02X\n", ZpRet + 2);
		fprintf(out, "\tsta $%02X\n", ZpRet + 2);
		fprintf(out, "\tlda $%02X\n", ZpArg0 + 3);
		fprintf(out, "\tsbc $%02X\n", ZpRet + 3);
		fprintf(out, "\tsta $%02X\n", ZpRet + 3);
		fprintf(out, "\trts\n");
		fprintf(out, ".endproc\n\n");
	}

	if (needudiv) {
		fprintf(out, ".proc __udiv32\n");
		fprintf(out, "\tlda #0\n");
		fprintf(out, "\tsta $%02X\n", ZpRet);
		fprintf(out, "\tsta $%02X\n", ZpRet + 1);
		fprintf(out, "\tsta $%02X\n", ZpRet + 2);
		fprintf(out, "\tsta $%02X\n", ZpRet + 3);
		fprintf(out, "\tldx #32\n");
		fprintf(out, "@loop:\n");
		fprintf(out, "\tasl $%02X\n", ZpArg0);
		fprintf(out, "\trol $%02X\n", ZpArg0 + 1);
		fprintf(out, "\trol $%02X\n", ZpArg0 + 2);
		fprintf(out, "\trol $%02X\n", ZpArg0 + 3);
		fprintf(out, "\trol $%02X\n", ZpRet);
		fprintf(out, "\trol $%02X\n", ZpRet + 1);
		fprintf(out, "\trol $%02X\n", ZpRet + 2);
		fprintf(out, "\trol $%02X\n", ZpRet + 3);
		fprintf(out, "\tsec\n");
		fprintf(out, "\tlda $%02X\n", ZpRet);
		fprintf(out, "\tsbc $%02X\n", ZpArg1);
		fprintf(out, "\tpha\n");
		fprintf(out, "\tlda $%02X\n", ZpRet + 1);
		fprintf(out, "\tsbc $%02X\n", ZpArg1 + 1);
		fprintf(out, "\tpha\n");
		fprintf(out, "\tlda $%02X\n", ZpRet + 2);
		fprintf(out, "\tsbc $%02X\n", ZpArg1 + 2);
		fprintf(out, "\tpha\n");
		fprintf(out, "\tlda $%02X\n", ZpRet + 3);
		fprintf(out, "\tsbc $%02X\n", ZpArg1 + 3);
		fprintf(out, "\tbcc @skip\n");
		fprintf(out, "\tsta $%02X\n", ZpRet + 3);
		fprintf(out, "\tpla\n");
		fprintf(out, "\tsta $%02X\n", ZpRet + 2);
		fprintf(out, "\tpla\n");
		fprintf(out, "\tsta $%02X\n", ZpRet + 1);
		fprintf(out, "\tpla\n");
		fprintf(out, "\tsta $%02X\n", ZpRet);
		fprintf(out, "\tinc $%02X\n", ZpArg0);
		fprintf(out, "\tjmp @next\n");
		fprintf(out, "@skip:\n");
		fprintf(out, "\tpla\n");
		fprintf(out, "\tpla\n");
		fprintf(out, "\tpla\n");
		fprintf(out, "@next:\n");
		fprintf(out, "\tdex\n");
		fprintf(out, "\tbne @loop\n");
		fprintf(out, "\tlda $%02X\n", ZpArg0);
		fprintf(out, "\tsta $%02X\n", ZpRet);
		fprintf(out, "\tlda $%02X\n", ZpArg0 + 1);
		fprintf(out, "\tsta $%02X\n", ZpRet + 1);
		fprintf(out, "\tlda $%02X\n", ZpArg0 + 2);
		fprintf(out, "\tsta $%02X\n", ZpRet + 2);
		fprintf(out, "\tlda $%02X\n", ZpArg0 + 3);
		fprintf(out, "\tsta $%02X\n", ZpRet + 3);
		fprintf(out, "\trts\n");
		fprintf(out, ".endproc\n\n");
	}

	needmul = 0;
	needdiv = 0;
	needudiv = 0;
	needmod = 0;
	needumod = 0;
	lbl = 0;
}
