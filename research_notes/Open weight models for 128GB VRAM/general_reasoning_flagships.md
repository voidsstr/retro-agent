# General-Purpose / Reasoning "Flagship-Class" Open-Weight LLMs That Fit in 128 GB (state as of 2026-11-24)

Scope: text-first general/reasoning models whose weights are about 40-110 GB in an official or widely used precision, so they run on a 128 GB unified-memory box (DGX Spark, Strix Halo, Mac) or 128 GB of GPUs with room for KV cache. Coding-only and vision-first specialists are excluded, but general models that are natively multimodal are included and flagged. The models on the "already being downloaded" list (GLM-5.1, GLM-4.6, gpt-oss-120b, Qwen3-235B-A22B-Instruct-2507, DeepSeek-V3.2-Exp, DeepSeek-V4-Pro, DeepSeek-R1-0528, Apriel-1.5-15B-Thinker, Kimi-K2-Instruct-0905, Llama-3.3-Nemotron-Super-49B-v1.5, Mistral-Small-3.2-24B-Instruct-2506) are not recommended here. gpt-oss-120b appears only as a benchmark reference point.

## Q1. Which models in this class are current (newest checkpoint per family, Nov 2026), and which are superseded?

### Takeaway
The 128 GB class turned over almost completely in 2026. These are the current in-class picks:
- **Qwen3.8-27B**: dense, Aug 2026, Apache-2.0.
- **Qwen3.8-Flash-Next**: 125B MoE with 6B active, plus a 51B n-gram embedding table, Aug 2026. It needs a 3-4-bit GGUF to fit.
- **inclusionAI Ling-3.2-air**: 124B total, 5.5B active, released 2026-11-23.
- **NVIDIA Nemotron 3.5 Super VL**: 120B total, 12B active, Oct 2026, with an official 75 GB NVFP4 build.
- **NVIDIA Nemotron 3.5 Bolt 74B-A9B**: Oct 2026.
- **Meta Muse Glimmer 30B**: Aug 2026, Apache-2.0.
- **Mistral Small 4 and Mistral Medium 3.5**: both Mar 2026.
- **Google Gemma 4 31B and 26B-A4B**: Mar 2026.
- **Sarvam-105B-1711**: Nov 2026.

Several labs reused the "Flash" name for models that are far too large for this class: GLM-5.3-Flash (321B), DeepSeek-V4.1-Flash (about 552B), MiniMax-M3.1-Flash (about 428B), Tencent Hy4 flash (220B) and ERNIE 5.5 Flash (280B). Several older models are now superseded: Llama 3.3 70B and Llama 4 Scout, GLM-4.5-Air, Qwen3-Next-80B, Qwen3.5/3.6 27B, Hunyuan-A13B, dots.llm1, Ling/Ring-flash-2.0, ERNIE-4.5-21B and Command A (03-2025 and Reasoning).

### Cited Findings
**How the data was collected:**
- All sizes in these notes were measured on 2026-11-24 from the Hugging Face API, never taken from memory. Each figure is the sum of `siblings[].size` from `https://huggingface.co/api/models/<repo>?blobs=true`.
- Only weight files were counted (.safetensors, .bin, .gguf). The `original/` duplicate .pth folders were excluded.
- For a GGUF repo, only the named quant's shards were counted. `mmproj` vision projectors and MTP/draft heads are listed separately.
- Sizes are decimal GB (10^9 bytes).
- Two Mistral repos ship each weight set twice, once as `consolidated*.safetensors` (Mistral format) and once as `model-*.safetensors` (HF format). Only one set is needed, so one set's size is reported ([HF API Mistral-Small-4](https://huggingface.co/api/models/mistralai/Mistral-Small-4-119B-2603?blobs=true), [HF API Mistral-Medium-3.5](https://huggingface.co/api/models/mistralai/Mistral-Medium-3.5-128B?blobs=true)).
- Release dates are the HF repo `createdAt` unless the model card states a date. A repo can be created before its public launch: Mistral-Small-4-119B-2603 was created 2026-01-23 but carries a "2603" (March 2026) version tag.

