#include "all.h"

void
mos_isel(Fn *fn)
{
	(void)fn;
	if (strcmp(T.name, "mos_65c02") == 0)
		mosvar = Mos65c02;
	else if (strcmp(T.name, "mos_65816") == 0)
		mosvar = Mos65816;
	else if (strcmp(T.name, "mos_2a07") == 0)
		mosvar = Mos2a07;
	else if (strcmp(T.name, "mos_6510") == 0)
		mosvar = Mos6510;
	else
		mosvar = MosNmos;
}
