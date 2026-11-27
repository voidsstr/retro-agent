# Small/fast open-weight generalists and fully-open / permissively licensed LLMs (state as of 2026-11-24)

Scope: (A) small, fast text generalists of about 1-40 GB at native precision, and (B) fully-open models (weights + data + code) and Apache-2.0/MIT/OpenMDW models up to about 110 GB. Coding-only and vision-first models are excluded. Several current generalists (Qwen3.5+, Gemma 4, Inkling 2, Muse Glimmer, Apertus v1.5, Nemotron 3.5 Bolt) take images as input, but they are general chat/reasoning models, not vision-first models, so they are included. Models on the coordinator's exclusion list are not covered.

**Measurement method.** All sizes were measured on 2026-11-24 from `https://huggingface.co/api/models/<repo>?blobs=true` by summing `siblings[].size` over the files a user would fetch. That means the root-level `*.safetensors` shards plus config, tokenizer and chat-template files. Excluded: gpt-oss's `original/` and `metal/` folders, Mistral's duplicate `consolidated.safetensors`, GGUF/ONNX and images. GB means 10^9 bytes. Context lengths come from each repo's `config.json` (`max_position_embeddings`) unless a model card is cited. "Created" is the repo's `createdAt` from the API, which can be earlier than the public release date (repos are often created private).

## Q1. Which small/fast and fully-open models are current and best as of Nov 2026, and which are superseded?

### Takeaway
Best small/fast picks as of late November 2026, as dense/MoE pairs:
- **Qwen3.8-27B** (Aug 2026, Apache-2.0)
- **Thinking Machines Inkling 2 Mini** (Nov 2026, 26B-A3.4B, Apache-2.0, about 20 GB native NVFP4/MXFP8)
- **Nanbeige4.3-33B** (Oct 2026)
- **Qwen3.6-35B-A3B** (Apr 2026)
- **Meta Muse Glimmer-30B** (Aug 2026, Apache-2.0)
- **Gemma 4**, now under Apache-2.0 (Mar-May 2026)

For on-device use: **MiniCPM5-2B**, **LFM3-1.7B** and the **Qwen3.5 0.8B-9B** line.

