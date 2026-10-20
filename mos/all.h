#include "../all.h"

enum {
	MosNmos,  /* classic NMOS 6502 */
	Mos2a07,  /* Ricoh 2A03/2A07 (NES) — 6502 sans BCD */
	Mos6510,  /* MOS 6510 (C64) — I/O port at $00-$01 */
	Mos65c02, /* WDC 65C02 */
	Mos65816, /* WDC 65816 */
};

extern int mosvar;

void mos_abi(Fn *);
void mos_isel(Fn *);
void mos_emitfn(Fn *, FILE *);
void mos_emitfin(FILE *);
void mos_emitdat(Dat *, FILE *);