**Newest checkpoint per family (from HF org listings sorted by creation date, 2026-11-24):**
- **Qwen (Alibaba)**
  - The newest open releases are Qwen3.8-27B (2026-08-05) and Qwen3.8-Flash-Next (2026-08-24), plus Qwen3.8-2.4T-A95B, which is far out of class ([HF Qwen org listing](https://huggingface.co/api/models?author=Qwen&sort=createdAt&direction=-1&limit=100)).
  - Qwen3.6-35B-A3B and Qwen3.6-27B are from Apr 2026. The Qwen3.5 family (122B-A10B, 35B-A3B, 27B) is from Feb 2026, and Qwen3-Next-80B is from Sep 2025 ([same listing](https://huggingface.co/api/models?author=Qwen&sort=createdAt&direction=-1&limit=100)).
  - There is no open Qwen3.7 mid-size model. Qwen's own tables compare against "Qwen3.7-Plus", a hosted model ([Qwen3.8-27B card](https://huggingface.co/Qwen/Qwen3.8-27B)).
  - Qwen describes Flash-Next as "This experimental preview of the architecture that will underpin Qwen4" ([Qwen3.8-Flash-Next card](https://huggingface.co/Qwen/Qwen3.8-Flash-Next)).
- **Zhipu (zai-org)**
  - The newest models are GLM-5.3 (753B) and GLM-5.3-Flash (2026-08-25). GLM-5.3-Flash has "320B total parameters and just 18B active" and is "the first natively multimodal model in the GLM-5 series" ([GLM-5.3-Flash card](https://huggingface.co/zai-org/GLM-5.3-Flash); [HF zai-org listing](https://huggingface.co/api/models?author=zai-org&sort=createdAt&direction=-1&limit=25)).
  - No GLM "Air" release followed GLM-4.5-Air (2025-07-20). An org search for "Air" returns only the GLM-4.5-Air repos ([HF search](https://huggingface.co/api/models?author=zai-org&search=Air&sort=createdAt&direction=-1&limit=20)).
  - GLM-4.7-Flash (30B-A3B, 2026-01-19) is the newest small GLM ([HF API](https://huggingface.co/api/models/zai-org/GLM-4.7-Flash?blobs=true)).
- **DeepSeek**
  - V4-Flash (2026-04-22) was succeeded by **V4-Flash-0731**, "the official release of DeepSeek-V4-Flash, superseding the preview version" ([V4-Flash-0731 card](https://huggingface.co/deepseek-ai/DeepSeek-V4-Flash-0731)).
  - V4.1-Flash (2026-09-10) and V4.1-Flash-1017 (2026-10-17) are much larger: Artificial Analysis lists V4.1 Flash 1017 at 552B total / 16B active ([AA open-source models](https://artificialanalysis.ai/models/open-source)).
- **MiniMax**
  - The releases are M2.5 (Feb 2026), M2.7 (Apr 2026), M3 (Jun 2026, 427B) and M3.1-Flash (2026-10-17) ([HF MiniMaxAI listing](https://huggingface.co/api/models?author=MiniMaxAI&sort=createdAt&direction=-1&limit=25)).
  - The M3.1-Flash card says "Approximately 428B total; 23B activated per token" ([card](https://huggingface.co/MiniMaxAI/MiniMax-M3.1-Flash)).
- **Meta**
  - The `meta-llama` org has released nothing since Llama 4 (Apr 2025) ([HF meta-llama listing](https://huggingface.co/api/models?author=meta-llama&sort=createdAt&direction=-1&limit=25)).
  - Meta's new open models are under `meta-models`: **Muse-Glimmer-30B** (2026-08-09) and Muse-Spark-Open-816B-A42B (2026-09-15) ([HF meta-models listing](https://huggingface.co/api/models?author=meta-models&sort=createdAt&direction=-1&limit=30)).
  - Muse Glimmer is "a 30-billion-parameter causal language model with a dedicated perception encoder, distilled from Muse Spark and purpose-built for autonomous agentic tasks on consumer hardware" ([card](https://huggingface.co/meta-models/Muse-Glimmer-30B)).
- **NVIDIA**
  - Nemotron 3 Super 120B-A12B was released 03/11/2026 ([card](https://huggingface.co/nvidia/NVIDIA-Nemotron-3-Super-120B-A12B-NVFP4)).
  - **Nemotron 3.5 Super VL 120B-A12B** followed on 10/15/2026 ([card](https://huggingface.co/nvidia/NVIDIA-Nemotron-3.5-Super-VL-120B-A12B-NVFP4)).
  - **Nemotron 3.5 Bolt 74B-A9B** was also released 10/15/2026 and is derived from 3.5 Super VL (`base_model: nvidia/NVIDIA-Nemotron-3.5-Super-VL-120B-A12B-BF16`) ([card](https://huggingface.co/nvidia/NVIDIA-Nemotron-3.5-Bolt-74B-A9B-BF16)).
  - Other releases are Nemotron 3.5 Lightning 30B-A3B (Aug 2026) and Nemotron 3 Ultra 550B (Jun 2026, out of class) ([HF nvidia search](https://huggingface.co/api/models?author=nvidia&search=Nemotron-3&sort=createdAt&direction=-1&limit=60)).
  - Nemotron 3.5 Super exists only as the VL model (plus a Base). There is no text-only 3.5 Super repo ([HF search](https://huggingface.co/api/models?search=Nemotron-3.5-Super&sort=downloads&direction=-1&limit=12)).
- **Mistral**
  - The in-class releases are Mistral-Small-4-119B-2603 (Mar 2026) and Mistral-Medium-3.5-128B (2026-03-31).
  - The newest release is Mistral-Large-4-1T-A52B (2026-09-30), which is out of class.
  - Leanstral (a Lean prover) and Shieldstral are specialists ([HF mistralai listing](https://huggingface.co/api/models?author=mistralai&sort=createdAt&direction=-1&limit=25)).
- **Google**
  - Gemma 4 was released 2026-03-11: 31B-it, 26B-A4B-it, E2B, E4B, and a 12B added 2026-05-23. No newer Gemma exists ([HF google gemma-4 search](https://huggingface.co/api/models?author=google&search=gemma-4&sort=createdAt&direction=-1&limit=40)).
- **inclusionAI (Ant)**
  - **Ling-3.2-air** was released 2026-11-23.
  - Ling-3.0-flash (2026-08-02) is 124B with 5.1B active.
  - Ling-3.1-flash (Oct 2026) is now 572B, out of class.
  - Ling/Ring-flash-2.0 (Sep 2025) are superseded ([HF inclusionAI Ling search](https://huggingface.co/api/models?author=inclusionAI&search=Ling&sort=createdAt&direction=-1&limit=40); [Ling-3.2-air card](https://huggingface.co/inclusionAI/Ling-3.2-air)).
- **Tencent**
  - **Hy4 flash** (2026-11-17) has "220B total parameters (including 64B Engram parameters) with just 12B active parameters per token" ([card](https://huggingface.co/tencent/Hy4-flash)).
  - Hy4 is 774B. Hunyuan-A13B (Jun 2025) is superseded ([HF tencent HY search](https://huggingface.co/api/models?author=tencent&search=HY-&sort=createdAt&direction=-1&limit=30)).
- **Baidu**
  - **ERNIE-5.5-Flash-Preview** (2026-11-23) has "280B total parameters, 14B of them active per token, and a 1M-token context window". It is "an intermediate checkpoint from post-training" ([card](https://huggingface.co/baidu/ERNIE-5.5-Flash-Preview)).
  - ERNIE-4.5-21B-A3B-Thinking (Sep 2025) is the last small ERNIE ([HF baidu listing](https://huggingface.co/api/models?author=baidu&search=ERNIE&sort=createdAt&direction=-1&limit=25)).
- **StepFun**
  - Step-3.7-Flash (2026-05-23) is a 198B vision-language MoE with about 11B active. It supersedes Step-3.5-Flash (2026-02-01, text-only).
  - Step-5-Preview (Sep 2026) is out of class ([HF stepfun listing](https://huggingface.co/api/models?author=stepfun-ai&search=Step&sort=createdAt&direction=-1&limit=30); [Step-3.7-Flash card](https://huggingface.co/stepfun-ai/Step-3.7-Flash)).
- **Xiaomi**
  - **MiMo-V2.6-Flash** comes in RL (2026-09-21) and MOPD (2026-09-27) checkpoints: "309B total / 15B activated", 1M context, with text, image, video and audio modalities ([card](https://huggingface.co/XiaomiMiMo/MiMo-V2.6-Flash-RL)).
- **Meituan**
  - LongCat-Flash-Lite (2026-01-27) was followed by LongCat-Flash-Lite-Sparse (2026-07-31): "69B total parameters and approximately 3B activated", non-thinking, with native 1M context ([card](https://huggingface.co/meituan-longcat/LongCat-Flash-Lite-Sparse)).
  - LongCat-2.0/2.5 are large models ([HF meituan listing](https://huggingface.co/api/models?author=meituan-longcat&sort=createdAt&direction=-1&limit=25)).
- **Moonshot**
  - Kimi-Linear-48B-A3B-Instruct (2025-10-30) is still the only Moonshot model in class. K2.5 through K3.1-Pro are all very large ([HF moonshotai listing](https://huggingface.co/api/models?author=moonshotai&sort=createdAt&direction=-1&limit=25)).
- **Cohere**
  - **Command A+ 05-2026** is Apache 2.0 with "25B active parameters and 218B total parameters". It supersedes the CC-BY-NC Command A (03-2025) and Command A Reasoning (08-2025) ([Command A+ card](https://huggingface.co/CohereLabs/command-a-plus-05-2026-bf16); [HF CohereLabs search](https://huggingface.co/api/models?author=CohereLabs&search=command&sort=createdAt&direction=-1&limit=20)).
- **Others with current in-class models:**
  - Sarvam-105B-1711 (2026-11-15) ([HF sarvamai listing](https://huggingface.co/api/models?author=sarvamai&sort=createdAt&direction=-1&limit=25)).
  - AI21 Jamba2 Mini (2026-01-06).
  - IBM Granite 4.2 30B (2026-08-07).
  - Prime Intellect INTELLECT-3.1 (2026-01-20).
  - Swiss AI Apertus v1.5 70B (2026-07-24, now multimodal).
  - LG EXAONE 4.5 33B (2026-04-04).
  - Upstage Solar-Open-100B (2025-12-10) and Solar-Open2-250B (2026-07-22).
  - AllenAI Olmo 3.1 32B (2025-12-10; no newer large Olmo).
  - Nous Arete-27B (2026-11-06) and Hermes 4.3 36B (2025-11-17).
  - Thinking Machines Inkling 2 Mini (released Nov 20, 2026).
  - These come from the per-org HF listings above and the measurements in Q2.
- **Thinking Machines:** Inkling (Jul 2026) is 952B, Inkling-Small is 266B, Inkling 2 Small is 594B with 17B active, and Inkling 2 Mini is a 20 GB official checkpoint ([HF search](https://huggingface.co/api/models?search=Inkling-2&sort=downloads&direction=-1&limit=15); [AA open-source](https://artificialanalysis.ai/models/open-source)).
- **xAI and OpenAI:** xAI has released nothing past grok-2 ([HF xai-org](https://huggingface.co/api/models?author=xai-org&sort=createdAt&direction=-1&limit=20)). OpenAI has released nothing past gpt-oss-120b/20b and gpt-oss-safeguard ([HF openai listing](https://huggingface.co/api/models?author=openai&sort=createdAt&direction=-1&limit=25)).
- **Excluded specialists seen during the sweep:**
  - poolside Laguna-S-2.1: "118B total ... designed for agentic coding and long-horizon work", OpenMDW-1.1 ([card](https://huggingface.co/poolside/Laguna-S-2.1)).
  - Qwen-AgentWorld-35B-A3B: "the first language world model" for simulating agent environments ([card](https://huggingface.co/Qwen/Qwen-AgentWorld-35B-A3B)).
  - Qwen3-Coder-Next, Cohere North-Mini-Code, Mistral Leanstral, Baichuan-M3-235B (medical), and GLM-4.6V (vision-first) ([HF listings above](https://huggingface.co/api/models?author=Qwen&sort=createdAt&direction=-1&limit=100)).

### Inferences
- **The "Flash/Air" naming no longer signals that a model fits in 128 GB.** GLM-5.3-Flash, DeepSeek-V4.1-Flash, MiniMax-M3.1-Flash, Ling-3.1-flash, Hy4 flash and ERNIE-5.5-Flash all exceed 160 GB even in their smallest official precision. The truly in-class "efficient MoE" tier is now about 120B with 5-12B active: Ling-3.2-air and Ling-3.0-flash, Nemotron 3/3.5 Super, Mistral Small 4, Qwen3.5-122B-A10B, and Qwen3.8-Flash-Next at 125B plus its n-gram table.
- **Dense 27-35B models have become strong contenders.** Qwen3.8-27B, Muse Glimmer 30B and Gemma 4 31B all fit at full BF16 with large KV headroom. Qwen3.8-27B in particular now outscores much larger 2025 MoEs on Artificial Analysis (see Q3).
- **Meta's Llama line should be treated as superseded** by Muse Glimmer 30B, which is Apache-2.0, unlike the Llama community licences.

### Gaps
- I did not find a lab announcement confirming the public launch date of every repo. Some HF `createdAt` dates (Mistral Small 4, Command A+) precede the actual launch.
- Any lab that publishes only on ModelScope, GitHub or its own site, and not on HF, would be missed by this sweep.

## Q2. Per-model specs: repo, total/active parameters, recommended precision and measured size, context, licence, modality, release date

### Takeaway
About 25 serious general/reasoning models fit comfortably in 128 GB. The strongest current ones ship an official build that fits:
- Qwen3.8-27B: BF16, 55.6 GB.
- Qwen3.6-35B-A3B: BF16, 71.9 GB.
- Qwen3.5-122B-A10B: official GPTQ-Int4, 78.9 GB.
- Nemotron 3.5 Super VL: NVFP4, 75.0 GB.
- Nemotron 3 Super: NVFP4, 80.3 GB.
- Mistral Small 4: NVFP4, 70.8 GB.
- Muse Glimmer 30B: BF16, 59.6 GB.
- Gemma 4 31B: BF16, 62.6 GB.
- Ling-3.0-flash: official int4, 77.0 GB.
- Sarvam-105B-1711: official quantized build, 70.2 GB.

Qwen3.8-Flash-Next, Ling-3.2-air, Nemotron 3.5 Bolt and Mistral Medium 3.5 need a community or third-party quant. Watch the licences:
- MiniMax-M2.7 is **non-commercial**.
- EXAONE 4.5 is **NC**.
- Mistral Medium 3.5 bars companies with monthly revenue above $20M.
- Qwen3.8-Flash-Next uses the Qwen Community License, which has MaaS and large-company conditions.
- The Llama 3.3/4 community licences and the Tencent, Upstage Solar and NVIDIA licences are custom.

### Cited Findings
**A. Current models that fit with the lab's own (or NVIDIA's) build.** "Params" means total/active. Context is from config or card.

| Model (newest checkpoint) | Repo to download (recommended precision) | Measured size | Params (total / active) | Context | Licence | Modality | Released | Sources |
|---|---|---|---|---|---|---|---|---|
| Qwen3.8-27B | `Qwen/Qwen3.8-27B` (BF16); `Qwen/Qwen3.8-27B-FP8` is 30.87 GB | 55.56 GB | 27.8B dense | 262,144 native, extensible to 1,000,000 | Apache-2.0 | Text + image (image-text-to-text) | 2026-08-05 | [API](https://huggingface.co/api/models/Qwen/Qwen3.8-27B?blobs=true), [FP8 API](https://huggingface.co/api/models/Qwen/Qwen3.8-27B-FP8?blobs=true), [card](https://huggingface.co/Qwen/Qwen3.8-27B) |
| Qwen3.6-35B-A3B | `Qwen/Qwen3.6-35B-A3B` (BF16); FP8 repo is 37.46 GB | 71.90 GB | 36.0B / 3B | 262,144, extensible to 1,010,000 | Apache-2.0 | Text + image/video | 2026-04-15 | [API](https://huggingface.co/api/models/Qwen/Qwen3.6-35B-A3B?blobs=true), [card](https://huggingface.co/Qwen/Qwen3.6-35B-A3B) |
| Qwen3.5-122B-A10B | `Qwen/Qwen3.5-122B-A10B-GPTQ-Int4` (official Int4). FP8 (127.16 GB) is too tight; BF16 is 250.17 GB | 78.86 GB | 122B / 10B per card (125.1B counted) | 262,144, extensible to 1,010,000 | Apache-2.0 | Unified vision-language | 2026-02-24 (Int4 2026-03-03) | [Int4 API](https://huggingface.co/api/models/Qwen/Qwen3.5-122B-A10B-GPTQ-Int4?blobs=true), [FP8 API](https://huggingface.co/api/models/Qwen/Qwen3.5-122B-A10B-FP8?blobs=true), [card](https://huggingface.co/Qwen/Qwen3.5-122B-A10B) |
| Nemotron 3.5 Super VL 120B-A12B | `nvidia/NVIDIA-Nemotron-3.5-Super-VL-120B-A12B-NVFP4`. BF16 is 248.91 GB; for non-NVIDIA hardware, `unsloth/...-GGUF` UD-Q4_K_XL is 86.91 GB plus a 1.73 GB mmproj | 75.01 GB | 120B / 12B (124.5B counted) | 1,048,576 (config) | OpenMDW-1.1 (permissive) | Vision-language (C-RADIO encoder) | 10/15/2026 | [API](https://huggingface.co/api/models/nvidia/NVIDIA-Nemotron-3.5-Super-VL-120B-A12B-NVFP4?blobs=true), [GGUF API](https://huggingface.co/api/models/unsloth/NVIDIA-Nemotron-3.5-Super-VL-120B-A12B-GGUF?blobs=true), [card](https://huggingface.co/nvidia/NVIDIA-Nemotron-3.5-Super-VL-120B-A12B-NVFP4) |
| Nemotron 3 Super 120B-A12B (superseded by 3.5 Super VL, but text-only) | `nvidia/NVIDIA-Nemotron-3-Super-120B-A12B-NVFP4`. FP8 is 128.35 GB and BF16 is 247.23 GB; unsloth UD-Q4_K_XL is 83.78 GB | 80.32 GB | 120B / 12B (123.6B counted) | 262,144 (RULER reported to 512K) | NVIDIA Nemotron Open Model License | Text | 03/11/2026 | [API](https://huggingface.co/api/models/nvidia/NVIDIA-Nemotron-3-Super-120B-A12B-NVFP4?blobs=true), [card](https://huggingface.co/nvidia/NVIDIA-Nemotron-3-Super-120B-A12B-NVFP4) |
| Mistral Small 4 (2603) | `mistralai/Mistral-Small-4-119B-2603-NVFP4` (consolidated format), or the HF-format FP8 shards in `mistralai/Mistral-Small-4-119B-2603` at 120.92 GB (tight); unsloth UD-Q4_K_XL is 74.16 GB | 70.81 GB | 119B / 6.5B (128 experts, 4 active) | 256K (card) | Apache-2.0 | Text + image; reasoning toggle | 2026-03 (repo created 2026-01-23) | [NVFP4 API](https://huggingface.co/api/models/mistralai/Mistral-Small-4-119B-2603-NVFP4?blobs=true), [API](https://huggingface.co/api/models/mistralai/Mistral-Small-4-119B-2603?blobs=true), [card](https://huggingface.co/mistralai/Mistral-Small-4-119B-2603) |
| Muse Glimmer 30B (Meta) | `meta-models/Muse-Glimmer-30B` (BF16) | 59.55 GB | 29.6B, incl. a 1.8B ViT | 131,072+ | Apache-2.0 | Text + image | Aug 2026 (2026-08-09) | [API](https://huggingface.co/api/models/meta-models/Muse-Glimmer-30B?blobs=true), [card](https://huggingface.co/meta-models/Muse-Glimmer-30B) |
| Gemma 4 31B-it | `google/gemma-4-31B-it` (BF16); Google also ships QAT w4a16 builds | 62.55 GB | 31.3B dense | 256K | Apache-2.0 | Text + image (+video) | 2026-03-11 | [API](https://huggingface.co/api/models/google/gemma-4-31B-it?blobs=true), [card](https://huggingface.co/google/gemma-4-31B-it) |
| Gemma 4 26B-A4B-it | `google/gemma-4-26B-A4B-it` (BF16) | 51.61 GB | 25.8B MoE, about 4B active (name) | 256K | Apache-2.0 | Text + image | 2026-03-11 | [API](https://huggingface.co/api/models/google/gemma-4-26B-A4B-it?blobs=true) |
| Ling-3.0-flash | `inclusionAI/Ling-3.0-flash-int4` (77.01 GB) or `-fp4` (70.37 GB). FP8 is 128.44 GB and BF16 is 254.98 GB | 77.01 GB | 124B / 5.1B (hybrid KDA + MLA) | 262,144 | MIT | Text (a separate -VL variant exists) | 2026-08-02 | [int4 API](https://huggingface.co/api/models/inclusionAI/Ling-3.0-flash-int4?blobs=true), [fp4 API](https://huggingface.co/api/models/inclusionAI/Ling-3.0-flash-fp4?blobs=true), [card](https://huggingface.co/inclusionAI/Ling-3.0-flash) |
| Sarvam-105B-1711 | `sarvamai/sarvam-105b-1711`: a compressed-tensors quantized build with 64.64 GB of weights plus a 5.51 GB DSpark draft. `-bf16` is 218.22 GB | 70.15 GB | 105B / 10.3B | 163,840 (config) | Apache-2.0 | Text | 2026-11-15 | [API](https://huggingface.co/api/models/sarvamai/sarvam-105b-1711?blobs=true), [BF16 API](https://huggingface.co/api/models/sarvamai/sarvam-105b-1711-bf16?blobs=true), [card](https://huggingface.co/sarvamai/sarvam-105b-1711) |
| GLM-4.7-Flash | `zai-org/GLM-4.7-Flash` (BF16) | 62.44 GB | 31.2B, 30B-A3B class | 202,752 | MIT | Text | 2026-01-19 | [API](https://huggingface.co/api/models/zai-org/GLM-4.7-Flash?blobs=true), [card](https://huggingface.co/zai-org/GLM-4.7-Flash) |
| LongCat-Flash-Lite | `meituan-longcat/LongCat-Flash-Lite-FP8`. The newer `-Sparse` (Jul 2026, 1M context) is BF16-only at 138.29 GB | 101.76 GB | 69B / about 3B | 327,680 (Sparse: 983,040) | MIT | Text, non-thinking | 2026-01-27 | [FP8 API](https://huggingface.co/api/models/meituan-longcat/LongCat-Flash-Lite-FP8?blobs=true), [Sparse API](https://huggingface.co/api/models/meituan-longcat/LongCat-Flash-Lite-Sparse?blobs=true), [card](https://huggingface.co/meituan-longcat/LongCat-Flash-Lite-Sparse) |
| Kimi-Linear-48B-A3B-Instruct | `moonshotai/Kimi-Linear-48B-A3B-Instruct` (BF16) | 98.25 GB | 49.1B / 3B | 1,048,576 | MIT | Text | 2025-10-30 | [API](https://huggingface.co/api/models/moonshotai/Kimi-Linear-48B-A3B-Instruct?blobs=true), [card](https://huggingface.co/moonshotai/Kimi-Linear-48B-A3B-Instruct) |
| Jamba2 Mini | `ai21labs/AI21-Jamba2-Mini-FP8` (55.08 GB) or BF16 (103.14 GB) | 55.08 GB | 52B / 12B (16 experts, top-2) | 262,144 | Apache-2.0 | Text, non-reasoning | 2026-01-06 | [FP8 API](https://huggingface.co/api/models/ai21labs/AI21-Jamba2-Mini-FP8?blobs=true), [card](https://huggingface.co/ai21labs/AI21-Jamba2-Mini) |
| Granite 4.2 30B | `ibm-granite/granite-4.2-30b` (BF16); FP8 is 30.11 GB | 58.55 GB | 29.3B dense | 131,072 in config; card claims 512K | Apache-2.0 | Text, thinking modes | 2026-08-07 | [API](https://huggingface.co/api/models/ibm-granite/granite-4.2-30b?blobs=true), [card](https://huggingface.co/ibm-granite/granite-4.2-30b) |
| EXAONE 4.5 33B | `LGAI-EXAONE/EXAONE-4.5-33B` (BF16); FP8 is 41.31 GB | 68.70 GB | 31.7B LLM + 1.29B vision encoder | 262,144 | **EXAONE AI Model License 1.2 - NC (non-commercial)** | Text + image | 2026-04-04 | [API](https://huggingface.co/api/models/LGAI-EXAONE/EXAONE-4.5-33B?blobs=true), [card](https://huggingface.co/LGAI-EXAONE/EXAONE-4.5-33B), [licence](https://huggingface.co/LGAI-EXAONE/EXAONE-4.5-33B/blob/main/LICENSE) |
| Olmo 3.1 32B Think / Instruct | `allenai/Olmo-3.1-32B-Think` (BF16) | 64.47 GB | 32.2B dense | 65,536 | Apache-2.0 (fully open) | Text | 2025-12-10 | [API](https://huggingface.co/api/models/allenai/Olmo-3.1-32B-Think?blobs=true) |
| Hermes 4.3 36B | `NousResearch/Hermes-4.3-36B` (BF16; Seed-OSS base) | 72.30 GB | 36.2B dense | 524,288 | Apache-2.0 | Text | 2025-11-17 | [API](https://huggingface.co/api/models/NousResearch/Hermes-4.3-36B?blobs=true) |
| Arete-27B (Nous) | `NousResearch/Arete-27B` (BF16) | 54.71 GB | 27.4B; a Qwen3.8-27B fine-tune on "about 52.8 million tokens" | 262,144 | Apache-2.0 | Text | 2026-11-06 | [API](https://huggingface.co/api/models/NousResearch/Arete-27B?blobs=true), [card](https://huggingface.co/NousResearch/Arete-27B) |
| GLM-4.5-Air (superseded, still popular) | `zai-org/GLM-4.5-Air-FP8` (tight); unsloth Q4_K_M is 72.98 GB | 112.56 GB | 110.5B, 106B-A12B class | 131,072 | MIT | Text | 2025-07-20 | [FP8 API](https://huggingface.co/api/models/zai-org/GLM-4.5-Air-FP8?blobs=true), [GGUF API](https://huggingface.co/api/models/unsloth/GLM-4.5-Air-GGUF?blobs=true) |

**B. Current models that fit only through a third-party quant** (the lab ships only BF16/FP8 that is too large):

| Model | Recommended file set (measured) | Lab-precision size (does not fit) | Params | Context | Licence | Modality | Released | Sources |
|---|---|---|---|---|---|---|---|---|
| **Qwen3.8-Flash-Next** | `unsloth/Qwen3.8-Flash-Next-GGUF`: UD-Q3_K_XL **89.46 GB** or UD-IQ4_XS **93.67 GB** (UD-Q4_K_XL 111.33 GB is too large in practice; see Q3). Alternatives: `ISTA-DASLab/Qwen3.8-Flash-Next-GSQ-RCO-GGUF` IQ3_S 83.62 GB (most downloaded); mmproj 1.81 GB; optional MTP file 24.62 GB | FP8 185.52 GB; BF16 360.0 GB | "125B with 6B activated, plus 51B n-gram embedding and 4B MTP" (180.0B on disk) | 262,144, extensible to 1,000,000 | **Qwen Community License 1.0** (permissive, with conditions: see below) | Text + image | 2026-08-24 | [unsloth API](https://huggingface.co/api/models/unsloth/Qwen3.8-Flash-Next-GGUF?blobs=true), [ISTA API](https://huggingface.co/api/models/ISTA-DASLab/Qwen3.8-Flash-Next-GSQ-RCO-GGUF?blobs=true), [FP8 API](https://huggingface.co/api/models/Qwen/Qwen3.8-Flash-Next-FP8?blobs=true), [card](https://huggingface.co/Qwen/Qwen3.8-Flash-Next) |
| **Ling-3.2-air** | `bartowski/Ling-3.2-air-GGUF`: Q5_K_M **97.62 GB**, Q4_K_M **80.39 GB**, Q6_K 111.43 GB (tight); mmproj 1.75 GB | FP8 129.28 GB (official) | 124B / 5.5B (127.9B counted) | 262,144 | MIT | Natively multimodal (same architecture as Ling-3.0-flash-VL) | 2026-11-23 | [GGUF API](https://huggingface.co/api/models/bartowski/Ling-3.2-air-GGUF?blobs=true), [FP8 API](https://huggingface.co/api/models/inclusionAI/Ling-3.2-air?blobs=true), [card](https://huggingface.co/inclusionAI/Ling-3.2-air) |
| **Nemotron 3.5 Bolt 74B-A9B** | `unsloth/NVIDIA-Nemotron-3.5-Bolt-74B-A9B-GGUF` Q8_0 **78.85 GB** (UD-Q6_K_XL 72.39 GB). NVIDIA's own `...-24GB-GGUF` (IQ2) is 22.02 GB plus a 1.73 GB mmproj | BF16 149.77 GB | 74B / 9B | 262,144 | OpenMDW-1.1 | Vision-language | 10/15/2026 | [unsloth API](https://huggingface.co/api/models/unsloth/NVIDIA-Nemotron-3.5-Bolt-74B-A9B-GGUF?blobs=true), [NVIDIA GGUF API](https://huggingface.co/api/models/nvidia/NVIDIA-Nemotron-3.5-Bolt-74B-A9B-24GB-GGUF?blobs=true), [card](https://huggingface.co/nvidia/NVIDIA-Nemotron-3.5-Bolt-74B-A9B-BF16) |
| **Mistral Medium 3.5** | `nvidia/Mistral-Medium-3.5-128B-NVFP4` **95.23 GB**, or `unsloth/Mistral-Medium-3.5-128B-GGUF` Q6_K **102.58 GB** / Q4_K_M 74.90 GB (mmproj 21.45 GB) | FP8 133.61 GB (one format) | 128B dense | 256K | **Modified MIT: no rights if your company's monthly revenue is above $20M** | Text + image | 2026-03-31 | [NVFP4 API](https://huggingface.co/api/models/nvidia/Mistral-Medium-3.5-128B-NVFP4?blobs=true), [GGUF API](https://huggingface.co/api/models/unsloth/Mistral-Medium-3.5-128B-GGUF?blobs=true), [card](https://huggingface.co/mistralai/Mistral-Medium-3.5-128B), [licence](https://huggingface.co/mistralai/Mistral-Medium-3.5-128B/blob/main/LICENSE) |
| Apertus v1.5 70B | `onprem-ai/Apertus-v1.5-70B-FP8` **76.16 GB** (community) | BF16 144.6 GB (gated=auto) | 72.0B dense | 262,144 (FP8 config) | Apache-2.0 | Text + image | 2026-07-24 | [FP8 API](https://huggingface.co/api/models/onprem-ai/Apertus-v1.5-70B-FP8?blobs=true), [BF16 API](https://huggingface.co/api/models/swiss-ai/Apertus-v1.5-70B?blobs=true) |
| Solar Open 100B | `nota-ai/Solar-Open-100B-Nota-FP8` **104.38 GB** or `cyankiwi/Solar-Open-100B-AWQ-4bit` 61.19 GB | BF16 205.31 GB | 102.6B / 12B | 128K | Upstage Solar License (Apache-based; derivatives must be named "Solar..." and display "Built with Solar") | Text | 2025-12-10 | [FP8 API](https://huggingface.co/api/models/nota-ai/Solar-Open-100B-Nota-FP8?blobs=true), [AWQ API](https://huggingface.co/api/models/cyankiwi/Solar-Open-100B-AWQ-4bit?blobs=true), [card](https://huggingface.co/upstage/Solar-Open-100B), [licence](https://huggingface.co/upstage/Solar-Open-100B/blob/main/LICENSE) |
| INTELLECT-3.1 | `cyankiwi/INTELLECT-3.1-AWQ-8bit` 115.68 GB (tight). The previous version's official `PrimeIntellect/INTELLECT-3-FP8` is 108.29 GB | BF16 213.71 GB | 106B / 12B (GLM-4.5-Air base) | 131,072 | MIT | Text | 2026-01-20 | [AWQ API](https://huggingface.co/api/models/cyankiwi/INTELLECT-3.1-AWQ-8bit?blobs=true), [card](https://huggingface.co/PrimeIntellect/INTELLECT-3.1) |
| Step-3.5-Flash (superseded by 3.7) | `stepfun-ai/Step-3.5-Flash-GGUF-Q4_K_S` **111.5 GB** (official; tight) | FP8 208.56 GB | 196B / 11B (199.4B counted) | 262,144 | Apache-2.0 | Text | 2026-02-01 | [GGUF API](https://huggingface.co/api/models/stepfun-ai/Step-3.5-Flash-GGUF-Q4_K_S?blobs=true), [FP8 API](https://huggingface.co/api/models/stepfun-ai/Step-3.5-Flash-FP8?blobs=true) |

**C. Older or superseded in-class models, for reference:**
- **Llama 3.3 70B Instruct**: BF16 141.11 GB. FP8 builds are `RedHatAI/Llama-3.3-70B-Instruct-FP8-dynamic` at 72.67 GB and `nvidia/Llama-3.3-70B-Instruct-FP8` at 72.66 GB. Llama 3.3 licence, 131,072 context ([API](https://huggingface.co/api/models/RedHatAI/Llama-3.3-70B-Instruct-FP8-dynamic?blobs=true)).
- **Llama 4 Scout 17B-16E**: 108.6B total, 16 experts.
  - BF16 is 217.28 GB. `RedHatAI/...-FP8-dynamic` is 114.64 GB, `nvidia/...-FP8` is 111.59 GB, and `RedHatAI/...-quantized.w4a16` is 64.87 GB.
  - Llama 4 licence; multimodal; configured context 10,485,760 ([RedHat FP8 API](https://huggingface.co/api/models/RedHatAI/Llama-4-Scout-17B-16E-Instruct-FP8-dynamic?blobs=true), [w4a16 API](https://huggingface.co/api/models/RedHatAI/Llama-4-Scout-17B-16E-Instruct-quantized.w4a16?blobs=true)).
- **Hermes 4 70B**: official FP8 at 72.67 GB, Llama 3 licence ([API](https://huggingface.co/api/models/NousResearch/Hermes-4-70B-FP8?blobs=true)).
- **Qwen3-Next-80B-A3B (Instruct/Thinking)**: 81.3B; FP8 82.05 GB; Apache-2.0; Sep 2025 ([API](https://huggingface.co/api/models/Qwen/Qwen3-Next-80B-A3B-Instruct-FP8?blobs=true)).
- **Qwen3.5-35B-A3B and 27B** (BF16 71.90 GB and 55.56 GB) and **Qwen3.6-27B** (BF16 55.56 GB) are superseded by Qwen3.6-35B-A3B and Qwen3.8-27B ([API](https://huggingface.co/api/models/Qwen/Qwen3.6-27B?blobs=true)).
- **Hunyuan-A13B-Instruct**: 80.4B; FP8 80.93 GB; GPTQ-Int4 42.67 GB.
  - Its config says 32,768 context.
  - Its licence "DOES NOT APPLY IN THE EUROPEAN UNION, UNITED KINGDOM AND SOUTH KOREA" ([API](https://huggingface.co/api/models/tencent/Hunyuan-A13B-Instruct-FP8?blobs=true), [licence](https://huggingface.co/tencent/Hunyuan-A13B-Instruct/blob/main/LICENSE)).
- **ERNIE-4.5-21B-A3B-Thinking**: 43.66 GB BF16, Apache-2.0, 131,072 context ([API](https://huggingface.co/api/models/baidu/ERNIE-4.5-21B-A3B-Thinking?blobs=true)).
- **Seed-OSS-36B-Instruct**: 72.30 GB BF16, Apache-2.0, 524,288 context, Aug 2025 ([API](https://huggingface.co/api/models/ByteDance-Seed/Seed-OSS-36B-Instruct?blobs=true)).
- **Ling-flash-2.0 / Ring-flash-2.0**: 102.9B, BF16 205.78 GB, Sep 2025 ([API](https://huggingface.co/api/models/inclusionAI/Ring-flash-2.0?blobs=true)).
- **dots.llm1.inst**: 142.8B, BF16 285.55 GB, 32,768 context. unsloth Q4_K_M is 94.48 GB ([GGUF API](https://huggingface.co/api/models/unsloth/dots.llm1.inst-GGUF?blobs=true)).
- **Command A (03-2025) and Command A Reasoning (08-2025)**: 111.1B, BF16 222.12 GB, **CC-BY-NC-4.0** ([API](https://huggingface.co/api/models/CohereLabs/command-a-reasoning-08-2025?blobs=true)).
- **Korean-focused in-class models:**
  - kanana-2-30b-a3b-thinking-2601: 61.34 GB, Kanana licence, 32,768 context ([API](https://huggingface.co/api/models/kakaocorp/kanana-2-30b-a3b-thinking-2601?blobs=true)).
  - HyperCLOVAX-SEED-Think-32B: 66.63 GB, vision, HyperCLOVA X licence ([API](https://huggingface.co/api/models/naver-hyperclovax/HyperCLOVAX-SEED-Think-32B?blobs=true)).
  - VAETKI: 112.2B, BF16 224.39 GB, MIT ([API](https://huggingface.co/api/models/NC-AI-consortium-VAETKI/VAETKI?blobs=true)).
- **Below the class floor but notable: Inkling 2 Mini.** The official `thinkingmachines/Inkling-2-Mini` is 20.16 GB (NVFP4-style modelopt checkpoint), Apache-2.0, "Date of release: November 20, 2026", multimodal ([API](https://huggingface.co/api/models/thinkingmachines/Inkling-2-Mini?blobs=true), [card](https://huggingface.co/thinkingmachines/Inkling-2-Mini)).

**Licence details that matter:**
- **Qwen Community License 1.0 (Qwen3.8-Flash-Next)** grants broad MIT-style rights, with two conditions ([licence](https://huggingface.co/Qwen/Qwen3.8-Flash-Next/blob/main/LICENSE)):
  - Products with "more than 100,000,000 monthly active users or US$ 20,000,000 ... monthly revenue" must display the model name.
  - "If the licensee or any of its affiliates conducts a Model as a Service or AI Work Assistant business, the licensee shall obtain a separate license from Qwen before Using the Software ... for any commercial purpose". Internal use is exempt.
- **Qwen3.8-27B, Qwen3.6 and Qwen3.5 remain Apache-2.0** ([Qwen3.8-27B card](https://huggingface.co/Qwen/Qwen3.8-27B); [Qwen3.6-35B-A3B card](https://huggingface.co/Qwen/Qwen3.6-35B-A3B)).
- **MiniMax-M2.7** has a "NON-COMMERCIAL LICENSE": "Non-commercial use permitted based on MIT-style terms; commercial use requires prior written authorization" ([licence](https://huggingface.co/MiniMaxAI/MiniMax-M2.7/blob/main/LICENSE)). M2.5 was tagged "modified-mit" ([API](https://huggingface.co/api/models/MiniMaxAI/MiniMax-M2.5?blobs=true)).
- **The Tencent Hy License 1.0 (Hy4 flash)** is "derived from the Apache License, Version 2.0 ... with modifications, mainly the additional commercial terms for large Model-as-a-Service ('MaaS') businesses". A MaaS business with revenue above US$50M must get a separate licence ([licence](https://huggingface.co/tencent/Hy4-flash/blob/main/LICENSE)).
- **Gemma 4 is Apache-2.0** ("License: Apache 2.0 | Authors: Google DeepMind") ([card](https://huggingface.co/google/gemma-4-31B-it)).
- **NVIDIA Nemotron 3.5 models use OpenMDW-1.1** ([Bolt card](https://huggingface.co/nvidia/NVIDIA-Nemotron-3.5-Bolt-74B-A9B-BF16)). Nemotron 3 Super uses the NVIDIA Nemotron Open Model License ([card](https://huggingface.co/nvidia/NVIDIA-Nemotron-3-Super-120B-A12B-NVFP4)).
- **Some third-party quants carry their own licence tag:**
  - `nvidia/MiniMax-M2.7-NVFP4` is tagged "nvidia-software-and-model-evaluation-license" ([API](https://huggingface.co/api/models/nvidia/MiniMax-M2.7-NVFP4?blobs=true)).
  - `nvidia/Mistral-Medium-3.5-128B-NVFP4` is tagged "nvidia-open-model-license" ([API](https://huggingface.co/api/models/nvidia/Mistral-Medium-3.5-128B-NVFP4?blobs=true)).

### Inferences
- **Official NVFP4 builds target NVIDIA Blackwell-class hardware such as DGX Spark.** These are Nemotron 3/3.5 Super, Mistral Small 4, and NVIDIA's Mistral-Medium-3.5 and Qwen3.5-122B NVFP4 (83.5 GB, [API](https://huggingface.co/api/models/nvidia/Qwen3.5-122B-A10B-NVFP4?blobs=true)). On Strix Halo or a Mac, the matching unsloth/bartowski GGUF (sizes above) is the practical equivalent. This hardware mapping is my inference and was not verified in this session.
- **Some "official" sizes are deceptively large.** Mistral repos double-store weights in two formats, and Llama repos add `original/` .pth copies (Llama 3.3: 282 GB raw versus 141 GB of safetensors). Download only one format.
- **Repo totals can include extra files.** The 70.15 GB Sarvam total includes a 5.51 GB speculative-decoding draft (DSpark). Several unsloth GGUFs ship separate MTP/DSpark/mmproj files that are optional for text-only use.

### Gaps
- **Sarvam-105B-1711's default quant format** is listed only as `compressed-tensors`. The card benchmarks a "Sarvam-105B-1711(NVFP4)" column, so NVFP4 is likely, but not confirmed from the config.
- **Kimi-Linear-48B AWQ builds** exist (e.g. cyankiwi), but I did not measure their size.
- **LongCat-Flash-Lite-Sparse quants** (Intel MXFP4 AutoRound, mlx) were not measured.
- **Active parameters for DeepSeek-V4-Flash(-0731)** are not stated in the 0731 card. MiniMax-M2.7's about 10B active is inferred only from Cerebras' "REAP-139B-A10B" repo naming ([API](https://huggingface.co/api/models/cerebras/MiniMax-M2.5-REAP-139B-A10B?blobs=true)).
- **Safetensors "parameter totals" are unreliable for quantized repos** (packed FP4/INT4 understates them, and extra heads inflate them). Wherever the card gives a figure, these notes use it.

## Q3. What is each model best at, and where does it stand on current benchmarks and leaderboards?

### Takeaway
On Artificial Analysis Intelligence Index **v5** (introduced 2026-10-27, with much harder agentic, science and coding evals), the in-class leaders are:
- Qwen3.8-Flash-Next ≈ 28 (needs a 3-4-bit quant).
- Ling-3.2-air ≈ 23.
- Qwen3.8-27B (Xhigh) = 20.
- Inkling 2 Mini ≈ 20 (below the class floor).
- Nemotron 3.5 Super VL = 14.

For comparison, the best released-weights model overall scores 32 (DeepSeek V4.1 Flash 1017 and GLM-5.3), and Kimi K3.1 Pro scores 46-47. Older-style models such as Gemma 4 31B, Mistral Small 4 and gpt-oss-120b score about 4-6 on v5.

By use case:
- **Agentic coding and tool use:** Qwen3.8-27B and Flash-Next, Muse Glimmer and Nemotron 3.5 Super VL lead.
- **Long context:** Nemotron Super and the linear-attention hybrids lead (Kimi-Linear, Ling, LongCat-Sparse).
- **Instruction following and multilingual chat:** Sarvam-105B and Qwen3.8-27B lead.
- **Grounded RAG:** Jamba2 Mini and Command A+ are positioned for it.

No in-class model appears among the LMArena open-weight leaders, which are all 550B or larger.

### Cited Findings
**Leaderboard context (dated):**
- **Artificial Analysis Intelligence Index v5 (article dated October 27, 2026)**
  - It adds FrontierCode 2.0 and Terminal-Bench-Science, uses HLE-Diamond, AA-Omniscience v1.1 and CritPt v2, and drops GDPval-AA, SciCode and AA-LCR.
  - "About 60% of the Index now uses private test sets."
  - Kimi K3.1 Pro (46) is the top open-weights model. Among models with released weights, "DeepSeek V4.1 Flash 1017 (max)" and "GLM-5.3 (max)" score 32. Claude Haiku 5.5 scores 36. Pre-v5 scores are not comparable.
  - Source: [AA v5](https://artificialanalysis.ai/articles/artificial-analysis-intelligence-index-v5).
- **AA open-source page (Index v5.0.2, fetched 2026-11-24):**
  - Kimi K3.1 Pro 47 (2.8T/104B); Tencent Hy4 35 (774B/50.2B); DeepSeek V4.1 Flash 1017 (Max) 32 (552B/16B); GLM-5.3 (Max) 32; Inkling 2 Small 31 (594B/17B); MiMo-V2.6-Pro 30; Ling 3.1 Flash 1019 30 (560B/25B); Mistral Large 4 (High) 26.
  - **Qwen3.8 27B (Xhigh) 20** and **Nemotron 3.5 Super VL 14 (120B/12B)**.
  - Source: [AA open-source models](https://artificialanalysis.ai/models/open-source).
- **AA leaderboard (v5 scale, fetched 2026-11-24; values read through a page summary, so confirm before quoting):**
  - **Qwen3.8-Flash-Next 28**, **Ling-3.2-air 23**, GLM-5.3-Flash Max 28, MiniMax-M3.1-Flash 29, MiMo-V2.6-Flash 27.
  - Command A+ 6, Nemotron 3.5 Lightning 6, Mistral Medium 3.5 5, Mistral Small 4 Reasoning 4, Gemma 4 31B Reasoning 4, gpt-oss-120b High 4.
  - Listed without a score: Qwen3.6 35B A3B, Qwen3.5 122B A10B, Gemma 4 26B A4B, Nemotron 3.5 Bolt, Sarvam 105B, Llama 4 Scout, Llama 3.3 70B, Olmo 3.1 32B Think, Granite 4.2 30B, Kimi Linear, EXAONE 4.5 33B.
  - Source: [AA leaderboard](https://artificialanalysis.ai/leaderboards/models).
- **Inkling 2 Mini** is described by AA as scoring "20 on its Intelligence Index ... an open weights looped transformer on the Pareto frontier for Intelligence vs. active parameters" (search snippet; [AA](https://artificialanalysis.ai/articles/recent-open-weights-model-launches)).
- **LMArena Text (as reported Nov 3, 2026 by a secondary aggregator, not LMArena directly):** Gemini 4 Carbon leads at 1528. The top open-weight models are Kimi K3 1488 (19th), GLM 5.3 1481 (28th) and DeepSeek V4.1 Flash 1476 (35th), all out of class ([af.net](https://af.net/realtime/google-gemini-4-carbon-leads-lmarena-text-leaderboard-with-1528-score/)).

**Per-model strengths and lab-reported benchmarks** (vendor numbers unless noted):
- **Qwen3.8-27B** (best dense model in class; agentic coding):
  - Terminal Bench 2.1 73.0, SWE-bench Pro 61.7, DeepSWE 1.1 42.2, IFBench 79.5, GPQA Diamond 89.2, HLE 30.8, LiveCodeBench v6 90.3, WebArena-Verified 64.8.
  - Thinking is on by default, adjustable via `reasoning_effort` ([card](https://huggingface.co/Qwen/Qwen3.8-27B)).
  - In Sarvam's third-party comparison table it scores Arena-Hard v2 80.8, τ²-Bench 85.8, BFCL v4 71.6, AIME 2026 93.6 ([Sarvam card](https://huggingface.co/sarvamai/sarvam-105b-1711)).
- **Qwen3.8-Flash-Next** (strongest in class if quantized; agentic coding and tool use):
  - DeepSWE 1.1 58.7, SWE-bench Pro 62.5, Toolathlon Verified 73.5, IFBench 81.3, GPQA Diamond 91.7, HLE 35.9, LiveCodeBench v6 91.9.
  - The same card scores DeepSeek-V4-Flash-0731 at 54.4 / 56.0 / 70.3 / 79.2 / 90.8 / 33.8 / 90.6 on those benchmarks.
  - It introduces "Qwen Sparse Attention (QSA)" ([card](https://huggingface.co/Qwen/Qwen3.8-Flash-Next)).
- **Qwen3.6-35B-A3B** (fast 3B-active general model):
  - SWE-bench Verified 73.4, SWE-bench Pro 49.5, Terminal-Bench 2.0 51.5, MMLU-Pro 85.2, GPQA 86.0, HLE 21.4, LiveCodeBench v6 80.4, AIME26 92.7, MMMU 81.7 ([card](https://huggingface.co/Qwen/Qwen3.6-35B-A3B)).
- **Qwen3.5-122B-A10B** (long context, tool use, 201 languages):
  - MMLU-Pro 86.7, GPQA Diamond 86.6, HLE w/ CoT 25.3, HLE w/ tool 47.5, SWE-bench Verified 72.0, Terminal Bench 2 49.4.
  - BFCL-V4 72.2, TAU2 79.5, AA-LCR 66.9, LongBench v2 60.2, MMMU 83.9.
  - The same table puts GPT-OSS-120B at 80.8 / 80.1 / 14.9 / 19.0 / 62.0 / 18.7, then BFCL "--", 50.7 AA-LCR and 48.2 LongBench v2 ([card](https://huggingface.co/Qwen/Qwen3.5-122B-A10B)).
- **Nemotron 3.5 Super VL** (agentic, browsing, 1M context, vision; NVFP4 numbers):
  - GPQA-Diamond 89.90, HLE text 31.44, LiveCodeBench v6 87.41, SWE-bench Verified 71.07, SWE-bench Pro 51.30, Terminal-Bench 2.1 62.78.
  - tau2 retail/airline/telecom 69.30 / 75.75 / 73.19, BrowseComp 77.5, AA-LCR v1.1 67.69, IFBench 71.50.
  - NVFP4 is within about 1 point of BF16 on most rows ([card](https://huggingface.co/nvidia/NVIDIA-Nemotron-3.5-Super-VL-120B-A12B-NVFP4)).
- **Nemotron 3 Super** (long-context specialist):
  - NVFP4: MMLU-Pro 83.33, GPQA 79.42, LiveCodeBench v6 78.44, HLE 17.42, Arena-Hard-V2 76.00, AA-LCR 58.06.
  - RULER-500 @512k 96.23; MMLU-ProX 79.37 ([card](https://huggingface.co/nvidia/NVIDIA-Nemotron-3-Super-120B-A12B-NVFP4)).
- **Nemotron 3.5 Bolt 74B-A9B** (local agentic VLM):
  - BF16: Terminal-Bench 2.1 53.93, HLE 28.73, GPQA Diamond 88.83, AA-LCR 67.38, MCP-Atlas 55.60, BrowseComp 70.00.
  - The 24 GB IQ2 build keeps most of that (50.98 / 25.30 / 86.43 / 64.06 / 52.20 / 66.75).
  - The same table puts Qwen3.6-35B-A3B at 44.38 / 19.56 / 83.40 / 61.06 / 51.20 / 48.74 ([card](https://huggingface.co/nvidia/NVIDIA-Nemotron-3.5-Bolt-74B-A9B-BF16)).
- **Muse Glimmer 30B** (agentic, math), versus Gemma4-31B and Qwen3.6-27B:
  - MCP Atlas 75.5 (54.2 / 62.5), SWE-Bench Pro 51.2 (36.9 / 50.2), SWE-Bench Verified 76.0 (66.6 / 77.2), OSWorld-Verified 65.9 (58.5 / 75.6).
  - AIME 2026 94.7 (89.2 / 94.1), GPQA Diamond (AA) 83.5 (85.7 / 84.2), HLE Text (AA) 22.0 (23.6 / 23.1).
  - Source: [card](https://huggingface.co/meta-models/Muse-Glimmer-30B).
- **Gemma 4 31B** (well-rounded dense model with strong vision; weak on the new hard agentic suites):
  - AIME 2026 89.2%, LiveCodeBench v6 80.0%, GPQA Diamond 84.3%, Tau2 76.9%, HLE 19.5% (no tools) / 26.5% (with search), MMMU Pro 76.9% ([card](https://huggingface.co/google/gemma-4-31B-it)).
  - Thinking Machines' table gives Gemma 4 31B DeepSWE 0.3% and Terminal Bench v4.0 0.0% ([Inkling 2 Mini card](https://huggingface.co/thinkingmachines/Inkling-2-Mini)).
- **Sarvam-105B-1711** (instruction following, Indian languages):
  - IFEval 96.5, IFBench 82.5, MultiChallenge 62.2, Arena-Hard v2 76.6, τ²-Bench 85.5, BFCL v4 62.4.
  - MMLU-Pro 85.2, GPQA Diamond 80.4, HLE 12.3, AIME 2026 89.7, LiveCodeBench v6 75.3, BrowseComp 41.4.
  - Its NVFP4 column is within about 1-2 points ([card](https://huggingface.co/sarvamai/sarvam-105b-1711)).
- **Mistral Small 4** in Sarvam's table: IFEval 82.3, GPQA 71.0, MMLU-Pro 80.6, τ² 55.3, AIME 2026 87.1. Mistral's own card publishes benchmarks only as images ([Sarvam card](https://huggingface.co/sarvamai/sarvam-105b-1711); [Mistral card](https://huggingface.co/mistralai/Mistral-Small-4-119B-2603)).
  - Nemotron-Super-120B in the same table: IFEval 90.8, GPQA 78.9, τ² 77.5.
  - Gemma-4-31B in the same table: IFEval 94.7, GPQA 84.3, τ² 81.1.
- **Mistral Medium 3.5:** "our first flagship merged model ... dense 128B", with a reasoning toggle, multimodal input and dozens of languages. Its benchmarks are published as images only ([card](https://huggingface.co/mistralai/Mistral-Medium-3.5-128B)).
- **GLM-4.7-Flash** (efficient 30B-A3B agent): AIME 25 91.6, GPQA 75.2, HLE 14.4, SWE-bench Verified 59.2, τ²-Bench 79.5, BrowseComp 42.8 ([card](https://huggingface.co/zai-org/GLM-4.7-Flash)).
- **Ling-3.0-flash / Ling-3.2-air:**
  - Ling-3.0 uses a "native hybrid linear attention architecture ... 5:1 alternating stacking of Kimi Delta Attention (KDA) and MLA" with "10,000+ interactive training environments" ([card](https://huggingface.co/inclusionAI/Ling-3.0-flash)).
  - Ling-3.2-air "shares the same native multimodal architecture as Ling-3.0-flash-VL" ([card](https://huggingface.co/inclusionAI/Ling-3.2-air)).
- **LongCat-Flash-Lite-Sparse** (cheap 3B-active, 1M-context agent): SWE-Bench Verified 68.20, SWE-Bench Pro 40.63, τ²-Telecom 95.18, MMLU-Pro 79.24, GPQA-Diamond 69.49, AIME 2026 65.73 ([card](https://huggingface.co/meituan-longcat/LongCat-Flash-Lite-Sparse)).
- **Kimi-Linear-48B** (long context): "reduces the need for large KV caches by up to 75% and boosts decoding throughput by up to 6× for contexts as long as 1M" ([card](https://huggingface.co/moonshotai/Kimi-Linear-48B-A3B-Instruct)).
- **Jamba2 Mini** (grounded enterprise QA and RAG, not a reasoner): "delivers precise question answering without the computational overhead of reasoning models", "leads on instruction following and grounding metrics" ([card](https://huggingface.co/ai21labs/AI21-Jamba2-Mini)).
- **Granite 4.2 30B:** AIME25 89.17, GPQA 66.41, MMLU-Pro 77.60, LiveCodeBench v6 75.77, Arena-Hard-V2 67.93, BFCL v4 61.39, RULER 128K 81.38 ([card](https://huggingface.co/ibm-granite/granite-4.2-30b)).
- **EXAONE 4.5 33B** (Korean, documents, vision; NC licence): AIME 2026 92.6, GPQA-Diamond 80.5, LiveCodeBench v6 81.4, MMLU-Pro 83.3, τ2 retail 77.9, IFBench 62.6, AA-LCR 50.6 ([card](https://huggingface.co/LGAI-EXAONE/EXAONE-4.5-33B)).
- **Solar Open 100B** (Korean), versus gpt-oss-120b high: KMMLU-Pro 64.0 vs 62.6, MMLU-Pro 80.4 vs 80.4, GPQA-Diamond 68.1 vs 78.0, AIME 2025 84.3 vs 91.7 ([card](https://huggingface.co/upstage/Solar-Open-100B)).
- **Command A+:** "optimized for agentic, multilingual, and reasoning-heavy tasks with a focus on enterprise performance", 48 languages, 128K input / 64K output ([card](https://huggingface.co/CohereLabs/command-a-plus-05-2026-bf16)).
- **Arete-27B** (Nous) targets propaganda/epistemics: "Arete-27B scored 2.88 ± 0.03 and Qwen3.8-27B scored 2.24 ± 0.05" on its held-out Propaganda_Eval ([card](https://huggingface.co/NousResearch/Arete-27B)).
- **Inkling 2 Mini** (below class), versus Qwen3.8-27B: DeepSWE 48.2% vs 42.2%, Terminal Bench v2.1 71.6% vs 73.0%, BrowseComp 79.0% vs 61.6%, MCP Atlas 82.7% vs 77.3%, HLE-Diamond 15.2% vs 15.3% ([card](https://huggingface.co/thinkingmachines/Inkling-2-Mini)).

**Real-world 128 GB unified-memory reports:**
- **Qwen3.8-Flash-Next on Strix Halo** (AMD Ryzen AI MAX+ 395, 128 GB, ROCm/llama.cpp; Aug 29, 2026) ([sleepingrobots](https://sleepingrobots.com/dreams/qwen38-flash-next-strix-halo/)):
  - Setup: an AtomicChat 4.27 bpw GGUF of **92.9 GB** at 131,072 context.
  - Speed: about 20.6 tok/s decode at short context, 15.3 tok/s at about 26K tokens, and prompt processing about 426 tok/s at about 4K.
  - The roughly 38.4 GB n-gram embedding table "pages from SSD via mmap".
  - "The Unsloth UD-Q4_K_XL build left too little memory and crashed at 32K context."
  - A hipCUB build fix is needed, or decode falls to 5-6 tok/s.
  - The EngramHalo.cpp fork reportedly reaches about 39 tok/s (unvalidated).
- **visorcraft Strix Halo table** (latest dated entry Mar 2026) ([visorcraft](https://github.com/visorcraft/strix-halo-llm-perf)):
  - MiniMax M2.5 Q3_K_M (101.76 GiB): 32.8 tok/s generation.
  - GPT-OSS 120B Q4_K_M (58.5 GiB): 53.4 tok/s.
  - Qwen3-235B-A22B Q3_K_M (104.72 GiB): 17.2 tok/s.
  - Llama 3.1 70B Q4_K_M: 5.1 tok/s, which shows how slow a dense 70B is on about 218 GB/s of bandwidth.
- **Search-result summaries (pages not fetched):**
  - Qwen3.8 27B at about 20.4 tok/s via Ollama/Vulkan on a 128 GB Beelink GTR9 Pro ([guide](https://raw.githubusercontent.com/hogeheer499-commits/strix-halo-guide/main/QWEN38_STRIX_HALO.md)).
  - Ling 3.0 Flash at about 50 tok/s decode with MTP drafting ([Level1Techs](https://forum.level1techs.com/t/what-i-run-with-my-strix-halo/254265)).
  - An AA hardware config lists Ling 3.0 Flash Q4_K_M at 77 GB ([AA configs](https://artificialanalysis.ai/hardware-inference-stack/laptops-workstations/configs)).

### Inferences
- **Best all-round pick that fits in its lab's official precision:** Qwen3.8-27B (dense, Apache-2.0, AA 20). Expect only about 10-20 tok/s at BF16 on a 218-270 GB/s unified-memory box, because every token reads all 27B weights; FP8 roughly doubles that. The speed figure is my inference from bandwidth, consistent with the about 20 tok/s Q4 reports.
- **Highest intelligence that fits:** Qwen3.8-Flash-Next at about 3.5-4.3 bpw (AA 28, close to the 32 of 550B+ models). It decodes fast (6B active), but you must accept the Qwen Community License conditions and a 3-4-bit quant.
- **Best NVIDIA-native option:** Nemotron 3.5 Super VL NVFP4 (75 GB, OpenMDW permissive, 1M context, NVFP4 at about BF16 quality). Ling-3.2-air (MIT, AA 23) is a strong alternative when you need many tokens per second, given its 5.5B active.
- **AA v5 scores of 4-6 for Gemma 4 31B, Mistral Small 4 and Medium 3.5, gpt-oss-120b and Command A+ reflect v5's hard agentic/terminal evals.** These models remain respectable on the classic benchmarks (GPQA, AIME, MMLU-Pro) listed above.

### Gaps
- I could not retrieve AA v5 scores for:
  - Nemotron 3 Super, Nemotron 3.5 Bolt and Muse Glimmer 30B.
  - Qwen3.6-35B-A3B, Qwen3.5-122B-A10B and GLM-4.7-Flash.
  - DeepSeek V4-Flash-0731, Step 3.7 Flash, Sarvam 105B, Granite 4.2, Jamba2, LongCat and Kimi-Linear.
- **Measurement-quality caveats:**
  - The leaderboard values above come from a summarizer over a dynamic page and should be spot-checked.
  - Almost all per-benchmark numbers are lab-reported, with lab-chosen harnesses (several labs re-scored "corrected" SWE-bench Pro subsets), so they are not directly comparable across cards.
  - LMArena standings come from a secondary aggregator, not the official leaderboard.
  - No quality measurements of 2-3-bit GGUF degradation were found for these 2026 MoEs.
- **Missing benchmark data:**
  - Ling-3.2-air, Ling-3.0-flash, Command A+, Jamba2 Mini, Step-3.7-Flash, GLM-5.3-Flash and Mistral cards publish their benchmark charts as images or prose only.
  - No 128 GB speed reports were found for Nemotron 3.5 Super VL, Ling-3.2-air, Muse Glimmer or Mistral Small 4.

## Q4. Which notable models just miss (about 110-170 GB), and what quant would make them fit?

### Takeaway
The most valuable near-misses are:
- **DeepSeek-V4-Flash-0731**: official 166.9 GB, MIT, 1M context, among the strongest agentic models.
- **ERNIE-5.5-Flash-Preview**: 165.9 GB, Apache-2.0, released 2026-11-23.
- **MiMo-V2.6-Flash**: 177.7 GB, MIT, omni-modal.
- **Command A+**: official W4A4 at 131.9 GB, Apache-2.0.
- **Step-3.7-Flash**: official NVFP4 at 129.2 GB, Apache-2.0.
- **Mistral Medium 3.5**: FP8 at 133.6 GB.
- **Ling-3.2-air**: FP8 at 129.3 GB.
- **Nemotron 3 Super and Qwen3.5-122B-A10B**: FP8 at about 127-128 GB.

All except ERNIE 5.5 have measured community or lab quants of 90-110 GB. ERNIE 5.5 has no reputable quant yet. Larger "Flash" models need 2-bit quants (GLM-5.3-Flash) or still do not fit even then (Hy4 flash, MiniMax-M3.1-Flash, DeepSeek-V4.1-Flash).

### Cited Findings
**Near-misses in the 110-170 GB band, with the quant that fits:**

| Model | Smallest official build (measured) | Measured quant that fits 128 GB | Licence / notes | Sources |
|---|---|---|---|---|
| **DeepSeek-V4-Flash-0731** (official release of V4-Flash; 1M context) | 166.89 GB (FP8 config with FP4-scale size; 304.2B counted) | `unsloth/DeepSeek-V4-Flash-0731-GGUF`: UD-IQ3_XXS **104.21 GB**, UD-Q2_K_XL 96.83 GB, UD-IQ2_M 90.93 GB. UD-Q3_K_XL is 128.21 GB and UD-Q4_K_XL 155.1 GB, both too big. NVIDIA's NVFP4 of V4-Flash is 168.28 GB | MIT. Terminal Bench 2.1 82.7 vs 61.8 for the preview; Toolathlon-Verified 70.3 | [GGUF API](https://huggingface.co/api/models/unsloth/DeepSeek-V4-Flash-0731-GGUF?blobs=true), [API](https://huggingface.co/api/models/deepseek-ai/DeepSeek-V4-Flash-0731?blobs=true), [NVFP4 API](https://huggingface.co/api/models/nvidia/DeepSeek-V4-Flash-NVFP4?blobs=true), [card](https://huggingface.co/deepseek-ai/DeepSeek-V4-Flash-0731) |
| **ERNIE-5.5-Flash-Preview** (280B / 14B, 1M context) | 165.9 GB ("routed experts ... stored in MXFP4 and run as W4A8"); FP8 301.87 GB; BF16 599.59 GB | **None from a reputable quanter yet.** The only GGUF is `peasantsmith/ERNIE-5.5-Flash-Preview-Maya-GGUF`, created 2026-11-24 with 0 downloads. A roughly 3-bit build would be needed | Apache-2.0. Lab-reported: Terminal-Bench 4.0 46.5, Toolathlon-Verified 75.0, MCP-Atlas 84.6, BrowseComp 87.4, HLE (no tools) 46.4, AA-LCR v1.1 79.0. Intermediate checkpoint | [API](https://huggingface.co/api/models/baidu/ERNIE-5.5-Flash-Preview?blobs=true), [card](https://huggingface.co/baidu/ERNIE-5.5-Flash-Preview), [quant search](https://huggingface.co/api/models?search=ERNIE-5.5-Flash&sort=downloads&direction=-1&limit=40) |
| **MiMo-V2.6-Flash** (RL / MOPD; 309B / 15B; 1M context; text/image/video/audio) | 177.74 GB (FP8 config); NVIDIA NVFP4 is 185.96 GB | `ggml-org/MiMo-V2.6-Flash-MOPD-GGUF` Q2_K_S **105.86 GB** (Q2_K 126.37 GB is too tight); mmproj 4.31 GB | MIT. AA v5 ≈ 27. Toolathlon-Verified 73.6, Terminal Bench 2.1 87.6 | [GGUF API](https://huggingface.co/api/models/ggml-org/MiMo-V2.6-Flash-MOPD-GGUF?blobs=true), [API](https://huggingface.co/api/models/XiaomiMiMo/MiMo-V2.6-Flash-RL?blobs=true), [card](https://huggingface.co/XiaomiMiMo/MiMo-V2.6-Flash-RL) |
| **Command A+ 05-2026** (218B / 25B; vision; 48 languages) | W4A4 131.89 GB (Cohere's recommended quant); FP8 225.01 GB | `bartowski/command-a-plus-05-2026-GGUF`: IQ3_M **106.74 GB**, Q3_K_M 102.58 GB, IQ3_XXS 93.93 GB, Q2_K 80.06 GB | Apache-2.0. AA v5 ≈ 6 | [GGUF API](https://huggingface.co/api/models/bartowski/command-a-plus-05-2026-GGUF?blobs=true), [W4A4 API](https://huggingface.co/api/models/CohereLabs/command-a-plus-05-2026-w4a4?blobs=true), [card](https://huggingface.co/CohereLabs/command-a-plus-05-2026-bf16) |
| **Step-3.7-Flash** (198B / about 11B; vision) | NVFP4 129.24 GB; FP8 212.52 GB | Official `stepfun-ai/Step-3.7-Flash-GGUF` Q3_K_M **93.80 GB** or IQ4_XS 104.99 GB; `unsloth/...` UD-Q3_K_XL 89.39 GB or UD-IQ4_XS 95.34 GB (mmproj 3.97 GB official) | Apache-2.0. Lab: leads ClawEval-1.1 at 67.1 | [official GGUF API](https://huggingface.co/api/models/stepfun-ai/Step-3.7-Flash-GGUF?blobs=true), [unsloth API](https://huggingface.co/api/models/unsloth/Step-3.7-Flash-GGUF?blobs=true), [card](https://huggingface.co/stepfun-ai/Step-3.7-Flash) |
| **Mistral Medium 3.5** (128B dense) | FP8 133.61 GB | NVIDIA NVFP4 **95.23 GB**; unsloth Q6_K 102.58 GB or Q4_K_M 74.90 GB | Modified MIT ($20M monthly revenue cap). AA v5 ≈ 5 | See Q2 table B |
| **Ling-3.2-air** | FP8 129.28 GB | bartowski Q5_K_M **97.62 GB** / Q4_K_M 80.39 GB | MIT. AA v5 ≈ 23 | See Q2 table B |
| **Qwen3.5-122B-A10B** | FP8 127.16 GB | Official GPTQ-Int4 **78.86 GB**; NVIDIA NVFP4 83.5 GB; unsloth Q6_K 101.01 GB | Apache-2.0 | See Q2 table A |
| **Nemotron 3 Super** | FP8 128.35 GB | Official NVFP4 **80.32 GB** | NVIDIA Nemotron Open Model License | See Q2 table A |
| **Nemotron 3.5 Bolt 74B** | BF16 149.77 GB (no FP8 repo exists) | unsloth Q8_0 **78.85 GB** | OpenMDW-1.1 | See Q2 table B |
| **Llama 3.3 70B / Hermes 4 70B / Apertus v1.5 70B** | BF16 141.11 / 141.11 / 144.6 GB | FP8 72.67 / 72.67 / 76.16 GB | Llama 3.3 / Llama 3 / Apache-2.0 | See Q2 |
| **Inkling-Small** (Thinking Machines, 266B, Jul 2026) | NVFP4 170.73 GB; BF16 531.91 GB | No GGUF measured | Apache-2.0 | [API](https://huggingface.co/api/models/thinkingmachines/Inkling-Small-NVFP4?blobs=true) |
| **MiniMax-M2.5-REAP-139B-A10B** (Cerebras expert-pruned) | FP8 140.43 GB | mradermacher/m51Lab GGUF variants exist; Strix Halo reports for M2.1-REAP-139B Q4_K_M (78.40 GiB) show 29.3 tok/s | Inherits MiniMax licence | [API](https://huggingface.co/api/models/cerebras/MiniMax-M2.5-REAP-139B-A10B?blobs=true), [visorcraft](https://github.com/visorcraft/strix-halo-llm-perf) |
| **K-EXAONE-236B-A23B** (LG) | FP8 245.12 GB; official GGUF IQ4_XS 127.70 GB (too tight) / Q4_K_M 143.47 GB | No smaller official quant | K-EXAONE licence | [GGUF API](https://huggingface.co/api/models/LGAI-EXAONE/K-EXAONE-236B-A23B-GGUF?blobs=true) |

**Larger models (above 170 GB official) and whether any quant fits:**
- **Qwen3.8-Flash-Next** (FP8 185.52 GB): fits at UD-Q3_K_XL 89.46 GB or UD-IQ4_XS 93.67 GB. See Q2 and Q3.
- **MiniMax-M2.7** (FP8 230.13 GB, 228.7B, 204,800 context, **non-commercial licence**):
  - `unsloth/MiniMax-M2.7-GGUF` UD-Q3_K_XL **101.94 GB** or UD-IQ4_XS 108.41 GB; UD-Q4_K_XL is 140.78 GB.
  - `nvidia/MiniMax-M2.7-NVFP4` is 139.89 GB, too big ([GGUF API](https://huggingface.co/api/models/unsloth/MiniMax-M2.7-GGUF?blobs=true), [NVFP4 API](https://huggingface.co/api/models/nvidia/MiniMax-M2.7-NVFP4?blobs=true)).
  - The architecturally identical M2.5 at Q3_K_M (101.76 GiB) runs at about 32.8 tok/s on Strix Halo ([visorcraft](https://github.com/visorcraft/strix-halo-llm-perf)).
- **GLM-5.3-Flash** (320B / 18B; FP8 328.34 GB; MIT; natively multimodal; AA v5 ≈ 28):
  - Only 2-bit quants fit: `unsloth/GLM-5.3-Flash-GGUF` UD-Q2_K_XL **108.72 GB**, UD-IQ2_XXS 101.84 GB, UD-IQ1_M 97.58 GB.
  - UD-IQ3_XXS is 120.37 GB (tight); NVIDIA's NVFP4 is 204.44 GB ([GGUF API](https://huggingface.co/api/models/unsloth/GLM-5.3-Flash-GGUF?blobs=true), [NVFP4 API](https://huggingface.co/api/models/nvidia/GLM-5.3-Flash-NVFP4?blobs=true)).
- **Tencent Hy4 flash** (220B / 12B incl. 64B Engram; FP8 287.07 GB):
  - The smallest build from Tencent's AngelSlim quant org is IQ2_AH at **135.0 GB**; Q3_K is 155.37 GB and SPARK-4Bit 162.51 GB. **None fit** ([AngelSlim GGUF API](https://huggingface.co/api/models/AngelSlim/Hy4-flash-GGUF?blobs=true), [FP8 API](https://huggingface.co/api/models/tencent/Hy4-flash-FP8?blobs=true)).
- **MiniMax-M3.1-Flash** (about 428B / 23B; MXFP8 252.42 GB):
  - The smallest unsloth quant, UD-IQ1_M, is **129.36 GB** and does not fit.
  - Only an expert-pruned community build fits: `Harsha001/MiniMax-M3.1-Flash-StrixHalo-GGUF` (UD-IQ1_M-E93) at 96.63 GB ([unsloth API](https://huggingface.co/api/models/unsloth/MiniMax-M3.1-Flash-GGUF?blobs=true), [StrixHalo API](https://huggingface.co/api/models/Harsha001/MiniMax-M3.1-Flash-StrixHalo-GGUF?blobs=true)).
- **DeepSeek-V4.1-Flash-1017** (552B / 16B per AA; FP8 510.3 GB): the smallest unsloth quant, UD-IQ1_S, is **232.98 GB**. Out of reach ([GGUF API](https://huggingface.co/api/models/unsloth/DeepSeek-V4.1-Flash-1017-GGUF?blobs=true), [AA](https://artificialanalysis.ai/models/open-source)).
- **Other large models that are out of reach:**
  - Solar-Open2-250B: BF16 500.58 GB, no measured quant ([API](https://huggingface.co/api/models/upstage/Solar-Open2-250B?blobs=true)).
  - Step-5-Preview: NVFP4 394.17 GB ([API](https://huggingface.co/api/models/stepfun-ai/Step-5-Preview-NVFP4?blobs=true)).
  - Motif-3: NVFP4 186.89 GB ([API](https://huggingface.co/api/models/Motif-Technologies/Motif-3-NVFP4?blobs=true)).
  - Muse-Spark-Open-816B-A42B ([HF listing](https://huggingface.co/api/models?author=meta-models&sort=createdAt&direction=-1&limit=30)).

### Inferences
- **On a 128 GB unified-memory box, plan for about 95-105 GB of weights at most.** The field report shows a 111 GB file crashing at 32K context while a 93 GB file ran at 131K. So the "fits" quants worth recommending are DeepSeek-V4-Flash-0731 UD-IQ3_XXS (104 GB) or UD-Q2_K_XL (97 GB), Step-3.7-Flash Q3_K_M (94 GB), MiMo-V2.6-Flash Q2_K_S (106 GB, tight), Command A+ Q3_K_M or IQ3_XXS (94-103 GB), and Mistral Medium 3.5 NVFP4 (95 GB). A 128 GB multi-GPU box with no OS reservation has a little more slack. This rule is extrapolated from one report.
- **Embedding tables may be pageable from SSD.** Qwen3.8-Flash-Next's 51B n-gram table and Hy4 flash's 64B Engram parameters are lookup tables, and the Strix Halo report shows the n-gram table paging from SSD via mmap. Hy4 flash might therefore become usable at about 135 GB on disk with part of it mmap-paged. This is untested speculation.
- **2-bit quants of 300B+ MoEs** (GLM-5.3-Flash UD-IQ2_XXS, MiMo Q2_K_S) carry real quality risk. No quality data for them was found, so they should be labelled experimental rather than recommended.

### Gaps
- ERNIE-5.5-Flash-Preview has no reputable quant under 128 GB yet; it was released one day before this research.
- No accuracy-retention data was found for the 2-3-bit GGUFs of DeepSeek-V4-Flash, MiMo-V2.6-Flash, GLM-5.3-Flash or Step-3.7-Flash.
- Inkling-Small and K-EXAONE-236B lack measured community quants under 110 GB.
- I did not verify DeepSeek-V4-Flash's exact total/active parameters from a primary source; only the measured repo sizes are certain.
