# Parser fuzzing

Two optional Clang libFuzzer targets instrument the real library with ASan/UBSan.
Normal builds keep fuzzing disabled and gain no runtime dependency.

- `asmodel-fuzz-json` checks parsing, deep copies, semantic round trips and stable
  serialization, including bounded nesting, integer precision and embedded NULs.
- `asmodel-fuzz-provider` decodes Chat/Responses, SSE, tool proposals and embedding
  batches. It checks bounds and nonnegative usage and releases partial results.
  No connection, model, tool execution or credentials are used.

Use disposable corpus copies so mutation never rewrites the reviewed seeds:

```sh
cmake -B build-fuzz -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DASMODEL_BUILD_TESTS=OFF -DASMODEL_BUILD_FUZZERS=ON
cmake --build build-fuzz -j4
mkdir -p fuzz-run/json fuzz-run/provider fuzz-run/artifacts
cp fuzz/corpus/json/* fuzz-run/json/
cp fuzz/corpus/provider/* fuzz-run/provider/
build-fuzz/asmodel-fuzz-json -seed=1 -max_len=16384 -timeout=5 \
  -max_total_time=30 -rss_limit_mb=512 -artifact_prefix=fuzz-run/artifacts/ fuzz-run/json
build-fuzz/asmodel-fuzz-provider -seed=1 -max_len=16384 -timeout=5 \
  -max_total_time=30 -rss_limit_mb=512 -artifact_prefix=fuzz-run/artifacts/ fuzz-run/provider
```

CI runs both targets and retains failing inputs. Findings must become deterministic
regressions before a fix is accepted; a corpus can also be replayed with `-runs=0`.
The checks call `abort` explicitly, so release builds cannot disable the oracle
through `NDEBUG`. This is parser coverage, not a security audit or provider
conformance campaign. Persistence, OS boundaries and concurrency need separate
targets and fault tests.

The [local run record](fuzzing-local.json) uses Clang 22.1.8, seed 1, 16 KiB inputs
and a requested 30 seconds per target (31 observed). It completed 2,049,489 JSON
and 305,386 provider executions without a finding. Leak detection was disabled
because the outer local sandbox disallows the required inspection. CI keeps its
normal sanitizer behavior; the CI jobs and local LeakSanitizer run remain untested
at this checkpoint. Counts and timing vary with compiler, corpus and machine.
