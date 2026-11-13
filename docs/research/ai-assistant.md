# AI 助手 (on-device multimodal model that drives the existing parameters): research notes

Date 2026-11-14. Web research plus repo reading, no code, no devices. Legend: **[V]** verified against a primary source fetched today (model card, GitHub/HF API, vendor doc); **[S]** secondary source (blog, aggregator, forum); **[M]** from memory or inference, not verified today; **[?]** could not verify. Phone numbers below are vendor numbers on other phones, not measured by us.

## 0. One-page summary

**What the app already has (repo).** The "pad + strength" model (`app/lib/model/pad_model.dart`, `docs/design/editor-panel.md` section 2) turns four categories (光线, 颜色, 胶片, 质地) into 2 pad axes (x, y in -1..+1, centre = auto) plus one strength `a` (0..1) each: 12 numbers that already map to about 25 engine parameters through `padRecipe(cat, x, y, a)`, a pure function that is range-safe by test. Behind it `core/src/edit_params.cpp` exposes about 110 keys (light 7, colour 4 + 8 HSL bands x 3, curves, effects 5, detail 4, grading 4 zones x 3 + blend/balance, film 9, look/tone/lut strengths, straighten). The engine already measures: scene key and metering (`solveMeter`, `sceneKeyEv`, pipeline-architecture section "1. Metering"), per-file exposure anchor (section 29), auto tone from a 256 px proxy (`toneStrength`), scene-adaptive single-shot HDR (section 33), modern renderings 通透/鲜明/柔和 (section 37), local masks and AI masks for person parts, face features, sky and depth (sections 40-41). So the assistant would sit on top of a deterministic auto that already does the numerically hard part (exposure, tone).

**Models.** The licensing picture changed in 2026: **Gemma 4 E2B/E4B (2 April 2026) are Apache-2.0** [V: HF API, model card], with native function calling and image budgets of 70-1120 tokens, and Google's LiteRT-LM (Apache-2.0, C++/Kotlin stable, Swift preview, GPU+NPU) publishes phone numbers: E2B 2.58 GB file, **S26 Ultra GPU prefill 3808 tok/s, decode 52 tok/s, TTFT 0.3 s**; E4B 3.65 GB, **1293 / 22 tok/s, TTFT 0.8 s** [V]. Qwen3-VL 2B/4B/8B and **Qwen3.5 0.8B/2B/4B (natively multimodal, March 2026)** are Apache-2.0 [V: HF API] and have the best Chinese. MiniCPM-V 4/4.5 weights are Apache-2.0 now [V]. Excluded for licence: FastVLM (Apple research-only [V]), Qwen2.5-VL-3B (qwen-research [V]), JarvisArt (non-commercial [V]), Gemma 3/3n (custom Gemma terms [V]), FiveK data (research-only [V]).

**Runtimes.** Best fit for Dapple: **LiteRT-LM** (same family as the LiteRT already in the app; Apache-2.0; Gemma 4 E2B/E4B, also Qwen/Phi-4/Llama; vision, function calling, constrained decoding [V], no Play-services dependency mentioned but not proven [?]) or **llama.cpp (MIT)** with libmtmd (any GGUF VLM, GBNF/JSON-schema grammar, Qwen3-VL supported since Oct 2025 [V mirror], Qwen3.5 support [?]). The NPU route (Qualcomm GenieX, BSD-3, new June 2026; AI Hub bundles) gives the best Qwen3-VL-4B numbers (TTFT 0.07-3.4 s, 16-21 tok/s on 8 Elite [V]) but the page says "Genie support will be deprecated soon", the bundle redistribution terms are unclear, and OPPO ColorOS availability is unproven [?]. Our own phone note (`ai-runtime.md`) shows the LiteRT GPU delegate paying 27-73 s of first-load shader compile without a cache: this is the main LiteRT-LM risk to test first.

**Task evidence.** Papers exist and work: MonetGPT (Adobe, SIGGRAPH 2025: visual puzzles to teach operations), JarvisArt (NeurIPS 2025: Qwen-VL based, 200+ Lightroom ops, SFT + GRPO-R), PhotoArtAgent and RetouchLLM (closed loop with a VLM critic), PerTouch (2-3 feedback rounds). All use 7B+ models or cloud VLMs, none targets a 2-4B phone model, and the only weights with a retouching skill (JarvisArt) are non-commercial. No source I found measures small-VLM colour-cast detection or numeric parameter accuracy: that is the gap our own prototype must measure.

