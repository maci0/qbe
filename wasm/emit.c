#include "all.h"

enum {
	DataBase = 1024,
	DataMax  = 1 << 20,
	SegMax   = 256,
};

typedef struct Seg Seg;

struct Seg {
	char *name;
	Lnk lnk;
	int off;
	int len;
};

static int modstarted;
static int dataoff = DataBase;
static uchar databuf[DataMax];
static int datalen;
static Seg segs[SegMax];
static int nseg;
static int curseg;
static int segoff;

static int
segaddr(char *name)
{
	int i;

	for (i=0; i<nseg; i++)
		if (strcmp(segs[i].name, name) == 0)
			return segs[i].off;
	/* allocate space for unknown symbol */
	if (nseg >= SegMax)
		die("too many data segments");
	if (dataoff + 8 > DataBase + DataMax)
		die("data section too large");
	i = nseg++;
	segs[i].name = name;
	segs[i].off = dataoff;
	segs[i].len = 8;
	dataoff += 8;
	return segs[i].off;
}

static char *wtyp[] = { "i32", "i64", "f32", "f64" };

static char *
sname(char *n)
{
	if (n[0] == '$')
		return n + 1;
	return n;
}

static void
emitmodhdr(FILE *f)
{
	if (modstarted)
		return;
	modstarted = 1;
	fputs("(module\n"
		"  (memory (export \"memory\") 1)\n"
		"  (global $__stack_pointer"
		" (mut i32) (i32.const 65536))\n", f);
}

static void
emitref(Ref r, int cls, Fn *fn, FILE *f)
{
	Con *c;
	int64_t v;
	int addr;
	char *t;

	t = wtyp[cls];
	switch (rtype(r)) {
	case RTmp:
		fprintf(f, "    local.get $t%d\n",
			(int)r.val - Tmp0);
		break;
	case RCon:
		c = &fn->con[r.val];
		switch (c->type) {
		case CBits:
			if (cls == Ks)
				fprintf(f, "    f32.const %a\n",
					(double)c->bits.s);
			else if (cls == Kd)
				fprintf(f, "    f64.const %a\n",
					c->bits.d);
			else
				fprintf(f, "    %s.const %"PRId64"\n",
					t, c->bits.i);
			break;
		case CAddr:
			addr = segaddr(str(c->sym.id));
			if (c->bits.i)
				fprintf(f, "    i32.const %d\n"
					"    i32.const %"PRId64"\n"
					"    i32.add\n",
					addr, c->bits.i);
			else
				fprintf(f, "    i32.const %d\n",
					addr);
			if (cls == Kl)
				fputs("    i64.extend_i32_u\n", f);
			break;
		default:
			fprintf(f, "    %s.const 0\n", t);
			break;
		}
		break;
	case RInt:
		v = r.val;
		if (v >= (1 << 28))
			v -= (1 << 29);
		fprintf(f, "    %s.const %"PRId64"\n", t, v);
		break;
	case RSlot:
		fputs("    global.get $__stack_pointer\n", f);
		fprintf(f, "    i32.const %d\n",
			(int)r.val & 0x1fffffff);
		fputs("    i32.add\n", f);
		break;
	default:
		break;
	}
}

static void
emitaddr(Ref r, Fn *fn, FILE *f)
{
	int cls;

	if (rtype(r) == RTmp) {
		cls = fn->tmp[r.val].cls;
		emitref(r, cls, fn, f);
		if (cls == Kl)
			fputs("    i32.wrap_i64\n", f);
	} else
		emitref(r, Kw, fn, f);
}

static void
emitlset(Ref r, FILE *f)
{
	if (!req(r, R))
		fprintf(f, "    local.set $t%d\n",
			(int)r.val - Tmp0);
}

