#include "../all.h"

void wasm_abi(Fn *);
void wasm_isel(Fn *);
void wasm_emitfn(Fn *, FILE *);
void wasm_emitfin(FILE *);
void wasm_emitdat(Dat *, FILE *);
