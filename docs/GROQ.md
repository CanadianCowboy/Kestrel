# Groq cloud chat (optional)

Kestrel keeps local inference as the default. Choose **Groq cloud…** in the
sidebar runtime panel to explicitly send chat requests to Groq. GPT-OSS 120B
(`openai/gpt-oss-120b`) is the default; GPT-OSS 20B is also available.

**Privacy:** Groq receives Kestrel's system instructions (including available
tool descriptions), conversation context, and tool results used in the reply.
Cloud inference is not an offline/local mode. Local models remain selectable
via **Load model** / `--model`. Selecting Groq is not persisted: each new launch
uses the ordinary local-model selection unless `--groq` is supplied.

## Credentials

In the Groq dialog, **Import API key file…** accepts a local text file containing
only a `gsk_…` key. It seals that key under the `groq` account of the
`com.kestrel.inference` service in the existing OS keystore. If the keystore is
unavailable or locked, importing fails with an actionable message; there is no
plaintext storage fallback. The original file is not deleted or modified.

Alternatively set `GROQ_API_KEY` or `GROQ_API_KEY_FILE` for session-only use. An
explicitly imported file takes priority, followed by `GROQ_API_KEY_FILE`,
`GROQ_API_KEY`, then the saved OS-keystore key. Kestrel never automatically
scans `docs/` for credentials, and keys are never placed in QSettings, logs,
model paths, prompts, or source code. Local `docs/*apikey.txt` files are ignored
by Git and must not be distributed with a build.

From a checkout with the user-supplied key file:

```bash
GROQ_API_KEY_FILE="$PWD/docs/groqapikey.txt" ./build/kestrel --groq
GROQ_API_KEY_FILE="$PWD/docs/groqapikey.txt" ./build/kestrel --groq --print-runtime
GROQ_API_KEY_FILE="$PWD/docs/groqapikey.txt" ./build/kestrel --groq --smoke-test
```

Use `--groq-model openai/gpt-oss-20b` to select the smaller model. `--groq` and
`--model` cannot be combined. No network request is made by `--print-runtime`;
the smoke test sends a small synthetic conversation through the real desktop
controller, worker, HTTP adapter, and rendered transcript model.

## Behavior and limits

- Uses Qt Network already required by the desktop app; no provider SDK or Qt
  dependency is added to the portable core/runtime library.
- Streams chat-completion SSE events from
  `https://api.groq.com/openai/v1/chat/completions`. The shared system prefix is
  sent first on each request, followed by conversation turns. Refreshing
  Undermind tools changes subsequent requests without duplicating the prefix.
- Uses low reasoning effort and excludes reasoning from visible/spoken output.
  The normal response token cap includes reasoning tokens; hitting it is
  reported as an incomplete reply rather than falsely reporting success.
- Tool use follows Kestrel's existing textual `<tool_call>` protocol and
  permission dispatcher, not Groq native tool calling. No permission is granted
  merely by selecting cloud inference.
- Stop cooperatively aborts the HTTP reply; requests have a 60-second deadline.
  Stream frames are bounded to 1 MiB; invalid, empty, truncated, and failed
  streams report failure. Redirects are not followed with the API key.
- HTTP 429 reports `Retry-After` when present. No automatic retries, provider
  switching, or paid calls in a retry loop. Check the Groq account Limits page
  for organization-wide request/token quotas; the limits in `groqinfo.txt` are
  reference material, not hard-coded guarantees.
- Groq manages prompt caching automatically. Kestrel does not claim a local
  resident prefix cache or guaranteed cache hit. Server usage can update context
  occupancy/throughput; preflight token counts remain approximate without a
  provider tokenizer, and streamed UI pieces are not exact model token counts.
- Idle GPU warmup and background idle session summarization never invoke the
  cloud backend. Explicit user-requested replies, tool follow-ups, and
  interruption/resume actions may use it.

API details: [chat completions](https://console.groq.com/docs/api-reference),
[reasoning](https://console.groq.com/docs/reasoning),
[rate limits](https://console.groq.com/docs/rate-limits).