static void
emitphis(Blk *src, Blk *dst, Fn *fn, FILE *f)
{
	Phi *p, *q;
	uint a, b;
	int clobber;

	for (p=dst->phi; p; p=p->link)
		for (a=0; a<p->narg; a++)
			if (p->blk[a] == src) {
				clobber = 0;
				for (q=p->link; q; q=q->link)
					for (b=0; b<q->narg; b++)
						if (q->blk[b] == src
						&& rtype(q->arg[b]) == RTmp
						&& q->arg[b].val
						   == p->to.val)
							clobber = 1;
				if (clobber)
					fprintf(f,
						"    local.get $t%d\n"
						"    local.set $sw_%c\n",
						(int)p->to.val - Tmp0,
						"wlsd"[p->cls]);
			}
	for (p=dst->phi; p; p=p->link)
		for (a=0; a<p->narg; a++)
			if (p->blk[a] == src) {
				clobber = 0;
				for (q=dst->phi; q!=p; q=q->link)
					for (b=0; b<q->narg; b++)
						if (q->blk[b] == src
						&& q->to.val
						   == p->arg[a].val)
							clobber = 1;
				if (clobber && rtype(p->arg[a]) == RTmp)
					fprintf(f,
						"    local.get $sw_%c\n",
						"wlsd"[p->cls]);
				else
					emitref(p->arg[a], p->cls,
						fn, f);
				fprintf(f, "    local.set $t%d\n",
					(int)p->to.val - Tmp0);
			}
}

static int
opcls(Ins *i)
{
	if (Oceqw <= i->op && i->op <= Ocugew) return Kw;
	if (Oceql <= i->op && i->op <= Ocugel) return Kl;
	if (Oceqs <= i->op && i->op <= Ocuos)  return Ks;
	if (Oceqd <= i->op && i->op <= Ocuod)  return Kd;
	return i->cls;
}

/* indexed by op - Ostoreb */
static int stocls[] = { Kw, Kw, Kw, Kl, Ks, Kd };
static char *wsto[] = {
	"i32.store8",  "i32.store16", "i32.store",
	"i64.store",   "f32.store",   "f64.store",
};

/* indexed by op - Oloadsb */
static char *wlod[] = {
	"load8_s", "load8_u", "load16_s", "load16_u",
	"load32_s", "load32_u", "load",
};

/* indexed by op - Oceqw */
static char *wcmp[] = {
	"i32.eq",   "i32.ne",    /* ceqw, cnew */
	"i32.ge_s", "i32.gt_s",  /* csgew, csgtw */
	"i32.le_s", "i32.lt_s",  /* cslew, csltw */
	"i32.ge_u", "i32.gt_u",  /* cugew, cugtw */
	"i32.le_u", "i32.lt_u",  /* culew, cultw */
	"i64.eq",   "i64.ne",    /* ceql, cnel */
	"i64.ge_s", "i64.gt_s",  /* csgel, csgtl */
	"i64.le_s", "i64.lt_s",  /* cslel, csltl */
	"i64.ge_u", "i64.gt_u",  /* cugel, cugtl */
	"i64.le_u", "i64.lt_u",  /* culel, cultl */
	"f32.eq",   "f32.ge",    /* ceqs, cges */
	"f32.gt",   "f32.le",    /* cgts, cles */
	"f32.lt",   "f32.ne",    /* clts, cnes */
	0,          0,            /* cos, cuos */
	"f64.eq",   "f64.ge",    /* ceqd, cged */
	"f64.gt",   "f64.le",    /* cgtd, cled */
	"f64.lt",   "f64.ne",    /* cltd, cned */
	0,          0,            /* cod, cuod */
};

/* indexed by op - Oexts */
static struct { int acls; char *sfx; } cvtmap[] = {
	{ Ks, "promote_f32" },    /* exts */
	{ Kd, "demote_f64" },     /* truncd */
	{ Ks, "trunc_f32_s" },    /* stosi */
	{ Ks, "trunc_f32_u" },    /* stoui */
	{ Kd, "trunc_f64_s" },    /* dtosi */
	{ Kd, "trunc_f64_u" },    /* dtoui */
	{ Kw, "convert_i32_s" },  /* swtof */
	{ Kw, "convert_i32_u" },  /* uwtof */
	{ Kl, "convert_i64_s" },  /* sltof */
	{ Kl, "convert_i64_u" },  /* ultof */
};

static int
retcls(int j)
{
	if (j == Jretw || isretbh(j))
		return Kw;
	if (j == Jretl) return Kl;
	if (j == Jrets) return Ks;
	return Kd;
}

