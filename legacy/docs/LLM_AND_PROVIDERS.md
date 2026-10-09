# LLM Layer — providers, routing, payloads, parsers

## 1. Key resolution & provider routing

The stack is **key-driven**: drop a key in and everything routes itself.

```
resolve_key():  OPENROUTER_API_KEY  >  DEEPSEEK_API_KEY (legacy)  >  built-in placeholder
api_url_for(k): DEPOT_API_URL (explicit override, always wins)
                else k starts "sk-or" → https://openrouter.ai/api/v1/chat/completions
                else                    → https://api.deepseek.com/chat/completions
default_model:  sk-or key → google/gemini-2.5-flash-lite ; else → deepseek-flash
```

`onion-start` adds a `.onion_key` file step (env → `.onion_key` → legacy env) and exports `OPENROUTER_API_KEY`. `main.cpp` refuses to run when the key is still the `PASTE…` placeholder.

## 2. Reasoning knob (critical, provider-specific)

```cpp
reasoning_param(key):
  sk-or key → "reasoning":{"effort":"low"}      // OpenAI-style (OpenRouter)
  else      → "reasoning_effort":"none"         // DeepSeek's own field
```

**Why:** DeepSeek's reasoning mode burns the entire `max_tokens` budget on hidden reasoning → empty `content`. DeepSeek only honours its own `reasoning_effort` field; it *ignores* the OpenAI-style `reasoning` object. Sending the wrong knob per provider silently breaks every reply. Do not "unify" these fields.

## 3. Transport (`curl_post`)

- Payload → `/tmp/depot_llm_<pid>_<seq>.json`; headers → `.hdr` with `chmod 0600` (key never world-readable, even briefly).
- `curl -4 -s --max-time 120 <url> -H @hdr --data-binary @tmp` via `popen`.
- Non-zero curl exit → `""` returned (HTTP/transport failure must not masquerade as success) → caller retries once.
- `DEPOT_DUMP_BODIES=1` dumps payloads+responses to `/tmp/depot_body_NNN.json`.
- One pipe fd per in-flight call → `onion-start` raises `ulimit -n` to 65535 (1024 breaks >~1000 threads).

## 4. Payloads

Common envelope: `temperature 0`, `stream false`, system prompt *"strict knowledge-curation assistant … answer ONLY with the JSON object requested"*.

| Caller | max_tokens | Requested JSON |
|---|---|---|
| Funnel agents (planner/worker/trimmer/critique/synth) | 6000 | varies per agent (see FUNNEL_ENGINE.md) |
| llm loop `call_llm` | 400 | `{"valid": bool, "fixed": null\|string, "admits_children": bool}` (L0–L4 always admits) |
| `vision.py` | 1200 / 4000 | image `image_url` content parts + question |

## 5. Parsers (`depot_core`)

- `content_json` — locates `"content"` in the chat body, **fully unescapes** the JSON string (`\" \\ \n \t \r \uXXXX`), tolerates raw (pre-decoded) content and pretty-printed multi-line bodies, then takes the outermost `{…}`.
- `parse_funnel_r1` → `vector<string>` from `"items"`.
- `parse_funnel_r4` → `vector<FunnelItem{title, atomic}>` — skips objects without titles, continues after malformed ones, missing `atomic` defaults false.
- `parse_funnel_c` → `{score (default 1.0), remove[], add[], issues}` (`issues:"null"` → empty).
- `parse_llm` → `{fixed, valid, admits, parse_ok, error}` — bool flags parsed only up to the closing brace of their field (a `"true"` inside a `fixed` string cannot flip flags); bodies without `content` (e.g. 401s) → `parse_ok=false` + error excerpt.

## 6. Failure semantics

`body_failed = body.empty() || body.find("\"error\"") != npos`. Empty bodies are the classic symptom of: wrong reasoning knob (see §2), an invalid key (401 body has no `content`), or provider rate-limiting. The funnel retries once, then settles the node (score 0.0) so the run never livelocks.

## 7. Vision (`vision.py`)

- Model: `google/gemini-2.5-flash-lite` (vision-capable, cheapest flash tier) via OpenRouter.
- MIME: png/jpeg/webp/gif; base64 data-URL `image_url` parts; >20 MB rejected, >5 MB warned; 120 s timeout.
- Subcommands: `ask <image> <question>` · `describe <image>` · `items <image> [--td …] [--out file.tsv]` → TSV ready for `data_depot ingest`.
- Key/env precedence identical to the C++ layer.

## 8. Provider history

Started on DeepSeek (`deepseek-flash`/`deepseek-v4-pro`), then moved to OpenRouter (`google/gemini-2.5-flash-lite`, $0.10/M prompt / $0.40/M completion). The routing above keeps both working simultaneously — switching providers is a key swap, never a code change.
