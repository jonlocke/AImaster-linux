# AImaster-linux

AImaster keeps its existing internal Ollama-style chat history and request flow, but now supports two upstream provider modes:

- `ollama_native` (default): sends requests to Ollama `/api/chat`.
- `openai_compatible`: adapts the same internal Ollama-style request model to `/v1/chat/completions` backends such as LiteLLM and similar OpenAI-compatible gateways.

## Current chat code path

- `processCommand()` in `src/ollama_client.cpp` routes interactive commands and chat turns.
- Chat requests are still assembled from AImaster's existing Ollama-style `messages` history.
- `executeProviderChat()` in `src/chat_provider.cpp` now selects the upstream provider, translates requests, performs the HTTP call, and maps responses back into the Ollama-style assistant message shape AImaster expects.
- Existing Ollama-native behavior remains the default when `provider_type` is omitted.

## Configuration

Existing configs continue to work unchanged. If you do nothing, AImaster still uses Ollama-native mode.

### Ollama-native mode (default / backward-compatible)

```ini
serial_port=/dev/ttyUSB0
baudrate=2400
serial_delay_ms=50
serial_newline=CRLF

provider_type=ollama_native
ollama_url=http://localhost:11434/api/chat
ollama_model=gemma3:4b
ollama_timeout_seconds=5

rag_chunks=25
rag_threshold=0.2
serial_wrap_cols=80
commands_csv=cmds.csv
```

### OpenAI-compatible mode

```ini
serial_port=/dev/ttyUSB0
baudrate=2400

provider_type=openai_compatible
api_base=http://localhost:4000
api_key=sk-example
model=gpt-4o-mini
timeout=30
temperature=0.2
num_predict=512
format=json
extra_header=X-Client: AImaster

# Legacy fields may stay in place; chat uses provider_type/api_base/model when set.
ollama_url=http://localhost:11434/api/chat
ollama_model=gemma3:4b
ollama_timeout_seconds=5
```

### LiteLLM example

```ini
provider_type=openai_compatible
api_base=http://localhost:4000
api_key=sk-litellm
model=openai/gpt-4o-mini
timeout=30
temperature=0.1
num_predict=400
extra_header=X-Title: AImaster
```

AImaster will POST to:

- Ollama-native: `http://host:11434/api/chat`
- OpenAI-compatible: `http://host:port/v1/chat/completions`

## Request/response mapping

### Request mapping

AImaster keeps building Ollama-style chat input and maps it as follows for `openai_compatible` mode:

- `model` -> `model`
- `messages` -> `messages`
- `options.temperature` -> `temperature`
- `options.num_predict` -> `max_tokens`
- `stream` -> `stream`
- Ollama `format=json` -> OpenAI `response_format={"type":"json_object"}` when possible
- tool metadata is passed through where configured

### Response mapping

OpenAI-compatible responses are normalized back into the Ollama-style assistant message shape used internally:

- assistant text is read from `choices[0].message.content` or streaming `choices[0].delta.content`
- finish reasons are normalized onto the assistant message
- token/accounting data is preserved in the stored assistant `usage` object when provided
- streaming SSE chunks are translated into the incremental text flow AImaster already expects

## Streaming behavior

Streaming remains enabled for interactive chat. In `openai_compatible` mode, SSE `data:` chunks are parsed and converted into the same incremental assistant text flow used for Ollama streaming. Partial malformed chunks are skipped with debug logging instead of crashing the session.

## Logging

Debug logging now includes:

- selected provider
- upstream URL
- streaming vs non-streaming mode
- translated request summary
- translated response summary

API keys are never written to logs.

## Known limitations

- RAG embedding/model verification still assumes Ollama-hosted embeddings and remains tied to the existing Ollama RAG path.
- `format` is mapped best-effort for OpenAI-compatible backends. Non-JSON structured output schemas beyond simple JSON mode are not fully translated yet.
- Tool/function calling fields are passed through where available, but full end-to-end tool execution semantics are not expanded in this patch.
- OpenAI-compatible model listing depends on `/v1/models` support from the upstream gateway.

## Migration note

No migration is required for existing Ollama users.

To switch to an OpenAI-compatible backend:

1. Set `provider_type=openai_compatible`.
2. Set `api_base` to the upstream base URL.
3. Set `api_key` if required.
4. Set `model` to the upstream chat model name.
5. Leave legacy `ollama_*` keys in place if you also use the current RAG embedding flow.

### MODEL
Type `MODEL` to fetch available models from your configured upstream and interactively select one. The choice is persisted to `config.txt`.

Execute `serial-passthru.bat` to pass the serial port from Windows to WSL, updating the batch file for your USB serial adapter IDs.

## `/speak` text-to-speech

AImaster now supports an additive local `/speak` command:

- `/speak on` enables streaming text-to-speech playback for assistant replies as words arrive.
- `/speak off` disables text-to-speech playback.
- Normal console output is unchanged; speech is played in addition to the printed response.
- `/speak` is handled locally and is never sent to the LLM as chat content.

When speech is enabled, AImaster now streams the assistant response into a background TTS queue as the LLM text arrives. Completed words are sent to the configured Quick-Piper-Endpoint `/speak` URL and played incrementally, so audio can begin before the full reply is finished. The client still prefers the documented `play=0&return_audio=1` flow and falls back to compatible JSON/base64 parsing when needed before attempting playback with the first available backend in this order:

1. `paplay`
2. `aplay`
3. `ffplay`

If speech synthesis, decoding, or playback fails, AImaster logs a concise warning and continues without interrupting the normal text flow.

### TTS configuration

These config keys are supported in `config.txt` and `config-example.txt`:

```ini
# Text-to-speech (disabled by default)
tts_enabled=false
tts_endpoint_url=http://127.0.0.1:8092/speak
tts_timeout_seconds=10
#tts_voice=en-us
#tts_speaker=narrator
```

Environment overrides are also supported:

- `AIMASTER_TTS_ENABLED`
- `AIMASTER_TTS_ENDPOINT`
- `AIMASTER_TTS_TIMEOUT_SECONDS`
- `AIMASTER_TTS_VOICE`
- `AIMASTER_TTS_SPEAKER`

Request payloads follow the upstream examples by sending `text` (and `prompt` as a compatibility alias), plus optional `voice` and `speaker` values in the JSON body. The client appends `play=0&return_audio=1` to the configured `/speak` URL so the shim returns WAV audio bytes directly; for resilience it also accepts compatible JSON/base64 fields such as `audio`, `audio_base64`, `wav_base64`, `audio_data`, and nested `data.*` variants.

### Troubleshooting audio

#### No audio heard

- Confirm `/speak on` is enabled.
- Confirm one of `paplay`, `aplay`, or `ffplay` is installed and available on `PATH`.
- Check stderr for the selected playback backend or warning message.
- Verify the endpoint returns playable WAV/base64 content.

#### Endpoint connectivity

- Verify `tts_endpoint_url` points to the running Quick-Piper endpoint.
- Increase `tts_timeout_seconds` if synthesis is slow.
- If needed, set `tts_voice` and `tts_speaker` to values accepted by your endpoint deployment.
- If the endpoint is unavailable or returns invalid JSON, AImaster keeps the normal text reply and logs a warning instead of crashing.
