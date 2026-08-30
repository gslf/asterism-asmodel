# asmodel

`asmodel` is the process-wide model runtime for Asterism components. It owns
loaded provider instances and their reusable contexts, applies resident/RAM/
VRAM budgets, performs ordered warm-up and LRU/idle eviction, and exposes one
API for generation, embeddings, tokenization and context/KV reuse.

Architecture and design: [docs/SPECS.md](docs/SPECS.md).

The core is provider-neutral. An embedding host registers a loader for local
backends such as llama.cpp. The built-in remote provider uses explicit,
versioned protocol profiles instead of treating every OpenAI-shaped server as
the same implementation:

| `remote_provider` | Constrained output | Per-call reasoning control | Reuse |
|---|---|---|---|
| `LLAMA_SERVER` | native GBNF / JSON Schema | guaranteed OFF; a GBNF micro-pass cannot emit reasoning | `cache_prompt` |
| `LMSTUDIO` | Chat Completions JSON Schema | guaranteed OFF on the verified path | server-managed cache |
| `VLLM` | JSON Schema or `structured_outputs.grammar` | guaranteed OFF on current protocol | server-managed cache |
| `GENERIC` / unresolved `AUTO` | text only | none guaranteed | none guaranteed |

Generation policy is a per-request contract through
`asmodel_generate_params`: `reasoning`, `reasoning_budget`, and
`require_constraint`. Unsupported combinations return
`ASMODEL_ERR_UNSUPPORTED` before inference; a server-reported length stop
returns `ASMODEL_ERR_LIMIT`. The runtime never retries or silently drops a
constraint. `asmodel_capabilities` and `asmodel_generation_info` expose the
declared profile, applied controls, finish reason, reasoning tokens, and cached
input tokens so callers can verify the contract.

Remote Chat Completions use SSE whenever a progress callback is supplied.
`stream_options.include_usage` preserves exact accounting across LM Studio,
llama.cpp server, and vLLM; received chunks emit zero-length progress
heartbeats without leaking reasoning text. `deadline_ms` is a per-request wall
duration, not a fixed transport timeout: zero leaves inference unbounded and a
positive expiry returns `ASMODEL_ERR_TIMEOUT`. Connection establishment keeps
its own short timeout. The provider never retries an inference implicitly.

Integrations must set `remote_provider` explicitly and choose reasoning per
call. Transport uses WinHTTP on Windows and libcurl when available elsewhere.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

See `include/asmodel.h` for the stable C API.
