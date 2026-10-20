# MOS 6502 Backend for QBE

QBE backend targeting the MOS 6502 family of processors.
Emits ca65-compatible assembly.

## Targets

| Flag | CPU | Features |
|------|-----|----------|
| `-t mos` | NMOS 6502 | Classic: `(zp),Y`, `JMP` only |
| `-t mos_2a07` | Ricoh 2A07 | NES: `mos` + clear decimal explicitly |
| `-t mos_6510` | MOS 6510 | C64: `mos` + shift zero page by +2 |
| `-t mos_65c02` | WDC 65C02 | Adds `BRA`, `STZ`, `(zp)` indirect |
| `-t mos_65816` | WDC 65816 | Adds `REP`/`SEP` 16-bit A mode |

## Supported

- 32-bit (`Kw`) & 64-bit (`Kl`) integer arithmetic: `add`, `sub`, `neg`
- 32-bit Multiplication/Division: `mul`, `div`, `udiv`, `rem`, `urem`
- Bitwise (`Kw`/`Kl`): `and`, `or`, `xor`
- Shifts (`Kw`/`Kl`): `shl`, `shr`, `sar`
- All 10 comparisons: `ceqw`, `cnew`, `csltw`, `cslew`, `csgtw`, `csgew`, `cultw`, `cugtw`, `culew`, `cugew`
- Negation: `neg`
- Extensions: `extsb`, `extub`, `extsh`, `extuh`, `extsw`, `extuw`
- Control flow: `jmp`, `jnz`, `ret`, `hlt`
- Phi nodes
- `copy`
- `alloc4`, `alloc8`, `alloc16`
- Loads: `loadsb`, `loadub`, `loadsh`, `loaduh`, `loadsw`, `loaduw`, `load`
- Stores: `storeb`, `storeh`, `storew`, `storel`, `stores`, `stored`
- Function calls with absolute parameters count (Stack-based `ZpSp` handling)
- Data sections (`.byte`, `.word`, `.dword`)
- Export/linkage
- **Variadic functions**: Native stack mapping via `Ovastart` and `Ovaarg`
- **Initial Aggregate support**: Memory copy routines (`Oblit0`/`Oblit1`)
- **Returning Aggregate/Structs by value**: Supported (Base Frame Pointer `ZpBp` added for nested returns)

## Not Supported

- **Floating point** (`Ks`, `Kd`): `die()` — 6502 has no FPU (Future emulation stub support pending)
- **64-bit Multiply/Divide/Remainder** (`Kl`): `die()` — cc65 (the main C frontend) does not support 64-bit `long long` types natively, so these are left unimplemented to conserve ROM bounds.
- **Recursive calls**

## Architecture

* `noreg=1`: skips QBE register allocator (6502 has only A/X/Y)
* Zero-page layout:
  - `$00-$01`: Software stack pointer (16-bit, LE)
  - `$02-$09`: Return value / scratch (`ZpRet`)
  - `$0A-$11`: First argument scratch (`ZpArg0`)
  - `$12-$19`: Second argument scratch (`ZpArg1`)
  - `$1A-$1B`: Pointer scratch (`ZpPtr`)
  - `$1C-$1D`: Base Frame Pointer (`ZpBp`)
  - `$1E+`: QBE temporaries (Dynamically allocated; 4 bytes per 32-bit `Kw`, 8 bytes per 64-bit `Kl`)
  - **Note**: The `mos_6510` target shifts all of these up by 2 bytes (starting at `$02` instead of `$00`) to avoid the Commodore 64's I/O port at `$00-$01`.
- Runtime helpers emitted on demand: `__mul32`, `__div32`, `__udiv32`, `__mod32`, `__umod32`
- 65816 variant uses 16-bit accumulator mode for ~2× faster 32-bit math

## Usage

Use the `-t` flag to select the target chip:

```sh
# NMOS 6502 (classic NES, C64, Apple II, Atari)
./qbe -t mos input.ssa > output.s

# Ricoh 2A07 (PAL NES with explicit cld)
./qbe -t mos_2a07 input.ssa > output.s

# MOS 6510 (C64 with zero-page shifted)
./qbe -t mos_6510 input.ssa > output.s

# WDC 65C02 (Apple IIe enhanced, Commander X16)
./qbe -t mos_65c02 input.ssa > output.s

# WDC 65816 (SNES, Apple IIGS)
./qbe -t mos_65816 input.ssa > output.s
```

Then assemble and link with cc65 tools:

```sh
ca65 -t none output.s -o output.o
ld65 -C config.cfg output.o -o output.bin
```

## Testing

```sh
make check-mos   # runs all upstream + wasm + mos tests
```

Requires cc65 toolchain (`ca65`, `ld65`, `sim65`).
The MOS suite runs 43 test programs across all 5 chip variants
(165 passing, 42 assembly-only on 65816 where sim65 cannot simulate).