static void
emitins(Ins *i, Fn *fn, FILE *f)
{
	Con *c;
	char *t;
	int cls, acls, al, fk, n;

	cls = i->cls;
	t = wtyp[cls];

	if (i->op == Onop || i->op == Odbgloc)
		return;
	if (ispar(i->op))
		return;
	if (isarg(i->op))
		return;

	if (i->op == Ocopy) {
		if (!req(i->arg[0], R)) {
			emitref(i->arg[0], cls, fn, f);
			emitlset(i->to, f);
		}
		return;
	}

	if (i->op == Ocall) {
		c = &fn->con[i->arg[0].val];
		fprintf(f, "    call $%s\n", sname(str(c->sym.id)));
		emitlset(i->to, f);
		return;
	}

	if (isalloc(i->op)) {
		al = i->op == Oalloc4 ? 4
			: i->op == Oalloc8 ? 8 : 16;
		fputs("    global.get $__stack_pointer\n", f);
		emitref(i->arg[0], Kw, fn, f);
		fputs("    i32.sub\n", f);
		if (al > 1) {
			fprintf(f, "    i32.const %d\n", -al);
			fputs("    i32.and\n", f);
		}
		fputs("    global.set $__stack_pointer\n"
			"    global.get $__stack_pointer\n", f);
		emitlset(i->to, f);
		return;
	}

	if (isstore(i->op)) {
		n = i->op - Ostoreb;
		emitaddr(i->arg[1], fn, f);
		emitref(i->arg[0], stocls[n], fn, f);
		fprintf(f, "    %s\n", wsto[n]);
		return;
	}

	if (isload(i->op)) {
		n = i->op - Oloadsb;
		if (cls == Kw
		&& (i->op == Oloadsw || i->op == Oloaduw))
			n = Oload - Oloadsb;
		emitaddr(i->arg[0], fn, f);
		fprintf(f, "    %s.%s\n", t, wlod[n]);
		emitlset(i->to, f);
		return;
	}

	if (Oexts <= i->op && i->op <= Oultof) {
		n = i->op - Oexts;
		emitref(i->arg[0], cvtmap[n].acls, fn, f);
		fprintf(f, "    %s.%s\n", t, cvtmap[n].sfx);
		goto Set;
	}

	switch (i->op) {
	case Oextsb:
	case Oextsh:
		emitref(i->arg[0], Kw, fn, f);
		fprintf(f, "    i32.extend%s_s\n",
			i->op == Oextsb ? "8" : "16");
		goto Set;
	case Oextub:
	case Oextuh:
		emitref(i->arg[0], Kw, fn, f);
		fprintf(f, "    i32.const %d\n",
			i->op == Oextub ? 255 : 65535);
		fputs("    i32.and\n", f);
		goto Set;
	case Oextsw:
	case Oextuw:
		emitref(i->arg[0], Kw, fn, f);
		if (rtype(i->arg[0]) == RTmp
		&& fn->tmp[i->arg[0].val].cls == Kl)
			fputs("    i32.wrap_i64\n", f);
		fprintf(f, "    i64.extend_i32_%c\n",
			i->op == Oextsw ? 's' : 'u');
		goto Set;
	case Ocast:
		emitref(i->arg[0], cls ^ 2, fn, f);
		fprintf(f, "    %s.reinterpret_%s\n",
			t, wtyp[cls ^ 2]);
		goto Set;
	case Oneg:
		if (KBASE(cls) == 1) {
			emitref(i->arg[0], cls, fn, f);
			fprintf(f, "    %s.neg\n", t);
		} else {
			fprintf(f, "    %s.const 0\n", t);
			emitref(i->arg[0], cls, fn, f);
			fprintf(f, "    %s.sub\n", t);
		}
		goto Set;
	case Ocos: case Ocuos:
	case Ocod: case Ocuod:
		fk = (i->op == Ocos || i->op == Ocuos)
			? Ks : Kd;
		n = (i->op == Ocos || i->op == Ocod);
		emitref(i->arg[0], fk, fn, f);
		emitref(i->arg[0], fk, fn, f);
		fprintf(f, "    %s.%s\n",
			wtyp[fk], n ? "eq" : "ne");
		emitref(i->arg[1], fk, fn, f);
		emitref(i->arg[1], fk, fn, f);
		fprintf(f, "    %s.%s\n",
			wtyp[fk], n ? "eq" : "ne");
		fputs(n ? "    i32.and\n"
			: "    i32.or\n", f);
		goto Set;
	default:
		break;
	}

	acls = opcls(i);
	emitref(i->arg[0], acls, fn, f);
	if (!req(i->arg[1], R))
		emitref(i->arg[1], acls, fn, f);

	if (Oceqw <= i->op && i->op <= Ocuod
	&& wcmp[i->op - Oceqw]) {
		fprintf(f, "    %s\n",
			wcmp[i->op - Oceqw]);
		goto Set;
	}

	switch (i->op) {
	case Oadd:  fprintf(f, "    %s.add\n", t); break;
	case Osub:  fprintf(f, "    %s.sub\n", t); break;
	case Omul:  fprintf(f, "    %s.mul\n", t); break;
	case Odiv:
		fprintf(f, "    %s.div%s\n", t,
			KBASE(cls) == 0 ? "_s" : "");
		break;
	case Oudiv: fprintf(f, "    %s.div_u\n", t); break;
	case Orem:  fprintf(f, "    %s.rem_s\n", t); break;
	case Ourem: fprintf(f, "    %s.rem_u\n", t); break;
	case Oand:  fprintf(f, "    %s.and\n", t); break;
	case Oor:   fprintf(f, "    %s.or\n", t); break;
	case Oxor:  fprintf(f, "    %s.xor\n", t); break;
	case Oshl:  fprintf(f, "    %s.shl\n", t); break;
	case Oshr:  fprintf(f, "    %s.shr_u\n", t); break;
	case Osar:  fprintf(f, "    %s.shr_s\n", t); break;
	default:
		die("no match for %s(%c)",
			optab[i->op].name, "wlsd"[i->cls]);
	}

Set:
	emitlset(i->to, f);
}

