# AI 助手: desktop prototype and evidence

Date 2026-11-14. Experiment harness only (no product code). Question: does a small local vision-language model (VLM), driving the app's
own high-level parameters through one function call, improve photos beyond the engine's deterministic auto, and which architecture
works best? Follows the plan in `docs/research/ai-assistant.md` section 5 (arms, gates).

Code: `experiments/assistant/` (Python, ctypes on the engine's C API). Results: `build/assistant/` (git-ignored). Weights and llama.cpp:
`tools/_dl/llm/` (git-ignored). Nothing in `core/`, `app/`, `tests/` was touched.

## 0. Summary (for the owner)

**Decisive evidence is still yours to give:** open the blind sheet (section 3.6, about 5 minutes). Everything below is automatic
evidence, and it points one way:

1. **The small models barely touch the numbers.** Given offsets from the auto, Gemma 4 E4B, Qwen3-VL 2B and 4B leave light, colour
   and texture at 0 on almost every photo (mean |offset| 0.00-0.03 pad steps); Gemma 4 E2B and Qwen3.5-4B move them a little, and E2B
   moves colour the WRONG way (warmer when the measured cast is already warm: correlation +0.34). What they actually do is pick a
   rendering base (a modern base on 80-100 % of photos, 50-70 % strength, almost never a film stock) plus 1-2 local area actions.
   In practice every model behaves like arm F already.
2. **No arm gets closer to expert edits than today's auto.** On 95 FiveK photos (thumbnail-scale global tone and colour) every VLM
   arm is slightly further from the experts than arm A (paired +0.2 to +0.8 dE2000; inter-expert spread is 9.4). A constant
   "modern_clean 70 %" with no model is +0.7, so the per-photo choice is a bit better than a fixed base, but not better than the auto. An oracle
   inside the same pad space gets to -4.4, so the room exists: the models do not find it.
3. **Personalisation: the kNN predictor (B) is as good as or better than every LLM arm.** Warm-film photographer: B 1.38 dE to their
   rule, best LLM (Qwen3.5-4B with the profile in context) 1.75 (significantly worse). Clean-contrast photographer: B 2.38, best LLM
   arm (F with B's numbers) 2.23 (95 % CI of the difference -0.43..+0.11: a tie). Synthetic photographers favour B by construction
   (their rules use the same scene features), so this is an upper bound for B; it is still the baseline the LLM must beat, and it does not.
4. **Constrained decoding works; the rationales do not.** 100 % valid calls for 6 of 7 models (SmolVLM2 61 %: it runs out of tokens on
   whitespace); without the grammar MiniCPM-V 4 and SmolVLM2 produce 0 % valid calls. But 29-38 % of the factual claims in the Chinese
   reasons contradict the measured facts (gate: <= 5 %), facts in the prompt are what makes them less wrong (image-only: 40-62 %).
5. **Loop of 3 rounds is useless here:** after seeing its own result the model returns the same call on 89-100 % of photos.
6. **Cost:** on this GPU 2-4 s per round. On 4 CPU threads (rough phone proxy) E2B 13 s per round, 4B models 35-52 s per round. From
   Google's published phone numbers, E2B on a phone GPU is about 3-4 s per round and E4B about 8-9 s.

**Recommendation: no-go for "the LLM edits the photo" (Gate 1 fails on rationale accuracy and on B; the improvement criterion
is unproven and the automatic proxies are negative). If the blind sheet shows a clear preference for L / Q / F over A and M, the only
architecture worth a phone test is F (semantic only: base + strength, area actions, reason ids) with numbers from A or B, a template-built
Chinese rationale from reason ids + measured numbers, and Gemma 4 E4B (or E2B for memory) through LiteRT-LM. The cheapest useful
release remains arm B + template rationale (no download).** Details in section 4.

---

## 1. Set-up (what was built)

### 1.1 Runtime and models

* **llama.cpp b12370** (official GitHub release, `llama-b12370-bin-win-cuda-12.4-x64.zip` + cudart, and the CPU build), `llama-server`
  with the model's mmproj, JSON-schema constrained decoding (`response_format: json_schema`, turned into a grammar by llama.cpp),
  `--reasoning off`, one slot, prompt cache off, seed 7. SHA-256 of every file checked against the GitHub / Hugging Face LFS digests
  (`experiments/assistant/dl.py`, log `build/assistant/logs/dl.log`: all OK).
* Models (4-bit, what a phone would run; all Apache-2.0 per their model cards, re-read in the downloaded READMEs):

| id | file (repo) | size (model + mmproj) | note |
|---|---|---|---|
| gemma4-e2b | `gemma-4-E2B_q4_0-it.gguf` + mmproj BF16 (google/gemma-4-E2B-it-qat-q4_0-gguf, Google's own QAT Q4_0) | 3.35 + 0.99 GB | |
| gemma4-e4b | `gemma-4-E4B_q4_0-it.gguf` + mmproj (google/gemma-4-E4B-it-qat-q4_0-gguf) | 5.15 + 0.99 GB | the QAT file keeps embeddings at higher precision (larger than ggml-org's 4.6 GB Q4_0) |
| qwen3vl-2b | `Qwen3VL-2B-Instruct-Q4_K_M` + mmproj Q8_0 (Qwen/Qwen3-VL-2B-Instruct-GGUF) | 1.11 + 0.45 GB | |
| qwen3vl-4b | `Qwen3VL-4B-Instruct-Q4_K_M` + mmproj Q8_0 (Qwen/Qwen3-VL-4B-Instruct-GGUF) | 2.50 + 0.45 GB | Q8_0 (4.28 GB) also run for the precision check |
| qwen35-4b | `Qwen3.5-4B-Q4_K_M` + mmproj Q8_0 (ggml-org/Qwen3.5-4B-GGUF) | 3.03 + 0.37 GB | **Qwen3.5-2B: no official multimodal GGUF exists** (Qwen publishes none; ggml-org has 0.8B without mmproj and 4B with one; only third-party 2B conversions). 4B used as the closest verified alternative. llama.cpp b12370 runs the hybrid DeltaNet architecture with vision. |
| smolvlm2-2b | `SmolVLM2-2.2B-Instruct-Q4_K_M` + mmproj Q8_0 (ggml-org) | 1.11 + 0.59 GB | English-centric |
| minicpmv4 | `ggml-model-Q4_K_M` + mmproj f16 (openbmb/MiniCPM-V-4-gguf) | 2.19 + 0.96 GB | MiniCPM-V 4 (4B) instead of 4.5 (8B, not phone-sized) |

### 1.2 Executor (the engine as it is today)

`experiments/assistant/engine.py` binds `libdapple.dll` (a snapshot of `build/check-desktop/bin` copied to `build/assistant/bin`, with the
x64 libomp/libc++ from the toolchain: the copy in check-desktop/bin is an ARM64 libomp). Per photo: `dapple_session_open` (preview
decode, 1536 px), `dapple_session_auto_json(0.6)` (the app's tone strength 60), `dapple_session_analyze_json`, renders through
`dapple_session_render` at 1024 px. **Arm A = exactly the app's default**: the auto correction without its legacy `exposure`, plus
`look std`, `lookStrength 100`, `toneStrength 60` (`app_state.dart wireParams`).

### 1.3 Tool surface (`tools.py`)

One call `apply_edit`, arguments = **offsets from the auto**, integers so the model emits few exact tokens:

```
{ reasons: [1-4 of 26 ids: underexposed, face_dark, sky_bright, warm_cast, keep_warm_mood, portrait, already_good, ...],
  light:   {x, y: -5..5 pad steps (1 step = 0.15 of the pad, so +-0.75 of the app's pad), s: -3..3 (strength strip, 0.1 per step)},
  color:   {x, y, s}, texture: {x, y, s},
  base: none | modern_clean | modern_vivid | modern_soft | one of 20 film stocks (negative + slide of the pack),
  base_strength: 3..10 (x10 %), hdr: 0..6 (x10, single-shot HDR),
  areas: up to 3 of {target: face|person|background|sky|top_gradient, op: lift|darken|warm|cool|pop|soften, amount 1..5},
  reason_zh: string 8..90 chars (two Chinese sentences) }
```

Pads go through a 1:1 Python port of `padRecipe` (light / colour / texture) and are added to the auto correction the way the engine's
`EditParams::operator+=` does (deltas from the defaults for additive keys). Area actions become local adjustments (pipeline-architecture
section 40) on raster masks: `lift` = +0.12 EV x amount and shadows, `darken` = -0.12 EV x amount and highlights (+saturation for sky),
`pop` = clarity + vibrance + a little exposure, `soften` = -clarity -texture, `warm`/`cool` = temp. Pinned parameters are deleted from
the schema (a pinned pad category or axis, `base`, `hdr`, `areas`) and forced to 0 again after the call.

**Masks on desktop.** The DLL is built with `DAPPLE_AI` off, so the engine's AI service cannot run here. The masks come from the
Python reference of the same models (`scripts/ai-eval/ai_ref.py`: BlazeFace face detector, MediaPipe selfie multiclass person parts with the
engine's gate, Depth Anything V2 small) on the neutral standard rendering (as section 41.1 specifies), handed to the engine with
`dapple_mask_raster_set`. **Sky** is a classical stand-in (top-connected smooth blue / bright-neutral regions that are far in depth,
excluding the person; `vision.sky_prior`), not the engine's `ai_masks_sky.cpp`. Known misses: BlazeFace short-range misses small faces
(group photos p8, p9 have no face found; a fruit bowl in the R5 frame got one false face), so `face` actions exist on 6 of 54 photos (5 real portraits + the fruit bowl).

### 1.4 Perception (`vision.py`)

The preview of the auto edit (JPEG, 768 px long edge) and a ~300-token text block of measured facts: CIE L* percentiles p1/p5/p50/p95/p99
and the engine's tonal class; clipped fractions (any channel >= 254 / <= 4); colour cast of near-neutral pixels (mean a*, b* where
C* < 18) with a word (neutral / warm / cool / green / magenta); mean / p90 chroma and the share of C* > 60; sky fraction, sky L*,
clipping and b*; faces (L*, p95, area, EV vs median), skin L*, hue angle and chroma; EXIF (camera, ISO, shutter, aperture, focal length,
hour); the current state; pinned parameters. Example (Galaxy S23 landscape):

```
Brightness (CIE L*, 0 black..100 white): p1 13, p5 19, median 47, p95 85, p99 92. Engine tonal class: key mid, range mid.
Clipping: 0.0% of pixels near white, 0.0% near black.
Colour cast of near-neutral pixels (60% of frame): a* -0.8, b* -3.7 -> cool/blue.
Saturation: mean chroma C* 14, p90 27, 0.0% of pixels very saturated (C*>60).
Sky (rough detector): 23% of frame, sky L* 51, sky clipped 0%, sky b* -26.
People: none detected.
EXIF: Samsung Galaxy S23, ISO 50, 1/2120 s, f/2.2, 2.2 mm, taken at 07h local.
```

### 1.5 Loop and guards (`assist.py`)

Round 1: system prompt (role, rules: small moves, trust the facts for brightness / cast, protect faces and highlights, ~350-token tool
description) + user message (image and/or facts, optional taste profile) -> constrained JSON. The engine renders it and the
**deterministic guards** run against the auto's own numbers: face p95 L* > 97 or face clipping up, highlight clipping +1.5 %, black
clipping +3 %, p90 chroma > max(1.35 x, +12), white-balance drift of the cast > 12 b* / 8 a* (only without a base). A violation shrinks
the offending group (light / colour x0.5; then the base strength x0.5; then areas off), re-renders, up to 3 times: clamped, never re-asked.
Rounds 2-3 (arm "3 rounds"): the model sees the new preview + new facts + which guards fired, returns TOTAL offsets or the same call with
`already_good`; stops when the call repeats.

### 1.6 Arms and configurations

| arm | what | where |
|---|---|---|
| A | engine auto (today's default) | `results/engine/A.jsonl` |
| B | A + kNN taste predictor (k = 7 nearest past edits by scene features: L* p5/p50/p95, cast a*/b*, chroma, sky, faces, person, clipping, night) | `taste.py` |
| C | VLM zero-shot, image + facts, 1 round, T 0 (`C_if1`); ablations image only (`C_i1`), facts only (`C_f1`), 3 rounds (`C_if3`), repeat at T 0 (`C_if1_rep`), T 0.7 x 3 seeds (`C_if1_t07a/b/c`), without constrained decoding (`C_if1_free`, schema in the prompt), with `light` and `base` pinned (`C_if1_pin`) | |
| D | C + taste profile in context (per scene type: median offsets and favourite base; 5 nearest past edits as few-shot lines), `D_<who>` | |
| E | cloud model, same tools (`llm.Cloud`, OpenAI-compatible) | not run: no key |
| F | VLM chooses only the semantic part (base / strength, hdr, areas, reasons, Chinese text); numbers from A (`F_A`) or B (`FB_<who>`) | |
| O | oracle: best light x/y/s and colour x/y offsets for FiveK expert C by coordinate search (upper bound of the pad space) | `fivek_eval.py` |

Image sets: **general set** = 45 CC0 RAWs (`build/rawsamples`) + 9 CC0 portraits (`build/ai-eval/portraits`) = 54; **FiveK** = 99 of an
evenly spaced 100 of the photos the dataset page links individually (one DNG LibRaw cannot unpack: 98 usable). FiveK: research licence,
**local evaluation only, nothing redistributed, nothing trained, never sent to a cloud service** (the runner refuses); the 16-bit expert
TIFFs came at ~50 KB/s, so the experts' **sRGB thumbnails (100 px long edge, experts A-E)** are the reference. Five RAW pairs are near-duplicates
(two compressions or resolutions of the same frame: X-S10, D850, Z8, OM-1, 6D II): kept in the tables, excluded from each other's taste
history and from the blind sheet.

### 1.7 Metrics and what each can show

| metric | measures | cannot show |
|---|---|---|
| (i) FiveK distance (dE2000 and \|dL*\| to the expert thumbnails, paired against A) | whether the edit moves toward what trained retouchers did to the same photo (global tone / colour, "postcard" style) | local contrast, sharpness, noise; a different but good rendering scores badly; FiveK is 2011 DSLR material and an American retouching taste |
| (ii) learned aesthetic predictor | - | **not run** (no permissively licensed checkpoint set up in time; NIMA / LAION-aesthetic weights' licences unchecked) |
| (iii) validity, T 0 repeat, T 0.7 spread (3 seeds), Q4 vs Q8 | reliability of the call | quality |
| (iv) claims vs facts (`assist.check_rationale`: people, sky, dark, bright, warm / cool / green / magenta cast, clipping, night; from the Chinese text and the reason ids) plus text vs action (`analyze.action_check`: 提亮 / 压暗 / 加暖 / 降温 / 柔化 vs the call) | hallucination of measurable facts | whether a correct claim is relevant; keyword matching misses paraphrases |
| (v) taste distance (pad steps and dE2000 at 256 px between the arm's render and the photographer's noise-free rule render, held-out, leave-one-shoot-out) | taste following | real taste: the photographers are synthetic |
| (vi) blind sheet | the owner's preference: the decisive test | pending |
| sensitivity | correlation of colour.x with the measured cast b* (should be negative), light.x with median L* | only for models that move the number |

---

## 2. Results

Full generated tables: `build/assistant/report.md` (+ `report.json`). Per-call records: `build/assistant/results/<model>/<config>.jsonl`;
renders: `build/assistant/renders/<config>/<model>/`. Two runs shared the GPU for most of the day, so `prompt_ms` / `gen_ms` in those
tables include contention; the clean latency numbers are the `lat_if3` / `cpu_if3` rows (section 2.6).

### 2.1 Per model, main configuration (C_if1: image + facts, 1 round, T 0; general set n = 54)

| model | valid | guards fired | mean \|pad offset\| (steps) | base none / modern / film | areas per photo | claims per reason | **claims contradicting facts** | text vs action mismatch | 2 sentences | colour.x vs cast b* |
|---|---|---|---|---|---|---|---|---|---|---|
| gemma4-e2b | 1.00 | 0.31 | 0.74 | 0 / 1.00 / 0 (always modern_soft 40-50 % or clean) | 0.9 | 1.6 | 0.31 | 0.02 | 0.87 | **+0.34 (wrong direction)** |
| gemma4-e4b | 1.00 | 0.20 | 0.03 | 0.04 / 0.96 / 0 | 1.0 | 1.9 | 0.32 | 0.03 | 0.80 | -0.40 |
| qwen3vl-2b | 1.00 | 0.30 | 0.00 | 0.20 / 0.80 / 0 | 1.5 | 1.1 | 0.29 | 0.07 | 0.69 | (never moves) |
| qwen3vl-4b | 1.00 | 0.41 | 0.02 | 0.15 / 0.85 / 0 | 1.3 | 2.3 | 0.32 | 0.16 | 0.74 | -0.41 |
| qwen35-4b | 1.00 | 0.35 | 0.32 | 0.19 / 0.81 / 0 | 0.5 | 2.4 | 0.38 | 0.12 | 0.17 (one sentence with semicolons) | -0.02 |
| minicpmv4 | 1.00 | 0.30 | 0.20 | 0.57 / 0.28 / 0.15 | 0.3 | 1.1 | 0.45 | 1.00 | 0.06 | -0.26 |
| smolvlm2-2b | **0.61** | 0.04 | 0.02 | 0.89 / 0.11 / 0 | 1.8 | 1.0 | 0.58 | - | 0.00 (57 % Chinese at all) | -0.03 |
| qwen3vl-4b **Q8_0** | 1.00 | 0.48 | 0.07 | 0.13 / 0.87 / 0 | 1.4 | 2.3 | 0.39 | 0.26 | 0.78 | -0.45 |

Readings: (1) the 4B-class models hand the numbers back to the auto: offsets are almost all 0, the change is the base + areas;
(2) the guards fired on 20-41 % of photos, mostly because a modern base at 50-70 % pushes highlight clipping or p90 chroma past the
bounds (the guard then halves the base strength), not because of the pads; (3) reason ids repeat (`sky_bright` four times) because the
grammar does not enforce `uniqueItems`; (4) the `sky_bright` / `face_bright` ids are chosen even when the facts say no sky / no face
(Gemma E2B picks `sky_bright` first on most photos). SmolVLM2 and MiniCPM-V 4 are out: degenerate Chinese (repetition loops such as
"天空很亮，天空很亮，…"), MiniCPM-V 4 text and action unrelated, SmolVLM2 truncated calls.

### 2.2 Ablations

**Perception (C_i1 image only / C_f1 facts only / C_if1 both), share of claims that contradict the facts and the colour response:**

| model | image only | facts only | both | colour.x vs cast: image / facts / both |
|---|---|---|---|---|
| gemma4-e2b | 0.40 | 0.26 | 0.31 | -0.10 / +0.53 / +0.34 |
| gemma4-e4b | 0.31 | 0.22 | 0.32 | -0.14 / -0.47 / -0.40 |
| qwen3vl-2b | 0.40 | 0.43 | 0.29 | - |
| qwen3vl-4b | 0.62 | 0.36 | 0.32 | -0.15 / -0.49 / -0.41 |
| qwen35-4b | 0.57 | 0.48 | 0.38 | -0.08 / -0.08 / -0.02 |

The colour cast reaches the larger models only through the measured facts (image only: |r| <= 0.15). Facts lower hallucination for
every model; the image adds content (portrait, flowers, colour chart, black-and-white: Gemma E4B names them correctly) but not
measurable truth. Image + facts is the right input; facts only is a cheap fallback.

**Rounds (C_if3 vs C_if1):** the second round returns the identical call (`already_good`) on 89-100 % of photos (changed: E2B 7 %,
E4B 0 %, Qwen3-VL-2B 2 %, Qwen3-VL-4B 4 %, Qwen3.5-4B 11 %), mean change 0.00-0.07 steps, for twice the latency. Its lower "wrong" rate
is an artefact: the last-round reason is mostly "already good". The closed loop of PhotoArtAgent / RetouchLLM does not work at this
size: the model does not critique its own render. Use 1 round.

**Constrained decoding (C_if1 vs C_if1_free, schema only in the prompt, 20 photos):** valid without the grammar: E2B 85 %, E4B /
Qwen3-VL / Qwen3.5 100 %, MiniCPM-V 4 0 %, SmolVLM2 0 %; free calls are shorter and make fewer claims. Keep the grammar.

**Pins (`light` and `base` removed from the schema, 20 photos):** respected 100 % for every model (by construction).

**Quantisation (Qwen3-VL-4B Q4_K_M vs Q8_0, same 54 + 95 photos):** base choice agrees on 87-96 %, pads differ by 0.07-0.11 steps,
but the area actions agree on only 20-35 % of photos; FiveK paired distance +0.51 (Q8) vs +0.61 (Q4). Higher precision does not change
any conclusion; the area choices are fragile (same order as T 0.7 noise).

### 2.3 Consistency (20 photos)

| model | T 0 repeat identical | T 0.7: pairwise distance (steps) | T 0.7: same base | T 0.7 vs T 0 |
|---|---|---|---|---|
| gemma4-e2b | 100 % | 0.69 | 57 % | 0.55 |
| gemma4-e4b | 100 % | 0.10 | 62 % | 0.07 |
| qwen3vl-2b | 100 % | 0.07 | 67 % | 0.03 |
| qwen3vl-4b | 100 % | 0.12 | 73 % | 0.09 |
| qwen35-4b | 100 % | 0.50 | 42 % | 0.47 |

Temperature 0 with a fixed seed is fully reproducible on one machine; at 0.7 the base flips on a third to a half of photos.

### 2.4 FiveK (95 photos with expert thumbnails of the same aspect; distance to expert C, paired against A; lower is closer)

| arm | model | dE to C | paired vs A | paired vs A, mean of experts A-E | closer than A by > 0.25 | further by > 0.25 |
|---|---|---|---|---|---|---|
| O (oracle pads for C) | - | 7.39 | **-4.35** | -2.52 | 99 % | 0 % |
| **A (auto)** | - | **11.74** | 0 | 0 | | |
| C_if1 | qwen35-4b | 11.96 | +0.22 | +0.06 | 27 % | 44 % |
| F_A | gemma4-e2b | 11.98 | +0.24 | +0.17 | 13 % | 42 % |
| F_A | gemma4-e4b | 12.16 | +0.42 | +0.38 | 18 % | 44 % |
| C_if1 | gemma4-e2b | 12.18 | +0.44 | +0.20 | 25 % | 56 % |
| C_if1 | gemma4-e4b | 12.20 | +0.46 | +0.46 | 15 % | 42 % |
| F_A | qwen35-4b | 12.21 | +0.47 | +0.47 | 20 % | 43 % |
| C_if1 | qwen3vl-4b (Q8) | 12.25 | +0.51 | +0.43 | 14 % | 48 % |
| C_if1 | qwen3vl-4b | 12.34 | +0.61 | +0.58 | 16 % | 55 % |
| C_if1 | qwen3vl-2b | 12.43 | +0.69 | +0.66 | 7 % | 44 % |
| M (constant modern_clean 70 %, no model) | - | 12.44 | +0.70 | +0.73 | 15 % | 49 % |
| F_A | qwen3vl-4b | 12.48 | +0.74 | +0.70 | 16 % | 56 % |

Inter-expert spread (experts A, B, D, E vs C): 9.43. Our auto is on average 5.9 L* darker and 6.0 C* less colourful than expert C:
FiveK retouchers brighten and saturate; the oracle mostly moves light x and colour y to the +-5 limits, so the pad range (+-0.75) is
also too small to reach them. No model moves in that direction; they add a modern base that deepens contrast. Note that the models
were never told the "house style" of FiveK; this measures expert-likeness, not quality.

### 2.5 Taste following (arms B / C / D / F(B); 54 held-out photos, leave-one-shoot-out history of 46-53 rows)

Two synthetic photographers (`taste.py`): **warm_film** (brighter, lifted shadows, warm, Portra 400 / Gold 200 at 70-80 %, grain,
lifts dark faces) and **clean_contrast** (neutralises the measured cast, contrasty, crisp, modern_clean / modern_vivid for landscapes,
darker skies, hdr 20 on bright-sky scenes); history = rule + Gaussian noise (sigma 0.6 steps) + 15 % random base swaps.

| arm | warm_film dE to rule | base match | clean_contrast dE to rule | base match |
|---|---|---|---|---|
| A auto | 6.95 | 0 | 4.54 | 0 |
| **B kNN** | **1.38** | 0.80 | 2.38 | 0.80 |
| C zero-shot (best: qwen35-4b / gemma4-e4b) | 6.51 | 0 | 3.50 | 0.41 |
| D VLM + profile: qwen35-4b | 1.75 (B better, CI of diff +0.08..+0.70) | 0.80 | 2.61 | 0.94 |
| D: qwen3vl-4b | 2.07 | 0.78 | 2.57 | 0.81 |
| D: gemma4-e4b | 2.62 | 0.72 | 2.39 (tie with B) | 0.80 |
| D: gemma4-e2b | 2.31 | 0.83 | 3.76 | 0.83 |
| F(B) (numbers from B, VLM picks base / areas): qwen3vl-4b | 1.77 (B better, +0.13..+0.69) | 0.81 | 2.34 | 0.80 |
| F(B): qwen35-4b / qwen3vl-2b | 3.84 / 5.15 | 0.43 / 0.11 | **2.23 / 2.23** (tie: CI -0.43..+0.12) | 0.89 / 0.85 |

The profile in context works (D is far better than C: the models copy the favourite base and offsets), but never better than the
kNN that produced the profile's numbers. Letting the VLM choose the base on top of B's numbers (F(B)) loses the photographer's film
stock when the model prefers its own modern base (warm_film base match 0.11-0.81).

### 2.6 Cost (clean runs: GPU alone; CPU = llama.cpp CPU build, 4 threads of an i7-14700KF; medians)

| model | where | round-1 prompt tokens (image + ~600 text) | prefill | decode tok/s | round-1 total | later round (prompt ~1.6-2.1 k) | 1 + repeat check | RSS |
|---|---|---|---|---|---|---|---|---|
| gemma4-e2b | GPU | 958 | 0.15 s | 77 | 2.2 s | 1.8 s | 4.1 s | |
| gemma4-e2b | CPU 4t | 969 | 4.6 s (209 tok/s) | 19 | 12.9 s | 16.1 s | 29 s | 4.0 GB |
| gemma4-e4b | GPU | 958 | 0.27 s | 51 | 3.5 s | 2.8 s | 6.3 s | |
| gemma4-e4b | CPU 4t | 969 | 25 s (39 tok/s) | 5.8 | 52 s | 66 s | 118 s | 6.8 GB |
| qwen3vl-2b | GPU | 1164 | 0.19 s | 118 | 2.0 s | 1.9 s | 3.9 s | |
| qwen3vl-2b | CPU 4t | 1167 | 10.8 s | 19 | 22 s | 35 s | 57 s | 3.6 GB |
| qwen3vl-4b | GPU | 1164 | 0.33 s | 63 | 3.0 s | 3.0 s | 6.0 s | |
| qwen3vl-4b | CPU 4t | 1167 | 17.6 s | 9.5 | 35 s | 54 s | 107 s | 6.4 GB |
| qwen35-4b | GPU | 1181 | 0.43 s | 53 | 4.2 s | 4.0 s | 8.2 s | |
| qwen35-4b | CPU 4t | 1184 | 19.7 s | 10.0 | 38 s | 54 s | 92 s | 5.8 GB |
| minicpmv4 / smolvlm2-2b | GPU | 1113 / 1147 | 0.5 / 0.3 s | 73 / 128 | 2.7 / 3.3 s | | | |

Output: 150-230 tokens per call (the JSON with pretty-print whitespace plus a 30-60 character Chinese reason); arm F about 90-145.
Engine work per round (render 1024 px + measurement + guard re-renders) is 0.15-0.6 s on this CPU and is not included. RSS on CPU
includes the memory-mapped weights and the F16/BF16 mmproj (Gemma's 0.99 GB projector is BF16; a Q8 projector halves it).

**Phone scaling assumption.** Google's published S26 Ultra numbers for E2B (LiteRT-LM, `docs/research/ai-assistant.md` section 1):
CPU prefill 557 tok/s, decode 47 tok/s; GPU 3808 / 52. Our 4-thread llama.cpp gets 209 / 19 for the same model, so the phone CPU path
with LiteRT-LM is about 2.5x our CPU proxy, and the phone GPU path decodes at about our desktop GPU's speed / 1.5. Projected per round
(1000 prompt tokens incl. the image, 160 output tokens): **E2B LiteRT GPU ~0.3 + 3.1 = 3.5 s; E4B LiteRT GPU ~0.8 + 7.3 = 8 s;
E2B LiteRT CPU ~1.8 + 3.4 = 5 s; Qwen3-VL-4B llama.cpp on an 8 Elite (17-21 tok/s published) ~9-13 s;** llama.cpp CPU on a phone is
probably close to our 4-thread numbers (13 s E2B, 35-52 s for 4B). Arm F halves the output tokens (about 100), so F with E2B on the
phone GPU is about 2-2.5 s. Not measured on a phone.

### 2.7 Chinese rationales (20 photos x 7 models: `build/assistant/rationales.md`)

| model | fluency | grounding | typical failure |
|---|---|---|---|
| gemma4-e4b | best: natural, short, two sentences | recognises content (色卡、黑白、花卉、人像), rarely says something the call did not do | labels a flower field or a colour chart `portrait` in the ids; 32 % of claims contradict facts |
| gemma4-e2b | fluent but formulaic: the same template on most photos ("天空亮度略微增加…整体色调微暖…") | weak | invents "夜景" on daylight snow, "偏暖" on neutral photos, sky actions on photos without sky |
| qwen3vl-4b | fluent | uses the facts (cast, face) | self-contradiction ("天空过亮，轻微提亮"), the same sentence twice |
| qwen3vl-2b | verbose, 3-4 sentences, meta-talk about "自动调整" | parrots the facts | describes things it did not do |
| qwen35-4b | terse technical style, one sentence with semicolons | the most fact words, the most wrong ones (38 %) | "自动编辑对比度不足 / 偏冷" against the numbers |
| minicpmv4 | unusable | - | repetition loops ("天空很亮，天空很亮，…"), keyword lists |
| smolvlm2-2b | unusable | - | empty, English or garbage |

Good examples (text, then the call): Gemma E4B on the colour chart: "照片是色卡，自动编辑已完成基础调整。轻微增强色彩饱和度以使色卡更清晰。";
on the Z8 landscape (sky 10 %, 2.7 % clipped): "天空过亮且有高光溢出，略微压暗天空以保留细节。整体色调偏暖，轻微调整色温使其更自然。" (sky darken 2 and
colour x -1: correct and grounded). Qwen3-VL-4B on the green-cast Pixel 7 photo: "背景偏绿，调暖以平衡色调。整体偏暗，微调亮度提升层次。" (warms the
background; but "微调亮度" is not in the call). Bad: Gemma E2B on the 5D snow scene: "…整体色调微暖以匹配夜景氛围。"
(daylight, neutral); Qwen3-VL-4B on the R6 dusk: "天空过亮，轻微提亮以平衡整体曝光。" twice, with a sky lift; Qwen3.5-4B on the bright R10 portrait
(median L* 83): "自动编辑过亮，需要压暗…画面偏冷，需要加暖色温。" then lifts exposure. Verdict: no model reaches the 95 % factual bar; Gemma E4B
is the only one whose free text could be shown after a programmatic check, and even then template text from reason ids + numbers is safer.

### 2.8 Desktop limitations of this harness
Masks are the Python reference, not the engine service (same models, but the sky is a stand-in and BlazeFace misses small faces: 6 of 54
photos have a face target, one of them a false face on a fruit bowl); the facts block is computed in Python from the rendered preview (the engine's own `stats` / `analyze` are
included in part: tonal class); FiveK uses 100 px thumbnails; the latency on the shared GPU is not the clean number (section 2.6 is).

---

## 3. Artefacts

### 3.1 Code (`experiments/assistant/`)
`engine.py` (C API binding), `vision.py` (colour maths, masks, facts), `tools.py` (schema, pad recipes, area actions), `assist.py` (loop,
guards, validation, rationale check), `llm.py` (llama-server and cloud clients), `taste.py` (photographers, kNN, profile), `data.py`
(image sets, per-photo preparation), `run.py` (runner: `prep`, `baseline`, `vlm <model> <tier>`), `control_m.py` (arm M),
`fivek_eval.py` (oracle, scores), `analyze.py` (tables), `rationale_dump.py`, `compare.py` (side-by-side sheets), `blind.py`,
`dl.py` / `fivek_dl.py` (downloads with hash checks), `chain.sh` / `master2.sh` / `fivek_loop.sh` (the unattended sequence used).

### 3.2 Reproduce
```
P=tools/py-ai/Scripts/python.exe          # venv; tifffile 2026.9.20 was added (only there)
$P -I experiments/assistant/dl.py llama base           # weights + llama.cpp (sha256 checked)
$P -I experiments/assistant/fivek_dl.py 100            # optional, local evaluation only
$P -I experiments/assistant/run.py prep && $P -I experiments/assistant/run.py baseline
sh experiments/assistant/chain.sh "gemma4-e4b qwen3vl-4b" "full taste fivek lat"
$P -I experiments/assistant/run.py vlm gemma4-e2b cpu cpu 4
$P -I experiments/assistant/control_m.py; $P -I experiments/assistant/fivek_eval.py oracle; $P -I experiments/assistant/fivek_eval.py score
$P -I experiments/assistant/analyze.py                 # -> build/assistant/report.md
```

### 3.3 Cloud arm E
`llm.Cloud` (OpenAI-compatible chat completions with `response_format: json_schema`). `OPENAI_API_KEY` and `OPENAI_BASE_URL` are not
set in this environment: **arm E was not run**. To run it: set the key (and `ASSISTANT_CLOUD_MODEL`, default `gpt-4.1-mini`) and
`run.py vlm cloud:<model> full`; it uses only the general set (CC0 RAWs and CC0 portraits); the runner refuses FiveK for a cloud model.

### 3.4 Blind contact sheet (the decisive test)
`build/assistant/blind/`: `index.html` (24 photos, 5 renderings each in random order per row, large images; keys 1-5 = best,
Shift + 1-5 = worst (optional), 0 = no preference, arrows to move, Z to zoom), `sheet.png` (the same rows as one static image),
`key.json` (answer key: do not open before voting). Serve and vote:
```
tools/py-ai/Scripts/python.exe -I experiments/assistant/blind.py serve     # http://127.0.0.1:8765/ ; votes -> build/assistant/blind/votes.json
tools/py-ai/Scripts/python.exe -I experiments/assistant/blind.py tally     # per-arm best / worst counts after voting
```
Arms on the sheet: A = auto (today), M = auto + constant modern_clean 70 % (no model: the control for "the model just adds a modern
base"), L = Gemma 4 E4B C_if1, Q = Qwen3.5-4B C_if1 (closest VLM arm to FiveK experts), F = Gemma 4 E4B arm F (base + areas only).
Reading the result: L / Q / F must beat both A **and** M to show model judgement; if M wins as often, ship a better default base instead.
The 24 photos exclude near-duplicates and colour-chart frames; differences are small on purpose (offsets, 50-70 % bases), so view large.

### 3.5 Taste profile export (what the app must write for the real test)

One JSON-lines file, written by the app on request (settings -> export), **parameters only, no pixels, no location**:
`dapple_taste_history.jsonl`, one line per photo whose edit the user finished (left the editor with changes kept), newest last:
```json
{"v": 1,
 "photo": "<16 hex: the engine's photo id (dapple_session_ai_photo_id) or any stable hash>",
 "taken_at": 1661662877, "edited_at": 1794536662,
 "camera": {"make": "Canon", "model": "EOS R10", "iso": 200, "shutter": 0.001, "aperture": 1.8, "focal": 50},
 "facts": {                                   // computed on the phone when the photo was opened (engine numbers, no pixels)
   "auto_stats": {"medianLuma": 0.69, "p01Luma": 0.033, "p99Luma": 1.37, "clippedFraction": 0.00003,
                  "neutralCastA": 0.0096, "neutralCastB": 0.0040, "neutralCoverage": 0.19},   // dapple_session_auto_json().stats
   "tone": {"key": "high", "range": "mid", "nearest": "high-mid", "p5": 0.25, "p50": 0.81, "p95": 0.92},  // dapple_session_analyze_json
   "ai": {"faces": 1, "person": 0.16, "sky": 0.0, "face_L": 61.0}     // from the AI-mask planes if they were computed, else null
 },
 "auto": {"toneStrength": 60, "correction": {"temp": 0, "tint": 0, "...": "dapple_session_auto_json().correction, non-default keys"}},
 "final": {                                   // EditState.toJson() of the kept edit
   "pads": {"light": {"x": 0.15, "y": -0.3, "a": 0.4, "off": {}}, "color": {"x": 0.3, "y": 0, "a": 0.5, "off": {}},
            "film": {"x": 0, "y": 0, "a": 0.6, "off": {}}, "texture": {"x": -0.15, "y": 0.3, "a": 0.5, "off": {}}},
   "film": "kodak_portra_400", "look": "std", "lookStrength": 100, "toneStrength": null, "toneTarget": null,
   "values": {"hdr": 0, "film_strength": 80},
   "local": [{"kind": "raster:ai:face", "ops": {"exposure": 6}}, {"kind": "linear", "ops": {"exposure": -8}}]   // summary: mask kind + ops, no strokes
 },
 "pinned": [],                               // what the user had pinned, if the assistant existed
 "source": "manual",                         // manual | assistant_kept | assistant_modified | assistant_undone
 "seconds_in_editor": 74}
```
The harness reads the same fields under other names (`taste.FEATS`); for the real test `taste.py` needs a 20-line adapter from
`auto_stats` / `tone` / `ai` to its feature vector (the desktop features were measured on the rendered preview in L*, the phone has
the engine's linear statistics: a different but equivalent scale). Rows with `source: assistant_undone` are negative examples.
Minimum useful size: about 50 edits (the kNN used 46-53 rows here).

---

## 4. Recommendation and gates

**Architecture.** Of C / D / F, **F** is the only form that is worth keeping, and mostly because the models already behave like F:
the numbers stay with the deterministic auto (or B when the user has a history), the model contributes (a) the rendering base and its
strength, (b) 0-2 area actions on the AI masks, (c) reason ids. The Chinese text is composed by the app from the reason ids and the
measured numbers (template), with the model's free sentence shown only when the programmatic check finds no contradiction. One round,
temperature 0, constrained decoding, guards as built here. C (numbers from the model) gives nothing: the 4B models refuse to move the
numbers and the 2B model moves them wrongly. D (profile in context) works only as well as the kNN that builds the profile.

**Model.** Gemma 4 E4B: the best Chinese, the most sensible scene recognition, colour response in the right direction with facts,
stable at T 0.7, Apache-2.0, and LiteRT-LM is its first-class runtime. E2B only if E4B's memory (6.8 GB RSS on CPU here; 3.7 GB file)
fails the phone gate: E2B's colour and rationale behaviour is visibly worse. Qwen3-VL-4B is equivalent on the automatic metrics at a
lower desktop cost but has the self-contradicting rationales; Qwen3.5-4B is closest to the FiveK experts and best in D but has the most
fact errors. MiniCPM-V 4 and SmolVLM2 are out.

**Gates (`ai-assistant.md` 5g).**
| Gate 1 criterion | result |
|---|---|
| D or E beats A in >= 55 % of non-tie votes, < 15 % visible-defect losses | **pending the blind sheet**; automatic proxies are negative (FiveK: every arm further from the experts than A; most edits differ only by a modern base) |
| not beaten by B on the owner's taste set | **fail / tie** on the synthetic photographers (B better on one, tie on the other); the owner's real history is not available |
| 0 % invalid calls with constrained decoding | **pass** for Gemma 4 E2B / E4B, Qwen3-VL 2B / 4B, Qwen3.5-4B, MiniCPM-V 4 (SmolVLM2 61 %: truncation) |
| rationale facts correct >= 95 % | **fail**: 62-71 % of claims agree with the facts (best Qwen3-VL-2B 71 %, Gemma E4B 68 %) |

**Verdict: no-go for the LLM-edits-the-photo feature on this evidence; conditional re-open only if the blind sheet shows L / Q / F
preferred over both A and M.** What does work and can ship without a model: arm B (kNN on the user's own edits, zero download) and a
template rationale from the measured facts.

**If the blind sheet re-opens it, the phone test must measure:** (1) LiteRT-LM with Gemma 4 E4B and E2B on the Find N5: first load with
and without the GPU kernel cache, per-call latency for arm F (about 100 output tokens) at 280 and 560 image tokens, peak RSS while the
engine holds its 2048 px session, thermal state after 5 calls in a row; (2) whether LiteRT-LM's constrained decoding accepts this schema
(string enums, integer ranges, nested objects, arrays with maxItems); (3) whether arm F's choices on the phone match the desktop's for
the same 54 photos (same model, different runtime and quantisation: expect the area choices to differ, section 2.2); (4) with the owner's
exported history (3.5): B vs F(B) on the owner's own last 50-100 edits, leave-one-out.

## 5. Not run / not done, and why
* **Arm E (cloud)**: no API key in the environment. Built, refuses FiveK, would send only the CC0 general set.
* **Owner votes**: the blind sheet is built; the votes are the owner's to give.
* **Learned aesthetic predictors** (NIMA, LAION-aesthetic, Q-Align): not run; licences of the checkpoints not checked in time.
* **Real taste history**: phones not available (no adb used); synthetic photographers instead, schema in 3.5.
* **Qwen3.5-2B**: no official multimodal GGUF (Qwen publishes none, ggml-org has 0.8B without a projector and 4B with one); third-party
  conversions not used. **MiniCPM-V 4.5** (8B) not run: not phone-sized; MiniCPM-V 4 (4B) instead. **Gemma 4 / Qwen at BF16**: only Q8
  of Qwen3-VL-4B was run for the precision check.
* **Engine AI masks and sky**: the DLL has `DAPPLE_AI` off; Python reference masks and a classical sky stand-in were used (2.8).
* **FiveK expert TIFFs**: ~50 KB/s from this network; the experts' sRGB thumbnails were used (global tone / colour only). FiveK was
  used for local evaluation only: nothing redistributed, nothing trained, never sent anywhere.
* **Phone measurements**: none (desktop CPU 4 threads as a proxy, scaling from Google's published numbers).
* **LiteRT-LM**: not tried on desktop; llama.cpp only.
