# AI assistant delivery: model hosting and cloud back end

Research date: 2026-11-14. Short, targeted web research. "VERIFIED" = read from a primary page or tested by me; "UNVERIFIED" = secondary source, search snippet, or not found. Not legal advice.

## 1. Hosting model files on GitHub

| Option | Real limits (source) | Verdict |
|---|---|---|
| Actions artifacts | Default retention 90 days; download needs read access to the repo (https://docs.github.com/en/actions/how-tos/manage-workflow-runs/download-workflow-artifacts). VERIFIED | Unsuitable: expires, needs auth. |
| Releases assets | Each asset < 2 GiB; no limit on total release size; up to 1000 assets per release; no bandwidth cap listed (https://docs.github.com/en/repositories/releasing-projects-on-github/about-releases). VERIFIED | Usable, only as split parts (< 2 GiB each). |
| Git LFS | Free quota 10 GiB storage + 10 GiB bandwidth per month; downloads by anyone count against the repo owner; when exceeded LFS is blocked until next month (https://docs.github.com/en/repositories/working-with-files/managing-large-files/about-storage-and-bandwidth-usage). Max file 2 GB on Free. VERIFIED | Unsuitable: about 3 downloads of a 3 GB model per month. Forks/clones also consume it. |
| GHCR (OCI blobs) | Container storage/bandwidth "currently free", with at least one month notice before change; public packages pull anonymously (https://docs.github.com/en/billing/concepts/product-billing/github-packages). Per-layer size limit not found. Pull needs an OCI token flow, not a plain URL. | Technically possible, awkward, policy may change. Not recommended. UNVERIFIED on blob limits. |

Anonymous release download and Range: public-repo release URLs work with no login (302 to release-assets.githubusercontent.com, signed short-lived URL). I tested a public release asset with `Range: bytes=0-99` and got `206 Partial Content`, `Accept-Ranges: bytes`, `Content-Range` (tested from this PC, not from China). Resume therefore works; the 302 target URL expires (about 1 hour), so a resuming client must re-request the github.com URL for a fresh redirect. Rate limit for anonymous release downloads: not documented; I found no number. Acceptable use for large binaries: the release docs set no total cap; I did not find an AUP clause on it. Treat model files as tolerated but not guaranteed.

Split design: 2 parts for E2B (2.59 GB: for example 2 x 1.3 GB), 2 parts for E4B (3.65 GB: 2 x 1.83 GB; each < 2 GiB = 2.147 GB), plus `manifest.json` with part URLs, sizes, SHA-256 of each part and of the joined file. Join by streaming append into the final file, verify the final SHA-256.

Reachability from mainland China: no systematic measurement found. Evidence is anecdotal: slow downloads of about 10 KB/s reported on old forum posts; GreatFire.org reports github.com release pages as "partly disrupted" in mainland China as of 2026-04-10 (https://en.greatfire.org/https/github.com/tiann/KernelSU/releases). UNVERIFIED for release-assets.githubusercontent.com in 2026. Release assets sit on cloud blob storage, whose reachability varies by ISP and time. Plan for GitHub as a fallback that "often works, sometimes slow", never primary for China. Ghproxy-style mirrors are third-party: they can inject or swap files, disappear, and violate GitHub ToS; with a pinned SHA-256 the integrity risk is covered, but availability and privacy (IP and download logging) are not. Do not ship one as a default.

## 2. Better sources in China, same official weights

Found:
- Google's LiteRT-LM docs list Gemma 4 E2B (2.58 GB) and E4B (3.65 GB) as `.litertlm`, hosted at Hugging Face `litert-community/gemma-4-E2B-it-litert-lm` and `...E4B-it-litert-lm`; docs say Gemma 4 is Apache-2.0 (https://developers.google.com/edge/litert-lm/models/gemma-4). VERIFIED (docs). The E2B file `gemma-4-E2B-it.litertlm` is 2,588,147,712 bytes; HF serves it by 302 to a signed CDN URL (tested, anonymous, no token). Google also publishes chip-specific variants (Tensor G5, Qualcomm, Intel): pick the generic file.
- ModelScope pages exist with titles for `google/gemma-4-E2B-it` and `litert-community/gemma-4-E2B-it-litert-lm` (page fetch returned only titles; file list, uploader (official or a copy), and licence NOT confirmed). The ModelScope download doc "模型的下载" lists topics: resolve URL format, anonymous download, resume, rate limits, concurrency advice (https://www.modelscope.cn/docs/models/download) but I could not read the body, so exact limits are UNVERIFIED. My test of a guessed resolve URL returned a plain 200 (likely a page, wrong branch name); the real URL pattern must be checked by the developer.
- Qwen3-VL-2B/4B base weights: released 2025-10-21 by Qwen, model cards point to ModelScope and HF (secondary). Official Qwen GGUF for Qwen3-VL: not confirmed. MNN: taobao-mnn publishes MNN models (a Qwen3-VL-2B EAGLE-3 draft model on HF, Qwen3.5-2B MNN); an official MNN Qwen3-VL-2B was not found. UNVERIFIED.
- hf-mirror.com: widely recommended third-party mirror, operator unclear (one source says Tsinghua, others "community"), no current status evidence (UNVERIFIED). Hugging Face itself is commonly slow/blocked in China (many Chinese guides; no primary source). Gitee/GitCode: GitCode has `hf_mirrors` copies of HF repos (seen in search); they are third-party copies, usable only with SHA-256 pinning. Terms of hot-linking and rate limits for ModelScope/GitCode: not found.
- Cloud object storage prices (Alibaba OSS/CDN, Tencent COS): the price pages are JS-rendered and returned no numbers; I did not get official per-GB figures. Alibaba's billing doc confirms outbound traffic is pay-as-you-go and can be offset by traffic packages (https://help.aliyun.com/zh/oss/product-overview/billing-overview). Last I know (not verified now), mainland outbound traffic is around CNY 0.2-0.5 per GB; a 3 GB model costs the developer about CNY 1 per download. 1,000 downloads = about CNY 1,000. Check the price page before deciding. This is the reason to prefer ModelScope (free to the developer) and to cap use.
- Default domain restriction: from 2025-12-22 10:00, Buckets created in certain mainland regions (including 华北6, 华南2, 华南3, 华中1) serve some file types from the default domain with `x-oss-force-download: true` and `Content-Disposition: attachment`; the official fix is binding an ICP-filed custom domain (https://help.aliyun.com/zh/oss/how-to-ensure-an-object-is-previewed-when-you-access-the-object). For a model binary, forced download is harmless, but a custom domain on a mainland CDN requires ICP filing (an individual can file; takes days to weeks). Default OSS domains may have other restrictions I did not find. UNVERIFIED.

Redistribution licences: Gemma 4 is stated as Apache-2.0 by Google (VERIFIED on docs); redistribution is allowed with the licence text and notice. Third-party fine-tunes on HF carry other licences (one listed Gemma Terms of Use): only mirror Google's own `litert-community` files. Qwen3-VL weights: Apache-2.0 is the user's premise; I did not re-check the model card. Confirm each repo's LICENSE file and ship it plus a NOTICE in the app (and alongside any copy you host).

### Recommended delivery design

Source order (app tries each; first success wins):
1. ModelScope official or litert-community repo resolve URL (primary for China).
2. Hugging Face `litert-community` resolve URL (primary outside China; test reachability with a 5 s HEAD).
3. GitHub Release parts (manifest + 2 parts).
4. User-imported file (system file picker; verify SHA-256 before accepting).
Optional: (5) your own OSS/COS/CDN bucket with ICP domain, only if volume justifies the cost.

Rules:
- Manifest in the app (signed or compiled in): model id, version, per-source URLs, size, SHA-256 of the final file (and per part for GitHub). Pin the hash in the app build; a source returning a different hash is rejected and deleted.
- HTTP Range resume into a `.part` file; keep ETag/length check; retry with backoff; re-resolve redirects on resume (signed URLs expire). Single connection by default (ModelScope reportedly advises limited concurrency; UNVERIFIED).
- Wi-Fi only by default, with an explicit "use mobile data" confirmation showing the size; storage check (needs 2x only if joining parts; stream-append avoids it, so require size + 300 MB free); download as a foreground service (Android) / background URLSession (iOS); store in app-private storage; allow delete.
- Show source and licence text in the settings screen.
Developer must set up: a manifest and hash for each model, GitHub Release parts and a release script, a monitor job that re-checks the source URLs and hashes (HEAD request on a schedule) on each model update, licence/NOTICE files, and a user-import path for offline use. Maintain: re-publish when the runtime format changes (LiteRT-LM versions can break `.litertlm` compatibility; pin runtime and file version together).

## 3. Cloud back end

### 3.1 Can a user spend a ChatGPT subscription in a third-party app?

The owner is partly right. As of November 2026:
- Codex CLI/IDE "Sign in with ChatGPT" is for Codex use under the subscription; API-key mode is billed at API rates (https://learn.chatgpt.com/docs/auth; formerly developers.openai.com/codex/auth). The page does not authorise third-party apps.
- OpenAI has a separate program, "Sign in with ChatGPT", and "plan usage": Plus and Pro users let an approved app send eligible requests to the Responses API (`https://api.openai.com/v1`) on their plan allowance, no API key (https://developers.openai.com/siwc/quickstart). VERIFIED from OpenAI's pages. Status: "currently available to selected commercial partners through a limited trial"; plan usage is open to all open-source partners and selected private clients; others get a waitlist/interest form. Press reports a DevDay launch on 2026-09-29 with 16 partners (UNVERIFIED, trade press).
- Technical contract (OpenAI docs, VERIFIED): OAuth authorization-code + PKCE (S256); first registration with `client_id=dynamic_agent_client`, the issued client ID is saved per account; loopback redirect `http://127.0.0.1:<port>/...` only documented, no custom scheme or mobile redirect described; no client secret. Requests must set `store:false` and `stream:true`, no system-role items, no `temperature`/token limits; function tools and images work if the model accepts them; no hosted MCP, file search, `tool_search`. Token lifetime 3600 s, refresh 30 days (from WorkOS write-up, secondary). Users can cap weekly use per app.
- Gaps: no documented mobile/native support, no region statement, no commercial app approval criteria. Mainland China is not a supported region for OpenAI services, so Chinese users likely cannot use it anyway.
- Reusing the Codex OAuth client or copying `~/.codex/auth.json` tokens from a third-party app: I found no OpenAI terms text allowing it. Secondary sources conflict (OpenClaw docs say it is supported; a Puter tutorial says no official surface lets a subscription pay for third-party model calls and that such tools mimic the Codex request shape). The Codex page says credentials are "like a password". Do not do this; use the official program if eligible, otherwise BYOK. UNVERIFIED on exact terms.
- Apps SDK / ChatGPT apps is the reverse direction (your app inside ChatGPT); not researched in depth; not relevant to a standalone photo editor.

Decision: Dapple (closed phone app, probably commercial, no server) is unlikely to qualify soon. File the interest form (cheap), but do not depend on it.

### 3.2 BYOK for OpenAI API

Prices per 1M tokens, standard tier (https://developers.openai.com/api/docs/pricing, VERIFIED): gpt-5.6-luna $0.20 in / $1.20 out; gpt-5-nano $0.05 / $0.40; gpt-5.4-mini $0.75 / $4.50; gpt-5.4 $2.50 / $15; gpt-5.6-terra $2 / $12; gpt-5.5 $5 / $30.
Cost per photo, assumption: a 1024 px image is about 1.1k input tokens (tile-based estimate; page gives no formula, UNVERIFIED) plus about 1k prompt/tool schema and growing context; 3 rounds, about 1.5k output total per my brief (plus reasoning tokens, which are billed as output and can multiply it). Roughly 9k input + 1.5-4k output over 3 rounds:
- luna: about $0.002 + $0.002-0.005 = under $0.01
- gpt-5.4-mini: about $0.007 + $0.007-0.018 = $0.01-0.03
- gpt-5.4: about $0.02 + $0.02-0.06 = $0.04-0.08
These are estimates, not measurements; measure real token usage.
Reachability: OpenAI blocked API traffic from unsupported regions from 2024-07-09; mainland China is not on the supported list (https://www.scmp.com/tech/policy/article/3267971, https://help.openai.com/zh-hans-cn/articles/5347006). VERIFIED by news and the help page. So BYOK OpenAI only works for users outside China or with a route that violates OpenAI's terms; do not design around that.

### 3.3 China-usable providers

My fetches of the price pages were mostly JS-rendered; only these are VERIFIED:
- Alibaba Bailian: `qwen3-vl-plus`, Beijing region, CNY 1 in / 10 out per 1M tokens for requests up to 32K (https://help.aliyun.com/zh/model-studio/model-pricing). Same 1k-image, 9k in/3k out job: about CNY 0.04 (estimate). OpenAI-compatible mode and function calling with images: documented by Alibaba to my knowledge, not re-verified here.
- SiliconFlow: GLM-4.5V CNY 1 in / 6 out per 1M (https://siliconflow.cn/pricing). About CNY 0.03 per job (estimate). Hosts open models; OpenAI-compatible (known; not re-verified).
- Not verified (pages did not render): Volcengine Ark Doubao vision pricing, Zhipu BigModel (GLM-4.6V/4.5V/4V-Flash; the page shows some are free, prices unseen), Moonshot/Kimi vision, MiniMax, DeepSeek vision. The Bailian model list shows `kimi/kimi-k3` and `qwen3.7-plus` under vision, so the lineup has moved: re-check before presetting. For each, a self-registered user can normally paste an API key (real-name verification applies on Chinese platforms); confirm per provider.

### 3.4 Recommendation

- One setting: "OpenAI-compatible endpoint": base URL, model name, API key, plus presets (DashScope, Ark, Zhipu, SiliconFlow, Moonshot, OpenAI, custom). Keys in Android Keystore/iOS Keychain, never in the repo. Capability test button: sends a tiny image + a trivial tool call to confirm vision + tool calling before enabling.
- Consent: first-use dialog that names the provider and host, says exactly what is sent (a downscaled preview about 1024 px, EXIF/GPS/metadata stripped, plus measured statistics such as histograms; no original file, no location, no filename), says the photo leaves the device and is subject to that provider's terms, and lets the user preview the exact payload. Per-use indicator (cloud badge while a request is in flight, a visible list of sent items afterwards), a master off switch, and default off. Local model path must work without any of this.
- Store policy: App Store requires disclosure of data sent to third parties and AI features (check current guideline text); Google Play data safety form likewise. In mainland China, generative AI services offered to the public are subject to the generative AI interim measures and the algorithm/model filing regime: if Dapple only forwards a user's own key to a third-party provider (no service you operate), your exposure is probably lower, but if you bundle a provider and offer it to the public, ask a lawyer. Chinese Android stores also require ICP filing for the app (备案) and a privacy policy. Flag only; not researched in primary sources.

## Open items to check before building
1. ModelScope: exact resolve URL, uploader identity and licence of the Gemma 4 / Qwen3-VL repos, rate limit values.
2. Real China download speeds for ModelScope vs release-assets.githubusercontent.com (measure from a Chinese ISP).
3. Alibaba OSS/CDN and Tencent COS price pages (current per-GB).
4. Volcengine, Zhipu, Moonshot, MiniMax, DeepSeek prices and image+tool-calling support.
5. Fill the SIWC interest form and read `/siwc/token-sharing-open-source/*` pages for mobile support.
