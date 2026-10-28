# Chat: direct ChatGPT account sign-in for Codex

Chat is an installable native Glaze app using Codex model access associated
with a ChatGPT account. It supports direct device-code authorization and a
one-time import from an official Codex CLI browser login. Neither path needs
a continuously running relay or an OpenAI API key.

This is a Codex-compatible chat client, not the ChatGPT website. Account
eligibility, allowed models, organization policies and Codex usage limits
still apply. A free ChatGPT account does not guarantee Codex access. Existing
ChatGPT website conversations are not synced. The app sends only text and
does not expose shell, file-editing, browsing or other model tools.

## Build and install

```sh
./build.sh out/pkg/chatgpt.gzp
# Or use the installed cross compiler:
make -j4 out/pkg/chatgpt.gzp

tools/mkcfimg.sh out/chat-cf.img 16 16 out/pkg/chatgpt.gzp
```

Boot Glaze with the card, open **Files → Card → chatgpt.gzp**, install on
flash, and launch **Chat** from Home. On hardware, replace the CF storage
card with a supported network card after installation. An updated Glaze
image providing `libwolfssl.so.1` is required. Check the device date before
HTTPS sign-in; certificates are checked against the system roots, host name
and date.

With the USB development client and an unlocked device:

```sh
out/host/glaze-usb put out/pkg/chatgpt.gzp /Documents/chatgpt.gzp
out/host/glaze-usb run /Documents/chatgpt.gzp
```

## Device-code login directly on Glaze

1. Open **Account** and select **Keep signed in** if you want the login
   retained after the app exits or the device restarts. It is off by default.
2. Tap **Sign in with ChatGPT**.
3. On your phone or computer, open `https://auth.openai.com/codex/device`,
   sign in to your account, and enter the code displayed on the BE-300.
4. Keep Chat open while authorization completes. A code expires after
   15 minutes. The app polls at the supplied interval, slows down when
   requested, and supports cancellation.
5. Once signed in, tap **Done**, type a message and tap **Send** or press
   Enter/OK.

Some accounts require enabling device-code authorization in their security
settings. If the server rejects the flow, use the browser-login import below.
No password is entered into Glaze. The browser used to approve the device
code runs on your phone or computer; Glaze's limited browser is not involved.

## Codex browser sign-in and one-time import