**Honest judgement.**
1. *Feasible technically*: yes on a 16 GB Snapdragon 8 Elite. A Gemma 4 E2B (or Qwen3.5-2B) call with one 280-560 token image and about 1000 tokens of measured-facts text prefills in 0.3-1 s and generates a 150-token tool call plus a 60-token rationale in 3-6 s. E4B: 1-2 s prefill, 8-12 s generation. Three rounds are 15-20 s (E2B) or 40-60 s (E4B) [estimates from the vendor numbers, M].
2. *Can a 2-4B VLM beat the deterministic auto at exposure/tone?* Almost certainly not, and it should not try: the engine measures better than any VLM sees (small VLMs read a 0.3-1 MP image with lossy tokens; they are weak at counting percentiles and colour casts [M, plausible, unverified]). Where it can add value: (a) *semantic decisions the deterministic code cannot make*: portrait vs landscape vs night vs product, mood/look/film-stock choice, "this sunset should stay warm, do not neutralise", "protect the skin"; (b) *personalisation* from the user's past edits as few-shot context (strongest argument, not provided by any regressor trained on FiveK); (c) the *explanation* in Chinese. (d) A tiny regression head or retrieval over the user's own edits does (b) numerically better and cheaper than an LLM: it must be the baseline the LLM has to beat.
3. *Recommendation*: do not let the model emit absolute values. Let it choose **offsets relative to the deterministic auto, in the 12-number pad space plus look/film-stock choice and 0-3 AI-mask area actions**, clamp them, and require the result to pass guards. Expect "a small, tasteful, personalised delta + rationale", not a leap in quality. Treat it as a premium feature whose value is taste matching and explanation; run the A/B (section 5g) before building any UI. Honest probability that a blind panel prefers E2B/E4B-assisted output over the existing auto on a general RAW set: low-moderate (maybe 40-55 % of images a wash, a few better, some worse: [M] guess); on the owner's own taste with few-shot history: higher but unproven.

---

## 1. Candidate models

All size/speed rows: licence/params from HF API [V] unless stated. "4-bit disk" = my estimate (0.55 GB per billion parameters incl. vision encoder, Q4_K_M-like) unless a vendor file size is given. RAM = weights + KV + vision encoder + activations; vendor peak numbers where given.

| Model (date) | Params | 4-bit disk / RAM | Vision input | Chinese | Tool calling / structured | Published phone speed |
|---|---|---|---|---|---|---|
| **Gemma 4 E2B** (2026-04-02, HF card updated 2026-11-03) | 2.3B effective, 5.1B with embeddings; ~150M vision enc. [V] | **2.58 GB** `.litertlm` [V]; peak CPU mem 676 MB (GPU) to 1733 MB (CPU) on S26 Ultra [V] | token budget 70/140/280/560/1120 per image, variable aspect [V] | 140+ languages pretrained, 35+ "out of the box"; Chinese not singled out [V]; quality for 2-sentence 中文 unverified | native structured tool use, thinking mode `<|think|>` [V] | S26 Ultra GPU prefill 3808, decode 52 tok/s, TTFT 0.3 s; CPU 557/47, 1.8 s; iPhone 17 Pro GPU 2878/56, 0.3 s [V] |
| **Gemma 4 E4B** | 4.5B eff., 8B with emb. [V] | **3.65 GB** [V]; peak 710 MB (Android GPU) to 3283 MB (CPU) [V] | same | same | same; Tau2 42.2 vs 24.5 for E2B [V] | S26 Ultra GPU 1293/22 tok/s, TTFT 0.8 s; CPU 195/18, 5.3 s; iPhone 17 Pro GPU 1189/25, 0.9 s [V] |
| Gemma 3n E2B/E4B (2025-07) | 5B / 8B raw, 2B/4B effective [S] | about 3 / 4.4 GB [M] | 256/512/768 px normalised [S: NVIDIA listing] | multilingual [M] | no native tool tokens (prompted JSON) [M] | superseded by Gemma 4; no numbers fetched |
| Gemma 3 4B / PaliGemma 2 | 4B / 3B | ~2.5 GB | 896 px SigLIP, 256 tokens [M]; PaliGemma: fixed 224/448 | ok | no native tools; PaliGemma is a transfer model, not chat | llama.cpp supports Gemma 3 mtmd [V mirror] |
| **Qwen3-VL-2B / 4B / 8B Instruct** (2025-10) | 2.13B / 4.44B / ~8.8B (safetensors totals [V]) | ~1.3 / 2.5 / 5 GB + mmproj 0.4-0.8 GB [M] | dynamic resolution, about 1 token per 32x32 px [M], configurable max pixels | best of the set [M: Qwen] | JSON tool calls via chat template [M]; strong constrained-JSON track record [M] | Qualcomm card, 4B, 8 Elite for Galaxy: QAIRT w4a16 NPU decode 20.9 tok/s at ctx 512, 16.3 at 4096, TTFT 0.07-3.4 s; llama.cpp q4_0 17-21 tok/s at 512 ctx, TTFT 0.1-4.4 s [V]. Arm learning path, 2B Q4_K_M+Q8 mmproj on ROG Phone 6D (old SoC): 25 s image+prompt eval, 14.9 tok/s [V] |
| **Qwen3.5-0.8B / 2B / 4B** (2026-03-02) | 0.87B / 2.27B / 4.66B incl. vision [V] | ~0.6 / 1.3 / 2.6 GB [M] | native multimodal (image-text-to-text); tokens/image not stated on card [V] | "201 languages and dialects", C-Eval reported [V] | "excels in tool calling"; non-thinking default for 2B; thinking mode loops on 2B [V] | none found; architecture is hybrid Gated DeltaNet + attention [V]: llama.cpp/MNN/LiteRT support for it **[?]** |
| Qwen2.5-VL 3B / 7B | 3B / 7B | | | good | Hermes-style | 3B is `qwen-research` licence (HF API [V]), 7B Apache-2.0 [V]; 72B Qwen licence [M] |
| **SmolVLM2 2.2B / 500M / 256M** (HF updated 2026-11) | as named | 1.3 / 0.35 / 0.2 GB [M] | 384-512 px tiles, few tokens [M] | English-centric [M]: weak for Chinese | no native tools | llama.cpp supports SmolVLM [V mirror]; no phone numbers fetched |
| MiniCPM-V 4 (4B) / 4.5 (8B) | [M] | ~2.5 / 5 GB [M] | up to 1.8 MP, strong token compression [M] | strong [M] | prompted JSON | card says weights+code Apache-2.0 and a *voluntary* registration questionnaire [V]; MiniCPM-V 2.6 older, gated, MiniCPM licence [V gated flag] |
| Phi-4-multimodal (5.6B) / Phi-3.5-vision (4.2B) | | ~3 GB | | Chinese weak-moderate [M] | function calling in Phi-4 [M] | MIT [V]; LiteRT-LM lists Phi-4 [V] |
| InternVL3.5-4B / InternVL3-2B | | | 448 px tiles, 256 tokens [M] | good [M] | prompted | 3.5-4B Apache-2.0 [V]; InternVL3-2B card shows apache-2.0 *and* license_name `qwen` [V]: read the model card before use |
| LLaVA-OneVision 0.5B | 0.5B | | | Qwen2 base | | Apache-2.0 [V]; too weak for this task [M] |
| Moondream2 | 1.9B | ~1.2 GB | 378 px [M] | English only [M] | none | Apache-2.0 per HF metadata [V]; not suited |
| **FastVLM** (Apple) | 0.5-7B | | very fast encoder | | | **`apple-amlr`: research purposes only, "does not include any commercial exploitation, product development"** [V: LICENSE_MODEL]. Excluded. |
| Phone-vendor open models (OPPO, Xiaomi, vivo) | | | | | | none with an open licence found **[?]** |

