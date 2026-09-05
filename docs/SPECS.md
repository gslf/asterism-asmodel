# asmodel — Architecture and Design

## 1. Why this project exists

Asterism uses several models for different jobs: routing, planning, generation,
memory curation and embeddings. Loading and controlling those models separately
inside every component would duplicate weights, waste RAM and VRAM, fragment KV
caches and make provider behavior inconsistent.

`asmodel` is the process-wide model runtime that removes that duplication. It
gives every Asterism component one contract for generation, embeddings,
tokenization and model residency, whether inference runs in-process or behind a
local HTTP server.

Its central idea is simple:

> Model execution is shared infrastructure. Provider differences must end at
> the runtime boundary, not leak into the agent, memory or tool layers.

## 2. Place in Asterism

The four projects divide responsibilities as follows:

- **asmodel** owns model instances, provider protocols and inference resources.
- **Asper** owns durable memory and bounded context materialization.
- **astools** owns tool contracts, validation, permissions and execution.
- **asngn** orchestrates turns and combines the other three components.

Both asngn and embedded Asper can borrow the same `asmodel_manager`. A curator,
an embedder and an answer generator therefore share one residency policy rather
than independently loading whatever they need.

## 3. What asmodel owns

The runtime is responsible for:

- registering model definitions and their provider-specific loaders;
- loading, warming, retaining and evicting model instances;
- enforcing resident-model, RAM and VRAM budgets;
- exposing generation, embedding and exact token-count operations;
- preserving reusable provider contexts and prompt/KV caches;
- applying structured-output and reasoning controls;
- reporting why generation stopped and how many tokens it consumed;
- propagating progress, cancellation, limits and transport failures precisely.

It does not plan agent actions, build prompts, store conversations or execute
tools. Those policies belong to asngn, Asper and astools.

## 4. Runtime model

An application creates one manager and registers a pool of model definitions.
Each definition identifies a role-capable model and either:

- an embedded loader supplied by the host, such as the llama.cpp adapter; or
- a built-in remote profile for a supported HTTP inference server.

The hot path is:

1. The caller selects a registered model and submits a typed request.
2. The manager makes the provider resident if necessary.
3. The provider applies the requested reasoning and output constraints.
4. Tokens are streamed through the progress callback when requested.
5. The manager returns output, finish reason and usage as one result.
6. The instance remains reusable until budgets or idleness require eviction.

Model loading and eviction are ordered. The manager never evicts an instance
that is in use, and least-recently-used idle instances are the first candidates
when a resource budget is exceeded.

## 5. Provider compatibility is an explicit contract

An OpenAI-shaped endpoint is not a complete protocol specification. llama.cpp
server, LM Studio and vLLM differ in structured-output fields, reasoning
controls, streaming details and cache reporting. Pretending they are identical
creates silent quality failures.

`asmodel` therefore uses explicit remote profiles:

| Profile | Structured output | Reasoning control | Prompt reuse |
|---|---|---|---|
| llama.cpp server | Native GBNF and JSON Schema | Requests reasoning off; validates available usage | `cache_prompt` |
| LM Studio | Chat Completions JSON Schema | Disabled on the verified profile | Server-managed cache |
| vLLM | JSON Schema and grammar through `structured_outputs` | Disabled on the verified profile | Server-managed cache |
| Generic | Plain text only | No guarantee | No guarantee |

The embedded path receives the same semantic request through the host loader,
so llama.cpp in-process and remote servers remain interchangeable at the
manager API rather than at the wire-format level.

Provider selection is explicit. A capability is either declared and applied or
the request fails before inference with `ASMODEL_ERR_UNSUPPORTED`. The runtime
does not silently remove a grammar, enable hidden reasoning or substitute an
unverified request shape.

## 6. Generation contract

Every generation request independently specifies:

- the output-token ceiling;
- whether reasoning is allowed and, where supported, its budget;
- an optional grammar or JSON Schema constraint;
- whether that constraint is mandatory;
- an optional wall-clock deadline;
- progress and cancellation callbacks.

A zero deadline means no generation deadline. Slow local inference is not a
failure by itself; the user or caller can cancel it. Connection establishment
has a separate short timeout so an unreachable server does not block forever.

When a backend reaches the output-token ceiling, the runtime returns
`ASMODEL_ERR_LIMIT` together with every decoded partial byte and a length finish
reason. It never retries the request. This distinction lets the caller continue
useful work instead of paying again for the same prefix.

