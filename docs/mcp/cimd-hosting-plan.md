# Kestrel public CIMD hosting and Cloudflare cloud-model plan

Status:
- **Public CIMD endpoints:** Deployed and verified live on `identity.josh-lynes.com`
  via the static-only Cloudflare Pages project `kestrel-identity`.
- **Kestrel Undermind OAuth client (`UndermindClient`):** Switched to the live
  platform CIMD identities (`kestrel-linux.json` on Linux, `kestrel-windows.json`
  on Windows), with saved token records bound to `client_id` and `resource`, and
  verified live with a real browser sign-in and tool discovery on Linux.
- **Cloudflare cloud models (`google/gemini-4-argon`):** Planning only. Cloud
  inference will be an explicit opt-in choice the user must make; local inference
  (`llama.cpp` / ONNX Runtime GenAI) remains the default.

## Agreed decisions

- Domain: `josh-lynes.com` (Cloudflare zone `9c7976732fba3b06cf317c69bb3ad945` on
  the Free Website plan).
- Dedicated hostname: `identity.josh-lynes.com` (confirmed unused prior to setup;
  existing tunnel CNAMEs and Email Routing MX/TXT records left untouched).
- Keep the current DNS provider and existing services unchanged.
- No new recurring hosting cost for the CIMD endpoints; static hosting runs on
  Cloudflare Pages' free tier (`kestrel-identity`).
- Upload manually from reviewed files in `docs/mcp/`, not automatically from
  feature branches or `main`.
- Separate production OAuth identities for Windows and Linux.
- A fresh Undermind sign-in after changing client IDs is acceptable.
- Cloud model inference through Cloudflare (starting with `google/gemini-4-argon`)
  is planned as an explicit user-selected option, never silent or automatic.

## Live public CIMD interface

| Platform | Exact client ID and metadata URL | Live status |
| --- | --- | --- |
| Linux | `https://identity.josh-lynes.com/oauth/kestrel-linux.json` | HTTP 200 (`application/json`) |
| Windows | `https://identity.josh-lynes.com/oauth/kestrel-windows.json` | HTTP 200 (`application/json`) |

Reviewed source files in the repository:
- `docs/mcp/kestrel-linux.json`
- `docs/mcp/kestrel-windows.json`
- `docs/mcp/_headers`
- `docs/mcp/kestrel-oauth-client.json` (preserved during transition)

These URLs identify the desktop apps, not users. Keep them unchanged across
releases, branches, and hosting-provider moves. Do not add version numbers, query
strings, or redirects. The JSON `client_id` matches its own public URL exactly.

Both desktop clients retain the existing local callback:
`http://127.0.0.1:8787/callback`. This is where the user's browser returns to the
running app; it is not the public metadata host. Keep the port-conflict diagnostic.

Linux metadata (`docs/mcp/kestrel-linux.json`):

```json
{
  "client_id": "https://identity.josh-lynes.com/oauth/kestrel-linux.json",
  "client_name": "Kestrel (Linux)",
  "client_uri": "https://github.com/CanadianCowboy/Kestrel",
  "redirect_uris": ["http://127.0.0.1:8787/callback"],
  "grant_types": ["authorization_code", "refresh_token"],
  "response_types": ["code"],
  "token_endpoint_auth_method": "none"
}
```

Windows metadata (`docs/mcp/kestrel-windows.json`) uses the identical schema with
`client_name` set to `Kestrel (Windows)` and `client_id` set to
`https://identity.josh-lynes.com/oauth/kestrel-windows.json`. Neither document
contains client secrets, private keys, user tokens, executable code, or user data.

## Deployed hosting configuration and verification

- **Pages project:** `kestrel-identity` (`kestrel-identity.pages.dev`, production
  branch `main`, Direct Upload, static files only — no Functions or Worker code).
