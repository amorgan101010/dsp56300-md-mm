# MD/MM JIT first-use latency

New DSP modes previously initialized dense metadata and dispatch tables on first use. With the MD/MM 0x800000-word program address space, each flat function-pointer table occupies 64 MiB on a 64-bit host. Repeated table construction contributed large first-note audio callbacks.

JIT metadata now uses typed sparse pages. Reading an untouched address returns an immutable empty entry; mutation constructs one page while preserving existing references. Destruction visits allocated entries. Generated code continues to use a flat dispatch table.

On macOS, each DSP prepares one complete anonymous dispatch template before rendering. New modes obtain private copy-on-write mappings with `mach_vm_remap`. Modified pages become private; clones own their mappings independently of the template. The implementation uses whole allocations, with no fixed-address replacement or partial unmapping. The old disabled MMU path remains disabled. Other platforms and failed mappings use the existing vector-backed dispatch fallback.

For the two MD/MM DSPs this adds roughly 128 MiB at preparation. Shared untouched pages can reduce memory after multiple modes become active. JIT compilation, small metadata allocations and page faults can still occur during rendering; this change does not guarantee every cold callback meets a small-buffer deadline.

Build `pagedArrayTest`, `cowMemoryTest` and `dsp56kTestRunner`, then run:

```sh
ctest --test-dir build -C Release --output-on-failure \
  --tests-regex '^(pagedArrayTest|cowMemoryTest|dsp56300_unitTests)$'
```

The page test covers holes, high addresses, stable references and noncopyable object destruction. The macOS COW test covers callable pointers, sibling isolation, template-before-clone destruction, arbitrary clone teardown, bounded physical commitment and fallback. It returns the configured skip code on other platforms; the DSP suite exercises their dispatch fallback. The repository CI builds/tests Linux, macOS and Windows.

This dependency change must merge into `release/md-mm-public-alpha-20260826` before the linked Gearmulator integration PR adopts the resulting merged release pin. A feature-branch pin used for draft validation is provisional.
