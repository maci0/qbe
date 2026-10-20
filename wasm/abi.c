#include "all.h"

void
wasm_abi(Fn *fn)
{
	Blk *b;
	Ins *i;

	for (b=fn->start; b; b=b->link)
		for (i=b->ins; i<&b->ins[b->nins]; i++) {
			switch (i->op) {
			case Ovastart:
			case Ovaarg:
				die("wasm: variadic functions "
					"not supported");
				break;
			case Oargc:
			case Oparc:
			case Opare:
				die("wasm: aggregate arguments "
					"not supported");
				break;
			}
		}
}