- **Custom domain:** `identity.josh-lynes.com` attached to `kestrel-identity` with
  a proxied DNS `CNAME` record (`identity.josh-lynes.com` ->
  `kestrel-identity.pages.dev`) and active Google-issued edge TLS certificate.
- **Response headers (`docs/mcp/_headers`):**
  ```text
  /oauth/kestrel-linux.json
    Content-Type: application/json
    Cache-Control: public, max-age=300
    X-Content-Type-Options: nosniff

  /oauth/kestrel-windows.json
    Content-Type: application/json
    Cache-Control: public, max-age=300
    X-Content-Type-Options: nosniff
  ```
- **Verified live behavior:**
  - `GET https://identity.josh-lynes.com/oauth/kestrel-linux.json` returns HTTP 200
    with `Content-Type: application/json`, `Cache-Control: public, max-age=300`,
    `X-Content-Type-Options: nosniff`, no redirects, and exact byte/JSON agreement
    with `docs/mcp/kestrel-linux.json`.
  - `GET https://identity.josh-lynes.com/oauth/kestrel-windows.json` returns HTTP 200
    with the same headers, no redirects, and exact agreement with
    `docs/mcp/kestrel-windows.json`.
  - Unknown paths (e.g., `/oauth/does-not-exist.json`) return HTTP 404 via the
    explicit top-level `404.html` rather than an SPA HTML fallback.

## Cloudflare CLI tooling setup

1. **Cloudflare skills (`cloudflare/skills`):**
   All 19 official Cloudflare skills (`agents-sdk`, `basin`, `cf-cli`, `cloudflare`,
   `cloudflare-email-service`, `cloudflare-one`, `cloudflare-one-migrations`,
   `durable-objects`, `gateway-workers`, `k2`, `nextjs-on-cloudflare`,
   `sandbox-migrate-to-next`, `sandbox-next`, `sandbox-stable`, `turnstile-spin`,
   `web-perf`, `workers-best-practices`, `workers-profiling`, `wrangler`) are
   installed in `.agents/skills/`.
2. **Cloudflare CLI (`cf`):**
   Installed at `~/.local/bin/cf` (`1.0.0-beta.17`, on `PATH`) and authenticated
   via OAuth (`~/.config/cloudflare/config/default.json`).
3. **Project-local Wrangler (`4.151.0`):**
   Installed at `.cache/cimd-tools/node_modules/.bin/wrangler` for Pages Direct
   Upload asset bundling (`wrangler pages deploy`).

To re-assemble and redeploy `build/cimd-public` manually when `docs/mcp/` changes:

```bash
rm -rf build/cimd-public
mkdir -p build/cimd-public/oauth
cp docs/mcp/kestrel-linux.json build/cimd-public/oauth/kestrel-linux.json
cp docs/mcp/kestrel-windows.json build/cimd-public/oauth/kestrel-windows.json
cp docs/mcp/_headers build/cimd-public/_headers
printf '<!doctype html><meta charset="utf-8"><title>404 Not Found</title><p>Not found.</p>\n' > build/cimd-public/404.html

WRANGLER_SEND_METRICS=false .cache/cimd-tools/node_modules/.bin/wrangler \
  pages deploy build/cimd-public --project-name kestrel-identity --branch main
```

## Completed repository integration for Kestrel's OAuth client ID

1. Added `cimd-metadata` validation to `.github/workflows/ci.yml`, without
   deployment credentials or publishing: exact identities, schema, callbacks,
   public-client auth method, absence of secrets, and a small document-size cap of
   5 KiB each.
2. Updated `src/app/undermindclient.cpp` (`undermindClientId()`) to select the
   matching identity at compile time for Linux or Windows, leaving macOS unchanged.
3. Bound saved OAuth records in `UndermindClient::loadTokens()` and `saveTokens()`
   to `client_id` and `resource`. Untagged legacy tokens or tokens saved under a
   different `client_id` are ignored in favour of fresh authorization.
