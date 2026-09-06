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

The standalone Linux CI runs GCC and Clang with ASan/UBSan. It explicitly builds
`asmodel-openai-smoke` so missing Python/libcurl cannot silently remove the HTTP
contract suite. These are scripted provider checks, not real-model evaluation.

See `include/asmodel.h` for the versioned C API.

## Accounting contract (ABI 8)

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

Generation and embedding parameters optionally carry a borrowed `request_id`
(1–128 printable ASCII bytes, excluding spaces). The manager validates it before
loading or queueing and passes it unchanged to a host-supplied provider, including
failed calls. Built-in HTTP adapters do not serialize it. This lets an embedding
host correlate local traces and accounting without a shared "last request" slot.
It does not deduplicate calls, authorize execution or provide an idempotency key.
Hosts must choose identities meaningful within their own trace namespace.


## Explicit output contracts

`output_schema` is caller-owned JSON Schema. The optional GBNF argument is an
alternative representation of the same application contract. llama-server uses
GBNF when supplied; LM Studio uses JSON Schema; vLLM prefers the supplied schema.
A required constraint with no supported representation fails before dispatch.
The provider never examines grammar productions to identify an action, judge,
classifier or memory operation, and never unwraps or rewrites application JSON.
`result_info.json_output` identifies the selected representation. Applications
must validate and interpret the returned value, including complete-but-invalid
provider responses. Role/block messages and native tool contracts are described
in [the input contract](docs/input.md). Attachments are not implemented yet.

Remote admission includes the selected schema in its conservative byte estimate.
Requests rejected before HTTP dispatch report known zero consumption. Declared
capability profiles are adapter contracts, not evidence that a particular server,
model and template combination has passed real-provider conformance tests.

## Structured transport validation

`asmodel_json.h` exposes the strict JSON codec used by the providers and engine
clients. The implementation separates parsing, value ownership, serialization
and byte/Unicode helpers; applications do not need a second copy of the codec.
OpenAI-shaped responses use structural field lookup, bounded integer usage and
explicit choice/vector indices. Duplicate keys, embedded NUL in output text,
non-finite vectors, wrong dimensions and incomplete SSE streams are rejected.
A provider name alone still does not establish real-model conformance.

## Embedding batches and pipeline identity

`asmodel_embed` accepts 1–256 texts and an explicit output capacity in floats.
`asmodel_embed_params` carries cancellation, a total request duration and a
per-call receipt. `completed` counts valid leading vectors, including partial
batches; successful HTTP alone cannot set it. Non-finite, zero-length, missing,
duplicate-index and wrong-dimension vectors fail validation. Every returned row
is L2-normalized. Input is never silently truncated. Unknown consumption stays
unknown after interruptions; usage is bound to the request.

The manager applies `spec.pipeline.query_prefix` or `document_prefix` exactly
once before both remote and native providers. Pipeline fields are immutable
copies. `asmodel_manager_embedding_key` returns a canonical JSON description
covering revision, tokenizer, pooling, prefixes, dimension, context and adapter.
Missing revision/tokenizer/pooling returns `UNSUPPORTED`: applications must not
reuse persistent vectors across restarts. Revision metadata is an operator
attestation; the runtime cannot detect a server secretly replacing weights.
Native hosts derive revision/tokenizer from the GGUF hash and declare pooling.

Remote batches use one array request; embedded adapters may evaluate rows
sequentially under one model context. Cancellable manager waits and remaining
durations apply to embedding calls too. Native callbacks cooperate through the
ggml abort hook. Model loading and synchronous WinHTTP I/O remain cooperation
limits; this is not a hard real-time guarantee. Conservative remote admission
uses UTF-8 bytes plus overhead until a verified tokenizer is available.

## Request ownership

Generation requires a positive output budget. The manager always supplies a
request-local receipt to the provider, even when its caller does not request one.
There is no provider-global last-result accessor. Timeout, cancellation and
backend errors can return validated partial text; callers must inspect the status
before treating it as a completed answer. Missing or interrupted usage stays
unknown. Transport progress callbacks may carry an empty fragment.

Queue time and request preparation consume the same deadline as inference.
Native synchronous loading and Windows HTTP cancellation remain backend limits.
Protocol mock tests exercise contracts, not real-model capability conformance.
