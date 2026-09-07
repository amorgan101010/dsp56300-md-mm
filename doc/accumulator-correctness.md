# Accumulator representation regressions

DSP accumulators occupy bits 63..8 of host storage. Raw 24-bit fields and 48-bit memory operands do not use that representation. Confusing the two caused x86 EFM-SD phase stores to lose eight bits and exposed additional condition-flag errors.

The fixes cover raw A10/B10 loads and stores, shift carry, rotation sign/zero flags, sticky overflow, signed boundary arithmetic, interpreter extension detection, and saturating long-memory transfers. Four focused fixes are reused from the firmware-hook work via commits that retain their original attribution; the broader firmware-hook changes are not included.

Instruction expectations follow the [DSP56300 Family Manual, revision 5](https://www.nxp.com/docs/en/reference-manual/DSP56300FM.pdf), particularly status definitions in section 5.4 and ABS, ASL, ASR, DEC, INC, long-memory moves, NEG, ROL and ROR in section 13. The interpreter is corrected where it disagreed with those definitions; it is not treated as an infallible oracle.

`dsp56300_accumulatorTests` adds 54 explicit expected-value cases and 7,680 deterministic interpreter/JIT comparisons. It exercises both accumulators, partial registers, raw and saturating transfers, zero/immediate/variable shifts, nonzero initial flags, three scaling modes, boundary values and seeded random values. It checks resulting registers, status and addressed memory. Failures include the instruction and initial state.

Run the complete existing suite and the new regression test with:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --config Release --parallel 3
ctest --test-dir build -C Release --output-on-failure
```

The existing CI matrix runs these tests on ARM64 macOS, x86-64 Linux and x86-64 Windows. No firmware, sound device, GUI or tester recordings are required. This is targeted coverage of the reproduced defects, not exhaustive verification of the instruction set or all instruction sequences.

## Follow-up review and sequence coverage

The September 6 follow-up checks the implementation against the manual independently of interpreter/JIT agreement:

| Correction | Architectural basis |
| --- | --- |
| Raw A10/B10 transfers | Sections 3.2.1 and 13, long-memory moves: concatenate A1:A0 or B1:B0; preserve the extension on raw loads. Host alignment is not part of the memory value. |
| Saturating A/B long stores | Section 3.1.6 and the long-memory instruction: scale, then limit to signed 48-bit range, without changing the source accumulator. Scaling in the right-aligned 56-bit domain prevents host overflow before limiting. |
| ASL/ASR carry | Instruction definitions specify the last discarded accumulator bit and C=0 for zero shifts. ASL overflow also sets sticky L. |
| ROL/ROR and logical shifts | These operate on bits 47..24; N comes from bit 47, Z from the 24-bit result, and other accumulator fields survive. |
| ABS/NEG/INC/DEC boundaries | Their full 56-bit results and standard V/L definitions require overflow at the signed minimum/maximum boundaries. ABS/NEG preserve C; INC/DEC update it for full-width carry/borrow. Unsigned host arithmetic defines wraparound. |
| Scale-up E | Section 5.4: the signed integer portion includes bit 55 in every supported scaling mode. |

The new 488 sequence cases comprise 104 explicit branch outcomes and 384 deterministic 24-instruction interpreter/JIT comparisons. They run with multi-instruction blocks, block linking, and the optimizer both enabled and disabled. Programs include parallel accumulator stores, partial loads, cross-accumulator shifts, sticky flags and branches. Three supported scaling modes and nonzero initial CCR values are included.

This exposed an additional interpreter defect: logical shifts and AND/OR/EOR/NOT wrote N while preceding arithmetic still had a deferred N update. Reading CCR later reinstated the stale arithmetic sign. Materializing preceding flags before these logical operations preserves E/U and allows the logical result to replace N. CLR also resolves preceding flags before replacing E/U/N. Explicit arithmetic/logical/branch programs protect the group; adding the six AND/OR/EOR/NOT/CLR cases before the correction produced 12 interpreter failures (both optimizer configurations), while the JIT passed.

As a negative control, the same expanded tests were run with the archived core built from alpha.10's DSP revision `8c919d2b`: 18 explicit cases fail on ARM and 30 on x86, including the x86 raw transfers. The differential sweep reports 1,565 ARM and 2,636 x86 mismatches; the sequence suite also rejects that baseline. With the corrections, all three groups pass locally on both architectures. These failures demonstrate detection of the original defects, not a count of independent bugs.

Coverage remains bounded: reserved scaling combinations, arithmetic saturation/16-bit modes, every opcode and arbitrary instruction sequences are not exhaustively verified.

Explicit INC/DEC cases also cross bit 47 without crossing bit 55, proving that a field-boundary transition is not a full-accumulator carry or borrow. A rotate/branch case independently checks ROL negative-flag behavior.

## September 7: DIV overflow condition across host flag updates

Tracing the first remaining ARM/x86 register difference located it in MD's mixer
DIV loop. The accumulator, carry and cycle counts agreed, but x86 sometimes
reported V=1 where ARM reported V=0. The x86 V/L helper cleared the emulated V
with a host AND before consuming the arithmetic condition. That AND overwrote
host parity/zero flags, so the subsequent condition could describe SR rather
than the arithmetic result. Capturing the condition before clearing V fixes it.
The same helper serves standalone and repeated DIV and other overflow updates.

The manual's DIV condition-code definition (section 13, page 13-54) requires V
from the change in the accumulator MSB during the left shift, C from the final
sign, sticky L, and preservation of E/U/N/Z. A new independent integer oracle
checks 1,168 results across both accumulators, both execution engines and both
optimizer settings. This includes 24-iteration sequences with the captured
firmware operands, positive/negative divisors and initially set flags. Two
single-iteration cases assert defined V/L behavior outside the valid quotient
range, without claiming a valid quotient for those operands.

Before the correction, the 1,152 valid-sequence checks produced 352 x86 failures
and zero ARM failures. All 1,168 final checks pass locally on both architectures,
as do the existing explicit, differential and sequence groups. The phase drift
itself was separately traced to a host scheduler deadline conversion, addressed
in the plugin integration PR rather than this DSP dependency.

## Manual boundary and execution-context validation

`manualAccumulatorTests.cpp` adds 13,664 independent expected-state checks:
7,680 DIV contexts, 64 reduced status-bit branch cases, 4,224 arithmetic/shift
boundaries, 160 rotates and 1,536 full-accumulator long stores. DIV runs both
accumulators with optimizer on/off, one-instruction blocks, an unrolled block,
and REP. No intermediate SR reads occur; final registers/SR and an immediately
following branch are checked. The unrolled JIT case also checks that the first
advancing dispatch reaches beyond the DIV sequence. Cases include pending
flags from preceding TST, both divisor signs, incoming C/V/L combinations and
single-step overflow probes outside the valid quotient range.

Boundary oracles use raw DSP bit positions and repeated one-bit shifts, not
interpreter/JIT arithmetic helpers. They check every defined CCR flag, sticky
S/L, ignored upper shift-control bits, preserved accumulator fields, unchanged
source/unrelated registers and exact memory values. Transfer cases cover both
sides of positive/negative limiting thresholds in all three scaling modes and
growth detected in either accumulator. They do not cover arithmetic-saturation
or 16-bit arithmetic modes.

This exposed two shared backend omissions beyond the original cross-CPU fixes:

- JSET/JCLR reading SR inside the conditional helper could materialize pending
  flags but save them only on the taken path. Reading the operand before entering
  that helper preserves the flags on both paths. `TST; JSET #1,SR,...` reproduces
  this without DIV; the relative bit-branch helper already reads outside its
  conditional helper.
- Full A/B long stores omitted the sticky S growth detector specified by table
  5-1 (page 5-14), independently of transfer limiting/L. The interpreter and both
  JITs now evaluate it before scaling. Raw A10/B10 moves retain their raw behavior.
  JIT evaluation precedes output-temp allocation to stay within x86's three-temp
  budget. This is targeted long-store coverage, not an audit of every S-affecting
  instruction.

A focused UBSan build also found full-SR reset masks cast to the narrower CCR
mask enum, and signed left shifts in interpreter DIV/ASL. Reset uses SRMask;
DIV/ASL wrap with unsigned host arithmetic. DIV divisor alignment and remainder
addition/subtraction likewise use unsigned arithmetic. All 4,256 focused
interpreter cases then pass with `-fsanitize=undefined
-fno-sanitize-recover=undefined`. This instruments host C++, not generated JIT
machine code. The interpreter-only invocation is:

```sh
dsp56kAccumulatorTests --manual-interpreter-only
```

Use `--manual-only` for just the new checks on both execution engines; the
normal invocation includes them along with all prior accumulator regressions.
The existing CI target therefore picks up the new coverage automatically.

The AppleClang 15 CI job exposed incorrect E expectations from the oracle's
per-bit boolean reduction for some negative results; both emulators returned
the manual-correct flag. The oracle now tests whether the raw integer portion
is all zero or all one, and six literal flag expectations independently check
that calculation before any DSP execution. Local AppleClang 16 did not
reproduce the older-toolchain failure. This follow-up changes tests only.