4. Updated `tests/app_tests.cpp` to validate the platform's chosen identity,
   persisted token binding (`client_id` and `resource`), and legacy-token
   migration behavior alongside browser-handoff, loopback, PKCE, JSON/SSE, and
   failure-path coverage.
5. Verified live browser authorization on Linux against Undermind's OAuth and MCP
   endpoints (`https://api.undermind.ai/o/authorize/` -> `/o/token/` ->
   `https://mcp.undermind.ai/mcp`).

---

## Planned feature: Opt-in Cloudflare cloud models (starting with `google/gemini-4-argon`)

### Product principle: explicit user choice

Kestrel is local-first. Local inference (`BackendKind::OrtGenAI` and
`BackendKind::LlamaCpp`) remains the default startup path and never sends prompts
over the network. Cloud inference via Cloudflare is an **explicit opt-in choice the
user must select** in the UI (or via an explicit launch flag/setting), with clear
visual indication in the header and runtime panel whenever a cloud model is active:
- When a local model is active, the header continues to show
  `Private workspace · local only`.
- When a Cloudflare cloud model is active, the header and runtime panel must clearly
  state `Cloud runtime · Cloudflare AI Gateway` and the selected model identifier
  (`google/gemini-4-argon`) so the user always knows when turns leave the machine.
- Background idle warmup (`ModelWarmup`) must remain disabled for cloud backends so
  idle ticks never consume paid cloud tokens.

### Target initial model: `google/gemini-4-argon`

Verified in Cloudflare's AI model catalog (`https://developers.cloudflare.com/ai/models/google/gemini-4-argon/`):
- **Canonical Cloudflare model ID:** `google/gemini-4-argon`
- **Context window:** `1,048,576` tokens (1M tokens)
- **Supported Cloudflare endpoints:**
  1. **OpenAI-compatible Chat Completions (recommended for Kestrel's `ModelBackend`):**
     `POST https://api.cloudflare.com/client/v4/accounts/{account_id}/ai/v1/chat/completions`
     with `"model": "google/gemini-4-argon"`, `"messages": [...]`, `"stream": true`.
  2. **Universal `/ai/run` endpoint (Google `contents` / `systemInstruction` schema):**
     `POST https://api.cloudflare.com/client/v4/accounts/{account_id}/ai/run`
     with `"model": "google/gemini-4-argon"`.
  3. **AI Gateway provider-native Google AI Studio passthrough (when using BYOK):**
     `POST https://gateway.ai.cloudflare.com/v1/{account_id}/{gateway_id}/google-ai-studio/v1/models/gemini-4-argon:streamGenerateContent`

### Authentication and billing options (to be chosen before implementation)

Cloudflare supports two credential modes for `google/gemini-4-argon`:
1. **Cloudflare Unified Billing:**
   - Uses a Cloudflare API token with **Account > Workers AI > Read** permission
     and the user's Cloudflare Account ID (`fcd80664ee12b7f09a9075bee8f72875`),
     deducting from prepaid AI Gateway Unified Billing credits.
   - Zero Data Retention (ZDR) is supported on Unified Billing requests when AI
     Gateway request/response logging is disabled.
2. **Bring Your Own Key (BYOK) via Cloudflare AI Gateway:**
   - Routes through an AI Gateway on the user's Cloudflare account using a Google
     AI Studio API key (either stored in the gateway's BYOK `default` alias or
     passed per request alongside `cf-aig-authorization`).

Credentials must be stored in Kestrel's OS keystore (`storage::SecretStore`,
matching `UndermindClient`), with environment-variable overrides
(`KESTREL_CLOUDFLARE_ACCOUNT_ID`, `KESTREL_CLOUDFLARE_API_TOKEN`, and optional
`KESTREL_CLOUDFLARE_GATEWAY_ID`) for development. Never log or echo tokens.

### Proposed architecture in Kestrel