Install the [official Codex CLI](https://github.com/openai/codex) on your
computer, then run:

```sh
python3 tools/chatgpt_signin.py --output /tmp/CODEX.AUTH
```

The helper launches `codex login` in a temporary, dedicated `CODEX_HOME`
with file credential storage. Complete **Sign in with ChatGPT** in the
browser opened by Codex. The official CLI handles the OAuth authorization
code, PKCE, state and loopback callback. The helper exports the resulting
login and removes that temporary Codex home. Your existing desktop Codex
login is not read or changed.

For a computer without a browser, the helper can instead use Codex's own
device-code mode:

```sh
python3 tools/chatgpt_signin.py --device-code --output /tmp/CODEX.AUTH
```

Transfer the file into Glaze's Documents folder, Downloads folder, or the
root of a CF card. Then open **Account → Import Codex login**:

```sh
out/host/glaze-usb put /tmp/CODEX.AUTH /Documents/CODEX.AUTH
```

The export is created with mode 0600 and never printed. It contains bearer
and refresh credentials. Remove host and device transfer copies after import;
Glaze does not silently delete files on a removable card. `CODEX.AUTH` is
ignored by Git. A supplied `--from-auth PATH` can export a specified Codex
`auth.json`, but a dedicated sign-in is preferred: sharing one rotating
refresh token between the desktop and handheld can invalidate a session.

This import path is a one-time transfer. The computer does not receive or
forward subsequent chat messages. After import Glaze talks directly to
OpenAI, including token refresh.

## Chat and account behavior

The default model is `gpt-6-luna`, present in the reference Codex model
catalog. The model field in **Account** accepts another Codex model name
available to your account. Unsupported models and account restrictions
produce a readable error; the app does not attempt to bypass them.

Conversations have at most twelve messages (six user/assistant pairs).
Starting another turn drops the oldest complete pair when full. Drafts are
limited to 1023 bytes and displayed replies to about 4 KiB. Longer replies
show a shortening marker. Text is plain, using Glaze's Latin-1 fonts;
unsupported glyphs become `?`. Responses stream into a provisional reply
while the network worker keeps the UI responsive.

Tap **Stop** to cancel locally. The server may already be processing the
request. An unanswered user message remains available for **Retry**; the app
does not silently repeat a generation after a connection failure. A single
retry after an explicit HTTP 401 is allowed following successful token
refresh, before any reply text has arrived.

Home cancels in-progress work and saves the draft and conversation before
suspending. Reopening resumes the app. **New** asks before clearing the
conversation. **Sign out** erases the app's saved credentials and memory
copy; it leaves the conversation and any separate transfer file intact.
This is local sign-out, not revocation of every OpenAI session.

Credentials stay in memory when **Keep signed in** is off. With it enabled,
they are saved under the installed bundle's `data/auth` using a temporary
file and rename, including rotated refresh tokens. Glaze has no encrypted
keychain or per-app filesystem isolation: saved credentials are plaintext
on device storage. Change this preference while signed out. Interrupted
token exchanges/refreshes can require signing in again; they are not
automatically retried with a possibly consumed one-time token.
Persistence requires a persistent filesystem; a GlazeBoot RAM filesystem
loses saved sign-in and conversation data when the device is reset.

## Protocol and limits

Protocol behavior was recovered from OpenAI's published Codex source at
[`84bef6ea8c747f01216df126bbb40fbc12a8adb1`](https://github.com/openai/codex/tree/84bef6ea8c747f01216df126bbb40fbc12a8adb1),
particularly `codex-rs/login/src/device_code_auth.rs`, OAuth client/server,
token data, model-provider configuration and response SSE handling.

- Device code: `auth.openai.com/api/accounts/deviceauth/usercode` and
  `/api/accounts/deviceauth/token`.
- Code exchange/refresh: `auth.openai.com/oauth/token`, the published Codex
  public client id, PKCE verifier and device callback URI.
- Chat: `chatgpt.com/backend-api/codex/responses`, bearer and account-id
  headers, `store:false`, `stream:true`, no tools.

OAuth authority is fixed in the release package. JWT metadata supplies
expiry/account information; it is not treated as a locally verified
identity or used to choose an arbitrary server. The service validates the
bearer. Imported provenance, account consistency and header values are
checked. API-key records and unsupported FedRAMP routing are rejected.
Refresh preserves the selected account and accepts rotated tokens.

JSON documents, nesting, strings, SSE event sizes and total response bytes
are bounded. Auth/import JSON is limited to 32 KiB, events to 64 KiB, and
stream transfer bytes to 512 KiB. A reply requires a terminal Responses
event; truncated streams and malformed JSON are errors. The HTTP API path
rejects compressed responses, controls in authentication headers, and
redirects. Existing browser requests keep their original behavior.

Codex account endpoints are not a promise of a stable general third-party
ChatGPT OAuth API. Changes to the service, client registration or account
policy may require updates. This implementation does not claim official
OpenAI application registration or endorsement.

## Validation

```sh
make -C host chatgpt-test
python3 -m py_compile tools/chatgpt_signin.py tests/chatgpt_oauth.py
```

Native ASan/UBSan tests exercise bounded JSON parsing and malformed inputs,
device code/pending/denied/expired states, browser and device CLI helper
arguments with isolated credential storage, import checks, token refresh
and rotation, one 401 recovery, account mismatch, usage/plan errors, TLS
certificate and host verification, chunked SSE/UTF-8, incomplete streams,
fallback final text and reply shortening. Synthetic credentials are used;
the tests never log into a real OpenAI account.

The BE-300 emulator also passed package installation, credential import,
remembered login across NAND reboot, refresh of an expired fixture token,
on-device authorization, streamed chat, sign-out, and a subsequent boot
remaining signed out. Browser script/frame regression tests passed after
the shared HTTP transport changes. These runs used synthetic local endpoints.

For emulator-only fixtures, `tests/chatgpt_oauth.py --serve 8766 --listen HOST_LAN_IP` supplies
synthetic device/token/Responses endpoints. A diagnostic package can be
built with `CHATGPT_TEST_ORIGIN=http://HOST_LAN_IP:8766`; a normal package
rebuild always recompiles the transport without that override. Never
import a real account into a diagnostic fixture build.

Live OpenAI sign-in could not be verified in this workspace: its network
proxy rejects connections to `auth.openai.com` and `chatgpt.com`. Local
protocol tests do not establish real account eligibility or that OpenAI
will accept this client. Physical-device sign-in remains to be tested.
