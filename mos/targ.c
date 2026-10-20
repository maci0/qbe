#include "all.h"

int mosvar;

Target T_mos = {
		.name = "mos",
		.noreg = 1,
		.abi0 = elimsb,
		.abi1 = mos_abi,
		.isel = mos_isel,
		.emitfn = mos_emitfn,
		.emitfin = mos_emitfin,
		.emitdat = mos_emitdat,
		.asloc = "$",
		.assym = "$",
		.cansel = 0,
};

Target T_mos_2a07 = {
		.name = "mos_2a07",
		.noreg = 1,
		.abi0 = elimsb,
		.abi1 = mos_abi,
		.isel = mos_isel,
		.emitfn = mos_emitfn,
		.emitfin = mos_emitfin,
		.emitdat = mos_emitdat,
		.asloc = "$",
		.assym = "$",
		.cansel = 0,
};

Target T_mos_6510 = {
		.name = "mos_6510",
		.noreg = 1,
		.abi0 = elimsb,
		.abi1 = mos_abi,
		.isel = mos_isel,
		.emitfn = mos_emitfn,
		.emitfin = mos_emitfin,
		.emitdat = mos_emitdat,
		.asloc = "$",
		.assym = "$",
		.cansel = 0,
};

Target T_mos_65c02 = {
		.name = "mos_65c02",
		.noreg = 1,
		.abi0 = elimsb,
		.abi1 = mos_abi,
		.isel = mos_isel,
		.emitfn = mos_emitfn,
		.emitfin = mos_emitfin,
		.emitdat = mos_emitdat,
		.asloc = "$",
		.assym = "$",
		.cansel = 0,
};

Target T_mos_65816 = {
		.name = "mos_65816",
		.noreg = 1,
		.abi0 = elimsb,
		.abi1 = mos_abi,
		.isel = mos_isel,
		.emitfn = mos_emitfn,
		.emitfin = mos_emitfin,
		.emitdat = mos_emitdat,
		.asloc = "$",
		.assym = "$",
		.cansel = 0,
};