Fully open: **IFM K2 Horizon** (Sept 2026; LLM360's successor org) is the strongest fully-open small family. **Olmo 3.1** (Dec 2025) and **Apertus v1.5** (public Oct 2026) remain the other fully-open references, but they trail badly on capability.

Superseded: Qwen3 (2504/2507), Qwen3.5-27B/35B-A3B, Qwen3.6-27B, Gemma 3, Granite 4.0/4.1, Nemotron Nano v2, Nemotron 3 Nano 30B-A3B (by 3.5 Lightning, by inference), LLM360 K2-Think/K2-V2 (by K2 Horizon), Apertus 2509 and Magistral Small. The Phi line has had no new text model since mid-2025.

### Cited Findings

**Current leaders (Artificial Analysis size-class pages, read 2026-11-24)**
- **AA's own ranking, small class (4-40B).** AA now ranks size classes by a "Progress Index v1". Scores: Nanbeige4.3-33B 312, Inkling 2 Mini 306, Qwen3.8 27B (Xhigh) 303, K2 Horizon 32B 285, K2 Horizon MoVA 36B A4B 281, Muse Glimmer (High) 267, K2 Horizon 7B 263, Qwen3.6 35B A3B (Reasoning) 261, G9v3-39A5B 257. Speeds shown: Inkling 2 Mini 187 tok/s, Muse Glimmer 103 tok/s, Qwen3.6 35B A3B 101 tok/s, Qwen3.8 27B 66 tok/s — [AA small open-source models](https://artificialanalysis.ai/models/open-source/small)
- **AA's own ranking, tiny class (≤4B).** K2 Horizon 3.7B 242, MiniCPM5-2B 221, LFM3-1.7B 216 (337 tok/s), Granite 4.2 3B 191, G9v3-3B 189, Nanbeige4.1-3B 180, Qwen3.5 2B (Reasoning) 177, NVIDIA Nemotron 3 Nano 4B 176, LFM2.5-2.6B 171 — [AA tiny open-source models](https://artificialanalysis.ai/models/open-source/tiny)
- **Qwen3.8 27B on the Intelligence Index.** It scores **20** on Intelligence Index v5.0.2 (Xhigh), ranked "#3 / 14". Other settings: Medium 18, Low 16, Non-reasoning 10. AA dates its release to August 14, 2026 — [AA Qwen3.8 27B release page](https://artificialanalysis.ai/models/releases/qwen3-8-27b)
- **Inkling 2 Mini vs peers, AA Index v5.0 (vendor-reproduced table).** Inkling 2 Mini 20.1, Qwen3.8-27B 20.1, Nemotron 3.5 Super VL 13.6, Muse Glimmer 6.3, Gemma 4 31B 4.4 — [thinkingmachines/Inkling-2-Mini card](https://huggingface.co/thinkingmachines/Inkling-2-Mini)

**Release timeline by lab (from the HF API listings, sorted by `createdAt`)**
- **Qwen.** Qwen3.5 (Feb 2026: 0.8B/2B/4B/9B/27B/35B-A3B/122B-A10B/397B-A17B) → Qwen3.6 (Apr 2026: 27B, 35B-A3B) → Qwen3.8-27B (created 2026-08-05; FP8 2026-08-13). No Qwen3.8 small (<27B) or 35B-A3B repo exists on HF as of 2026-11-24. Qwen3.8-Flash-Next (180B) and Qwen3.8-2.4T-A95B carry licence "other" — [HF API: author=Qwen](https://huggingface.co/api/models?author=Qwen&sort=createdAt&direction=-1)
- **Google.** Gemma 4 E2B/E4B (2026-03-02), 26B-A4B and 31B (2026-03-11), 12B (2026-05-23), all Apache-2.0, plus "-assistant" MTP drafters (Apr-May 2026). Gemma-3-based models such as TranslateGemma (Jan 2026) still carry the Gemma licence — [HF API: author=google](https://huggingface.co/api/models?author=google&sort=createdAt&direction=-1)
- **Microsoft.**
  - The newest Phi text models are Phi-4-mini-flash-reasoning (2025-06-19) and Phi-tiny/mini-MoE (2025-06-23). Phi-4-reasoning-vision-15B (Jan 2026) is vision. No Phi-5 repo exists — [HF API search: microsoft Phi](https://huggingface.co/api/models?author=microsoft&search=Phi&sort=createdAt&direction=-1)
  - Microsoft's newest small LM is **Aion-1.0-Instruct-2.3B** (created 2026-11-18, MIT), billed as "the foundational Small Language Model (SLM) in the Windows ecosystem", with GA planned "by end of Jan 2027". It is weak on knowledge (MMLU-Pro 33.35) — [microsoft/Aion-1.0-Instruct-2.3B](https://huggingface.co/microsoft/Aion-1.0-Instruct-2.3B)
- **IBM.** Granite 4.1 (2026-04-06) → Granite 4.2 3B/8B/30B (created 2026-08-07, released Aug 25, 2026). 4.2 is dense and adds thinking modes, post-trained from Granite-4.1-30B-Base — [ibm-granite/granite-4.2-30b](https://huggingface.co/ibm-granite/granite-4.2-30b)
- **Mistral.** Ministral 3 3B/8B/14B (Base/Instruct/Reasoning, suffix 2512) remain the newest small models. Mistral Small 4 (119B, 6.5B active) "unifies the Instruct, Reasoning (formerly Magistral), and Devstral families" — [mistralai/Mistral-Small-4-119B-2603](https://huggingface.co/mistralai/Mistral-Small-4-119B-2603)
- **NVIDIA.** Nemotron Nano 9B/12B v2 (Aug 2025) → Nemotron 3 Nano 30B-A3B (Dec 2025) and Nemotron 3 Nano 4B (Mar 2026) → Nemotron 3.5 Lightning 30B-A3B (released Aug 11, 2026). The Lightning card lists Nemotron 3 Nano only as a baseline and does not say "supersedes" — [Nemotron-3.5-Lightning card](https://huggingface.co/nvidia/NVIDIA-Nemotron-3.5-Lightning-30B-A3B-BF16)
- **NVIDIA Bolt.** Nemotron 3.5 Bolt 74B-A9B (released 10/15/2026) is "a compressed version of the Nemotron 3.5 Super VL 120B-A12B parent". It comes with a 2-bit "-24GB" build — [Bolt card](https://huggingface.co/nvidia/NVIDIA-Nemotron-3.5-Bolt-74B-A9B-BF16)
- **Hugging Face.** SmolLM3-3B (2025-07-08) is still the newest SmolLM. The org's 2026 repos are small experiments (nanowhale-100m, GSM8K SFTs) — [HF API: author=HuggingFaceTB](https://huggingface.co/api/models?author=HuggingFaceTB&sort=createdAt&direction=-1)
- **Liquid AI.** LFM2.5-1.2B (2026-01-06), LFM2.5-8B-A1B (2026-05-28), LFM2.5-24B-A2B (2026-10-12), LFM3-1.7B (2026-10-23) — [HF API: author=LiquidAI](https://huggingface.co/api/models?author=LiquidAI&sort=createdAt&direction=-1)
- **Moonshot.** Kimi-Linear-48B-A3B (2025-10-30) is still Moonshot's only small model. Everything newer is trillion-scale (K2.5/K2.6/K3/K3.1-Pro) — [HF API: author=moonshotai](https://huggingface.co/api/models?author=moonshotai&sort=createdAt&direction=-1)
- **OpenAI.** gpt-oss-20b (2025-08-04) has no successor. The only later small repos are gpt-oss-safeguard-20b (a safety classifier) and circuit-sparsity — [HF API: author=openai](https://huggingface.co/api/models?author=openai&sort=createdAt&direction=-1)

**New 2026 entrants not in the original brief**
- **Thinking Machines Inkling 2 Mini.** Created 2026-11-17, card release date Nov 20, 2026. 26.4B total, 3.4B active, about 5.4B effective with three loop passes. Looped sparse MoE with 128 experts (top-8), 1M context, Apache-2.0 — [Inkling-2-Mini](https://huggingface.co/thinkingmachines/Inkling-2-Mini)
- **Meta Superintelligence Lab Muse Glimmer-30B.** Created 2026-08-09. Dense, about 29.6B including a 1.8B perception encoder. Apache-2.0 under the `meta-models` org — [meta-models/Muse-Glimmer-30B](https://huggingface.co/meta-models/Muse-Glimmer-30B). The legacy `meta-llama` org has published no repos since Sept 2025 — [HF API: author=meta-llama](https://huggingface.co/api/models?author=meta-llama&sort=createdAt&direction=-1)
- **Nanbeige4.3-33B** (Kanzhun's Nanbeige LLM Lab). Created 2026-10-24, Apache-2.0, 512K context — [Nanbeige/Nanbeige4.3-33B](https://huggingface.co/Nanbeige/Nanbeige4.3-33B)
- **IFM K2 Horizon** (Institute of Foundation Models). 0.9B, 3.7B, 7B, 32B dense, MoVA-36B-A4B and 375B-A23B, all created 2026-09-01 under Apache-2.0. Described as "Fully open. Training data/recipe and the training code are public." Training logs are under the `llm360` W&B namespace — [IFM/K2-Horizon-32B](https://huggingface.co/IFM/K2-Horizon-32B)
- **LLM360 → IFM rename.** The API returns the same repo, with the same `createdAt` of 2025-12-01, for `LLM360/K2-V2` and `IFM/K2-V2`, so the LLM360 org now resolves to IFM — [HF API LLM360/K2-V2](https://huggingface.co/api/models/LLM360/K2-V2); [HF API IFM/K2-V2](https://huggingface.co/api/models/IFM/K2-V2)
- **Other new entrants:**
  - MiniCPM5-1B (2026-05-21) and MiniCPM5-2B (2026-09-06), Apache-2.0 — [HF API: author=openbmb](https://huggingface.co/api/models?author=openbmb&sort=createdAt&direction=-1)
  - Zyphra ZAYA1-8B (2026-05-04, 8.4B total / 760M active, Apache-2.0) — [Zyphra/ZAYA1-8B](https://huggingface.co/Zyphra/ZAYA1-8B)
  - GLM-4.7-Flash (2026-01-19, 30B-A3B, MIT) — [zai-org/GLM-4.7-Flash](https://huggingface.co/zai-org/GLM-4.7-Flash)
  - AI21 Jamba2-3B and Jamba2-Mini (2026-01-06, Apache-2.0), and ai9stars G9v3-3B / G9v3-39A5B (2026-07-21, Apache-2.0) — [HF API search G9v3](https://huggingface.co/api/models?search=G9v3)

**Fully-open lineages**
- **Ai2.** Olmo 3 (Nov 2025) → Olmo 3.1 32B Think/Instruct (2025-12-10; "another 3 weeks of RL for 32B Think") → Olmo-Hybrid-7B (2026-01-28) and its Instruct/Think variants (Feb 2026). No Olmo 4 / Olmo 3.5 / 32B hybrid exists on HF — [Olmo-3.1-32B-Think](https://huggingface.co/allenai/Olmo-3.1-32B-Think); [Olmo-Hybrid-Instruct-DPO-7B](https://huggingface.co/allenai/Olmo-Hybrid-Instruct-DPO-7B)
- **Swiss AI.** Apertus 2509 (8B/70B, Sept 2025) → Apertus v1.1 0.5B/1.5B/4B (Feb-May 2026) → Apertus v1.5 8B/70B. Repos were created 2026-07-24; the paper was published Oct 11 and the release-notes blog Oct 15, 2026. v1.5 adds images/audio input, a thinking mode and 262,144 context ("four-fold increase") — [swiss-ai/Apertus-v1.5-70B](https://huggingface.co/swiss-ai/Apertus-v1.5-70B)
- **Marin.** Marin 8B Instruct (2025-05-14) and Marin 32B Base (2025-10-25). No marin-32b-instruct exists (the API returns an auth error, i.e. not public). 2026 Marin output is scaling-law "delphi" checkpoints and data tools — [HF API search marin-community](https://huggingface.co/api/models?author=marin-community&search=marin&sort=createdAt&direction=-1)
- **AA Openness Index.** Top score 89 is shared by Olmo 3 7B Think, Olmo 3.1 32B Think, Apertus 70B Instruct, K2-V2 (Low/Medium/High) and K2 Think V2. NVIDIA Nemotron 3.5 Super VL scores 83. The index launched Dec 1, 2025 — [AA Openness Index](https://artificialanalysis.ai/ja/evaluations/artificial-analysis-openness-index); [AA announcement](https://artificialanalysis.ai/articles/announcing-artificial-analysis-openness-index)

### Inferences
- **Best dense ~27-33B generalists for a 128 GB box:** Qwen3.8-27B and Nanbeige4.3-33B. Both lead AA's small class and post GPQA ≈89. Muse Glimmer-30B and Gemma 4 31B are a tier lower on agentic/AA scores, but they are Apache-2.0 and well tooled.
- **Best small MoE (A3-A5B) speed picks:** Inkling 2 Mini (fastest measured at 187 tok/s on AA, and only about 20 GB native), Qwen3.6-35B-A3B, Gemma 4 26B-A4B, Nemotron 3.5 Lightning 30B-A3B, GLM-4.7-Flash and LFM2.5-24B-A2B (2.45B active).
- **gpt-oss-20b and Kimi-Linear-48B-A3B are now "legacy but unique".** gpt-oss-20b is the smallest strong 4-bit-native model (13.8 GB). Kimi-Linear stands out for 1M-context linear attention. Neither has a successor, so both are archive-worthy for reference rather than as top performers.
- **Phi is effectively dormant for text.** Archive phi-4 / Phi-4-reasoning-plus only for completeness; Aion-1.0 is a Windows on-device SLM, not a competitive generalist.
- **Fully-open capability gap.** K2 Horizon 32B (GPQA 82.8 on AA figures) is far ahead of Olmo 3.1 32B Think (GPQA 57.5) and Apertus v1.5 70B (GPQA 35.94, MMLU-Pro 56.38), so K2 Horizon is now the default fully-open pick.

### Gaps
- No LMArena standings could be retrieved for any Nov-2026 small model. The cached LMArena pages found were from Dec 2025 and did not list Qwen3.8, Gemma 4, Muse Glimmer or Inkling — [LMArena text](https://lmarena.ai/leaderboard/text)
- AA's size-class pages show only the top 12 entries. Gemma 4, gpt-oss-20b, Nemotron 3.5 Lightning, Granite 4.2 30B and LFM2.5-24B-A2B did not appear, so their Progress Index values are unknown.
- Whether Nemotron 3.5 Lightning officially replaces Nemotron 3 Nano 30B-A3B is not stated by NVIDIA.
- Purpose and quality of several repos were not verified: Qwen-AgentWorld-35B-A3B, tencent/Hy-Memory-30B-A3B, NousResearch/Arete-27B (config `qwen3_5`, so likely a Qwen 27B fine-tune), ai9stars G9v3 and Microsoft Aion's planned Windows variant.

## Q2. Repo ids, parameters, recommended precision and API-measured size, context, licence, openness, release date

### Takeaway
Most current small generalists ship BF16 as the reference format, with official FP8/NVFP4 siblings. Exceptions:
- **Native low precision:** gpt-oss-20b (MXFP4, 13.79 GB) and Inkling 2 Mini (mixed NVFP4/MXFP8, 20.19 GB).
- **FP8 by default:** Ministral 3 Instruct repos.
- **FP32 storage, double the BF16 size:** several fully-open or research repos (K2-V2, K2-Think-V2, Marin, sarvam-30b, rnj-1.5).

Licences: Apache-2.0 and MIT dominate. Exceptions to flag:
- LiquidAI's LFM Open License v1.0, which does not license commercial use by companies with ≥$10M revenue.
- NVIDIA's custom Nemotron licences on the older Nemotron 3 models (3.5 moved to OpenMDW-1.1).
- Falcon's custom licence.
- Cohere tiny-aya, CC-BY-NC (non-commercial).
- Qwen3.8-Flash-Next's qwen-community-1.0.

### Cited Findings

#### A. Small/fast generalists: current picks (all sizes API-measured)

| Repo id (HF) | Total / active params | Weights as shipped | Measured fetch GB | Context | Licence | Open | Created (release) |
|---|---|---|---|---|---|---|---|
| [Qwen/Qwen3.8-27B](https://huggingface.co/Qwen/Qwen3.8-27B) | 27.78B dense (incl. vision enc.) | BF16 | **55.59** | 262,144 (1M via YaRN) | Apache-2.0 | weights | 2026-08-05 (AA: Aug 14, 2026) |
| [Qwen/Qwen3.8-27B-FP8](https://huggingface.co/Qwen/Qwen3.8-27B-FP8) | same | official FP8 | **30.89** | 262,144 | Apache-2.0 | weights | 2026-08-13 |
| [Qwen/Qwen3.6-35B-A3B](https://huggingface.co/Qwen/Qwen3.6-35B-A3B) | 35.95B / 3B (256 experts, 8+1 active) | BF16 | **71.93** | 262,144 (1,010,000 YaRN) | Apache-2.0 | weights | 2026-04-15 |
| [Qwen/Qwen3.6-35B-A3B-FP8](https://huggingface.co/Qwen/Qwen3.6-35B-A3B-FP8) | same | official FP8 | **37.49** | 262,144 | Apache-2.0 | weights | 2026-04-15 |
| [Qwen/Qwen3.5-9B](https://huggingface.co/Qwen/Qwen3.5-9B) | 9.65B dense | BF16 | **19.33** | 262,144 | Apache-2.0 | weights | 2026-02-27 |
| [Qwen/Qwen3.5-4B](https://huggingface.co/Qwen/Qwen3.5-4B) | 4.66B | BF16 | **9.34** | 262,144 | Apache-2.0 | weights | 2026-02-27 |
| [Qwen/Qwen3.5-2B](https://huggingface.co/Qwen/Qwen3.5-2B) | 2.27B | BF16 | **4.57** | 262,144 | Apache-2.0 | weights | 2026-02-28 |
| [Qwen/Qwen3.5-0.8B](https://huggingface.co/Qwen/Qwen3.5-0.8B) | 0.87B | BF16 | **1.77** | 262,144 | Apache-2.0 | weights | 2026-02-28 |
| [thinkingmachines/Inkling-2-Mini](https://huggingface.co/thinkingmachines/Inkling-2-Mini) | 26.39B / 3.4B (5.4B effective, 3 loops; 128 experts top-8) | **native mixed NVFP4+MXFP8** | **20.19** | 1M (card) | Apache-2.0 | weights | 2026-11-17 (card: Nov 20, 2026) |
| [Nanbeige/Nanbeige4.3-33B](https://huggingface.co/Nanbeige/Nanbeige4.3-33B) | 33.12B (llama-type; dense/MoE not stated) | BF16 | **66.26** | 524,288 | Apache-2.0 | weights | 2026-10-24 |
| [meta-models/Muse-Glimmer-30B](https://huggingface.co/meta-models/Muse-Glimmer-30B) | 29.78B dense (incl. 1.8B perception enc.) | BF16 | **59.58** | 131,072 | Apache-2.0 (+usage policy, 18+) | weights | 2026-08-09 |
| [google/gemma-4-31B-it](https://huggingface.co/google/gemma-4-31B-it) | 31.27B dense | BF16 | **62.58** | 262,144 | Apache-2.0 | weights | 2026-03-11 |
| [google/gemma-4-26B-A4B-it](https://huggingface.co/google/gemma-4-26B-A4B-it) | 25.81B / 3.8B (128 experts, 8+1 shared) | BF16 | **51.64** | 262,144 | Apache-2.0 | weights | 2026-03-11 |
| [google/gemma-4-12B-it](https://huggingface.co/google/gemma-4-12B-it) | 11.96B dense | BF16 | **23.95** | 262,144 | Apache-2.0 | weights | 2026-05-23 |
| [google/gemma-4-E4B-it](https://huggingface.co/google/gemma-4-E4B-it) | 8.00B (4.5B effective) | BF16 | **16.02** | 131,072 | Apache-2.0 | weights | 2026-03-02 |
| [google/gemma-4-E2B-it](https://huggingface.co/google/gemma-4-E2B-it) | 5.12B (2.3B effective) | BF16 | **10.28** | 131,072 | Apache-2.0 | weights | 2026-03-02 |
| [openai/gpt-oss-20b](https://huggingface.co/openai/gpt-oss-20b) | 20.91B / 3.6B (32 experts, top-4) | **native MXFP4** (MoE weights) | **13.79** (whole repo incl. original/+metal/ = 41.3) | 131,072 | Apache-2.0 | weights | 2025-08-04 |
| [nvidia/NVIDIA-Nemotron-3.5-Lightning-30B-A3B-BF16](https://huggingface.co/nvidia/NVIDIA-Nemotron-3.5-Lightning-30B-A3B-BF16) | 31.58B / 3B (Mamba-2+MoE+attn, MTP layers; 128 experts/6) | BF16 reference | **65.85** | 262,144 in config (card: up to 1M) | **OpenMDW-1.1** | weights + partial data | 2026-08-01 (card: Aug 11, 2026) |
| [nvidia/NVIDIA-Nemotron-3.5-Lightning-30B-A3B-NVFP4](https://huggingface.co/nvidia/NVIDIA-Nemotron-3.5-Lightning-30B-A3B-NVFP4) | same | NVFP4 (card's production pick) | **21.58** | 1,048,576 in config | OpenMDW-1.1 | as above | 2026-08-04 |
| [nvidia/NVIDIA-Nemotron-3.5-Bolt-74B-A9B-24GB](https://huggingface.co/nvidia/NVIDIA-Nemotron-3.5-Bolt-74B-A9B-24GB) | 74B / 9B (2-bit ModelOpt) | native 2-bit build | **24.12** | 262,144 | OpenMDW-1.1 | weights + partial data | 2026-10-07 (card: 10/15/2026) |
| [nvidia/NVIDIA-Nemotron-3-Nano-4B-BF16](https://huggingface.co/nvidia/NVIDIA-Nemotron-3-Nano-4B-BF16) | 3.97B | BF16 | **7.96** | 262,144 | NVIDIA Nemotron Open Model License (custom) | weights | 2026-03-07 |
| [ibm-granite/granite-4.2-30b](https://huggingface.co/ibm-granite/granite-4.2-30b) | 29.28B dense (64 layers, GQA) | BF16 | **58.56** | 131,072 (card: 128K native, 512K ext.) | Apache-2.0 | weights + dataset listing + repo | 2026-08-07 (card: Aug 25, 2026) |
| [ibm-granite/granite-4.2-30b-fp8](https://huggingface.co/ibm-granite/granite-4.2-30b-fp8) | same | official FP8 | **30.12** | 131,072 | Apache-2.0 | as above | 2026-08-13 |
| [ibm-granite/granite-4.2-8b](https://huggingface.co/ibm-granite/granite-4.2-8b) | 8.79B | BF16 | **17.59** | 131,072 | Apache-2.0 | as above | 2026-08-07 |
| [ibm-granite/granite-4.2-3b](https://huggingface.co/ibm-granite/granite-4.2-3b) | 3.66B | BF16 | **7.33** | 131,072 | Apache-2.0 | as above | 2026-08-07 |
| [LiquidAI/LFM2.5-24B-A2B](https://huggingface.co/LiquidAI/LFM2.5-24B-A2B) | 23.97B / 2.45B (64 experts, top-4; 30 conv + 10 GQA layers) | BF16 | **47.96** | 131,072 | **LFM Open License v1.0** | weights | 2026-10-12 |
| [LiquidAI/LFM3-1.7B](https://huggingface.co/LiquidAI/LFM3-1.7B) | 1.73B (conv + SWA + global attn) | BF16 | **3.48** | 262,144 | LFM Open License v1.0 | weights | 2026-10-23 |
| [LiquidAI/LFM2.5-8B-A1B](https://huggingface.co/LiquidAI/LFM2.5-8B-A1B) | 8.47B / ~1B (32 experts/4) | BF16 | **16.95** | 128,000 | LFM Open License v1.0 | weights | 2026-05-28 |
| [LiquidAI/LFM2.5-1.2B-Instruct](https://huggingface.co/LiquidAI/LFM2.5-1.2B-Instruct) | 1.17B | BF16 | **2.35** | 128,000 | LFM Open License v1.0 | weights | 2026-01-06 |
| [zai-org/GLM-4.7-Flash](https://huggingface.co/zai-org/GLM-4.7-Flash) | 31.22B / ~3B ("30B-A3B"; 64 experts/4) | BF16 | **62.47** | 202,752 | MIT | weights | 2026-01-19 |
| [mistralai/Ministral-3-14B-Instruct-2512](https://huggingface.co/mistralai/Ministral-3-14B-Instruct-2512) | 13.95B (13.5B LM + 0.4B vision) | **FP8 default** | **15.76** | 262,144 | Apache-2.0 | weights | 2025-10-31 (suffix 2512) |
| [mistralai/Ministral-3-14B-Instruct-2512-BF16](https://huggingface.co/mistralai/Ministral-3-14B-Instruct-2512-BF16) | same | BF16 | **27.92** | 262,144 | Apache-2.0 | weights | 2025-10-31 |
| [mistralai/Ministral-3-14B-Reasoning-2512](https://huggingface.co/mistralai/Ministral-3-14B-Reasoning-2512) | same | BF16 | **27.92** | 262,144 | Apache-2.0 | weights | 2025-10-31 |
| [mistralai/Ministral-3-8B-Instruct-2512](https://huggingface.co/mistralai/Ministral-3-8B-Instruct-2512) | 8.92B | FP8 default | **10.45** | 262,144 | Apache-2.0 | weights | 2025-10-31 |
| [mistralai/Ministral-3-3B-Instruct-2512](https://huggingface.co/mistralai/Ministral-3-3B-Instruct-2512) | 3.85B | FP8 default | **4.71** | 262,144 | Apache-2.0 | weights | 2025-10-31 |
| [openbmb/MiniCPM5-2B](https://huggingface.co/openbmb/MiniCPM5-2B) | 2.52B dense (Llama arch) | BF16 | **5.04** | 131,072 | Apache-2.0 | weights + many training datasets | 2026-09-06 |
| [Zyphra/ZAYA1-8B](https://huggingface.co/Zyphra/ZAYA1-8B) | 8.84B (card: 8.4B) / 760M (16 experts, top-1) | BF16 | **17.72** | 131,072 | Apache-2.0 | weights | 2026-05-04 |
| [HuggingFaceTB/SmolLM3-3B](https://huggingface.co/HuggingFaceTB/SmolLM3-3B) | 3.08B | BF16 | **6.17** | 65,536 (128K YaRN) | Apache-2.0 | **fully open** (data mix, configs, checkpoints) | 2025-07-08 |
| [moonshotai/Kimi-Linear-48B-A3B-Instruct](https://huggingface.co/moonshotai/Kimi-Linear-48B-A3B-Instruct) | 49.12B / 3B (KDA:MLA 3:1; 256 experts/8) | BF16 | **98.25** | 1M (card) | MIT | weights | 2025-10-30 |
| [ai21labs/AI21-Jamba2-3B](https://huggingface.co/ai21labs/AI21-Jamba2-3B) | 3.03B (Mamba hybrid) | BF16 | **6.40** | 262,144 | Apache-2.0 | weights | 2026-01-06 |
| [baidu/ERNIE-4.5-21B-A3B-Thinking](https://huggingface.co/baidu/ERNIE-4.5-21B-A3B-Thinking) | 21.83B / ~3B | BF16 | **43.67** | 131,072 | Apache-2.0 | weights | 2025-09-08 |
| [arcee-ai/Trinity-Mini](https://huggingface.co/arcee-ai/Trinity-Mini) | 26.12B MoE (128 experts/8) | BF16 | **52.27** | 131,072 | **OpenMDW-1.1** | weights | 2025-12-01 |
| [ServiceNow-AI/Apriel-1.6-15b-Thinker](https://huggingface.co/ServiceNow-AI/Apriel-1.6-15b-Thinker) | 14.86B (successor to the excluded 1.5) | BF16 | **29.75** | 262,400 | MIT | weights | 2025-11-28 |
| [XiaomiMiMo/MiMo-V2.6-Distill-Qwen-9B](https://huggingface.co/XiaomiMiMo/MiMo-V2.6-Distill-Qwen-9B) | 9.41B (Qwen3.5-arch distill) | BF16 | **18.85** | 262,144 | MIT | weights | 2026-09-21 |
| [microsoft/Aion-1.0-Instruct-2.3B](https://huggingface.co/microsoft/Aion-1.0-Instruct-2.3B) | 2.29B | BF16 | **4.59** | 32,768 | MIT | weights | 2026-11-18 |
| [tiiuae/Falcon-H1R-7B](https://huggingface.co/tiiuae/Falcon-H1R-7B) | 7.59B (hybrid) | BF16 | **15.18** | 262,144 | **Falcon LLM licence (custom)** | weights | 2025-10-29 |
| [EssentialAI/rnj-1.5-instruct](https://huggingface.co/EssentialAI/rnj-1.5-instruct) | 8.31B | **FP32 stored** | **33.26** | 163,840 | Apache-2.0 | weights | 2026-04-22 |
| [ai9stars/G9v3-39A5B](https://huggingface.co/ai9stars/G9v3-39A5B) | 38.97B / ~5B | BF16 | **77.96** | 131,072 | Apache-2.0 | weights | 2026-07-21 |
| [ai9stars/G9v3-3B](https://huggingface.co/ai9stars/G9v3-3B) | 2.99B | BF16 | **5.99** | 131,072 | Apache-2.0 | weights | 2026-07-21 |

Sources for the active-parameter and architecture columns:
- **Qwen / Gemma / Inkling / Muse:** [Qwen3.6-35B-A3B card](https://huggingface.co/Qwen/Qwen3.6-35B-A3B); [Gemma 4 31B card](https://huggingface.co/google/gemma-4-31B-it); [Inkling-2-Mini card](https://huggingface.co/thinkingmachines/Inkling-2-Mini); [Muse Glimmer card](https://huggingface.co/meta-models/Muse-Glimmer-30B)
- **NVIDIA / IBM / Liquid / Mistral:** [Lightning card](https://huggingface.co/nvidia/NVIDIA-Nemotron-3.5-Lightning-30B-A3B-BF16); [Bolt card](https://huggingface.co/nvidia/NVIDIA-Nemotron-3.5-Bolt-74B-A9B-BF16); [Granite 4.2 card](https://huggingface.co/ibm-granite/granite-4.2-30b); [LFM2.5-24B-A2B card](https://huggingface.co/LiquidAI/LFM2.5-24B-A2B); [LFM3 card](https://huggingface.co/LiquidAI/LFM3-1.7B); [Ministral 3 card](https://huggingface.co/mistralai/Ministral-3-14B-Instruct-2512)
- **Others:** [ZAYA1-8B card](https://huggingface.co/Zyphra/ZAYA1-8B); [Kimi-Linear card](https://huggingface.co/moonshotai/Kimi-Linear-48B-A3B-Instruct); [gpt-oss-20b card](https://huggingface.co/openai/gpt-oss-20b); [GLM-4.7-Flash card](https://huggingface.co/zai-org/GLM-4.7-Flash)
- Expert counts with "/" come from `config.json`.

#### A'. Superseded / reference small models (measured, for completeness)

| Repo id | Params | Shipped | Measured GB | Ctx | Licence | Created | Superseded by |
|---|---|---|---|---|---|---|---|
| [Qwen/Qwen3.6-27B](https://huggingface.co/Qwen/Qwen3.6-27B) | 27.78B | BF16 | 55.59 | 262,144 | Apache-2.0 | 2026-04-21 | Qwen3.8-27B |
| [Qwen/Qwen3.5-35B-A3B](https://huggingface.co/Qwen/Qwen3.5-35B-A3B) | 35.95B/3B | BF16 | 71.93 | 262,144 | Apache-2.0 | 2026-02-24 | Qwen3.6-35B-A3B |
| [Qwen/Qwen3-30B-A3B-Instruct-2507](https://huggingface.co/Qwen/Qwen3-30B-A3B-Instruct-2507) | 30.53B/3B | BF16 | 61.08 | 262,144 | Apache-2.0 | 2025-07-28 | Qwen3.5/3.6 A3B |
| [nvidia/NVIDIA-Nemotron-3-Nano-30B-A3B-BF16](https://huggingface.co/nvidia/NVIDIA-Nemotron-3-Nano-30B-A3B-BF16) | 31.58B/3B | BF16 | 63.17 (FP8 repo 32.70) | 262,144 | NVIDIA Nemotron Open Model License | 2025-12-04 | Nemotron 3.5 Lightning (inferred) |
| [ibm-granite/granite-4.0-h-small](https://huggingface.co/ibm-granite/granite-4.0-h-small) | 32.21B MoE (72 experts/10) | BF16 | 64.42 | 131,072 | Apache-2.0 | 2025-09-16 | Granite 4.1/4.2 30B (dense) |
| [mistralai/Magistral-Small-2509](https://huggingface.co/mistralai/Magistral-Small-2509) | 24.01B | BF16 | 48.04 (repo 96.1 with consolidated dup) | 131,072 | Apache-2.0 | 2025-09-12 | Mistral Small 4 (reasoning mode) |
| [microsoft/phi-4](https://huggingface.co/microsoft/phi-4) | 14.66B | BF16 | 29.33 | 16,384 | MIT | 2024-12-11 | none (line dormant) |
| [microsoft/Phi-4-reasoning-plus](https://huggingface.co/microsoft/Phi-4-reasoning-plus) | 14.66B | BF16 | 29.33 | 32,768 | MIT | 2025-04-17 | none |
| [microsoft/Phi-4-mini-instruct](https://huggingface.co/microsoft/Phi-4-mini-instruct) | 3.84B | BF16 | 7.69 | 131,072 | MIT | 2025-02-19 | none |
| [microsoft/Phi-4-mini-flash-reasoning](https://huggingface.co/microsoft/Phi-4-mini-flash-reasoning) | 3.85B | BF16 | 7.72 | 262,144 (config) | MIT | 2025-06-19 | none |
| [Qwen/Qwen3.8-Flash-Next](https://huggingface.co/Qwen/Qwen3.8-Flash-Next) | 180B MoE (512 experts/10) | BF16 | 360.02 | 262,144 | **qwen-community-1.0 (custom)** | 2026-08-24 | (out of size range; flag licence) |

#### B. Fully-open models (weights + data + code)

| Repo id | Params | Shipped | Measured GB | Ctx | Licence | What is open | Created (release) |
|---|---|---|---|---|---|---|---|
| [IFM/K2-Horizon-32B](https://huggingface.co/IFM/K2-Horizon-32B) | 34.78B (card: 32B dense) | BF16 | **69.59** | 524,288 | Apache-2.0 | weights, TxT360-v2 + pretrain/midtrain data, xllm training code, per-stage intermediate checkpoints, W&B logs | 2026-09-01 |
| [IFM/K2-Horizon-MoVA-36B-A4B](https://huggingface.co/IFM/K2-Horizon-MoVA-36B-A4B) | 37.44B / 4B (MoE + "Mixture-of-Values attention") | BF16 | **74.92** | 524,288 | Apache-2.0 | same + Eval360 eval code | 2026-09-01 |
| [IFM/K2-Horizon-7B](https://huggingface.co/IFM/K2-Horizon-7B) | 9.00B | BF16 | **18.02** | 524,288 | Apache-2.0 | family claim: fully open | 2026-09-01 |
| [IFM/K2-Horizon-3.7B](https://huggingface.co/IFM/K2-Horizon-3.7B) | 5.06B | BF16 | **10.14** | 524,288 | Apache-2.0 | family claim: fully open | 2026-09-01 |
| [IFM/K2-Horizon-0.9B](https://huggingface.co/IFM/K2-Horizon-0.9B) | 1.08B | BF16 | **2.16** | 131,072 | Apache-2.0 | family claim: fully open | 2026-09-01 |
| [allenai/Olmo-3.1-32B-Think](https://huggingface.co/allenai/Olmo-3.1-32B-Think) | 32.23B | BF16 | **64.48** | 65,536 | Apache-2.0 | Dolma 3 + Dolci data, OLMo-Core / Open-Instruct / OLMo-Eval code, checkpoints (logs "coming soon") | 2025-12-10 |
| [allenai/Olmo-3.1-32B-Instruct](https://huggingface.co/allenai/Olmo-3.1-32B-Instruct) | 32.23B | BF16 | **64.48** | 65,536 | Apache-2.0 | same | 2025-12-10 |
| [allenai/Olmo-3-7B-Think](https://huggingface.co/allenai/Olmo-3-7B-Think) | 7.30B | BF16 | **14.61** | 65,536 | Apache-2.0 | same | 2025-11-18 |
| [allenai/Olmo-3-7B-Instruct](https://huggingface.co/allenai/Olmo-3-7B-Instruct) | 7.30B | BF16 | **14.61** | 65,536 | Apache-2.0 | same | 2025-11-19 |
| [allenai/Olmo-Hybrid-Instruct-DPO-7B](https://huggingface.co/allenai/Olmo-Hybrid-Instruct-DPO-7B) | 7.43B (hybrid RNN) | BF16 | **14.87** | 32,768 (config) | Apache-2.0 | Dolma 3 / Dolci, code, `step_XXXX` checkpoints | 2026-02-20 |
| [swiss-ai/Apertus-v1.5-8B](https://huggingface.co/swiss-ai/Apertus-v1.5-8B) | 8.90B | BF16 (+F32 tokenizer parts) | **18.44** | 262,144 (card) | Apache-2.0 + AUP click-through (gated=auto) | "open weights + open data + open values + full training details"; checkpoints/tech report "in the coming weeks" | 2026-07-24 (public ~Oct 15, 2026) |
| [swiss-ai/Apertus-v1.5-70B](https://huggingface.co/swiss-ai/Apertus-v1.5-70B) | 72.01B | BF16 | **144.65** (over the 110 GB budget; no official FP8) | 262,144 | Apache-2.0 + AUP | same | 2026-07-24 (public ~Oct 15, 2026) |
| [HuggingFaceTB/SmolLM3-3B](https://huggingface.co/HuggingFaceTB/SmolLM3-3B) | 3.08B | BF16 | **6.17** | 65,536 | Apache-2.0 | public data mixture + training configs + intermediate checkpoints (mid/post-train data "will be uploaded later") | 2025-07-08 |
| [IFM/K2-V2-Instruct](https://huggingface.co/IFM/K2-V2-Instruct) (= LLM360/K2-V2-Instruct) | 70B dense (72.55B) | **FP32 PyTorch .bin** (6 shards) | **290.22** (over budget) | 524,288 | Apache-2.0 | "360-open": TxT360 data mixtures, k2v2_train code, eval360, intermediate checkpoints | 2025-12-03 |
| [IFM/K2-Think-V2](https://huggingface.co/IFM/K2-Think-V2) | 72.55B | **FP32** safetensors | **290.22** (over budget) | 262,144 | Apache-2.0 | 360-open | 2026-01-08 |
| [marin-community/marin-32b-base](https://huggingface.co/marin-community/marin-32b-base) | 32.52B (base only) | **FP32** | **130.10** | 4,096 (config) | Apache-2.0 | open-lab process (claims not re-verified) | 2025-10-25 |
| [marin-community/marin-8b-instruct](https://huggingface.co/marin-community/marin-8b-instruct) | 8.03B | **FP32** | **32.14** | 4,096 (config) | Apache-2.0 | as above | 2025-05-14 |

Sources for the openness column: [K2-Horizon-32B](https://huggingface.co/IFM/K2-Horizon-32B); [K2-Horizon-MoVA](https://huggingface.co/IFM/K2-Horizon-MoVA-36B-A4B); [K2-V2-Instruct](https://huggingface.co/IFM/K2-V2-Instruct); [Olmo 3.1 32B Think](https://huggingface.co/allenai/Olmo-3.1-32B-Think); [Olmo Hybrid](https://huggingface.co/allenai/Olmo-Hybrid-Instruct-DPO-7B); [Apertus v1.5 70B](https://huggingface.co/swiss-ai/Apertus-v1.5-70B); [SmolLM3-3B](https://huggingface.co/HuggingFaceTB/SmolLM3-3B).

Partially open data:
- **Nemotron 3.5 Lightning.** "we release our final pre-training and post-training data", but code/math/multilingual data needs gating and approval, and some datasets are private — [Lightning card](https://huggingface.co/nvidia/NVIDIA-Nemotron-3.5-Lightning-30B-A3B-BF16)
- **MiniCPM5-2B.** Releases Ultra-FineWeb, UltraX, UltraData-Code/Math, UltraData-SFT-2605, UltraData-SFT-Agent-2609 and UltraData-RL-2609. The training code is not stated — [MiniCPM5-2B card](https://huggingface.co/openbmb/MiniCPM5-2B)

#### B'. Permissively licensed (Apache-2.0 / MIT / OpenMDW) models of any size that fit the ≤110 GB budget (weights-only openness unless noted)

| Repo id | Params | Shipped | Measured GB | Ctx | Licence | Created |
|---|---|---|---|---|---|---|
| [mistralai/Mistral-Small-4-119B-2603-NVFP4](https://huggingface.co/mistralai/Mistral-Small-4-119B-2603-NVFP4) | 119B / 6.5B (128 experts, 4 active) | official NVFP4 | **70.85** | 256k (card) | Apache-2.0 | 2026-03-03 |
| [mistralai/Mistral-Small-4-119B-2603](https://huggingface.co/mistralai/Mistral-Small-4-119B-2603) | same | FP8 (default) | **120.96** (over) | 256k (card; config 1,048,576) | Apache-2.0 | 2026-01-23 |
| [moonshotai/Kimi-Linear-48B-A3B-Instruct](https://huggingface.co/moonshotai/Kimi-Linear-48B-A3B-Instruct) | 49.12B / 3B | BF16 | **98.25** | 1M | MIT | 2025-10-30 |
| [ai21labs/AI21-Jamba2-Mini](https://huggingface.co/ai21labs/AI21-Jamba2-Mini) | 51.57B MoE (16 experts/2) | BF16 | **103.15** | 262,144 | Apache-2.0 | 2026-01-06 |
| [meituan-longcat/LongCat-Flash-Lite-FP8](https://huggingface.co/meituan-longcat/LongCat-Flash-Lite-FP8) | 69.07B MoE | FP8 (BF16 repo = 138.19) | **101.77** | 327,680 | MIT | 2026-02-06 |
| [PrimeIntellect/INTELLECT-3-FP8](https://huggingface.co/PrimeIntellect/INTELLECT-3-FP8) | 106.89B MoE (GLM-4.5-Air base) | FP8 | **108.31** | 131,072 | MIT | 2025-11-26 |
| [PrimeIntellect/INTELLECT-3.1](https://huggingface.co/PrimeIntellect/INTELLECT-3.1) | 106.85B | BF16 only | **213.73** (over) | 131,072 | MIT | 2026-01-20 |
| [ByteDance-Seed/Seed-OSS-36B-Instruct](https://huggingface.co/ByteDance-Seed/Seed-OSS-36B-Instruct) | 36.15B dense | BF16 | **72.31** | 524,288 | Apache-2.0 | 2025-08-20 |
| [NousResearch/Hermes-4.3-36B](https://huggingface.co/NousResearch/Hermes-4.3-36B) | 36.15B (Seed-OSS base) | BF16 | **72.31** | 524,288 | Apache-2.0 | 2025-11-17 |
| [NousResearch/Arete-27B](https://huggingface.co/NousResearch/Arete-27B) | 27.36B (qwen3_5 arch) | BF16 | **54.74** | 262,144 | Apache-2.0 | 2026-11-06 |
| [nvidia/NVIDIA-Nemotron-3.5-Bolt-74B-A9B-BF16](https://huggingface.co/nvidia/NVIDIA-Nemotron-3.5-Bolt-74B-A9B-BF16) | 74.89B / 9B | BF16 | **149.79** (over; use the 24.12 GB 2-bit build) | 262,144 | OpenMDW-1.1 | 2026-10-09 |
| [sarvamai/sarvam-30b-fp8](https://huggingface.co/sarvamai/sarvam-30b-fp8) | 32.15B MoE (128/6) | FP8 (main repo is **FP32, 128.65**) | **38.66** | 131,072 | Apache-2.0 | 2026-03-26 |

Measured but over the ≤110 GB budget at official precisions:

| Repo | Measured GB | Format / licence |
|---|---|---|
| Qwen/Qwen3.5-122B-A10B-FP8 | 127.20 | FP8, Apache |
| zai-org/GLM-4.5-Air-FP8 | 112.59 | FP8, MIT |
| sarvamai/sarvam-105b-fp8 | 112.57 | FP8, Apache |
| stepfun-ai/Step-3.7-Flash-NVFP4 | 129.25 | NVFP4, Apache |
| inclusionAI/Ling-3.2-air | 129.30 | FP8, MIT; created 2026-11-23 |
| Zyphra/ZAYA1-74B-preview | 149.59 | BF16, Apache |

All six are measured via [HF API](https://huggingface.co/api/models/Qwen/Qwen3.5-122B-A10B-FP8?blobs=true) with the same method.

**Licence notes**
- **LFM Open License v1.0.** Commercial use is not licensed for an entity with "annual revenue of 10 million United States dollars ($10,000,000) or more". Derivatives are allowed and there are no field-of-use limits — [LFM licence text](https://huggingface.co/LiquidAI/LFM2.5-24B-A2B/blob/main/LICENSE)
- **OpenMDW-1.1.** Released May 28, 2026 by the Linux Foundation. It is a permissive licence covering "Model Materials" (the model plus associated data, docs and software). NVIDIA adopted it for Nemotron; OSI approval was still under review in Aug 2026 — [LF press release](https://www.linuxfoundation.org/press/linux-foundation-releases-openmdw-1.1-nvidia-adopts-openmdw-for-cosmos-isaac-gr00t-ising-and-nemotron-ai-model-families); [OSI license-review thread](https://lists.opensource.org/pipermail/license-review_lists.opensource.org/2026-August/006125.html)
- **Licences on other excluded or flagged repos.** Cohere tiny-aya 3.35B (Feb-Sep 2026) is **CC-BY-NC-4.0**. LG EXAONE 4.5 33B and Kakao Kanana-2 carry "other" (custom) licences. Apple SimpleSD is apple-amlr (research). All are taken from cardData in the HF API listings — [HF API: author=CohereLabs](https://huggingface.co/api/models?author=CohereLabs&sort=createdAt&direction=-1)
- **Drafts for Qwen3.8.** nvidia/Qwen3.5-*-Qwen3.8-27B-Draft carry "nvidia-open-model-agreement", not Apache — [HF API nvidia/Qwen3.5-4B-Qwen3.8-27B-Draft](https://huggingface.co/api/models/nvidia/Qwen3.5-4B-Qwen3.8-27B-Draft)

### Inferences
- **A suggested "small/fast core" archive** comes to about 791 GB across 26 repos: Qwen3.8-27B, Qwen3.6-35B-A3B, the Qwen3.5 0.8/2/4/9B set (35.0 GB), all five Gemma 4 sizes plus the four measured drafters (167.4 GB), gpt-oss-20b, Inkling-2-Mini, Muse-Glimmer-30B plus drafter, Nanbeige4.3-33B, Nemotron-3.5-Lightning BF16, Granite 4.2 3/8/30B, LFM2.5-24B-A2B, LFM3-1.7B, GLM-4.7-Flash, MiniCPM5-2B and Ministral-3-14B-Reasoning.
- **Group B sets.** The fully-open set (K2 Horizon 0.9/3.7/7/32/MoVA-36B-A4B, Olmo 3.1 32B Think/Instruct, Olmo 3 7B Think/Instruct, Apertus v1.5 8B, SmolLM3) adds about 358 GB. The permissive mid-size set (Kimi-Linear, Jamba2-Mini, LongCat-Flash-Lite-FP8, INTELLECT-3-FP8, Mistral-Small-4 NVFP4) adds about 482 GB. Totals were computed from the measured figures above.
- **FP32-stored repos.** K2-V2, K2-Think-V2, Marin, sarvam-30b and rnj-1.5 can be cast to BF16 locally to halve disk/VRAM. Even in BF16, the 70B fully-open models (Apertus v1.5 70B 144.6 GB; K2-V2 ≈145 GB) exceed 128 GB VRAM once KV cache is counted, so they need FP8 self-quantization (no official FP8 found).
- **Licence trend.** Google (Gemma 4), Meta (Muse Glimmer) and NVIDIA (Nemotron 3.5 via OpenMDW) moved to permissive licences in 2026. Alibaba's larger Qwen3.8 models moved the other way, to a custom community licence, while the 27B stayed Apache-2.0.

### Gaps
- **Marin openness.** The data/code openness claims were not re-verified this session; only the HF API metadata was read.
- **Missing exact figures.** Inkling 2 Mini and Kimi-Linear do not declare context in `config.json` (the card values are used). Nanbeige4.3-33B's dense-vs-MoE status is unconfirmed. Trinity-Mini's, Jamba2-Mini's and LongCat-Flash-Lite's active-parameter counts were not verified.
- **Not measured.** The Gemma 4 E2B "-assistant" drafter (0.08B per the API listing) and Gemma 4 QAT builds.

## Q3. Strengths and benchmark standing (with dates)

### Takeaway
On self-reported cards (Aug-Nov 2026), Qwen3.8-27B and Nanbeige4.3-33B lead the ~30B class (GPQA ≈89, AIME 2026 ≈95, Terminal-Bench 2.1 73-81). Muse Glimmer-30B leads Gemma 4 31B on agentic tasks. Inkling 2 Mini matches Qwen3.8-27B on AA v5.0 (20.1) with about 1/8 of the active compute.

In the A3B class:
- Qwen3.6-35B-A3B is strongest on reasoning/coding.
- LFM2.5-24B-A2B leads instruction-following (IFEval 96.75).
- GLM-4.7-Flash leads agentic tool use (τ²-Bench 79.5).

Under 4B, MiniCPM5-2B and LFM3-1.7B beat Qwen3.5-2B/4B on most reported rows.

Fully-open models trail: K2 Horizon 32B (AA-reported GPQA 82.8) is the only one within range of the open-weights leaders.

AA's Intelligence Index v5.0.2 compresses scores (Qwen3.8-27B = 20). Do not mix it with older-version figures.

### Cited Findings

**Qwen family**
- **Qwen3.8-27B** (card, Aug 2026). GPQA Diamond 89.2, HLE 30.8, LiveCodeBench v6 90.3, IFBench 79.5, SWE-bench Pro 61.7, OSWorld-Verified 84.3. Reasoning effort xhigh (default) / medium / low, and thinking can be toggled. Hybrid Gated DeltaNet + gated attention (3:1), 64 layers, MTP "trained with multiple steps" — [Qwen3.8-27B card](https://huggingface.co/Qwen/Qwen3.8-27B)
- **Qwen3.8 27B on AA.** Output speed is 66 tok/s and it used 128M tokens for the Intelligence Index, vs a median of 89.3M (verbose) — [AA release page](https://artificialanalysis.ai/models/releases/qwen3-8-27b)
- **Conflicting index figures.** Third-party coverage cites "52" for Qwen3.8-27B and "38" for Qwen3.6-27B on an earlier AA index version. That is not comparable with v5.0.2, where Qwen3.8 27B = 20 — [web summary citing AA](https://www.morphllm.com/qwen-3-8); contradicted by [AA v5.0.2 page](https://artificialanalysis.ai/models/releases/qwen3-8-27b)
- **Qwen3.6-35B-A3B** (card, Apr 2026). MMLU-Pro 85.2, GPQA 86.0, HLE 21.4, LiveCodeBench v6 80.4, AIME26 92.7, SWE-bench Verified 73.4, Terminal-Bench 2.0 51.5. That beats Gemma 4 26B-A4B (82.6 / 82.3 / 8.7 / 77.1 / 88.3 / 17.4) on every row shown — [Qwen3.6-35B-A3B card](https://huggingface.co/Qwen/Qwen3.6-35B-A3B)

**Other ~30B dense and looped models**
- **Nanbeige4.3-33B** (card; "highest thinking effort, same harness"). Terminal-Bench 2.1 80.9 vs Qwen3.8-27B 73.0. SWE-bench Pro 60.3 vs 61.7. GPQA 89.5 vs 89.2 (Gemma 4 31B 91.3 in that table). AIME 2026 95.2 vs 94.5. AA-LCR 84.0 vs 82.0. Tau3 48.8 vs 48.5 — [Nanbeige4.3-33B card](https://huggingface.co/Nanbeige/Nanbeige4.3-33B)
- **Muse Glimmer-30B** (card; High reasoning vs Gemma4-31B Thinking and Qwen3.6-27B Thinking). AIME 2026 94.7 / 89.2 / 94.1. GPQA (AA) 83.5 / 85.7 / 84.2. IFBench 77.0 / 76.0 / 70.8. MCP Atlas 75.5 / 54.2 / 62.5. SWE-bench Verified 76.0 / 66.6 / 77.2. AA-LCR 80.0 / 68.3 / 73.3. Reasoning strength low/medium/high/xhigh is set in the system prompt — [Muse Glimmer card](https://huggingface.co/meta-models/Muse-Glimmer-30B)
- **Inkling 2 Mini** (card). AA Index v5.0 20.1 (same as Qwen3.8-27B). SWE-bench Verified 79.2% vs Qwen3.8-27B 77.8%. Terminal Bench v2.1 71.6% vs 73.0%. HLE-Diamond 15.2% vs 15.3%. LongBench v2 66.9% vs 52.3%. MRCR v2 (256K-512K) 46.4% vs 38.0%. The card notes lower quality outside English ("clipped phrasing and mixed languages" in Chinese) — [Inkling-2-Mini card](https://huggingface.co/thinkingmachines/Inkling-2-Mini)
- **Gemma 4** (card, instruction-tuned):
  - MMLU-Pro: 31B 85.2, 26B-A4B 82.6, 12B 77.2, E4B 69.4, E2B 60.0
  - GPQA Diamond: 84.3 / 82.3 / 78.8 / 58.6 / 43.4
  - AIME 2026 (no tools): 89.2 / 88.3 / 77.5 / 42.5 / 37.5
  - LiveCodeBench v6: 80.0 / 77.1 / 72.0 / 52.0 / 44.0
  - MMMLU (multilingual): 88.4 / 86.3 / 83.4 / 76.6 / 67.4
  - E2B/E4B also take audio input — [Gemma 4 31B card](https://huggingface.co/google/gemma-4-31B-it)

**A3B-class MoEs**
- **Nemotron 3.5 Lightning 30B-A3B** (NVIDIA-measured). MMLU-Pro 81.94, GPQA 75.44, SWE-bench Verified 51.56, IFBench 71.88, AA-LCR 52.00. Baselines in the same table: Nemotron 3 Nano 78.46 / 74.05 and gpt-oss-20b 76.40 / 71.46 (MMLU-Pro / GPQA). Qwen3.6-35B-A3B leads most rows. It has MTP layers and a 1M context — [Lightning card](https://huggingface.co/nvidia/NVIDIA-Nemotron-3.5-Lightning-30B-A3B-BF16)
- **Nemotron 3.5 Bolt.** BF16 74B-A9B scores GPQA 88.83, HLE 28.73, Terminal-Bench 2.1 53.93. The 2-bit 24 GB build keeps GPQA 86.43 and Terminal-Bench 2.1 50.98, above Qwen3.6-35B-A3B's 83.40 / 44.38 in NVIDIA's measurement — [Bolt card](https://huggingface.co/nvidia/NVIDIA-Nemotron-3.5-Bolt-74B-A9B-BF16)
- **GLM-4.7-Flash** (card). AIME25 91.6, GPQA 75.2, LiveCodeBench v6 64.0, HLE 14.4, SWE-bench Verified 59.2, τ²-Bench 79.5, BrowseComp 42.8. Same table: gpt-oss-20b 91.7 / 71.5 / 61.0 / 10.9 / 34.0 / 47.7 / 28.3 — [GLM-4.7-Flash card](https://huggingface.co/zai-org/GLM-4.7-Flash)
- **LFM2.5-24B-A2B** (card).
  - Benchmarks: GPQA 74.42, IFEval 96.75, IFBench 74.69, BFCLv4 65.01, MATH-500 96.04. Qwen3.6-35B-A3B scores 83.69 / 93.06 / 64.21 / 65.00 / 96.73.
  - Speed: 114 tok/s on a Ryzen AI Max+ 395 (Q4_K_M) and 184 tok/s on an Apple M5 Max.
  - Pretraining: ~33T tokens — [LFM2.5-24B-A2B card](https://huggingface.co/LiquidAI/LFM2.5-24B-A2B)
- **gpt-oss-20b.** 21B total / 3.6B active, MXFP4, runs within 16 GB, reasoning effort low/medium/high, uses the harmony format, with built-in browsing/Python tools and structured outputs — [gpt-oss-20b card](https://huggingface.co/openai/gpt-oss-20b)
- **Kimi-Linear-48B-A3B.** KV cache reduced "by up to 75%", decoding throughput "up to 6×" at 1M context, MMLU-Pro 51.0 (4k), RULER 84.3 (128k), 5.7T training tokens — [Kimi-Linear card](https://huggingface.co/moonshotai/Kimi-Linear-48B-A3B-Instruct)

**Dense small and mid models**
- **Granite 4.2** (card, Aug 2026):
  - MMLU-Pro: 30B 77.60, 8B 74.04, 3B 67.84
  - GPQA: 66.41 / 64.14 / 54.80
  - AIME25: 89.17 / 86.67 / 78.33
  - LiveCodeBench v6: 75.77 / 73.24 / 69.71
  - IFBench: 77.17 / 79.33 / 74.33
  - BFCL v4: 61.39 / 52.39 / 52.41
  - RULER 128K: 81.38 / 71.41 / 55.30
  - Modes: thinking, non-thinking and low-effort. No external baselines are given — [Granite 4.2 30B card](https://huggingface.co/ibm-granite/granite-4.2-30b)
- **Ministral 3 14B** (card). Reasoning variant: AIME25 0.850, GPQA 0.712, LiveCodeBench 0.646, vs Qwen3-14B Thinking 0.737 / 0.663 / 0.593. Instruct variant: Arena Hard 0.551 vs Qwen3-14B non-thinking 0.427 — [Ministral 3 14B card](https://huggingface.co/mistralai/Ministral-3-14B-Instruct-2512)
- **Mistral Small 4** (119B-A6.5B). GPQA Diamond 71.2. Claims about 40% lower latency and 3x more requests/s than Mistral Small 3. `reasoning_effort` none/high — [Mistral Small 4 card](https://huggingface.co/mistralai/Mistral-Small-4-119B-2603)

**Tiny / on-device**
- **LFM3-1.7B** (card). IFEval 96.66, IFBench 73.03, GPQA 63.37, BFCL V4 71.39, LiveCodeBench v6 73.46. It averages 49.37 across the card's rows vs Qwen3.5-9B 47.24 and Qwen3.5-4B 39.27. 16 languages, about 1 GB at 4-bit. Not recommended for agentic coding — [LFM3-1.7B card](https://huggingface.co/LiquidAI/LFM3-1.7B)
- **MiniCPM5-2B** (card). MMLU-Pro 70.8, GPQA 70.2, AIME 2025 86.5, IFEval 86.7, LiveCodeBench v6 69.1, BFCL v4 66.6. Card average 53.9 vs Qwen3.5-2B 28.0, Gemma-4-E2B 24.6 and Qwen3.5-4B 51.1 — [MiniCPM5-2B card](https://huggingface.co/openbmb/MiniCPM5-2B)
- **ZAYA1-8B** (760M active). AIME'26 89.1, MMLU-Pro 74.2, GPQA 71.0, IFEval 85.58, LiveCodeBench v6 65.8, vs Qwen3.5-4B 84.5 / 79.1 / 76.2 / 89.8 — [ZAYA1-8B card](https://huggingface.co/Zyphra/ZAYA1-8B)
- **SmolLM3-3B** (thinking). AIME 2025 36.7, GPQA 41.7, IFEval 71.2, BFCL 88.8. Six native languages (EN/FR/ES/DE/IT/PT) — [SmolLM3 card](https://huggingface.co/HuggingFaceTB/SmolLM3-3B)
- **Aion-1.0 2.3B.** MMLU 55.15, MMLU-Pro 33.35, GPQA 28.18, IFEval 83.90, MATH-500 48.00 — [Aion card](https://huggingface.co/microsoft/Aion-1.0-Instruct-2.3B)

**Fully-open models**
- **K2-Horizon-32B** (AA figures per card) vs Qwen3.8-27B / Muse Glimmer / Granite 4.2 30B:
  - GPQA Diamond: 82.8 / 90.5 / 83.5 / 64.4
  - HLE: 25.2 / 33.9 / 22.0 / 11.2
  - Terminal-Bench 2.1: 62.6 / 79.8 / 51.7 / 26.6
  - AA-Omniscience non-hallucination: 69.7 / 69.7 / 18.1 / 74.4 — [K2-Horizon-32B card](https://huggingface.co/IFM/K2-Horizon-32B)
- **K2-Horizon-MoVA-36B-A4B.** GPQA 80.8, HLE 25.2, Terminal-Bench 2.1 58.6, tau3-Banking 26.8 (best in its table) — [MoVA card](https://huggingface.co/IFM/K2-Horizon-MoVA-36B-A4B)
- **Olmo 3.1 32B Think vs Qwen 3 32B:**
  - AIME 2025: 78.1 vs 70.9
  - IFEval: 93.8 vs 86.5
  - IFBench: 68.1 vs 37.3
  - GPQA: 57.5 vs 67.3
  - MMLU: 86.4 vs 88.8
  - LiveCodeBench v3: 83.3 vs 90.2
  - Safety: 83.6 vs 69.0 — [Olmo 3.1 32B Think card](https://huggingface.co/allenai/Olmo-3.1-32B-Think)
- **Olmo Hybrid Instruct DPO 7B vs Olmo 3 7B DPO.** MMLU 73.6 vs 69.1, GPQA 38.0 vs 37.9, IFEval 80.5 vs 82.0, AIME 2025 10.2 vs 20.4. Claimed "roughly 2x data efficiency" in pretraining — [Olmo Hybrid card](https://huggingface.co/allenai/Olmo-Hybrid-Instruct-DPO-7B)
- **Apertus v1.5 70B.** MMLU-Pro 56.38, GPQA 35.94, GSM8K 84.99, IFBench 36.67 (thinking on). "Massively multilingual"; no comparison table yet — [Apertus v1.5 70B card](https://huggingface.co/swiss-ai/Apertus-v1.5-70B)
- **K2-V2 (High reasoning, post-SFT).** AIME25 80.2, GPQA-D 69.3, LiveCodeBench v6 67.0, IFEval 90.0, LongBench V2 42.6. Reasoning effort low/medium/high — [K2-V2-Instruct card](https://huggingface.co/IFM/K2-V2-Instruct)

**Index context**
- AA Index v5 is a recalibrated, harder index. A third-party roundup dated Oct 27, 2026 says the best open-weight models score 30-32 on v5.0 vs 58 for the best closed model. Treat this as unverified secondary data — [slash-digital roundup](https://slash-digital.io/en/insights/open-weight-models-2026/)

### Inferences
- **Reasoning/knowledge per GB.** Qwen3.8-27B (BF16 55.6 GB / FP8 30.9 GB) is the best single dense pick. Inkling 2 Mini is the best "fast + strong" pick at 20 GB, with 1M context. Nanbeige4.3-33B is the strongest agentic-terminal model in the class, by its own card.
- **Instruction-following / tool calling on small budgets:** LFM2.5-24B-A2B (IFEval 96.75, BFCLv4 65.01) and LFM3-1.7B (IFEval 96.66, BFCL 71.39) stand out. Licence-sensitive users must account for the LFM $10M revenue cap.
- **Long context:** Inkling 2 Mini (1M; LongBench v2 66.9), Kimi-Linear (1M, linear attention), K2 Horizon / Nanbeige (512K native) and Nemotron 3.5 Lightning (1M) are the long-context specialists. Qwen reaches 1M only via YaRN.
- **Fully-open trade-off.** Olmo 3.1 remains strong on instruction-following and safety but weak on GPQA. Apertus v1.5 is mainly valuable for multilingual coverage and compliance/openness rather than raw capability.

### Gaps
- No independent (non-vendor) MMLU-Pro/GPQA/AIME/IFEval numbers were found for most Nov-2026 releases. Almost all numbers above are self-reported or "AA figures" quoted in vendor cards.
- LMArena standings are unavailable (see Q1).
- AA Intelligence Index v5.0.2 values for Gemma 4 12B/26B, gpt-oss-20b, Nemotron 3.5 Lightning, Granite 4.2, LFM2.5-24B, MiniCPM5 and the fully-open models were not retrieved.
- MMLU-Pro is not reported for Qwen3.8-27B, Inkling 2 Mini or Muse Glimmer on their cards. IFEval is missing for many.

## Q4. Best draft-model companions for speculative decoding

### Takeaway
In 2026 most labs ship dedicated drafters rather than relying on "a smaller sibling".

Official or vendor-backed options:
- **Gemma 4:** official MTP "-assistant" drafters, 0.16-0.97 GB, up to 3x.
- **Qwen3.5/3.6/3.8:** built-in MTP heads, plus NVIDIA- and Tencent-trained Qwen3.8-27B drafters.
- **Muse Glimmer:** a DFlash drafter (2.56B, 3.1x on an RTX 5090).
- **Nemotron 3.5:** MTP layers plus DSpark/DFlash/LiLiCorr drafters.
- **GLM-4.7-Flash:** MTP.
- **LFM2.5-24B-A2B and MiniCPM5-2B:** DSpark drafters.
- **Mistral Small 4:** an EAGLE head.

The classic same-tokenizer small-sibling approach is documented only indirectly: NVIDIA built its Qwen3.8-27B drafts from Qwen3.5-0.8B/4B.

### Cited Findings
- **Gemma 4 MTP drafters** (Google, May 5, 2026). They predict several tokens, then the target verifies them in parallel. They use the target's activations and share its KV cache. Google claims "up to a 3x speedup without any degradation", and up to ~2.2x on the 26B MoE at batch 4-8. Apache-2.0, with support in transformers, MLX, vLLM, SGLang and Ollama — [Google blog](https://blog.google/innovation-and-ai/technology/developers-tools/multi-token-prediction-gemma-4/?authuser=0)
- **Gemma 4 drafter sizes** (API-measured):

  | Drafter | Params | GB |
  |---|---|---|
  | [google/gemma-4-31B-it-assistant](https://huggingface.co/google/gemma-4-31B-it-assistant) | 0.47B | 0.97 |
  | [google/gemma-4-26B-A4B-it-assistant](https://huggingface.co/google/gemma-4-26B-A4B-it-assistant) | 0.42B | 0.87 |
  | [google/gemma-4-12B-it-assistant](https://huggingface.co/google/gemma-4-12B-it-assistant) | 0.42B | 0.88 |
  | [google/gemma-4-E4B-it-assistant](https://huggingface.co/google/gemma-4-E4B-it-assistant) | 0.08B | 0.19 |

  All are BF16. QAT-matched variants exist (e.g. `gemma-4-31B-it-qat-q4_0-unquantized-assistant`) — [HF API: author=google](https://huggingface.co/api/models?author=google&sort=createdAt&direction=-1)
- **Qwen3.6-35B-A3B built-in MTP.** vLLM: `--speculative-config '{"method":"qwen3_next_mtp","num_speculative_tokens":2}'`. SGLang: `--speculative-algo NEXTN --speculative-num-steps 3 --speculative-eagle-topk 1 --speculative-num-draft-tokens 4`. No speedup is stated — [Qwen3.6-35B-A3B card](https://huggingface.co/Qwen/Qwen3.6-35B-A3B). Qwen3.8-27B is also "trained with multiple steps" MTP, but its card lists no flags — [Qwen3.8-27B card](https://huggingface.co/Qwen/Qwen3.8-27B)
- **External drafts for Qwen3.8-27B** (all created Nov 2026; API-measured):
  - [nvidia/Qwen3.5-4B-Qwen3.8-27B-Draft](https://huggingface.co/nvidia/Qwen3.5-4B-Qwen3.8-27B-Draft): 4.21B, 8.43 GB
  - [nvidia/Qwen3.5-0.8B-Qwen3.8-27B-Draft](https://huggingface.co/nvidia/Qwen3.5-0.8B-Qwen3.8-27B-Draft): 0.75B, 1.52 GB
  - Both carry the nvidia-open-model-agreement licence. NVIDIA also lists nvidia/Llama-3.2-1B-Qwen3.8-27B-Draft (1.48B).
  - [tencent/Qwen3.8-27B-DSpine-b16](https://huggingface.co/tencent/Qwen3.8-27B-DSpine-b16): 2.03B, 4.05 GB, Apache-2.0. Its purpose is inferred from the name only.
  - Source: [HF API: author=nvidia](https://huggingface.co/api/models?author=nvidia&sort=createdAt&direction=-1); [HF API: author=tencent](https://huggingface.co/api/models?author=tencent&sort=createdAt&direction=-1)
- **Muse Glimmer-30B DFlash drafter.** It proposes 16-token blocks in one pass. Measured with the 17 GB K-quant model at batch 1, greedy decoding:
  - RTX 5090: 74.9 → 233.4 tok/s (3.1x)
  - Apple M4 Max: 1.5x
  - Apple M5 Max: 1.8x
  - The drafter repo is [meta-models/Muse-Glimmer-30B-assistant](https://huggingface.co/meta-models/Muse-Glimmer-30B-assistant) (2.56B, BF16, 5.11 GB, API-measured) — [Muse Glimmer card](https://huggingface.co/meta-models/Muse-Glimmer-30B)
- **Nemotron 3.5 Lightning.** Built-in MTP layers plus two drafters, both shipped as NVFP4 repos with no numeric speedup given. DSpark is "a semi-autoregressive speculative-decoding drafter that proposes a whole block of candidate tokens in a single forward pass", recommended for DGX Spark and low concurrency. DFlash uses "a lightweight block-diffusion model". DSpark drafter size: [nvidia/...-NVFP4-DSpark](https://huggingface.co/nvidia/NVIDIA-Nemotron-3.5-Lightning-30B-A3B-NVFP4-DSpark), 0.76B, 1.35 GB — [Lightning card](https://huggingface.co/nvidia/NVIDIA-Nemotron-3.5-Lightning-30B-A3B-BF16)
- **Nemotron 3.5 Bolt.** "LiLiCorr drafter via DFlash" with 7 speculative tokens in the vLLM recipes — [Bolt card](https://huggingface.co/nvidia/NVIDIA-Nemotron-3.5-Bolt-74B-A9B-BF16)
- **GLM-4.7-Flash.** vLLM `--speculative-config.method mtp --speculative-config.num_speculative_tokens 1`. SGLang `--speculative-algorithm EAGLE --speculative-num-steps 3 --speculative-eagle-topk 1 --speculative-num-draft-tokens 4` — [GLM-4.7-Flash card](https://huggingface.co/zai-org/GLM-4.7-Flash)
- **Liquid and OpenBMB DSpark drafters:**
  - [LiquidAI/LFM2.5-24B-A2B-DSpark](https://huggingface.co/LiquidAI/LFM2.5-24B-A2B-DSpark): 0.33B, 0.66 GB, API-measured. The card calls it a "DSpark draft model for speculative decoding with SGLang"; it was marked "Coming soon" when the card was read, though the repo exists (created 2026-10-19) — [LFM2.5-24B-A2B card](https://huggingface.co/LiquidAI/LFM2.5-24B-A2B)
  - MiniCPM5-2B-DSpark runs in SGLang via `--speculative-algorithm DSPARK` with block size 7, "keeping the target model's outputs unchanged" — [MiniCPM5-2B card](https://huggingface.co/openbmb/MiniCPM5-2B)
- **Mistral Small 4 EAGLE head:** `mistralai/Mistral-Small-4-119B-2603-eagle` (no speedup figure) — [Mistral Small 4 card](https://huggingface.co/mistralai/Mistral-Small-4-119B-2603)
- **DeepSeek generic drafters** (2026-06-28): DSpark/DFlash/EAGLE-3 drafters for Qwen3-4B/8B/14B and Gemma-4-12B, e.g. `deepseek-ai/dspark_gemma4_12b_block7` (3.43B params) — [HF API: author=deepseek-ai](https://huggingface.co/api/models?author=deepseek-ai&sort=createdAt&direction=-1)

### Inferences
- **Best pairings to archive on a 128 GB box:**
  - Qwen3.8-27B + nvidia/Qwen3.5-0.8B-Qwen3.8-27B-Draft (1.52 GB) or the built-in MTP heads (no download).
  - Each Gemma 4 model + its "-assistant" (<1 GB each).
  - Muse-Glimmer-30B + Muse-Glimmer-30B-assistant (5.11 GB; the only drafter here with a published 3x local speedup).
  - Nemotron 3.5 Lightning + DSpark (1.35 GB).
  - LFM2.5-24B-A2B + DSpark (0.66 GB).
  - For legacy Qwen3.5/3.6 targets, Qwen3.5-0.8B/2B are tokenizer-compatible small siblings (NVIDIA's Qwen3.5-based Qwen3.8 drafts imply a shared tokenizer).
- **Same-tokenizer pairs that are plausible but undocumented:** gpt-oss-20b as a draft for gpt-oss-120b (same harmony format), Granite 4.2 3B for 4.2 30B, Ministral 3 3B for Ministral 3 14B, and Olmo 3 7B for Olmo 3.1 32B. These are inferences from shared lineage only.

### Gaps
- **No measured speedups** were published for the Qwen MTP heads, the Nemotron DSpark/DFlash/LiLiCorr drafters, LFM2.5 DSpark, MiniCPM5 DSpark or the Mistral EAGLE head.
- **No vendor documentation** was found for gpt-oss-20b → gpt-oss-120b, Granite 3B → 30B, Ministral 3B → 14B, or Olmo 7B → 32B draft pairing.
- **Unverified repos.** Tencent "DSpine" and NVIDIA "Llama-3.2-1B-Qwen3.8-27B-Draft" (cross-tokenizer?) were not opened, so their method and licence terms beyond the API metadata are unverified.