Streaming transports emit progress heartbeats even when a server chunk contains
no user-visible text. Reasoning text is not leaked through the answer callback.
Usage metadata includes prompt, generated, reasoning and cached-input tokens
when the provider reports them.

## 7. Performance strategy

The most expensive local-model operations are weight loading, memory transfer,
prompt ingestion and repeated prefix evaluation. asmodel attacks those costs at
their source:

- **Shared residency:** one loaded instance serves all borrowing components.
- **Resource budgets:** resident count, RAM and VRAM limits prevent accidental
  overcommit while keeping the most useful models warm.
- **Ordered warm-up:** high-value roles can be made ready before the first turn.
- **LRU and idle eviction:** cold instances leave before active or recently used
  ones.
- **Context reuse:** embedded contexts and provider prompt caches survive across
  compatible requests.
- **Streaming:** callers receive useful output immediately and can stop work that
  is no longer needed.
- **No implicit retries:** transport and model failures remain visible and do not
  multiply latency or compute behind the caller's back.

These policies are centralized so memory curation and agent generation cannot
independently make conflicting residency decisions.

## 8. Token economy without quality loss

Token economy here means removing duplicated computation, not shortening useful
answers or weakening prompts.

- Exact tokenization lets upper layers budget against the consuming model rather
  than a rough global estimate.
- Prompt/KV reuse avoids re-evaluating unchanged prefixes.
- Constrained micro-passes produce only the small decision object the caller
  needs; they do not spend tokens explaining syntax already enforced by a
  grammar.
- Reasoning is disabled for deterministic routing and protocol decisions, while
  remaining available to tasks that benefit from it.
- Partial output is returned at limits, allowing continuation instead of a full
  retry.
- Exact usage and finish metadata let asngn measure savings and detect a genuine
  budget problem instead of guessing.

No automatic fallback is allowed to trade away a required constraint. Saving
tokens is valid only when the observable task contract remains intact.

## 9. How this helps small language models

Small language models are especially sensitive to prompt noise, ambiguous
output formats and limited compute. asmodel improves their effective ability by
making the inference environment predictable:

- grammar-constrained outputs remove invalid action syntax from the search
  space;
- per-task reasoning policy reserves reasoning tokens for work that needs them;
- exact context accounting prevents accidental truncation of instructions;
- cached prefixes leave more compute for new information;
- streaming cancellation avoids wasting a limited machine on a wrong branch;
- capability checks prevent a provider mismatch from masquerading as model
  incompetence.

The runtime does not make a weak model know more. It removes infrastructure
failure modes that would otherwise consume the model's limited capacity.

## 10. Reliability and observability

The public contract distinguishes invalid requests, unsupported capabilities,
token limits, cancellation, timeouts, transport failures and model failures.
Callers can therefore choose the correct recovery instead of retrying every
error.

Capability queries expose what a registered provider can guarantee. Generation
info exposes what was actually applied, the finish reason and usage. Manager
statistics expose residency and resource behavior. These are correctness data,
not optional debug decoration.

Embedded backends are isolated behind a C-facing loader contract. Remote
transport uses WinHTTP on Windows and libcurl where available elsewhere.

## 11. Fundamental invariants

The implementation must preserve these rules:

1. There is one process-wide owner for reusable model resources.
2. Provider-specific wire behavior never escapes the provider boundary.
3. Required constraints and reasoning controls are never silently discarded.
4. A token-limit result preserves all valid partial output.
5. Inference is never retried implicitly.
6. A zero generation deadline is unbounded and cancellation remains available.
7. Usage and finish metadata describe the operation that actually ran.
8. Resource eviction never invalidates an active request.

## 12. Public surface

`include/asmodel.h` is the authoritative C99 API. It covers manager lifecycle,
model registration, warm-up and eviction, generation, embeddings, token counts,
capabilities, generation metadata and statistics.

Configuration examples and build instructions belong in the repository README.
Provider additions belong behind a new explicit profile with capability tests;
they must not broaden the generic profile by assumption.


### Output contract ownership (ABI 4)

Callers supply `output_schema` explicitly alongside any alternative free GBNF.
The provider chooses a supported encoding and reports `json_output`; it does not
infer semantics from grammar text or normalize application objects. Asngn owns
its action/classification/judge contracts. Asper owns curation/review/recall
contracts. Tool argument schemas come from Astools' typed manifests. Complete
JSON values are validated at their owning application boundary. Multimodal
messages and native tool-call history remain future parts of the request IR.
