# MCP desktop acceptance test

## Reference and supported scope

The implementation targets MCP `2025-06-18` Streamable HTTP and uses the
`2025-11-25` authorization guidance for CIMD discovery:

- https://modelcontextprotocol.io/specification/2025-06-18/basic/transports
- https://modelcontextprotocol.io/specification/2025-06-18/server/tools
- https://modelcontextprotocol.io/specification/2025-06-18/server/resources
- https://modelcontextprotocol.io/specification/2025-06-18/server/prompts
- https://modelcontextprotocol.io/specification/2025-06-18/server/utilities/completion
- https://modelcontextprotocol.io/specification/2025-11-25/basic/authorization

Kestrel's local endpoint is stateless, loopback-only, and Bearer-token protected.
It returns JSON to POST requests and 202 with no body to notifications. It does
not issue session IDs, offer an SSE listener, or implement subscriptions or
list-change notifications. GET and DELETE return 405. Clients must refresh lists
when capabilities change. Logging level selection is supported; streaming log
notifications are not implemented.

The endpoint exposes tool input schemas, text resources, a conversation URI
template, a conversation-summary prompt, and prompt argument completion.
Conversation and non-sensitive memory reads require explicit consent in the app.
Sensitive memories are excluded from the memory resource. The consent applies
to clients holding the endpoint's token, so share that token only with trusted
local clients. Revoking consent blocks subsequent reads; it cannot recall data
already returned to a client.

Remote connections support the negotiated `2025-06-18` protocol, JSON/SSE response
bodies, public servers, static Bearer tokens, and OAuth PKCE S256 with CIMD or a
client ID supplied through the C++ configuration interface. OAuth discovery uses
RFC 9728 and RFC 8414/OIDC fallback locations, validates the issuer, and requires
HTTPS authorization endpoints. Undermind retains its provider-specific endpoints.
OAuth credentials are stored in the OS keystore, bound to the client ID and
resource. A server without CIMD support reports an actionable error rather than
attempting insecure authorization. Dynamic client registration is not implemented.
Server configurations and manually supplied Bearer tokens are session-only; the
UI says so. The client is not a universal implementation of every MCP feature:
legacy HTTP+SSE, stdio subprocesses, pagination, multimodal rendering, and
server-initiated sampling/elicitation are not provided by these controls.

## Build and launch

Use Qt 6.6+ and a UI-enabled build:

```sh
cmake -S . -B build-linux -DKESTREL_BUILD_UI=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-linux -j 2
ctest --test-dir build-linux --output-on-failure
./build-linux/kestrel
```

Use your existing build directory if it already contains your inference runtime
configuration. A loaded language model is not required to test MCP controls.
For an actual chat test, let the installed local GGUF load automatically; there
is no Groq/cloud inference path. Do not mistake preview content for real inference.

## Hands-on app test

1. Open **Activity** using `Ctrl+Shift+A`. The local MCP status should be stopped.
2. Click **Start server**. A loopback endpoint with an automatically chosen port
   appears in a selectable URL field. Click **Copy token**; do not publish it.
3. Leave **Allow MCP to read conversations and non-sensitive memories** unchecked.
   An authenticated `resources/read` call to
   `kestrel://conversations/active` must be denied. Missing/wrong Bearer tokens
   must receive HTTP 401 even for `ping`.
4. Check the consent box. The same authenticated read should now succeed.
   `resources/list` includes runtime status, the active conversation, and local
   non-sensitive memories; `resources/templates/list` exposes
   `kestrel://conversations/{id}`. `prompts/list` includes
   `summarize_active_conversation`.
5. In **Add MCP server for this session**, enter name `Local test`, paste the
   local endpoint URL, and paste its token into the optional masked token field.
   Click **Add**, then the new row's **Connect**. Expect a connected status with
   resource/prompt counts. The token input should clear after adding.
6. Click the row's **×** to remove the test connection. It should disappear and
   its forwarded tools should no longer be available.
7. To test a real remote server, add its HTTPS MCP URL. Leave the token blank
   for a public server or CIMD OAuth server. Click **Connect**. A protected
   server should open browser authorization, complete the loopback callback,
   and show a connected status. Approve only the intended server and scopes.
   Unsupported OAuth metadata should instead produce a useful status message.
   Undermind has its own **Connect Undermind** button.
8. For a chat integration test, wait for automatic local model loading, connect
   a trusted remote server, and ask Kestrel to list available tools and perform one harmless read.
   Inspect Activity for the actual call and result, not merely a textual claim.
9. Uncheck consent and confirm conversation reads are denied again. Click
   **Stop server**; new requests to the old address must fail to connect.

The automated app tests load `src/ui/Main.qml`, activate the same handlers,
connect the application to its own live endpoint, exercise consent and removal,
and cover OAuth browser/callback behavior with scripted remote responses.
They do not contact a real provider account or prove real-model inference.
