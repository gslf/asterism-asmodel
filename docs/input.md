# Messages and native tool contracts (ABI 7)

Generation and prompt counting accept `asmodel_input`, a borrowed immutable
sequence of roles and content blocks. System/developer instructions precede the
conversation. Text blocks join within their original role. Tool arguments are
JSON objects; results carry their matching call ID. Duplicate IDs, unresolved
calls, repeated/orphan results, malformed arguments and invalid UTF-8 fail before
loading a model. Parallel results may arrive in any order after their call batch.
A complete batch is required before the next conversational message.

Input is bounded to 256 messages, 64 blocks/message, 256 historical calls and
8 MiB of payload. Tool names use ASCII letters/digits/underscore/hyphen (64 bytes);
IDs use the same alphabet (128 bytes). Applications map registry identifiers to
these names explicitly. Instruction-like text in a result keeps the tool role;
role separation is not, by itself, a solution to prompt injection.

Chat Completions and Responses receive their native message/call/result objects.
The embedded C chat-template adapters support arbitrary text message sequences.
They reject tool blocks and native tool generation rather than flattening them.
An unsupported template fails instead of silently falling back to ChatML.
Attachments, native embedded tool rendering and provider state handles remain
future work; no placeholder attachment or unsupported modality is transmitted.

## Native proposals

`params.tools` carries 1–64 distinct function schemas and auto/required/none
selection. The root parameter schema must be an object. Schemas and descriptions
are bounded to 1 MiB total and included in conservative admission/reservations.
Native tools cannot be combined with GBNF or a separate output schema in this
adapter. A zero-initialized `asmodel_tool_calls` receives at most 32 calls.
Call `asmodel_tool_calls_clear` when finished; starting another request clears the
previous output. Different concurrent requests require separate output storage.

Only `ASMODEL_OK` with `ASMODEL_FINISH_TOOL_CALLS` yields complete proposals.
SSE fragments are assembled by index with bounded IDs/names/arguments. Unknown
names, duplicate/reused IDs, missing required calls and malformed JSON arguments
fail. Length stops, cancellation, timeout and protocol errors discard all calls,
including apparently complete early calls in an incomplete batch. Partial text
can remain available with an error status. Consumption remains independent of
whether a proposal passes validation.

A schema is an output request, not an execution grant. The host must validate
arguments against its actual tool schema and current permissions before invoking
anything. asmodel checks object structure and the declared tool set; it does not
implement a universal JSON Schema validator or execute tools.

Asngn/Asper adapters forward the full input and contract. Their existing micro
phases deliberately construct two text messages; engine use of a native decision
path is a separate milestone. No new routing policy is inferred from provider
names or passing fake-model tests.

## Validation and protocol references

Deterministic tests cover role preservation, Unicode, result correlation, queue
admission, malformed inputs, streamed arguments, missing/unknown/duplicate tools,
reused IDs, truncated generations and both HTTP encodings. Tests run against a
local mock; they do not establish real-model or deployed-server conformance.

Protocol references consulted on 2026-09-06:
[llama-server](https://github.com/ggml-org/llama.cpp/blob/master/tools/server/README.md)
and [LM Studio Responses](https://lmstudio.ai/docs/developer/openai-compat/responses).