static void
emitcallargs(Blk *b, Ins *call, Fn *fn, FILE *f)
{
	Ins *i;

	for (i=call-1; i>=b->ins; i--)
		if (!isarg(i->op))
			break;
	for (i=i+1; i<call; i++)
		emitref(i->arg[0], i->cls, fn, f);
}

static int
tmpispar(Fn *fn, int t)
{
	Ins *i;

	for (i=fn->start->ins;
	     i<&fn->start->ins[fn->start->nins]
	     && ispar(i->op); i++)
		if ((int)i->to.val == t)
			return 1;
	return 0;
}

void
wasm_emitfn(Fn *fn, FILE *out)
{
	Blk *b;
	Ins *i;
	int n, j, t;

	emitmodhdr(out);

	fprintf(out, "  (func $%s", sname(fn->name));
	if (fn->lnk.export)
		fprintf(out, " (export \"%s\")", sname(fn->name));
	fputc('\n', out);

	for (i=fn->start->ins;
	     i<&fn->start->ins[fn->start->nins]; i++) {
		if (!ispar(i->op))
			break;
		fprintf(out, "    (param $t%d %s)\n",
			(int)i->to.val - Tmp0, wtyp[i->cls]);
	}

	for (b=fn->start; b; b=b->link) {
		j = b->jmp.type;
		if (isret(j) && j != Jret0) {
			fprintf(out, "    (result %s)\n",
				wtyp[retcls(j)]);
			break;
		}
	}

	fputs("    (local $blk i32)\n"
		"    (local $sw_w i32)\n"
		"    (local $sw_l i64)\n"
		"    (local $sw_s f32)\n"
		"    (local $sw_d f64)\n", out);
	for (t=Tmp0; t<fn->ntmp; t++) {
		if (fn->tmp[t].cls == Kx)
			continue;
		if (tmpispar(fn, t))
			continue;
		fprintf(out, "    (local $t%d %s)\n",
			t - Tmp0, wtyp[fn->tmp[t].cls]);
	}

	if (fn->nblk > 1) {
		fprintf(out, "    i32.const %d\n",
			(int)fn->nblk - 1);
		fputs("    local.set $blk\n", out);
	}
	fputs("    (block $exit\n"
		"    (loop $loop\n", out);
	for (n=(int)fn->nblk-1; n>=0; n--)
		fprintf(out, "    (block $case_%d\n", n);

	fputs("    local.get $blk\n"
		"    br_table", out);
	for (n=0; n<(int)fn->nblk; n++)
		fprintf(out, " $case_%d", n);
	fputs(" $exit\n", out);

	for (n=(int)fn->nblk-1; n>=0; n--) {
		fprintf(out, "    ) ;; @%s\n", fn->rpo[n]->name);
		b = fn->rpo[n];

		for (i=b->ins; i<&b->ins[b->nins]; i++) {
			if (i->op == Ocall)
				emitcallargs(b, i, fn, out);
			emitins(i, fn, out);
		}

		switch (b->jmp.type) {
		case Jjmp:
			emitphis(b, b->s1, fn, out);
			fprintf(out, "    i32.const %d\n",
				(int)fn->nblk - 1 - (int)b->s1->id);
			fputs("    local.set $blk\n"
				"    br $loop\n", out);
			break;
		case Jjnz:
			if (rtype(b->jmp.arg) == RTmp
			&& fn->tmp[b->jmp.arg.val].cls == Kl) {
				emitref(b->jmp.arg, Kl, fn, out);
				fputs("    i64.const 0\n"
					"    i64.ne\n", out);
			} else
				emitref(b->jmp.arg, Kw, fn, out);
			fputs("    (if\n"
				"      (then\n", out);
			emitphis(b, b->s1, fn, out);
			fprintf(out, "        i32.const %d\n",
				(int)fn->nblk - 1 - (int)b->s1->id);
			fputs("        local.set $blk\n"
				"        br $loop)\n"
				"      (else\n", out);
			emitphis(b, b->s2, fn, out);
			fprintf(out, "        i32.const %d\n",
				(int)fn->nblk - 1 - (int)b->s2->id);
			fputs("        local.set $blk\n"
				"        br $loop)\n"
				"    )\n", out);
			break;
		case Jhlt:
			fputs("    unreachable\n", out);
			break;
		default:
			if (isret(b->jmp.type)) {
				if (b->jmp.type != Jret0
				&& !req(b->jmp.arg, R))
					emitref(b->jmp.arg,
						retcls(b->jmp.type),
						fn, out);
				fputs("    return\n", out);
			} else {
				die("unhandled jump %d",
					b->jmp.type);
			}
			break;
		}
	}

	fputs("    ) ;; loop\n"
		"    ) ;; exit\n"
		"    unreachable\n"
		"  )\n", out);
}

