# asmodel

`asmodel` is the process-wide model runtime for Asterism components. It owns
loaded provider instances and their reusable contexts, applies resident/RAM/
VRAM budgets, performs ordered warm-up and LRU/idle eviction, and exposes one
API for generation, embeddings, tokenization and context/KV reuse.

The core is provider-neutral. An embedding host registers a loader for local
backends such as llama.cpp. A built-in OpenAI-compatible provider implements
`/chat/completions` and `/embeddings` for LM Studio, vLLM, Unsloth Studio
and similar servers. It uses WinHTTP on Windows and libcurl when available on
other platforms. This keeps `asper` and `asngn` independently usable while
allowing them to borrow the same manager when they run in one process.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

See `include/asmodel.h` for the stable C API.