1. **Vendor SDK and Qt boundary (`src/runtime/`):**
   - Keep `src/runtime/` portable C++20 with no Qt headers.
   - Add `BackendKind::CloudflareAi` (`"cloudflare-ai"`) to
     `src/runtime/modelbackend.h` and `src/runtime/backendregistry.cpp`.
   - Do **not** include `BackendKind::CloudflareAi` in `kPreferenceOrder` inside
     `backendregistry.cpp`: automatic startup selection must only pick local
     backends (`OrtGenAI`, `LlamaCpp`), preserving local-first behavior unless the
     user explicitly selects the cloud backend.
   - Define a small portable HTTP/SSE transport seam in `src/runtime/` (or inject a
     transport callback into `CloudflareAiBackend`) so `tests/runtime_tests.cpp`
     and `tests/core_tests.cpp` can test streaming, tool-call tags, cancellation,
     and error diagnostics without Qt or live network calls, while `src/app/`
     provides the Qt Network implementation on the worker thread.
2. **Implementing `CloudflareAiBackend` (`ModelBackend` contract):**
   - `status()`: reports `available = true` and `modelLoaded = true` only when the
     account ID, credential, and model ID (`google/gemini-4-argon`) are configured;
     reports `contextLimit = 1048576` and actionable setup instructions in `detail`
     when credentials are missing.
   - `setSystemPrompt()` / `clearSharedPrefix()`: stores Kestrel's shared persona
     and tool instructions prefix and sends it as the `system` message (or
     `systemInstruction`), updating `contextUsed` from the response's `usage`
     metadata (`prompt_tokens` / `completion_tokens` or `usageMetadata`).
   - `generate()`: runs on `GenerationWorker`'s thread, streams SSE token deltas
     to `onToken`, polls an atomic cancellation flag between chunks so
     `cancel()` from the UI thread aborts the in-flight HTTP reply promptly, and
     reports actionable HTTP/auth/billing errors (401 token scope, 402/400 credit
     balance, 429 rate limit, network timeout) through `onComplete`.
   - Tool calling: preserves Kestrel's `<tool_call>` protocol through the system
     prompt so `GenerationWorker`'s existing streaming parser and MCP/local tool
     dispatch continue to work identically across local and cloud models.
3. **UI and controller integration (`src/app/` and `src/ui/`):**
   - Add an explicit runtime selector in `Main.qml` (`Local model` vs
     `Cloudflare AI (google/gemini-4-argon)`), with a model/credential configuration
     sheet backed by `SecretStore`.
   - Persist the user's explicit model choice in `storage::Profile` only when the
     user selects it, and allow switching back to a local `.gguf` or ONNX Runtime
     GenAI model at any time.
   - Update the workspace subtitle in `Main.qml` so it reflects whether the active
     backend is `local only` or `Cloudflare cloud model`.

## Sources

- [OAuth Client ID Metadata Document draft](https://drafts.oauth.net/draft-ietf-oauth-client-id-metadata-document/draft-ietf-oauth-client-id-metadata-document.html)
- [Cloudflare Agent Setup instructions](https://developers.cloudflare.com/agent-setup/prompt.md)
- [Cloudflare CLI (`cf`) documentation](https://developers.cloudflare.com/cf/)
- [Cloudflare AI Gateway REST API](https://developers.cloudflare.com/ai-gateway/usage/rest-api/)
- [Cloudflare AI Gateway Unified Billing](https://developers.cloudflare.com/ai-gateway/features/unified-billing/)
- [Gemini 4 Argon on Cloudflare AI](https://developers.cloudflare.com/ai/models/google/gemini-4-argon/)
- [Cloudflare Pages Direct Upload](https://developers.cloudflare.com/pages/get-started/direct-upload/)
- [Cloudflare Pages custom domains](https://developers.cloudflare.com/pages/configuration/custom-domains/)
- [Cloudflare Pages response headers](https://developers.cloudflare.com/pages/configuration/headers/)