static void
databyte(int c)
{
	if (datalen >= DataMax)
		die("data section too large");
	databuf[datalen++] = (uchar)c;
}

void
wasm_emitdat(Dat *d, FILE *out)
{
	char *s;
	int i, len;

	emitmodhdr(out);

	switch (d->type) {
	case DStart:
		if (nseg >= SegMax)
			die("too many data segments");
		curseg = nseg++;
		segs[curseg].name = str(intern(d->name));
		segs[curseg].lnk = *d->lnk;
		segs[curseg].off = dataoff;
		segs[curseg].len = 0;
		segoff = dataoff;
		break;
	case DEnd:
		dataoff = segoff;
		segs[curseg].len = dataoff - segs[curseg].off;
		if (segs[curseg].lnk.export)
			fprintf(out,
				"  (global $%s i32 (i32.const %d))\n"
				"  (export \"%s\" (global $%s))\n",
				sname(d->name), segs[curseg].off,
				sname(d->name), sname(d->name));
		break;
	case DZ:
		for (i=0; i<d->u.num; i++)
			databyte(0);
		segoff += (int)d->u.num;
		break;
	case DB:
		if (d->isstr) {
			s = d->u.str;
			len = (int)strlen(s);
			if (len >= 2 && s[0] == '"'
			&& s[len-1] == '"')
				for (i=1; i<len-1; i++) {
					if (s[i] != '\\'
					|| i+1 >= len-1) {
						databyte(s[i]);
						continue;
					}
					i++;
					switch (s[i]) {
					case 'n':  databyte('\n'); break;
					case 't':  databyte('\t'); break;
					case '0':  databyte(0);    break;
					case '\\': databyte('\\'); break;
					default:   databyte(s[i]);
					}
				}
			segoff = DataBase + datalen;
		} else {
			databyte(d->u.num & 0xff);
			segoff++;
		}
		break;
	case DH:
	case DW:
	case DL:
		len = d->type == DH ? 2
			: d->type == DW ? 4 : 8;
		for (i=0; i<len; i++)
			databyte((d->u.num >> (i*8)) & 0xff);
		segoff += len;
		break;
	}
}

void
wasm_emitfin(FILE *out)
{
	int i;
	uchar c;

	if (!modstarted) {
		fputs("(module)\n", out);
		return;
	}

	if (datalen > 0) {
		fprintf(out, "  (data (i32.const %d) \"", DataBase);
		for (i=0; i<datalen; i++) {
			c = databuf[i];
			if (c >= 0x20 && c < 0x7f
			&& c != '"' && c != '\\')
				fputc(c, out);
			else
				fprintf(out, "\\%02x", c);
		}
		fputs("\")\n", out);
	}
	fputs(")\n", out);

	modstarted = 0;
	datalen = 0;
	dataoff = DataBase;
	nseg = 0;
	curseg = 0;
	segoff = 0;
}