Notes. (1) The Gemma 4 numbers are for a Samsung S26 Ultra (SoC not stated on the page [V]); the Find N5 has a Snapdragon 8 Elite (previous generation to the S26 Ultra's chip if that is Elite Gen 5 [M]), so expect somewhat slower. A Redmi-class mid phone dropped to 8-12 tok/s decode in a developer guide [S]. (2) Gemma 4 E2B/E4B with MTP drafters are reported 1.6x/2.2x faster decode [S: InfoQ via search]; not required. (3) Image tokens matter more than model size for latency only up to about 560 tokens; for photo assessment 280-560 is enough, because the task is global look, not OCR.

## 2. Runtimes

| Runtime | Licence | Multimodal | Constrained / tool output | Android / iOS | GPU / NPU | Fit |
|---|---|---|---|---|---|---|
| **LiteRT-LM** (google-ai-edge) | Apache-2.0 [V] | vision + audio [V]; Gemma 4 E2B/E4B, Gemma 3n, Qwen, Llama, Phi-4 [V] | function calling [V]; v0.19 "accelerated logit masking for constrained decoding" [V]; schema format not verified [?] | Kotlin stable, C++ stable, Swift early preview, Python stable; Flutter community [V]; MediaPipe LLM Inference is maintenance-only, migrate to LiteRT-LM [V] | GPU and NPU [V]; NPU for Gemma on Snapdragon not found [?] | Best first test: same family as the LiteRT 1.4 already shipped; C++ API fits our engine; official phone numbers. Risks: first-run GPU compile time (our own measurement 27-73 s for other models in `ai-runtime.md`), Play-services independence not stated **[?]**, binary size not stated **[?]** |
| **llama.cpp** + libmtmd | MIT [V] | image/audio (video listed in newer docs) via mmproj; Gemma 3, SmolVLM, Qwen3-VL listed [V mirror]; "very heavy development, breaking changes expected" [V] | GBNF grammar and JSON-schema sampling are part of the sampler chain (independent of input embeddings) [M]; combined use with mtmd not verified **[?]** | Android NDK/iOS builds, Metal; Vulkan and OpenCL (Adreno) back ends exist [M]; Arm learning path shows Qwen3-VL on Android [V] | CPU NEON/i8mm solid; GPU back ends model-dependent [M] | Best for desktop prototype (same GGUF, same grammar) and as the shipping fallback; widest model coverage; vision-encoder cost on phone CPU is the weak point (25 s on an old SoC [V]) |
| MNN-LLM (Alibaba) | Apache-2.0 [V] | image input supported [S]; exact VLM list **[?]** | no grammar found | Android/iOS, MNN Chat app [S] | CPU + OpenCL; paper: up to 8.6x prefill vs llama.cpp on CPU [V paper abstract] | Strong on Qwen family and Chinese phones; try if llama.cpp vision prefill is too slow; Qwen3.5 support **[?]** |
| MLC-LLM | Apache-2.0 [V] | VLMs supported (Phi-3.5-vision, Qwen2-VL) [M] | JSON mode [M] | Android (OpenCL), iOS (Metal) via TVM [M] | | Heavy model-compile pipeline and runtime size; lower priority |
| ExecuTorch (+QNN) | BSD-style (GitHub SPDX NOASSERTION [V]) | LLaVA/Qwen examples [M] | none built in | Android/iOS | QNN delegate needs Qualcomm's SDK and per-model compile [M] | Possible but the most engineering; skip |
| Qualcomm GenieX / Genie / AI Hub | GenieX BSD-3-Clause [V]; Qwen3-VL bundle card says apache-2.0 [V]; AI Hub bundle redistribution terms **[?]** | VLM flow in demo [V]; Qwen3-VL 2B/4B/8B bundles exist [V] | local OpenAI-compatible server; no grammar mentioned [V] | Android on Snapdragon 8 Elite / Elite Gen 5 tested on Qualcomm Device Cloud; other OEM phones not stated [V] | NPU w4a16 and llama.cpp hybrid [V]; **"Genie support will be deprecated soon"** on the Qwen3-VL card [V] | Fastest numbers but vendor-locked, Snapdragon-only, deprecation notice; ColorOS NPU access without vendor keys **[?]** (test on the Find N5 before counting on it). MediaTek: no source fetched **[?]** |
| Core ML / MLX (iOS) | Apple frameworks; MLX is MIT [M] | MLX-VLM supports Qwen/Gemma/SmolVLM [M] | | iOS | ANE/GPU | Option for iOS later; LiteRT-LM Swift preview or llama.cpp Metal is the cross-platform path |

Sharing the app's runtimes: LiteRT 1.4 and the minimal ONNX Runtime (`ai-runtime.md`: 4.3 MB + 2.8 MB per ABI) cannot run these LLMs; LiteRT-LM is a separate native library (size not verified [?]) that may share the LiteRT core. Plan a *separate, optional* native library delivered with the model (dynamic feature download), not in the base APK, so the base app stays small.

## 3. Evidence about the task

| Work | Idea | Feedback / loop | Result and limits | Licence of usable artefacts |
|---|---|---|---|---|
| **MonetGPT** (Adobe, arXiv 2505.06176, SIGGRAPH 2025) [V] | Pre-train an MLLM on *visual puzzles* built from the imaging ops so it learns what each op does, then critique + plan op sequence + parameters; executed through a function library | none at inference (explicitly "no inference-time optimisation") | direct SFT on artist sequences only partly improved because the MLLM did not understand ops; global ops only, no masks/crops; 8k expert images so artist bias; artifacts such as saturated regions [V] | paper only; no weights found **[?]** |
| **JarvisArt** (NeurIPS 2025, arXiv 2506.17612) [V] | MLLM agent drives 200+ Lightroom ops; SFT with chain-of-thought then GRPO-R with tool-use reward; MMArt-55K data | Agent-to-Lightroom protocol, rendered result | weights `JarvisArt-1208`, training and eval scripts released (Dec 2025) [V] | **JarvisArt Non-Commercial License v1.0; commercial use needs a request to the author** [V]; dataset MMArt-PPR10K tagged apache-2.0 [V] but built from PPR10K whose terms I did not check **[?]** |
| **PhotoArtAgent** (arXiv 2505.23130) [V] | VLM "artist" writes a rationale, sets Lightroom parameters, re-views the result and reflects | closed-loop reflection | cloud-size VLM; qualitative | **[?]** |
| **RetouchLLM** (arXiv 2510.08054) [V] | training-free: VLM critic vs reference, LLM writes Python to adjust | critic-generator loop | editable code, transparency | **[?]** |
| **PerTouch** (AAAI 2026, arXiv 2511.12998) [V/S] | VLM agent with scene memory and region-wise control (uses diffusion/ControlNet) | "typically converges in 2-3 rounds" [S: third-party summary]; high compute | not usable on-device | **[?]** |
| Exposure (Hu et al., SIGGRAPH 2018), Distort-and-Recover (Park 2018) | RL / white-box filters trained on FiveK | reward = similarity to expert | pre-VLM baselines [M] | unverified |
| Image-Adaptive 3D LUT (Zeng 2020) | small CNN predicts 3 basis LUTs + weights | none | real-time, tiny [M]; code Apache-2.0 [V GitHub API]; HF/GitHub dated 2023 | **code Apache-2.0 [V], but weights trained on FiveK (research-only data, see below) are a licence risk** |
| CSRNet, HDRNet, NILUT | conditional sequential modulation; bilateral grid; neural implicit LUT | | tiny nets, FiveK-trained [M] | licences **[?]** (check each repo) |

Dataset licence: MIT-Adobe FiveK page: "You can use these photos for research under the terms of the following licenses" (Adobe, AdobeMIT); 5000 DNGs plus 5 experts' Lightroom catalogues with slider values [V: data.csail.mit.edu/graphics/fivek]. So: **local academic evaluation yes; training a shipped model on it, no; do not redistribute.**

What the evidence says (honestly separating verified and inferred):
- *What worked* [V]: teaching the model what each operation does before asking it to edit (MonetGPT puzzles); RL with tool-use reward (JarvisArt); closed loop re-viewing the rendering (PhotoArtAgent, RetouchLLM, PerTouch 2-3 rounds).
- *Failure modes reported* [V]: artifacts/saturated regions (MonetGPT); artist bias; no regional edits.
- *Failure modes I expect but could not source* [M]: colour-cast blindness at 280-560 tokens (global tint is a small signal in embeddings), over-saturation bias (training sets of "pretty" edits), run-to-run inconsistency at temperature > 0, weak numeric calibration (a VLM says "+0.7 EV" with no measurement). Mitigations are in section 5: measured facts as text, constrained decoding, temperature near 0, deltas relative to auto, deterministic guards.
- *Small-model aesthetic judgement*: no direct measurement found **[?]**. The prototype must measure agreement of E2B/E4B/Qwen3.5 with human A/B choices on our own render pairs.
- Non-LLM baselines to beat: (a) the existing deterministic auto; (b) a regression/retrieval head on the 12 pad numbers trained on the owner's own edit history (no third-party data, no licence issue); (c) a 3D-LUT predictor only as a research comparator because its natural training set is research-only.

## 4. Device gating, storage, delivery

**Android capability detection** (APIs from memory unless marked): `ActivityManager.getMemoryInfo().totalMem` and `isLowRamDevice()` [M]; `Build.VERSION.MEDIA_PERFORMANCE_CLASS` (Android 12+; class 12 guarantees 6+ GB RAM, but the Android blog warns RAM alone gives no performance guarantee [V]; most Chinese phones report 0 or unreported [M], so it can only add confidence, never exclude); `Build.SOC_MODEL` (API 31) against a short allow-list [M]; `PowerManager.getThermalHeadroom` and thermal status listener (API 30+) before and during a run [M]; `StatFs` free space for 2.6-3.7 GB plus slack. Recommended policy: **gate = (totalMem >= 11 GB) AND (arm64) AND (free storage >= model + 1 GB) AND (a 10-20 s on-device self-benchmark of the chosen model reaches a minimum prefill and decode rate, cached per app version and thermal state)**. Show "unsupported" only when a hard condition fails; if the benchmark fails, hide the entry the same way. No SoC list needs maintaining if the benchmark is the authority; the RAM and arm64 checks are cheap pre-filters. Keep the model out of the engine's process memory budget: the engine already needs about 1.3-1.7 GB for full-resolution decode (pipeline-architecture section 29 table), so run the assistant before opening the heavy full-resolution session, or unload it (single in-flight rule).

**iOS**: `ProcessInfo.physicalMemory`, `os_proc_available_memory()`; entitlement `com.apple.developer.kernel.increased-memory-limit` is "available on some device models" per Apple's page and only slightly raises the kill threshold in tests [V/S: Apple developer page and forums]. A forum report puts the budget around 6 GB on both 8 GB and 12 GB iPhones [S, anecdotal]. The repo's own history shows full-resolution RAF export being killed for memory on iPhones (commit b66b957), so on iOS the assistant should be limited to Pro-class phones (8 GB+) and E2B-size models, and must unload before export. Vendor numbers show iPhone 17 Pro GPU peak 1450 MB (E2B) and 3380 MB (E4B) [V]: E4B on iOS is risky.

**Delivering 2.6-3.7 GB without Google Play** (no server exists):
- *In-app download from object storage + CDN the developer rents*: resumable ranged download, SHA-256 check, per-file split into 256 MB parts. Ballpark (Alibaba Cloud, third-party 2025 summaries, [S]): OSS outbound traffic about 0.25 CNY/GB off-peak, 0.5 CNY/GB peak; 100 GB package about 295 CNY/year; CDN mainland as low as 0.15 CNY/GB. Example: 10,000 downloads x 3 GB = 30 TB, about 4.5-15 k CNY. Check the official price pages (not fetched [?]). Cost is per download, so cap re-downloads and keep the file per app version.
- *ICP*: serving to mainland users from a mainland CDN generally requires the domain to be ICP-filed (and the app itself may need app filing) [M]; Hong Kong/overseas nodes avoid the filing but are slower and priced differently [S]. Flag only, no legal advice.
- *Hugging Face directly*: unreliable/blocked from mainland [M]; mirrors are third-party. ModelScope hosts Qwen/Gemma families [M], terms for app-initiated download **[?]**.
- *User-side import*: "choose a model file" via the system file picker; needed anyway as an offline fallback and for open-source users; also solves the open-source-repo case (the repo ships no weights).
- *iOS*: Background Assets framework / On-Demand Resources can host large packages on Apple's servers [M, not verified today]; this also needs checking for availability in China.
- *Redistribution licences*: Apache-2.0 for Gemma 4 / Qwen3 / Qwen3.5 / MiniCPM-V 4 / SmolVLM2 allows hosting converted files if the licence and NOTICE are kept [M]; Gemma 3/3n custom terms carried a prohibited-use policy and "pass-on" conditions [M], which is another reason to prefer Gemma 4. Quantised conversions (.litertlm, GGUF) of Apache models are derivatives under the same licence.

## 5. Recommended architecture for Dapple to evaluate

### (a) Tool schema (small, mapped to pads; pinned parameters are removed)

One function `apply_edit`, schema generated at call time from the app's param specs, with the user's pinned keys deleted from it:

```
apply_edit({
  light:   {x:-1..1, y:-1..1, a:0..1},    // pad: x 暗-亮, y 反差; a = 自动调光强度
  color:   {x:-1..1, y:-1..1, a:0..1},    // x 冷-暖, y 淡-浓; a = 风格强度/鲜艳加重
  film:    {x:-1..1, y:-1..1, a:0..1},    // optional; shoot/print + effect strength
  texture: {x:-1..1, y:-1..1, a:0..1},
  look:    enum[std, 通透, 鲜明, 柔和, ...],   // rendering base (section 37)
  film_stock: enum[none, ...stocks],
  areas:   [{kind: enum[face, skin, sky, background, person], op: enum[lift, darken, warm, cool, soften, clarity], amount: -1..1}]  // max 3, built on AI masks (sections 40-41)
})
```
About 14 numbers + 2 enums + up to 3 areas, versus 110 engine keys. Reasons: the pad recipe is range-safe by construction ("no setting looks bad"), the model emits few numbers (fewer places to hallucinate), and `padRecipe` stays the single mapping, so assistant output is the same as a user dragging the pads (undo, reset, UI display all free). Pinned parameter handling: if the user pinned `exposure`, remove the `light.x` field (or bound it to 0 and subtract its contribution); the pad cell shows the pad value that results; resolve at the *pad* level (pin a whole category, or a pad axis) to keep schema simple. Values are *offsets from the deterministic auto* (centre = 0 = auto) with the allowed range shrunk (e.g. +-0.5) on the first version. Fine parameters (HSL bands, curves, grading) are out of scope for v1.

Constrained decoding: JSON-schema/GBNF (llama.cpp) or LiteRT-LM's constrained decoding and native tool-call format [V that both exist; exact schema support of LiteRT-LM **[?]**]. Numbers quantised to steps of 0.1 (enum of strings or a regex) to reduce token count and jitter. A `rationale_facts` field is *not* free text from the model: the model outputs `reasons: [enum ids]` (e.g. `under_exposed_face`, `blown_sky`, `warm_cast`, `flat_contrast`), the app composes the Chinese sentences from templates with the measured numbers, and an optional free-text pass is allowed only for a short sentence (see e). Guards then run on the final result (c).

### (b) Perception inputs
1. Preview rendered by the engine at the model's budget: 280-560 tokens (roughly 640-900 px long edge for Gemma 4 [M]); one image only in round 1, plus the *new* preview in later rounds. Optionally a second image: the 256 px histogram plot (cheap tokens; VLMs read plots poorly [M], so only as redundancy).
2. Text facts from the engine (cheap, exact, already computed): luminance percentiles p1/p5/p50/p95/p99 in stops relative to scene key, clipped fractions per channel (highlight/shadow), scene key and exposure anchor EV, white-balance estimate and the measured colour-cast vector (a*, b* of the neutral-weighted mean), mean/p90 chroma, face boxes with face-mean luminance and skin-tone hue/chroma (from the AI-mask face detector), sky fraction and its clipping, depth/foreground split, noise estimate (ISO from EXIF), EXIF (ISO, focal length, exposure time, lens, time of day if present), current pad state and which parameters are pinned, the deterministic auto's own proposal and why. About 400-800 tokens.
3. Taste profile (d), about 150-300 tokens.

### (c) Loop
Round 0: deterministic auto result (never worse than today). Round 1: model proposes deltas; engine renders the preview; **deterministic guards** run: no face clipping (face p95 < clip point), highlight clipping not increased beyond a bound, chroma cap (over-saturation bias), shadow floor, WB shift cap (+-X), pad range clamped, no change to pinned parameters; violations are *clamped* (not re-asked) and noted. Round 2-3 (hard cap 3, stop early when the model returns "no change", delta < epsilon, or the metrics already satisfy the target band): the model sees the new preview + metrics diff, outputs further deltas. A final "revert to auto" option: if the model's own scoring is not better or guards fire twice, keep round 0. Temperature 0-0.3, seed fixed, so run-to-run output is reproducible for the same image and state.

### (d) Personalisation
Store, per completed edit session, a row `(scene_type, measured facts bucket, final pad state, kept/undone)`. Scene types: deterministic buckets from engine facts and AI masks (portrait, landscape, night, backlit, high-contrast...). Compact taste profile: median pad position per category and scene type (+ spread), the owner's most used look/film stock, and 3-5 recent nearest-neighbour edits as few-shot lines of text ("scene: backlit portrait, face -1.6 EV vs key: you chose light x +0.3, y -0.1, color x +0.2"). This is the strongest argument for an LLM in the loop: it can combine a measured fact and a stated preference in context without training. Counter-argument and required baseline: a kNN/median-regression over the same rows reproduces most of that without an LLM; the LLM must beat it in the A/B (g). Profile stays on-device; no cloud, no account.

### (e) Output
Two sentences in Chinese, generated *after* the final state, from the final measured facts and the delta (ids of reasons + numbers), e.g. "人脸偏暗约 1.2 档,已提亮并压低高光,避免天空过曝。整体偏冷,已略微加暖保留黄昏氛围。" Sentences must reference only facts present in the prompt; check numbers against the facts programmatically and fall back to template text on mismatch. Qwen3.5/Qwen3-VL are the safer Chinese writers; Gemma 4 E2B Chinese quality is unverified **[?]** and must be tested (a fallback is template-only rationale).

### (f) Expected latency, memory, battery (Snapdragon 8 Elite, 16 GB), derived from vendor numbers above [M]
| | E2B (Gemma 4, LiteRT-LM GPU) | E4B | Qwen3-VL-4B (QAIRT NPU / llama.cpp) |
|---|---|---|---|
| prefill per round (~1.5 k tokens incl. image) | 0.4-1 s | 1.2-2 s | 0.1-3.4 s (NPU) / 0.1-4.4+ s |
| generation (150-tool-call + 60 rationale = ~200 tokens) | 4 s at 52 tok/s | 9 s at 22 tok/s | 10-12 s at 16-21 tok/s |
| 1 + up to 3 rounds | 8-20 s | 25-50 s | 25-50 s |
| resident memory | ~0.7-1.7 GB (+ model mapped 2.6 GB) | ~0.7-3.3 GB (+ 3.7 GB) | not stated |
| first load | **unknown: GPU shader compile risk (27-73 s seen for other models in ai-runtime.md)** | same | NPU bundle load [?] |
| battery/thermal | not published **[?]**; expect a 10-20 s burst, a few % of a battery per 20 runs, throttling on repeated runs | worse | |
Measure on the Find N5 with `getThermalHeadroom` and the battery-stats counters before any claim.

### (g) Evaluation plan with go/no-go gates
1. **Desktop prototype** (days): llama.cpp (`llama-server`, GGUF Q4 of Gemma 4 E2B/E4B, Qwen3-VL-4B, Qwen3.5-2B/4B; JSON-schema constrained) + the project CLI (`cli/`) as the tool executor that renders previews and returns metrics. Image set: 100-200 public RAWs with permissive licences (the project's test corpus `tests/engine/golden/corpus.txt`, RAW samples with CC0/CC-BY licences [?]) plus the owner's own photos; FiveK (5000 DNGs + 5 expert catalogues) for *local evaluation only* (research-only licence [V]); no training on it for shipping.
2. **Arms**: (A) existing deterministic auto; (B) A + kNN/median taste regression (no LLM); (C) A + small VLM deltas, no facts text; (D) A + small VLM with facts, constrained; (E) D + 1-3 render feedback rounds; (U) strong cloud VLM with the same tools as an *upper bound only* (not a product path).
3. **Judging**: blind A/B on phone-sized and full-screen crops by the owner and 2-3 other photographers (randomised side, 100+ pairs per arm, report win/tie/loss with confidence intervals), plus automatic checks: clipping fractions, chroma outliers, face luminance, WB drift, run-to-run variance across 5 seeds.
4. **Gate 1 (desktop, go to phone)**: arm D or E beats arm A in at least 55 % of non-tie votes AND loses in under 15 % with a visible defect, AND is not beaten by arm B on the owner's own taste set; invalid tool calls 0 % with constrained decoding; rationale facts correct in at least 95 % (programmatic check). If arm B equals arm D: ship the regressor instead and drop the LLM, keeping only the rationale if useful.
5. **Gate 2 (phone feasibility, Find N5, then one iPhone)**: LiteRT-LM and llama.cpp builds run offline without Google services; first-load time with and without shader/kernels cache; 3-round latency under 25 s (E2B) with the thermal state "none" and under 40 s after three consecutive runs; peak RSS under 3 GB while the engine holds its preview session; no process kill when the user opens full-resolution export afterwards; battery cost measured; Chinese rationale accepted by the owner in 9 of 10 samples.
6. **Gate 3 (product)**: licence review of the chosen weights (Apache-2.0 file and NOTICE kept), delivery path chosen and costed (CDN price per 1,000 downloads), gate logic validated on at least three phones (Find N5, a mid-range phone that must show "unsupported", an iPhone), and a user-facing "revert to auto" always one tap away.
No-go outcomes are fine: cheapest useful release is arm B (taste regression, zero download) plus a template rationale.

## 6. Licence table (code, weights, policy checked separately)

| Item | Code licence | Weights licence | Use policy / conditions | Verdict for a commercial app that may be open-sourced |
|---|---|---|---|---|
| Gemma 4 E2B/E4B | n/a | **Apache-2.0** (HF API, model card [V]) | none stated; Google dropped the custom terms (reports [S]) | **Usable.** Recommended first candidate |
| Gemma 3 / 3n / PaliGemma 2 | | `gemma` custom terms, gated "manual" [V] | Gemma prohibited-use policy and pass-on conditions [M] | Avoid: no benefit over Gemma 4 |
| Qwen3-VL 2B/4B/8B | Apache-2.0 [V] | Apache-2.0 [V] | none | **Usable**; best Chinese |
| Qwen3.5 0.8B/2B/4B | | Apache-2.0 [V] | none | **Usable**; runtime support for the new architecture to check |
| Qwen2.5-VL-3B / 7B | | 3B `qwen-research`, 7B Apache-2.0 [V] | 3B non-commercial | Skip 3B; 7B too large |
| SmolVLM2 256M/500M/2.2B | | Apache-2.0 [V] | | Usable; Chinese weak |
| MiniCPM-V 4 / 4.5 | Apache-2.0 [V] | Apache-2.0 per card; optional registration [V] | none stated | Usable (4.5 likely too large [M]); re-read the licence file at release time |
| MiniCPM-V 2.6 and older | | MiniCPM licence, gated [V flag] | registration/terms [M] | Avoid |
| Phi-4-multimodal, Phi-3.5-vision | | MIT [V] | | Usable; Chinese weaker |
| InternVL3.5-4B | | Apache-2.0 [V] | | Usable. InternVL3-2B: licence ambiguous (card shows apache-2.0 and `qwen`) [V] |
| LLaVA-OneVision 0.5B, Moondream2 | | Apache-2.0 [V] | | Usable but unsuitable |
| FastVLM | check repo LICENSE | `apple-amlr`, research purposes only [V] | no commercial exploitation or product development [V] | **Not usable** |
| JarvisArt (weights, code) | non-commercial | non-commercial [V] | commercial licence on request | Not usable; study the method only |
| MonetGPT, PhotoArtAgent, RetouchLLM, PerTouch | not found | not found | | Method only |
| MIT-Adobe FiveK | | data: research use only (Adobe licences) [V] | no redistribution | Local evaluation only |
| Image-Adaptive 3D LUT | Apache-2.0 [V] | trained weights inherit data terms [M] | | Code fine; do not ship FiveK-trained weights |
| CSRNet, HDRNet, NILUT | **[?]** | **[?]** | | Check before any use |
| LiteRT-LM | Apache-2.0 [V] | n/a | | Usable |
| llama.cpp | MIT [V] | n/a | | Usable |
| MNN, MLC-LLM | Apache-2.0 [V] | n/a | | Usable |
| ExecuTorch | BSD-style (SPDX NOASSERTION [V]) | n/a | | Likely usable; read LICENSE |
| Qualcomm GenieX | BSD-3-Clause [V] | AI Hub bundles: terms not found **[?]**; Qwen3-VL card says apache-2.0 [V] | QAIRT SDK licence not read **[?]** | Runtime fine; redistribution of compiled bundles and the "Genie deprecated" notice need confirmation |

## 7. Sources (accessed 2026-11-14)
- Gemma 4 model card https://ai.google.dev/gemma/docs/core/model_card_4 ; HF https://huggingface.co/google/gemma-4-E2B-it ; HF API licence fields for all HF models listed above (https://huggingface.co/api/models/<id>).
- LiteRT-LM https://github.com/google-ai-edge/LiteRT-LM ; Gemma 4 performance table https://developers.google.com/edge/litert-lm/models/gemma-4.md.txt ; MediaPipe LLM Inference status https://developers.google.com/edge/mediapipe/solutions/genai/llm_inference/android
- Qwen3.5-2B https://huggingface.co/Qwen/Qwen3.5-2B ; Qwen3-VL https://huggingface.co/collections/Qwen/qwen3-vl ; Qualcomm Qwen3-VL-4B card https://huggingface.co/qualcomm/Qwen3-VL-4B-Instruct ; GenieX https://github.com/qualcomm/geniex , https://geniex.aihub.qualcomm.com/en/resources/faq.md
- Arm Qwen3-VL Android path https://learn.arm.com/learning-paths/mobile-graphics-and-gaming/ai-portal-mobile-vision-language/3-run-model/
- llama.cpp multimodal docs https://github.com/ggml-org/llama.cpp/blob/master/docs/multimodal.md ; MNN-LLM paper https://arxiv.org/abs/2506.10443
- MiniCPM-V 4.5 card https://huggingface.co/openbmb/MiniCPM-V-4_5 ; Apple FastVLM licence https://raw.githubusercontent.com/apple/ml-fastvlm/main/LICENSE_MODEL
- MonetGPT https://arxiv.org/abs/2505.06176 ; JarvisArt https://arxiv.org/abs/2506.17612 , https://github.com/LYL1015/JarvisArt (LICENSE) ; PhotoArtAgent https://arxiv.org/abs/2505.23130 ; RetouchLLM https://arxiv.org/abs/2510.08054 ; PerTouch https://arxiv.org/abs/2511.12998
- FiveK licence https://data.csail.mit.edu/graphics/fivek/ ; Image-Adaptive 3D LUT https://github.com/HuiZeng/Image-Adaptive-3DLUT
- Android performance class https://source.android.com/docs/compatibility/17/mpc , https://android-developers.googleblog.com/2022/03/using-performance-class-to-optimize.html ; Apple entitlement https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.developer.kernel.increased-memory-limit , forum https://developer.apple.com/forums/thread/834805
- OSS/CDN prices: Alibaba Cloud developer community articles (third-party summaries, e.g. https://developer.aliyun.com/article/1691983): not official price pages.
- Repo: `docs/design/editor-panel.md`, `app/lib/model/pad_model.dart`, `core/src/edit_params.cpp`, `docs/engine/pipeline-architecture.md` (sections 1 metering, 29, 33, 37, 40, 41), `docs/research/ai-runtime.md`.

## 8. Not verified (to close before a decision)
LiteRT-LM binary size and Play-services independence; LiteRT-LM constrained-decoding schema interface; Gemma 4 and Qwen3.5 Chinese quality at 2B; Qwen3.5 support in llama.cpp/MNN; any Find N5 / ColorOS NPU access; MediaTek runtime story; battery numbers; official Alibaba/Tencent price pages and ICP rules; ModelScope and Apple Background Assets terms for this use; CSRNet/HDRNet/NILUT/PPR10K licences; small-VLM colour-cast and aesthetic accuracy (the prototype measures these).
