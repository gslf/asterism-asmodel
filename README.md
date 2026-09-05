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
| `LLAMA_SERVER` | native GBNF / JSON Schema | requests OFF; GBNF constrains emitted text | `cache_prompt` |
| `LMSTUDIO` | Chat Completions JSON Schema | requests OFF; check returned usage | server-managed cache |
| `VLLM` | JSON Schema or `structured_outputs.grammar` | requests OFF; validate the deployed server | server-managed cache |
| `GENERIC` / unresolved `AUTO` | text only | none guaranteed | none guaranteed |

Generation policy is a per-request contract through
`asmodel_generate_params`: `reasoning`, `reasoning_budget`, and
`require_constraint`, and optional `output_schema`. Unsupported combinations return
`ASMODEL_ERR_UNSUPPORTED` before inference; a server-reported length stop
returns `ASMODEL_ERR_LIMIT`. The runtime never retries or silently drops a
constraint. `asmodel_capabilities` and `asmodel_generation_info` expose the
declared profile, applied controls, finish reason, reasoning tokens, and cached
input tokens so callers can verify the contract.

Remote Chat Completions use SSE whenever a progress callback is supplied.
`stream_options.include_usage` requests usage from the server;
`usage_known` indicates whether it was actually returned. Received chunks emit zero-length progress
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

## Accounting contract (ABI 4)

`asmodel_provider_measure_prompt` distinguishes exact, estimated and unavailable
counts. Exact status requires a successful template-aware callback and tokenizer
and chat-template identities. Remote byte heuristics are estimates. Without a
verified tokenizer the admission policy reserves at least UTF-8 bytes plus 256;
this is conservative and uncalibrated, not an exact tokenizer or a universal
upper bound. `result_info` binds diagnostics and usage to one generation while
the provider is locked. A missing usage response must not be interpreted as zero.
Queue/load time is deducted from the request duration before backend dispatch;
generation queue waits observe cancellation and deadlines. Native model loading
itself still runs under the manager lock and cannot yet be interrupted; a backend
must cooperate to bound time spent inside its loader or inference callback.


## Explicit output contracts

`output_schema` is caller-owned JSON Schema. The optional GBNF argument is an
alternative representation of the same application contract. llama-server uses
GBNF when supplied; LM Studio uses JSON Schema; vLLM prefers the supplied schema.
A required constraint with no supported representation fails before dispatch.
The provider never examines grammar productions to identify an action, judge,
classifier or memory operation, and never unwraps or rewrites application JSON.
`result_info.json_output` identifies the selected representation. Applications
must validate and interpret the returned value, including complete-but-invalid
provider responses. This is the output-contract part of the planned IR; role/block
messages, attachments and native tool-call history are not implemented yet.

Remote admission includes the selected schema in its conservative byte estimate.
Requests rejected before HTTP dispatch report known zero consumption. Declared
capability profiles are adapter contracts, not evidence that a particular server,
model and template combination has passed real-provider conformance tests.
