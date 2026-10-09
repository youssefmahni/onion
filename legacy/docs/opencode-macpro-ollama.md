# Mac Pro Ollama Server + OpenCode

How to connect OpenCode to the local LLM server running on the Mac Pro
(`Mac-Pro-de-mac`, macOS 12.7.6, headless, CPU-only).

## 1. The server

| Item      | Value                                            |
| --------- | ------------------------------------------------ |
| VPN       | `http://10.8.0.91:11434`                         |
| LAN       | `http://192.168.1.57:11434`                      |
| OpenAI API| `http://10.8.0.91:11434/v1` (or `...:57/v1`)     |

Models available (all Q4, CPU inference):

| Model ID                     | Use            | Speed        |
| ---------------------------- | -------------- | ------------ |
| `qwen2.5-coder-agent:latest` | coding / tools | ~4.7 tok/s   |
| `llama3.1-agent:latest`      | general        | ~4.5 tok/s   |
| `llama3.2-agent:latest`      | fast, weak     | ~10.3 tok/s  |
| `nomic-embed-text:latest`    | embeddings     | -            |

The `-agent` tags are the base models wrapped with `num_ctx 16384` so
OpenCode's tool schemas and system prompts fit in context.

## 2. Add the provider to OpenCode

Global config: `~/.config/opencode/opencode.json`
(or project-level: `opencode.json` in your project root)

```json
{
  "$schema": "https://opencode.ai/config.json",
  "provider": {
    "ollama": {
      "npm": "@ai-sdk/openai-compatible",
      "name": "Mac Pro (Ollama)",
      "options": {
        "baseURL": "http://10.8.0.91:11434/v1",
        "apiKey": "ollama",
        "timeout": false,
        "chunkTimeout": false
      },
      "models": {
        "qwen2.5-coder-agent:latest": { "name": "Qwen2.5-Coder 7B (Mac Pro)" },
        "llama3.1-agent:latest": { "name": "Llama 3.1 8B (Mac Pro)" },
        "llama3.2-agent:latest": { "name": "Llama 3.2 3B (Mac Pro)" }
      }
    }
  }
}
```

Notes:

- `baseURL` uses the VPN IP — works from any VPN client. On the LAN only,
  use `http://192.168.1.57:11434/v1`.
- `apiKey` is required by the SDK but ignored by Ollama.
- `timeout`/`chunkTimeout` are disabled on purpose: prompt evaluation on
  this CPU takes minutes for large prompts, and the default timeouts
  (~5 min) would abort every request before the first token.

## 3. Use it

1. Restart OpenCode (config is not hot-reloaded).
2. Run `/models` and pick one of the "Mac Pro" models.
3. The provider ID is `ollama`, so full references look like
   `ollama/qwen2.5-coder-agent:latest`.

## 4. Optional: dedicated agent on the local model

`.opencode/agent/local-mac.md`:

```markdown
---
description: Fast local agent running on the Mac Pro Ollama server
mode: primary
model: ollama/qwen2.5-coder-agent:latest
---

You are a concise coding assistant running on a local CPU server.
Prefer short replies and small diffs.
```

Then select it from the agent list or set
`"default_agent": "local-mac"` in `opencode.json`.

## 5. Creating more `-agent` tags (bigger context)

On the Mac (`ssh mac@10.8.0.91`), to wrap any pulled model with a larger
context window:

```bash
printf 'FROM llama3.1:8b\nPARAMETER num_ctx 16384\n' > /tmp/mf
/usr/local/bin/ollama create llama3.1-agent -f /tmp/mf
```

No re-download happens — the new tag reuses the existing blobs.

## 6. Expectations

- **It works, but it's slow for agentic use.** The bottleneck is prompt
  processing (~6.5 tok/s on the 8B): OpenCode sends a large system prompt
  plus tool schemas on every turn, so expect minutes before the first
  token, then ~4.5 tok/s generation.
- 7–8B tool calling is mediocre — `qwen2.5-coder-agent` is the most
  reliable of the three.
- The server keeps one model resident (`OLLAMA_KEEP_ALIVE=-1`,
  `OLLAMA_MAX_LOADED_MODELS=1`); switching models costs a few seconds of
  load time.
- Ollama is pinned to v0.4.7 (last release supporting macOS 12). Newer
  models (Qwen3, etc.) need manual GGUF import via `ollama create` with a
  Modelfile pointing at a downloaded GGUF.
