#include "all.h"

typedef struct Insl Insl;

struct Insl {
	Ins i;
	Insl *link;
};

static void
stkblob(Ref r, Typ *t, Fn *fn, Insl **ilp)
{
	Insl *il;
	uint64_t sz;
	il = alloc(sizeof *il);
	sz = (t->size + 3) & ~3; /* align to 4 bytes */
	il->i = (Ins){Oalloc, Kl, r, {getcon(sz, fn)}};
	il->link = *ilp;
	*ilp = il;
}

void
mos_abi(Fn *fn)
{
	Blk *b;
	Ins *i, *i0, *ia;
	Insl *il;
	Ins call_ins;
	int j, retty;

	/* Reject float returns */
	for (b=fn->start; b; b=b->link) {
		j = b->jmp.type;
		if (j == Jretd || j == Jrets) {
			die("mos: float return not supported");
		}
		for (i=b->ins; i<&b->ins[b->nins]; i++) {
			if (i->cls == Kd || i->cls == Ks) {
				if (i->op != Onop && i->op != Odbgloc && !ispar(i->op) &&
						!isarg(i->op) && i->op != Ocopy)
					die("mos: floating point not supported");
			}
		}
	}

	/* Rewrite instructions */
	for (b=fn->start; b; b=b->link) {
		b->visit = 0;
	}

	b = fn->start;
	il = 0;
	do {
		if (!(b = b->link))
			b = fn->start;
		if (b->visit)
			continue;
		b->visit = 1;

		curi = &insb[NIns];

		/* Lower Jretc struct returns to pointers */
		j = b->jmp.type;
		if (j == Jretc) {
			if (fn->retty >= 0) {
				if (req(fn->retr, R)) {
					fn->retr = newtmp("abi", Kl, fn);
				}
				emit(Oblit1, 0, R, INT(typ[fn->retty].size), R);
				emit(Oblit0, 0, R, b->jmp.arg, fn->retr);
			}
			b->jmp.type = Jret0;
		}

		for (i=&b->ins[b->nins]; i!=b->ins;) {
			switch ((--i)->op) {
			default:
				emiti(*i);
				break;
			case Ocall:
				for (i0=i; i0>b->ins; i0--) {
					if (!isarg((i0-1)->op))
						break;
				}
				if (!req(i->arg[1], R)) {
					retty = i->arg[1].val;
					stkblob(i->to, &typ[retty], fn, &il);

					call_ins = *i;
					call_ins.to = R;
					call_ins.arg[1] = R;
					emiti(call_ins);

					/* re-emit existing arguments */
					for (ia=i-1; ia>=i0; ia--) {
						emiti(*ia);
					}

					/* emit hidden pointer arg last (executed first) */
					emit(Oarg, Kl, R, i->to, R);
				} else {
					emiti(*i);
					for (ia=i-1; ia>=i0; ia--) {
						emiti(*ia);
					}
				}
				i = i0;
				break;
			case Odbgloc:
				emiti(*i);
				break;
			}
		}

		/* If start block, receive hidden struct pointer parameter */
		if (b == fn->start && fn->retty >= 0) {
			if (req(fn->retr, R)) {
				fn->retr = newtmp("abi", Kl, fn);
			}
			emit(Opar, Kl, fn->retr, TYPE(fn->retty), R);
		}

		if (b == fn->start) {
			for (; il; il=il->link)
				emiti(il->i);
		}

		idup(b, curi, &insb[NIns]-curi);
	} while (b != fn->start);

	if (debug['A']) {
		fprintf(stderr, "\n> After ABI lowering:\n");
		printfn(fn, stderr);
	}
}
