#include "all.h"

Target T_wasm = {
	.name = "wasm",
	.noreg = 1,
	.abi0 = elimsb,
	.abi1 = wasm_abi,
	.isel = wasm_isel,
	.emitfn = wasm_emitfn,
	.emitfin = wasm_emitfin,
	.emitdat = wasm_emitdat,
	.asloc = "$",
	.assym = "$",
	.cansel = 0,
};
