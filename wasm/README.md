# WebAssembly Backend for QBE

QBE backend targeting WebAssembly.
Emits WAT (WebAssembly Text Format) output.

## Targets

| Flag | Output |
|------|--------|
| `-t wasm` | WAT text format (`.wat`) |

## Supported

- 32-bit (`Kw`) & 64-bit (`Kl`) integer arithmetic: `add`, `sub`, `mul`, `div`, `udiv`, `rem`, `urem`
- Bitwise (`Kw`/`Kl`): `and`, `or`, `xor`
- Shifts (`Kw`/`Kl`): `shl`, `shr`, `sar`
- All 10 comparisons for each width: `ceqw`..`cugew`, `ceql`..`cugel`
- Negation: `neg`
- Extensions: `extsb`, `extub`, `extsh`, `extuh`, `extsw`, `extuw`
- Floating point (`Ks`, `Kd`): full arithmetic, comparisons, conversions
- Casts: `cast` (bitwise reinterpret between int/float)
- Control flow: `jmp`, `jnz`, `ret`, `hlt`
- Phi nodes (with swap locals for parallel assignment)
- `copy`
- `alloc4`, `alloc8`, `alloc16`
- Loads: `loadsb`, `loadub`, `loadsh`, `loaduh`, `loadsw`, `loaduw`, `load`
- Stores: `storeb`, `storeh`, `storew`, `storel`, `stores`, `stored`
- Function calls with typed parameters and results
- Data sections with forward reference support
- Export/linkage

## Not Supported

- **Variadic functions** (`Ovastart`/`Ovaarg`): `die()` — no standard WASM variadic ABI
- **Aggregate arguments** (`Oargc`/`Oparc`/`Opare`): `die()` — no struct passing
- **Block memory copies** (`Oblit0`/`Oblit1`): not implemented

## Architecture

* `noreg=1`: skips QBE register allocator (WASM is a stack machine with typed locals)
* Control flow: structured via `block`/`loop`/`br_table` dispatch
  - Each QBE basic block becomes a `br_table` case
  - Jumps are implemented as `local.set $blk` + `br $loop`
* Memory model:
  - Linear memory: 1 page (64KB), exported as `"memory"`
  - Stack pointer: `$__stack_pointer` mutable global, starts at 65536, grows downward
  - Data section: starts at offset 1024, emitted as a single `(data ...)` segment
* Locals:
  - `$blk`: block dispatch index (i32)
  - `$sw_w`, `$sw_l`, `$sw_s`, `$sw_d`: phi swap temporaries (one per type)
  - `$t0`..`$tN`: QBE temporaries, typed per use (i32/i64/f32/f64)
* Functions are emitted inline in the module with `(func ...)` declarations
* Called functions are emitted as `call $name` (caller must ensure all callees are defined in the same module)

## Usage

```sh
./qbe -t wasm input.ssa > output.wat
```

To validate or run with wasmtime:

```sh
# Validate
wasmtime compile output.wat

# Run assertions (.wast format)
wasmtime wast output.wast
```

To compile to native via a host C compiler (for testing):

```sh
# See test/wasm/run_native.sh for the full pipeline
```

## Testing

```sh
make check-wasm         # WAT validation tests (requires wasmtime)
make check-wasm-native  # Native execution tests (requires host CC + wasmtime)
```
