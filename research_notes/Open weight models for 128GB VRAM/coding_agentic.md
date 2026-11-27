# Coding-specialized and agentic/tool-use open-weight LLMs that fit in 128 GB (state as of 2026-11-24)

## 1. Which coding/agentic models in this size class are current and strongest as of Nov 2026, and which are superseded?

### Takeaway
In late Nov 2026 the strongest coding models that really fit a 128 GB box are mostly general agentic releases, not coding specialists. They are Qwen3.8-27B (Aug 2026), Qwen3.8-Flash-Next through NVIDIA's 102 GB FP4-Local build (Aug/Oct 2026; restrictive licence), and NVIDIA Nemotron 3.5 Super VL 120B-A12B (official 75 GB NVFP4, Oct 2026). The current coding-specialized releases are Qwen3-Coder-Next 80B-A3B (Jan/Feb 2026), Kwaipilot KAT-Coder-V2.5-Dev 35B-A3B (Jul 2026), Devstral Small 2 24B (Dec 2025) and Mistral Medium 3.5 128B dense (Mar 2026). Mistral says Medium 3.5 supersedes Devstral, but it needs a ~Q6 GGUF to fit. Qwen's HF org has no Qwen3.5/3.6/3.8 "Coder" model. Almost every 2025 SWE fine-tune is superseded.

### Cited Findings
**Current leaders that fit in about 110 GB**
- **Qwen3.8-27B** (dense, Apache-2.0, HF repo created 2026-08-05) self-reports Terminal-Bench 2.1 (Terminus) 73.0, SWE-bench Pro 61.7, NL2Repo 42.3, DeepSWE 1.1 42.2 and LiveCodeBench v6 90.3. The same table gives Qwen3.6-27B 63.4 / 53.5 / 36.2 / 13.3 / 83.9. — [Qwen3.8-27B model card](https://huggingface.co/Qwen/Qwen3.8-27B)
- **Qwen3.8-Flash-Next** is 125B with 6B activated, plus a 51B n-gram embedding and 4B MTP. It self-reports SWE-bench Pro 62.5, SWE-bench Multilingual 81.0, DeepSWE 1.1 58.7, NL2Repo 48.1, Toolathlon Verified 73.5 and LiveCodeBench v6 91.9. Licence: "qwen-community-1.0". — [Qwen3.8-Flash-Next model card](https://huggingface.co/Qwen/Qwen3.8-Flash-Next)
- **Qwen Community License 1.0:** a licensee running a "Model as a Service" or "AI Work Assistant" business (defined as including AI-assisted coding products) must get a separate licence from Qwen for any commercial use; internal use is exempt. Products above 100M MAU or US$20M monthly revenue must display the model name. — [Qwen3.8-Flash-Next LICENSE](https://huggingface.co/Qwen/Qwen3.8-Flash-Next/blob/main/LICENSE)
- **NVIDIA's Qwen3.8-Flash-Next-FP4-Local** is a quantization-aware-distilled NVFP4/MXFP8 build of about 96 GiB, validated on DGX Spark and RTX PRO 6000. Scores vs BF16: GPQA Diamond 91.5 vs 92.2, ToolEvalBench 88.7 vs 89.4. — [nvidia/Qwen3.8-Flash-Next-FP4-Local card](https://huggingface.co/nvidia/Qwen3.8-Flash-Next-FP4-Local)
- **NVIDIA Nemotron 3.5 Super VL 120B-A12B** (OpenMDW-1.1, HF repo created 2026-10-12), NVFP4 vs BF16: SWE-bench Verified 71.07 vs 71.10, SWE-bench Multilingual 67.00 vs 67.72, SWE-bench Pro 51.30 vs 51.78, Terminal-Bench 2.1 62.78 vs 60.67, LiveCodeBench v6 87.41 vs 88.22. The card names "coding" agents on "DGX Spark systems" as an intended use. — [Nemotron 3.5 Super VL NVFP4 card](https://huggingface.co/nvidia/NVIDIA-Nemotron-3.5-Super-VL-120B-A12B-NVFP4)
- **Qwen3-Coder-Next** (80B total / 3B active, coding agent model; tech report 2026-02-28):
  - SWE-bench Verified: 70.6 (SWE-Agent), 71.1 (MiniSWE-Agent), 71.3 (OpenHands).
  - SWE-bench Pro: 42.7 (SWE-Agent).
  - Terminal-Bench 2.0: 36.2 (Terminus2-json).
  - Aider-Polyglot: 66.20.
  - Source: [Qwen3-Coder-Next Technical Report, arXiv 2603.00729](https://arxiv.org/html/2603.00729v1)
- **KAT-Coder-V2.5-Dev** is the open-weight release of Kwaipilot's July 2026 KAT-Coder-V2.5: "an MOE model with a total parameter count of 35B and 3B activated". Its card reports, all from Kwaipilot's own Claude Code harness:
  - SWE-bench Verified 69.40, Multilingual 63.00, Pro 45.96.
  - Terminal-Bench 2.1: 41.02 average (32.60 Terminus-2 / 49.44 Claude Code).
  - In the same harness Kwaipilot measured Qwen3.6-35B-A3B at only 64.40 on Verified, and says its numbers run about 10 pp below Qwen's official ones.
  - Source: [KAT-Coder-V2.5-Dev card](https://huggingface.co/Kwaipilot/KAT-Coder-V2.5-Dev)
- **Mistral Medium 3.5 128B** "supersedes all our previous coding models, namely Devstral, across all benchmarks". It scores 77.6% on SWE-Bench Verified and 91.4% on τ³-Telecom, and "replaces Devstral 2 in our coding agent, Vibe CLI". — [Mistral-Medium-3.5-128B card](https://huggingface.co/mistralai/Mistral-Medium-3.5-128B)
- **Mistral Small 4** (119B MoE, 6B active; announced 2026-03-16) "unif[ies]… Magistral…, Pixtral…, and Devstral for agentic coding". Its coding results are given only as charts; the text says it beats GPT-OSS 120B on LiveCodeBench with 20% less output. — [Mistral Small 4 announcement](https://mistral.ai/news/mistral-small-4/)
- **Devstral Small 2 (24B, Apache-2.0):** self-reported SWE-bench Verified 68.0%, Multilingual 55.7%, Terminal-Bench 2 22.5%. **Devstral 2 (123B):** 72.2% / 61.3% / 32.6%. — [Devstral-2-123B card](https://huggingface.co/mistralai/Devstral-2-123B-Instruct-2512)
- **StepFun Step-3.7-Flash** (198B MoE, about 11B active, Apache-2.0) self-reports SWE-Bench Pro 56.3 and Terminal-Bench 2.1 59.5. — [Step-3.7-Flash card](https://huggingface.co/stepfun-ai/Step-3.7-Flash)
- **MiniMax-M2.7** self-reports SWE-Pro 56.22%, SWE Multilingual 76.5, Multi-SWE-Bench 52.7, VIBE-Pro 55.6%, Terminal Bench 2 57.0% and NL2Repo 39.8%. — [MiniMax-M2.7 card](https://huggingface.co/MiniMaxAI/MiniMax-M2.7)
- **Its licence is non-commercial:** "Any Commercial Use… is prohibited without obtaining a separate, prior written authorization from MiniMax". — [MiniMax-M2.7 LICENSE](https://huggingface.co/MiniMaxAI/MiniMax-M2.7/raw/main/LICENSE)
- **The popular GGUF mislabels that licence:** unsloth's M2.7 GGUF repo declares the licence as "modified-mit" in its metadata. — [HF API unsloth/MiniMax-M2.7-GGUF](https://huggingface.co/api/models/unsloth/MiniMax-M2.7-GGUF?blobs=true)
- **IBM's current Granite with coding/agentic RL is granite-4.2-30b** (dense, Apache-2.0, Aug 2026): SWE-bench Verified 57, SWE-bench Pro 33.29, Terminal-Bench 2.1 29.24, BFCL v4 61.39, LiveCodeBench v6 75.77. — [granite-4.2-30b card](https://huggingface.co/ibm-granite/granite-4.2-30b)
- **No new Qwen Coder model:** Qwen's HF listing since 2025-11 contains Qwen3-Coder-Next, -FP8, -GGUF and -Base, and no Qwen3.5/3.6/3.8 "Coder" repo. Newer Qwen releases are Qwen3.5, Qwen3.6, Qwen3.8-27B, Qwen3.8-Flash-Next and Qwen-AgentWorld-35B-A3B. — [HF API Qwen listing](https://huggingface.co/api/models?author=Qwen&sort=createdAt&direction=-1&limit=300)
- **Qwen-AgentWorld-35B-A3B is not a coding agent.** It is a "language world model" that simulates a Linux terminal or other environments ("predict the terminal output"). — [Qwen-AgentWorld card](https://huggingface.co/Qwen/Qwen-AgentWorld-35B-A3B)
- **Mistral has published no new Devstral weights since Devstral 2** (repo created 2025-11-28). Later releases are Mistral Small 4, Medium 3.5, Leanstral 1.5 and Large 4 1T. — [HF API mistralai listing](https://huggingface.co/api/models?author=mistralai&sort=createdAt&direction=-1&limit=300)
- **Salesforce has published no new xLAM release since the xLAM-2 "fc-r" models (Mar 2025).** — [HF API Salesforce listing](https://huggingface.co/api/models?author=Salesforce&sort=createdAt&direction=-1&limit=300)
- **Hermes:** the latest is Hermes-4.3-36B (Nov 2025, Seed-OSS architecture). Nous's newest model is Arete-27B (2026-11-06), a Qwen3.8-27B fine-tune aimed at removing CCP-framed answers, not a coding model. — [HF API NousResearch listing](https://huggingface.co/api/models?author=NousResearch&sort=createdAt&direction=-1&limit=300); [Arete-27B card](https://huggingface.co/NousResearch/Arete-27B)

**Superseded or older (still downloadable)**
- **Qwen3-Coder-30B-A3B-Instruct** (Jul 2025), in recent third-party tables:
  - Kwaipilot, Claude Code harness: SWE-bench Verified 31.80, Terminal-Bench 2.1 13.50 — [KAT-Coder-V2.5-Dev card](https://huggingface.co/Kwaipilot/KAT-Coder-V2.5-Dev)
  - OpenThoughts, OpenHands: SWE-bench Verified 49.2, Terminal-Bench 2.0 10.1 — [OpenThinker-Agent-v1 card](https://huggingface.co/open-thoughts/OpenThinker-Agent-v1)
- **Nemotron 3 Super 120B-A12B (Mar 2026), superseded by Nemotron 3.5 Super VL.** Its SWE-bench Verified (OpenHands) is reported as 60.5 on one NVIDIA card and 59.5 on another. — [Nemotron-Cascade-2 card](https://huggingface.co/nvidia/Nemotron-Cascade-2-30B-A3B); [Puzzle-75B card](https://huggingface.co/nvidia/NVIDIA-Nemotron-Labs-3-Puzzle-75B-A9B-BF16)
- **Qwen3.5-122B-A10B (Feb 2026):** SWE-bench Verified 72.0, Terminal Bench 2 49.4, BFCL-V4 72.2. — [Qwen3.5-122B-A10B card](https://huggingface.co/Qwen/Qwen3.5-122B-A10B)
- **Kimi-Dev-72B (Jun 2025):** 60.4% SWE-bench Verified. — [Kimi-Dev-72B card](https://huggingface.co/moonshotai/Kimi-Dev-72B)
- **KAT-Dev-72B-Exp (Oct 2025):** 74.6% SWE-bench Verified "strictly with the SWE-agent scaffold". — [KAT-Dev-72B-Exp card](https://huggingface.co/Kwaipilot/KAT-Dev-72B-Exp)
- **KAT-Dev 32B:** 62.4%. — [KAT-Dev card](https://huggingface.co/Kwaipilot/KAT-Dev)
- **Skywork-SWE-32B:** 38.0% pass@1 (OpenHands). — [Skywork-SWE-32B card](https://huggingface.co/Skywork/Skywork-SWE-32B)
- **DeepSWE-Preview:** 42.2% Pass@1, or 59.0% with test-time scaling. — [DeepSWE-Preview card](https://huggingface.co/agentica-org/DeepSWE-Preview)
- **SWE-Swiss-32B:** 60.2%. — [SWE-Swiss-32B card](https://huggingface.co/SWE-Swiss/SWE-Swiss-32B)
- **Seed-OSS-36B-Instruct (Aug 2025):** SWE-Bench Verified 56 (OpenHands). — [Seed-OSS-36B card](https://huggingface.co/ByteDance-Seed/Seed-OSS-36B-Instruct)
- **Nemotron-Terminal-32B (Feb 2026):** Terminal-Bench 2.0 27.4% with Terminus 2. — [Nemotron-Terminal-32B card](https://huggingface.co/nvidia/Nemotron-Terminal-32B)
- **GLM-4.7-Flash (30B-A3B, Jan 2026):** SWE-bench Verified 59.2, τ²-Bench 79.5. — [GLM-4.7-Flash card](https://huggingface.co/zai-org/GLM-4.7-Flash)
- **Codestral-22B-v0.1** (2024-05-29) carries the **MNPL (non-commercial)** licence and a 32,768-token context. — [HF API Codestral-22B-v0.1](https://huggingface.co/api/models/mistralai/Codestral-22B-v0.1?blobs=true); [config](https://huggingface.co/mistralai/Codestral-22B-v0.1/resolve/main/config.json)
- **IQuest-Coder-V1-40B** (Dec 2025) claims SWE-Bench Verified 76.2% and LiveCodeBench v6 81.1%. — [IQuest-Coder card](https://huggingface.co/IQuestLab/IQuest-Coder-V1-40B-Instruct)
- **IQuest's earlier claim was 81.4%,** and it was re-run down to 76.2% after its harness exposed the full git history including future commits. Both accounts are secondary blogs; the primary source is reportedly IQuest's GitHub issue #14. — [byteiota](https://byteiota.com/iquest-coders-81-score-drops-to-76-after-scandal/); [agentmarketcap](https://agentmarketcap.ai/blog/2026/04/16/iquest-coder-v1-benchmark-gaming-scandal-procurement-disclosure)

**"Flash" names no longer mean small — these coding-relevant models do not fit at all**
- **MiniMax-M3.1-Flash:** "Approximately 428B total; 23B activated". — [MiniMax-M3.1-Flash card](https://huggingface.co/MiniMaxAI/MiniMax-M3.1-Flash)
- **GLM-5.3-Flash:** "320B total parameters and just 18B active". — [GLM-5.3-Flash card](https://huggingface.co/zai-org/GLM-5.3-Flash)
- **DeepSeek-V4.1-Flash-1017:** 763.21B params, 510.30 GB. — [HF API](https://huggingface.co/api/models/deepseek-ai/DeepSeek-V4.1-Flash-1017?blobs=true)
- **Kimi-K2.7-Code:** 1026.88B params, 595.18 GB. — [HF API](https://huggingface.co/api/models/moonshotai/Kimi-K2.7-Code?blobs=true)
- **Qwen3-Coder-480B-A35B-Instruct-FP8:** 482.15 GB. — [HF API](https://huggingface.co/api/models/Qwen/Qwen3-Coder-480B-A35B-Instruct-FP8?blobs=true)
- **NVIDIA-Nemotron-Labs-Teacher-Competition-Coding:** 560.52B params, 1121.06 GB. — [HF API](https://huggingface.co/api/models/nvidia/NVIDIA-Nemotron-Labs-Teacher-Competition-Coding?blobs=true)

### Inferences
- "Coding specialist" open models have largely been overtaken by general agentic releases in this size class.
  - On self-reported SWE-bench Pro: Qwen3.8-27B 61.7 and Qwen3.8-Flash-Next 62.5, vs Qwen3-Coder-Next 42.7 and KAT-Coder-V2.5-Dev 45.96.
  - The harnesses differ (Claude Code vs SWE-Agent) and Qwen evaluates on a corrected Pro subset, so the gap is directionally right but its size is uncertain.
- Practical ranking for a 128 GB box (with caveats):
  1. Qwen3.8-27B FP8: about 31 GB, leaving very large KV headroom.
  2. Nemotron 3.5 Super VL NVFP4: 75 GB, permissive OpenMDW licence, no quant loss on SWE-bench.
  3. Qwen3.8-Flash-Next FP4-Local: 102 GB. Possibly the strongest, but its licence is restrictive for coding-assistant businesses.
  4. Qwen3-Coder-Next FP8: 80 GB, Apache-2.0, fast decode thanks to 3B active.
  5. KAT-Coder-V2.5-Dev and Qwen3.6-35B-A3B: small, fast 3B-active MoEs.
  6. Mistral Medium 3.5 (Q5/Q6 GGUF) only if dense-model slowness is acceptable.
- Licence traps matter more than usual in this class:
  - MiniMax-M2.7: non-commercial.
  - Qwen3.8-Flash-Next: separate licence needed for coding-product and MaaS businesses.
  - Devstral 2 and Mistral Medium 3.5: no rights for companies above $20M monthly revenue (see Q2).
  - Codestral and xLAM-2: non-commercial.

### Gaps
- No independent, same-harness comparison of all candidates exists in the sources found. The official SWE-bench leaderboard's newest entry is 2026-02-26 and Aider's is 2025-10-03 (see Q3), so every 2026 number above is vendor-reported unless noted.
- Could not find text-form SWE-bench or Terminal-Bench numbers for Mistral Small 4 (they are in charts only) or for GLM-5.3-Flash (its card's coding table did not extract as text).
- Could not verify IQuest's GitHub issue #14 directly.

## 2. For each model: exact HF repo id, total/active params, recommended precision and its measured download size, context length, licence, release date

### Takeaway
Every size below is the sum of `siblings[].size` from the Hugging Face API, measured on 2026-11-24 in GB (10^9 bytes), counting only the weight files of the chosen precision or quant. Inside the about 110 GB budget, the best official builds are:

| Model | Official build | Size |
|---|---|---|
| Qwen3.8-27B | FP8 | 30.87 GB |
| Qwen3-Coder-Next | FP8 | 80.38 GB |
| Nemotron 3.5 Super VL | NVFP4 | 75.01 GB |
| Qwen3.8-Flash-Next | NVIDIA FP4-Local | 102.26 GB |
| Mistral Small 4 | NVFP4 | 70.81 GB |
| Devstral Small 2 | FP8 | 25.79 GB |
| Qwen3.6-35B-A3B | FP8 | 37.46 GB |

KAT-Coder-V2.5-Dev ships only BF16 (69.33 GB). Devstral 2, Mistral Medium 3.5, MiniMax-M2.x, Step-3.7-Flash and DeepSeek-V4-Flash need GGUF quants.

### Cited Findings
**Measurement method and caveats**
- Sizes come from `https://huggingface.co/api/models/<repo>?blobs=true`. Context lengths come from each repo's `config.json` (`max_position_embeddings`) unless the card states otherwise.
- **Mistral repos ship each weight set twice** (HF-format shards plus `consolidated.safetensors`), so a download should fetch only one. Example: Devstral-2's repo total is 256.53 GB, but one format is 128.25 GB. — [HF API Devstral-2](https://huggingface.co/api/models/mistralai/Devstral-2-123B-Instruct-2512?blobs=true)
- **API "params" understate FP4-packed repos.** Examples: Nemotron-3-Super-NVFP4 reports 67.23B and MiniMax-M3.1-Flash 220.49B, while their cards say 120B and about 428B. — [HF API Nemotron-3-Super-NVFP4](https://huggingface.co/api/models/nvidia/NVIDIA-Nemotron-3-Super-120B-A12B-NVFP4?blobs=true); [MiniMax-M3.1-Flash card](https://huggingface.co/MiniMaxAI/MiniMax-M3.1-Flash)
- **A 128 GB Mac leaves about 96 GB for the GPU by default.** — [llmconfigurator Local AI Report #6](https://llmconfigurator.com/en/reports/best-local-coding-models-by-hardware)
- **DGX Spark exposes 121 GiB of integrated memory** (one tester's figure). — [classmethod DGX Spark test](https://dev.classmethod.jp/en/articles/dgx-spark-single-node-flash-next-vs-glm-3bit-vs-27b/)

**A. Coding-specialized, current**
- **Qwen3-Coder-Next** (`Qwen/Qwen3-Coder-Next-FP8`)
  - Params: 79.67B total; 80B total / 3B active (10 of 512 experts).
  - Recommended **FP8: 80.38 GB**. BF16 (`Qwen/Qwen3-Coder-Next`) is 159.36 GB.
  - Qwen's own GGUF (`Qwen/Qwen3-Coder-Next-GGUF`): Q8_0 84.81 GB, Q6_K 65.53 GB, Q5_K_M 56.71 GB, Q4_K_M 48.41 GB.
  - unsloth (`unsloth/Qwen3-Coder-Next-GGUF`): UD-Q8_K_XL 86.35 GB, UD-Q6_K_XL 73.12 GB, UD-Q4_K_XL 49.61 GB, MXFP4_MOE 48.03 GB.
  - Context 262,144. Apache-2.0. HF repo created 2026-01-30 (FP8 on 2026-02-01).
  - Sources: [HF API FP8](https://huggingface.co/api/models/Qwen/Qwen3-Coder-Next-FP8?blobs=true); [HF API BF16](https://huggingface.co/api/models/Qwen/Qwen3-Coder-Next?blobs=true); [HF API GGUF](https://huggingface.co/api/models/Qwen/Qwen3-Coder-Next-GGUF?blobs=true); [HF API unsloth GGUF](https://huggingface.co/api/models/unsloth/Qwen3-Coder-Next-GGUF?blobs=true); [model card](https://huggingface.co/Qwen/Qwen3-Coder-Next)
- **KAT-Coder-V2.5-Dev** (`Kwaipilot/KAT-Coder-V2.5-Dev`)
  - Params: 34.66B measured; 35B total / 3B active MoE (qwen3_5_moe architecture: 256 experts, 8 per token).
  - **BF16 69.33 GB** is the only official precision found.
  - Context 262,144. Apache-2.0. HF repo created 2026-07-23.
  - Sources: [HF API](https://huggingface.co/api/models/Kwaipilot/KAT-Coder-V2.5-Dev?blobs=true); [card](https://huggingface.co/Kwaipilot/KAT-Coder-V2.5-Dev)
- **Devstral Small 2** (`mistralai/Devstral-Small-2-24B-Instruct-2512`)
  - Params: 24.01B dense.
  - Official **FP8: 25.79 GB** per format.
  - Context: the card says 256k; config `max_position_embeddings` is 393,216 (YaRN factor 48).
  - Apache-2.0. HF repo created 2025-11-28; its SWE-bench leaderboard entry is dated 2025-12-09.
  - Sources: [HF API](https://huggingface.co/api/models/mistralai/Devstral-Small-2-24B-Instruct-2512?blobs=true); [config](https://huggingface.co/mistralai/Devstral-Small-2-24B-Instruct-2512/resolve/main/config.json); [card](https://huggingface.co/mistralai/Devstral-Small-2-24B-Instruct-2512)
- **Devstral 2** (`mistralai/Devstral-2-123B-Instruct-2512`)
  - Params: 125.03B dense.
  - Official FP8 is **128.25 GB, so it does not fit** under 110 GB.
  - Use `unsloth/Devstral-2-123B-Instruct-2512-GGUF`: Q6_K 102.58 GB, UD-Q6_K_XL 109.84 GB, Q5_K_M 88.32 GB, UD-Q5_K_XL 88.25 GB, Q4_K_M 74.90 GB.
  - Context 256k (config 262,144 with YaRN).
  - **Modified MIT: no rights if the company's global monthly revenue exceeds $20M.** HF repo created 2025-11-28.
  - Sources: [HF API](https://huggingface.co/api/models/mistralai/Devstral-2-123B-Instruct-2512?blobs=true); [HF API unsloth GGUF](https://huggingface.co/api/models/unsloth/Devstral-2-123B-Instruct-2512-GGUF?blobs=true); [LICENSE](https://huggingface.co/mistralai/Devstral-2-123B-Instruct-2512/blob/main/LICENSE)
- **Mistral Medium 3.5** (`mistralai/Mistral-Medium-3.5-128B`)
  - Params: 127.70B dense. Official FP8 is **133.61 GB (does not fit)**.
  - Use `unsloth/Mistral-Medium-3.5-128B-GGUF`: Q6_K 102.58 GB, UD-Q6_K_XL 109.16 GB, Q5_K_M 88.32 GB, UD-Q5_K_XL 88.38 GB, Q4_K_M 74.90 GB. The repo also has 21.45 GB of vision mmproj files across 3 variants; these are optional for code.
  - Context 256k. **Modified MIT with the same $20M monthly-revenue exclusion.** HF repo created 2026-03-31.
  - Sources: [HF API](https://huggingface.co/api/models/mistralai/Mistral-Medium-3.5-128B?blobs=true); [HF API unsloth GGUF](https://huggingface.co/api/models/unsloth/Mistral-Medium-3.5-128B-GGUF?blobs=true); [LICENSE](https://huggingface.co/mistralai/Mistral-Medium-3.5-128B/blob/main/LICENSE); [card](https://huggingface.co/mistralai/Mistral-Medium-3.5-128B)
- **Mistral Small 4** (`mistralai/Mistral-Small-4-119B-2603-NVFP4`)
  - Params: 119B total / 6.5B active (128 experts, 4 active, 1 shared).
  - Official **NVFP4: 70.81 GB**. Official FP8 (`mistralai/Mistral-Small-4-119B-2603`) is 120.92 GB per format.
  - unsloth GGUF: UD-Q6_K 99.43 GB, UD-Q5_K_XL 89.45 GB, UD-Q4_K_XL 74.16 GB, MXFP4_MOE 71.81 GB.
  - Context: 256k per the card (config 1,048,576 with YaRN). Apache-2.0. Announced 2026-03-16.
  - Sources: [HF API NVFP4](https://huggingface.co/api/models/mistralai/Mistral-Small-4-119B-2603-NVFP4?blobs=true); [HF API FP8](https://huggingface.co/api/models/mistralai/Mistral-Small-4-119B-2603?blobs=true); [HF API unsloth GGUF](https://huggingface.co/api/models/unsloth/Mistral-Small-4-119B-2603-GGUF?blobs=true); [card](https://huggingface.co/mistralai/Mistral-Small-4-119B-2603); [announcement](https://mistral.ai/news/mistral-small-4/)
- **Codestral** (`mistralai/Codestral-22B-v0.1`, superseded)
  - Params: 22.25B. BF16: 44.49 GB.
  - Context 32,768. **MNPL non-commercial licence.** Released 2024-05-29.
  - Source: [HF API](https://huggingface.co/api/models/mistralai/Codestral-22B-v0.1?blobs=true)
- **Qwen3-Coder-30B-A3B-Instruct** (`Qwen/Qwen3-Coder-30B-A3B-Instruct-FP8`, superseded)
  - Params: 30.53B / 3.3B active. FP8: 31.18 GB; BF16: 61.07 GB.
  - Context 262,144. Apache-2.0. Released 2025-07-31.
  - Sources: [HF API FP8](https://huggingface.co/api/models/Qwen/Qwen3-Coder-30B-A3B-Instruct-FP8?blobs=true); [card](https://huggingface.co/Qwen/Qwen3-Coder-30B-A3B-Instruct)
- **IQuest-Coder-V1-40B** (`IQuestLab/IQuest-Coder-V1-40B-Instruct`; Loop variant also exists)
  - Params: 39.79B dense. BF16: 79.59 GB.
  - Context 131,072 (128K). **Custom "iquestcoder" licence.** Released 2025-12-30.
  - Sources: [HF API](https://huggingface.co/api/models/IQuestLab/IQuest-Coder-V1-40B-Instruct?blobs=true); [card](https://huggingface.co/IQuestLab/IQuest-Coder-V1-40B-Instruct)

**B. NVIDIA Nemotron**
- **Nemotron 3.5 Super VL** (`nvidia/NVIDIA-Nemotron-3.5-Super-VL-120B-A12B-NVFP4`)
  - Params: 120B / 12B active.
  - Official **NVFP4: 75.01 GB**. BF16 is 248.91 GB.
  - unsloth GGUF: UD-Q4_K_XL 86.91 GB, NVFP4 GGUF 77.69 GB, UD-Q5_K_XL 110.70 GB.
  - Context up to 1M (config 1,048,576). OpenMDW-1.1. HF repo created 2026-10-12.
  - Sources: [HF API NVFP4](https://huggingface.co/api/models/nvidia/NVIDIA-Nemotron-3.5-Super-VL-120B-A12B-NVFP4?blobs=true); [HF API BF16](https://huggingface.co/api/models/nvidia/NVIDIA-Nemotron-3.5-Super-VL-120B-A12B-BF16?blobs=true); [HF API unsloth GGUF](https://huggingface.co/api/models/unsloth/NVIDIA-Nemotron-3.5-Super-VL-120B-A12B-GGUF?blobs=true); [card](https://huggingface.co/nvidia/NVIDIA-Nemotron-3.5-Super-VL-120B-A12B-NVFP4)
- **Nemotron 3.5 Bolt** (`nvidia/NVIDIA-Nemotron-3.5-Bolt-74B-A9B-BF16`)
  - Params: 74B / 9B active. BF16: **149.77 GB (does not fit)**.
  - Use `unsloth/NVIDIA-Nemotron-3.5-Bolt-74B-A9B-GGUF`: Q8_0 78.85 GB, UD-Q6_K_XL 72.39 GB, UD-Q4_K_XL 50.22 GB.
  - NVIDIA's own heavily compressed `-24GB` build is 24.08 GB; its GGUF is 22.02 GB.
  - Context 256K. OpenMDW-1.1. Released 2026-10-07/09.
  - Sources: [HF API BF16](https://huggingface.co/api/models/nvidia/NVIDIA-Nemotron-3.5-Bolt-74B-A9B-BF16?blobs=true); [HF API unsloth](https://huggingface.co/api/models/unsloth/NVIDIA-Nemotron-3.5-Bolt-74B-A9B-GGUF?blobs=true); [HF API 24GB](https://huggingface.co/api/models/nvidia/NVIDIA-Nemotron-3.5-Bolt-74B-A9B-24GB?blobs=true); [card](https://huggingface.co/nvidia/NVIDIA-Nemotron-3.5-Bolt-74B-A9B-BF16)
- **Nemotron 3 Super** (`nvidia/NVIDIA-Nemotron-3-Super-120B-A12B-NVFP4`, superseded)
  - Params: 120B / 12B active. NVFP4: 80.32 GB; FP8: 128.35 GB.
  - Context up to 1M per the card (config default 262,144).
  - **NVIDIA Nemotron Open Model License.** Released 2026-03-10.
  - Sources: [HF API NVFP4](https://huggingface.co/api/models/nvidia/NVIDIA-Nemotron-3-Super-120B-A12B-NVFP4?blobs=true); [HF API FP8](https://huggingface.co/api/models/nvidia/NVIDIA-Nemotron-3-Super-120B-A12B-FP8?blobs=true); [card](https://huggingface.co/nvidia/NVIDIA-Nemotron-3-Super-120B-A12B-NVFP4)
- **Nemotron 3.5 Lightning** (`nvidia/NVIDIA-Nemotron-3.5-Lightning-30B-A3B-NVFP4`)
  - Params: 30B / 3B active. NVFP4: 21.56 GB; BF16: 65.83 GB.
  - Context up to 1M. OpenMDW-1.1. Released Aug 2026.
  - Sources: [HF API NVFP4](https://huggingface.co/api/models/nvidia/NVIDIA-Nemotron-3.5-Lightning-30B-A3B-NVFP4?blobs=true); [card](https://huggingface.co/nvidia/NVIDIA-Nemotron-3.5-Lightning-30B-A3B-BF16)
- **Nemotron Labs Puzzle** (`nvidia/NVIDIA-Nemotron-Labs-3-Puzzle-75B-A9B-NVFP4`)
  - A compressed Nemotron 3 Super: 75.3B total, versus the parent's 120.7B total / 12.8B active.
  - NVFP4: 53.50 GB; FP8: 83.03 GB. OpenMDW-1.1. Released 2026-06-24.
  - Sources: [HF API](https://huggingface.co/api/models/nvidia/NVIDIA-Nemotron-Labs-3-Puzzle-75B-A9B-NVFP4?blobs=true); [card](https://huggingface.co/nvidia/NVIDIA-Nemotron-Labs-3-Puzzle-75B-A9B-BF16)
- **Nemotron-Terminal** (`nvidia/Nemotron-Terminal-32B`)
  - A Qwen3-32B fine-tune. BF16: 65.52 GB. Context 40,960.
  - NVIDIA Open Model License. Released 2026-02-17.
  - Source: [HF API](https://huggingface.co/api/models/nvidia/Nemotron-Terminal-32B?blobs=true)
- **Nemotron-Cascade-2** (`nvidia/Nemotron-Cascade-2-30B-A3B`)
  - BF16: 63.16 GB. Context 262,144. Released 2026-03-18.
  - Source: [HF API](https://huggingface.co/api/models/nvidia/Nemotron-Cascade-2-30B-A3B?blobs=true)

**C. General models that lead coding in this size class (comparators)**
- **Qwen3.8-27B** (`Qwen/Qwen3.8-27B-FP8`)
  - Params: 27.78B dense.
  - Official **FP8: 30.87 GB**; BF16: 55.56 GB. `nvidia/Qwen3.8-27B-NVFP4`: 21.92 GB. unsloth Q8_0: 29.05 GB.
  - Context 262,144 natively, extensible to 1M. Apache-2.0. HF repo created 2026-08-05.
  - Sources: [HF API FP8](https://huggingface.co/api/models/Qwen/Qwen3.8-27B-FP8?blobs=true); [HF API NVFP4](https://huggingface.co/api/models/nvidia/Qwen3.8-27B-NVFP4?blobs=true); [HF API unsloth](https://huggingface.co/api/models/unsloth/Qwen3.8-27B-GGUF?blobs=true); [card](https://huggingface.co/Qwen/Qwen3.8-27B)
- **Qwen3.8-Flash-Next** (base repo `Qwen/Qwen3.8-Flash-Next`)
  - Params: 180.00B total stored; 125B + 51B n-gram embedding + 4B MTP, with 6B active.
  - Official FP8 is 185.52 GB, so it does not fit. Options that fit:
    - **`nvidia/Qwen3.8-Flash-Next-FP4-Local`: 102.26 GB.**
    - `unsloth/Qwen3.8-Flash-Next-GGUF`: UD-IQ4_XS 93.67 GB, UD-Q3_K_XL 89.46 GB, UD-Q4_K_XL 111.33 GB.
    - `ISTA-DASLab/Qwen3.8-Flash-Next-GSQ-RCO-GGUF`: IQ3_S 83.62 GB, IQ3_XXS 75.84 GB.
    - `ISTA-DASLab/Qwen3.8-Flash-Next-GSQ-RCO-Coder-GGUF`: repo total 59.32 GB; the card says 58.4 GB. It is 50%-expert-pruned and calibrated toward code.
  - `nvidia/Qwen3.8-Flash-Next-NVFP4` is 132.68 GB.
  - Context 262,144, extensible to 1M.
  - **Qwen Community License 1.0.** The FP4-Local build is additionally under the NVIDIA open model agreement. HF repo created 2026-08-24.
  - Sources: [HF API FP8](https://huggingface.co/api/models/Qwen/Qwen3.8-Flash-Next-FP8?blobs=true); [HF API FP4-Local](https://huggingface.co/api/models/nvidia/Qwen3.8-Flash-Next-FP4-Local?blobs=true); [HF API NVFP4](https://huggingface.co/api/models/nvidia/Qwen3.8-Flash-Next-NVFP4?blobs=true); [HF API unsloth](https://huggingface.co/api/models/unsloth/Qwen3.8-Flash-Next-GGUF?blobs=true); [HF API ISTA GSQ-RCO](https://huggingface.co/api/models/ISTA-DASLab/Qwen3.8-Flash-Next-GSQ-RCO-GGUF?blobs=true); [HF API ISTA Coder](https://huggingface.co/api/models/ISTA-DASLab/Qwen3.8-Flash-Next-GSQ-RCO-Coder-GGUF?blobs=true); [ISTA Coder card](https://huggingface.co/ISTA-DASLab/Qwen3.8-Flash-Next-GSQ-RCO-Coder-GGUF)
- **Qwen3.6-35B-A3B** (`Qwen/Qwen3.6-35B-A3B-FP8`)
  - Params: 35.95B / 3B active. FP8: 37.46 GB; BF16: 71.90 GB.
  - Context 262,144. Apache-2.0. Released 2026-04-15.
  - Sources: [HF API](https://huggingface.co/api/models/Qwen/Qwen3.6-35B-A3B-FP8?blobs=true); [card](https://huggingface.co/Qwen/Qwen3.6-35B-A3B)
- **Qwen3.6-27B** (`Qwen/Qwen3.6-27B-FP8`)
  - FP8: 30.87 GB. Context 262,144. Apache-2.0. Released 2026-04-21.
  - Source: [HF API](https://huggingface.co/api/models/Qwen/Qwen3.6-27B-FP8?blobs=true)
- **Qwen3.5-122B-A10B** (`Qwen/Qwen3.5-122B-A10B-GPTQ-Int4`)
  - Params: 122B / 10B active.
  - Official **GPTQ-Int4: 78.86 GB**; FP8: 127.16 GB.
  - Context 262,144. Apache-2.0. Released Feb–Mar 2026.
  - Sources: [HF API GPTQ](https://huggingface.co/api/models/Qwen/Qwen3.5-122B-A10B-GPTQ-Int4?blobs=true); [HF API FP8](https://huggingface.co/api/models/Qwen/Qwen3.5-122B-A10B-FP8?blobs=true)
- **Step-3.7-Flash** (`stepfun-ai/Step-3.7-Flash-GGUF`)
  - Params: 198B / about 11B active (201.37B stored).
  - Official FP8 212.52 GB and NVFP4 129.24 GB do not fit.
  - Official GGUF: **IQ4_XS 104.99 GB**, Q3_K_L 102.50 GB, Q3_K_M 93.80 GB. unsloth: UD-IQ4_XS 95.34 GB, UD-Q3_K_XL 89.39 GB.
  - Context 262,144. Apache-2.0. Released 2026-05-23.
  - Sources: [HF API GGUF](https://huggingface.co/api/models/stepfun-ai/Step-3.7-Flash-GGUF?blobs=true); [HF API NVFP4](https://huggingface.co/api/models/stepfun-ai/Step-3.7-Flash-NVFP4?blobs=true); [HF API unsloth](https://huggingface.co/api/models/unsloth/Step-3.7-Flash-GGUF?blobs=true); [card](https://huggingface.co/stepfun-ai/Step-3.7-Flash)
- **MiniMax-M2.7** (`MiniMaxAI/MiniMax-M2.7`)
  - Params: 228.69B; 256 experts, 8 per token. Official FP8 230.13 GB does not fit.
  - `unsloth/MiniMax-M2.7-GGUF`: **UD-Q3_K_XL 101.94 GB**, UD-Q3_K_S 93.64 GB, UD-IQ3_XXS 80.10 GB, UD-IQ4_XS 108.41 GB.
  - Context 204,800. **Non-commercial licence.** Released 2026-04-09.
  - Sources: [HF API](https://huggingface.co/api/models/MiniMaxAI/MiniMax-M2.7?blobs=true); [HF API unsloth](https://huggingface.co/api/models/unsloth/MiniMax-M2.7-GGUF?blobs=true)
- **MiniMax-M2.5** (`unsloth/MiniMax-M2.5-GGUF`)
  - Quants: UD-Q3_K_XL 101.30 GB, Q3_K_M 109.27 GB, UD-IQ3_XXS 93.32 GB. Official FP8 is 230.13 GB.
  - Context 196,608. Modified-MIT. Released 2026-02-12.
  - Sources: [HF API unsloth](https://huggingface.co/api/models/unsloth/MiniMax-M2.5-GGUF?blobs=true); [HF API](https://huggingface.co/api/models/MiniMaxAI/MiniMax-M2.5?blobs=true)
- **GLM-4.7-Flash** (`zai-org/GLM-4.7-Flash`)
  - Params: 31.22B (30B-A3B). BF16: 62.44 GB.
  - Context 202,752. MIT. Released 2026-01-19.
  - Source: [HF API](https://huggingface.co/api/models/zai-org/GLM-4.7-Flash?blobs=true)
- **LongCat-Flash-Lite** (`meituan-longcat/LongCat-Flash-Lite-FP8`)
  - Params: 68.5B / 2.9–4.5B active. FP8: 101.76 GB; BF16: 138.18 GB.
  - Context 327,680. MIT. Released 2026-01/02.
  - Sources: [HF API FP8](https://huggingface.co/api/models/meituan-longcat/LongCat-Flash-Lite-FP8?blobs=true); [card](https://huggingface.co/meituan-longcat/LongCat-Flash-Lite)
- **granite-4.2-30b** (`ibm-granite/granite-4.2-30b`)
  - Params: 29.28B dense. BF16: 58.55 GB.
  - Context 128K native (512K extended). Apache-2.0. Released 2026-08-07.
  - Sources: [HF API](https://huggingface.co/api/models/ibm-granite/granite-4.2-30b?blobs=true); [card](https://huggingface.co/ibm-granite/granite-4.2-30b)
- **Seed-OSS-36B-Instruct** (`ByteDance-Seed/Seed-OSS-36B-Instruct`)
  - Params: 36.15B. BF16: 72.30 GB. Context 524,288. Apache-2.0. Released 2025-08-20.
  - Source: [HF API](https://huggingface.co/api/models/ByteDance-Seed/Seed-OSS-36B-Instruct?blobs=true)
- **Seed-Coder-8B-Instruct** (`ByteDance-Seed/Seed-Coder-8B-Instruct`)
  - BF16: 16.50 GB. Context 32,768. MIT. Released 2025-04.
  - Source: [HF API](https://huggingface.co/api/models/ByteDance-Seed/Seed-Coder-8B-Instruct?blobs=true)

**D. Tool-use specialists**
- **xLAM-2-32b-fc-r** (`Salesforce/xLAM-2-32b-fc-r`)
  - BF16: 65.53 GB. Context 32,768. **CC-BY-NC-4.0 (non-commercial).**
  - Source: [HF API](https://huggingface.co/api/models/Salesforce/xLAM-2-32b-fc-r?blobs=true)
- **Llama-xLAM-2-70b-fc-r** (`Salesforce/Llama-xLAM-2-70b-fc-r`)
  - BF16: 141.11 GB (does not fit). Context 131,072. **CC-BY-NC-4.0.** Released 2025-03.
  - Source: [HF API](https://huggingface.co/api/models/Salesforce/Llama-xLAM-2-70b-fc-r?blobs=true)
- **Hermes-4.3-36B** (`NousResearch/Hermes-4.3-36B`)
  - BF16: 72.30 GB. Context 524,288. Apache-2.0. Released 2025-11-17.
  - Source: [HF API](https://huggingface.co/api/models/NousResearch/Hermes-4.3-36B?blobs=true)
- **Hermes-4-70B-FP8** (`NousResearch/Hermes-4-70B-FP8`)
  - FP8: 72.67 GB. Llama 3 licence.
  - Source: [HF API](https://huggingface.co/api/models/NousResearch/Hermes-4-70B-FP8?blobs=true)

**E. Superseded SWE fine-tunes (sizes for completeness)**
- **Kimi-Dev-72B** (`moonshotai/Kimi-Dev-72B`)
  - BF16: 145.41 GB. Context 131,072. MIT. Released 2025-06-16.
  - unsloth Q8_0: 77.26 GB; Q6_K: 64.35 GB.
  - Sources: [HF API](https://huggingface.co/api/models/moonshotai/Kimi-Dev-72B?blobs=true); [HF API unsloth](https://huggingface.co/api/models/unsloth/Kimi-Dev-72B-GGUF?blobs=true)
- **KAT-Dev-72B-Exp** (`Kwaipilot/KAT-Dev-72B-Exp-FP8`)
  - Official FP8: 75.21 GB. Apache-2.0. Released 2025-10-10.
  - Source: [HF API](https://huggingface.co/api/models/Kwaipilot/KAT-Dev-72B-Exp-FP8?blobs=true)
- **32B Qwen2.5-based SWE agents**, each 65.53 GB in BF16 with a 32,768 context:
  - Skywork-SWE-32B: Apache-2.0
  - openhands-lm-32b-v0.1: MIT
  - SWE-agent-LM-32B: Apache-2.0
  - SWE-Swiss-32B: MIT
  - Sources: [HF API Skywork](https://huggingface.co/api/models/Skywork/Skywork-SWE-32B?blobs=true); [HF API OpenHands LM](https://huggingface.co/api/models/all-hands/openhands-lm-32b-v0.1?blobs=true); [HF API SWE-agent-LM](https://huggingface.co/api/models/SWE-bench/SWE-agent-LM-32B?blobs=true); [HF API SWE-Swiss](https://huggingface.co/api/models/SWE-Swiss/SWE-Swiss-32B?blobs=true)
- **DeepSWE-Preview** ships **F32 weights: 131.05 GB**. Context 40,960. MIT.
  - Source: [HF API DeepSWE](https://huggingface.co/api/models/agentica-org/DeepSWE-Preview?blobs=true)

### Inferences
- **Easy fits with large KV-cache headroom** (official builds of 21–40 GB): Qwen3.8-27B, Qwen3.6-27B/35B-A3B, Devstral Small 2, Nemotron Lightning, and Qwen3-Coder-30B-A3B for legacy use.
- **Comfortable fits** (official builds of 70–85 GB): Qwen3-Coder-Next FP8, Nemotron 3.5 Super VL NVFP4, Mistral Small 4 NVFP4, Qwen3.5-122B GPTQ-Int4, KAT-Coder-V2.5-Dev BF16.
- **Tight fits** (100–110 GB, community GGUF or FP4): Qwen3.8-Flash-Next FP4-Local, Devstral 2 or Mistral Medium 3.5 Q6_K, Step-3.7-Flash IQ4_XS, MiniMax-M2.x UD-Q3_K_XL. These may exceed a 128 GB Mac's default GPU allocation of about 96 GB.
- Qwen3.8-Flash-Next's 51B n-gram embedding can be paged from disk (see Q4), so its resident footprint can be much smaller than its file size. Neither its file size nor its total parameter count says how much memory it needs.

### Gaps
- No measured official FP8/NVFP4 build exists for KAT-Coder-V2.5-Dev, Kimi-Dev-72B or xLAM-2-70b; community quants were not measured.
- The FP8 builds of granite-4.2-30b (`ibm-granite/granite-4.2-30b-fp8`) and Nemotron-Cascade/Terminal were not measured.
- Release dates are HF repo-creation dates unless an announcement is cited. Repos can predate the public launch (Mistral Small 4's repo was created 2026-01-23; it was announced 2026-03-16).

## 3. Scores on SWE-bench Verified / Pro, Terminal-Bench, Aider polyglot, LiveCodeBench and BFCL, with dates and scaffolds

### Takeaway
Most 2026 coding numbers are vendor-reported and use different harnesses (Claude Code, OpenHands, SWE-Agent, mini-SWE-agent, Terminus-2, internal scaffolds) and different benchmark versions (Terminal-Bench 2.0, 2.1 or 4.0; SWE-bench Pro "refined" subsets). Harness alone moves scores by 10–20 pp. The public neutral leaderboards are stale for this generation: the swebench.com bash-only board's last entry is 2026-02-26, Aider's newest entry is 2025-10-03, and BFCL V4 holds only 2025 open models. On the numbers available, Qwen3.8-27B/Flash-Next and Nemotron 3.5 Super VL lead the fitting models, and the larger quantized MiniMax M2.x and DeepSeek-V4-Flash post the highest raw scores.

### Cited Findings
**SWE-bench Verified**
- **Official leaderboard** (swebench.com; Verified board's last entry 2026-02-26). mini-SWE-agent bash-only results for open models:
  - MiniMax M2.5: 75.8% (v2.0.0, 2026-02-17)
  - GLM 5: 72.8%
  - Kimi K2.5: 70.8%
  - MiniMax M2: 61.0% (v1.17.0)
  - Devstral Small (2512): 56.4% (v1.17.2, 2025-12-09)
  - Qwen3-Coder 480B/A35B: 55.4% (v1.0.0)
  - Devstral (2512): 53.8% (v1.17.2)
  - Source: [SWE-bench leaderboards, embedded JSON](https://www.swebench.com/)
- **Vendor-reported figures for models that fit or nearly fit:**

  | Model | SWE-bench Verified | Harness / date | Source |
  |---|---|---|---|
  | Mistral Medium 3.5 | 77.6% | — | [card](https://huggingface.co/mistralai/Mistral-Medium-3.5-128B) |
  | Qwen3.6-27B | 77.2 | internal bash + file-edit scaffold, 200K context, Apr 2026 | [card](https://huggingface.co/Qwen/Qwen3.6-27B) |
  | Qwen3.6-35B-A3B | 73.4 | same | [card](https://huggingface.co/Qwen/Qwen3.6-35B-A3B) |
  | Devstral 2 | 72.2% | — | [card](https://huggingface.co/mistralai/Devstral-2-123B-Instruct-2512) |
  | Qwen3.5-122B-A10B | 72.0 | — | [card](https://huggingface.co/Qwen/Qwen3.5-122B-A10B) |
  | Nemotron 3.5 Super VL | 71.10 BF16 / 71.07 NVFP4 | NVIDIA | [card](https://huggingface.co/nvidia/NVIDIA-Nemotron-3.5-Super-VL-120B-A12B-NVFP4) |
  | Qwen3-Coder-Next | 70.6 / 71.1 / 71.3 | SWE-Agent / MiniSWE / OpenHands | [arXiv](https://arxiv.org/html/2603.00729v1) |
  | KAT-Coder-V2.5-Dev | 69.40 | claude_code@2.1.195 | [card](https://huggingface.co/Kwaipilot/KAT-Coder-V2.5-Dev) |
  | Devstral Small 2 | 68.0% | — | [card](https://huggingface.co/mistralai/Devstral-2-123B-Instruct-2512) |
  | GLM-4.7-Flash | 59.2 | — | [card](https://huggingface.co/zai-org/GLM-4.7-Flash) |
  | granite-4.2-30b | 57 | — | [card](https://huggingface.co/ibm-granite/granite-4.2-30b) |
  | LongCat-Flash-Lite | 54.4 | — | [card](https://huggingface.co/meituan-longcat/LongCat-Flash-Lite) |
  | Nemotron 3.5 Lightning | 51.56 | NVIDIA NeMo Gym harness | [card](https://huggingface.co/nvidia/NVIDIA-Nemotron-3.5-Lightning-30B-A3B-BF16) |

- **MiniMax-M2.5** (FP8 too big; Q3 GGUF fits): 80.2% with Claude Code as scaffold (average of 4 runs), 79.7 on Droid, 76.1 on OpenCode. — [MiniMax-M2.5 card](https://huggingface.co/MiniMaxAI/MiniMax-M2.5)
- **DeepSeek-V4-Flash at Max effort** (April preview): 79.0 Verified, 52.6 Pro, 73.3 Multilingual. — [DeepSeek-V4-Flash card](https://huggingface.co/deepseek-ai/DeepSeek-V4-Flash)
- **Harness variance, same model:** Qwen3.6-35B-A3B scores 73.4 officially, 70.12 in NVIDIA's NeMo Gym harness and 64.40 in Kwaipilot's Claude Code harness. — [Qwen3.6-35B-A3B card](https://huggingface.co/Qwen/Qwen3.6-35B-A3B); [Nemotron 3.5 Lightning card](https://huggingface.co/nvidia/NVIDIA-Nemotron-3.5-Lightning-30B-A3B-BF16); [KAT-Coder-V2.5-Dev card](https://huggingface.co/Kwaipilot/KAT-Coder-V2.5-Dev)
- **Same harness, compressed build:** ISTA-DASLab measured Qwen3.8-Flash-Next BF16 at 82.80 Verified and its 58.4 GB GSQ-RCO-Coder build at 79.20 (updated 2026-10-17). — [ISTA Coder card](https://huggingface.co/ISTA-DASLab/Qwen3.8-Flash-Next-GSQ-RCO-Coder-GGUF)

**SWE-bench Pro**
- **Vendor-reported:**

  | Model | SWE-bench Pro | Harness / note | Source |
  |---|---|---|---|
  | Qwen3.8-Flash-Next | 62.5 | — | [card](https://huggingface.co/Qwen/Qwen3.8-Flash-Next) |
  | Qwen3.8-27B | 61.7 | Claude Code, temp 1.0, 256K, Qwen-corrected task set | [card](https://huggingface.co/Qwen/Qwen3.8-27B) |
  | Step-3.7-Flash | 56.3 | — | [card](https://huggingface.co/stepfun-ai/Step-3.7-Flash) |
  | MiniMax-M2.7 | 56.22 | — | [card](https://huggingface.co/MiniMaxAI/MiniMax-M2.7) |
  | Qwen3.6-27B | 53.5 | — | [card](https://huggingface.co/Qwen/Qwen3.6-27B) |
  | Nemotron 3.5 Super VL | 51.78 / 51.30 NVFP4 | NVIDIA | [card](https://huggingface.co/nvidia/NVIDIA-Nemotron-3.5-Super-VL-120B-A12B-NVFP4) |
  | KAT-Coder-V2.5-Dev | 45.96 | Kwaipilot Claude Code | [card](https://huggingface.co/Kwaipilot/KAT-Coder-V2.5-Dev) |
  | Qwen3-Coder-Next | 42.7 | SWE-Agent | [arXiv](https://arxiv.org/html/2603.00729v1) |
  | granite-4.2-30b | 33.29 | — | [card](https://huggingface.co/ibm-granite/granite-4.2-30b) |

- **Locally on 2× DGX Spark** (40-instance enterprise subset, same harness):
  - Qwen3.8-Flash-Next FP8 or NVFP4: 36/40.
  - Qwen3.8-Flash-Next, NVIDIA NVFP4 checkpoint, 1M YaRN context: 38/40.
  - GLM-5.3-Flash NVFP4: 36/40.
  - DeepSeek-V4-Flash-0731 MXFP4: 33/40. A first run scored 22/40 because the vLLM tokenizer defaulted thinking to off.
  - Source: [NVIDIA forum, 2026-09-08](https://forums.developer.nvidia.com/t/qwen3-8-flash-next-glm-5-3-flash-deepseek-v4-flash-on-2x-dgx-spark-prefill-ttft-swe-bench-pro/382697)

**Terminal-Bench (versions are not comparable)**
- **TB 2.1:**

  | Model | Terminal-Bench 2.1 | Harness / note | Source |
  |---|---|---|---|
  | Qwen3.8-27B | 73.0 | Terminus | [card](https://huggingface.co/Qwen/Qwen3.8-27B) |
  | Nemotron 3.5 Super VL | 62.78 NVFP4 / 60.67 BF16 | NVIDIA | [card](https://huggingface.co/nvidia/NVIDIA-Nemotron-3.5-Super-VL-120B-A12B-NVFP4) |
  | Step-3.7-Flash | 59.5 | — | [card](https://huggingface.co/stepfun-ai/Step-3.7-Flash) |
  | Nemotron 3.5 Bolt | 53.93 BF16 / 50.98 24GB build | NVIDIA | [card](https://huggingface.co/nvidia/NVIDIA-Nemotron-3.5-Bolt-74B-A9B-BF16) |
  | KAT-Coder-V2.5-Dev | 41.02 | average of Terminus-2 32.60 and Claude Code 49.44 | [card](https://huggingface.co/Kwaipilot/KAT-Coder-V2.5-Dev) |
  | granite-4.2-30b | 29.24 | — | [card](https://huggingface.co/ibm-granite/granite-4.2-30b) |

  The Nemotron 3.5 Bolt card also lists Qwen3.6-35B-A3B at 44.38 in NVIDIA's measurement.
- **Larger models on TB 2.1:**
  - DeepSeek-V4-Flash-0731: 82.7 (DeepSeek Harness minimal mode, max effort) — [card](https://huggingface.co/deepseek-ai/DeepSeek-V4-Flash-0731)
  - MiMo-V2.6-Flash: 87.6 — [card](https://huggingface.co/XiaomiMiMo/MiMo-V2.6-Flash-RL)
- **TB 2.0:**
  - Qwen3.6-27B 59.3 and 35B-A3B 51.5 (Harbor/Terminus-2, average of 5 runs) — [Qwen3.6-27B card](https://huggingface.co/Qwen/Qwen3.6-27B)
  - MiniMax-M2.7: 57.0 — [card](https://huggingface.co/MiniMaxAI/MiniMax-M2.7)
  - DeepSeek-V4-Flash Max: 56.9 — [card](https://huggingface.co/deepseek-ai/DeepSeek-V4-Flash)
  - Qwen3.5-122B-A10B: 49.4 — [card](https://huggingface.co/Qwen/Qwen3.5-122B-A10B)
  - Qwen3-Coder-Next: 36.2 (Terminus2-json), 34.2 (xml), 30.9 (Claude Code), 25.8 (Qwen Code) — [arXiv](https://arxiv.org/html/2603.00729v1)
  - Devstral 2: 32.6; Devstral Small 2: 22.5 — [card](https://huggingface.co/mistralai/Devstral-2-123B-Instruct-2512)
  - Nemotron-Terminal-32B: 27.4 — [card](https://huggingface.co/nvidia/Nemotron-Terminal-32B)
- **Official tbench.ai leaderboard:** TB 2.1 has a leaderboard at tbench.ai (2.1 released May 2026), and search results report community submissions as closed. I could not extract open-model rows from it. — [tbench.ai TB 2.1 leaderboard](https://tbench.ai/leaderboard/terminal-bench/2.1); [TB 2.1 release note](https://tbench.ai/news/terminal-bench-2-1)
- **Newest cards use other benchmarks.** MiniMax-M3.1-Flash and DeepSeek-V4.1-Flash-1017 report "Terminal Bench 4.0", DeepSWE v1.1 and NL2Repo instead of SWE-bench Verified. — [MiniMax-M3.1-Flash card](https://huggingface.co/MiniMaxAI/MiniMax-M3.1-Flash); [DeepSeek-V4.1-Flash-1017 card](https://huggingface.co/deepseek-ai/DeepSeek-V4.1-Flash-1017)

**Aider polyglot**
- The official leaderboard was last updated 2025-11-20. Its newest open-model entry is DeepSeek-V3.2-Exp (74.2%, 2025-10-03). It has no GLM, MiniMax, Devstral, Qwen3.5+ or Qwen3-Coder-Next entries. — [aider.chat leaderboards](https://aider.chat/docs/leaderboards/)
- The only 2026 self-report found: Qwen3-Coder-Next Aider-Polyglot 66.20, vs 60.40 for Qwen3-Coder-480B in the same table. — [arXiv 2603.00729](https://arxiv.org/html/2603.00729v1)

**LiveCodeBench v6** (vendor-reported)
- 90.3 — Qwen3.8-27B — [card](https://huggingface.co/Qwen/Qwen3.8-27B)
- 91.9 — Qwen3.8-Flash-Next — [card](https://huggingface.co/Qwen/Qwen3.8-Flash-Next)
- 88.22 BF16 / 87.41 NVFP4 — Nemotron 3.5 Super VL — [card](https://huggingface.co/nvidia/NVIDIA-Nemotron-3.5-Super-VL-120B-A12B-NVFP4)
- 87.2 — Nemotron-Cascade-2-30B-A3B — [card](https://huggingface.co/nvidia/Nemotron-Cascade-2-30B-A3B)
- 83.9 — Qwen3.6-27B; 80.4 — Qwen3.6-35B-A3B — [card](https://huggingface.co/Qwen/Qwen3.6-27B)
- 81.1 — IQuest-Coder-V1-40B — [card](https://huggingface.co/IQuestLab/IQuest-Coder-V1-40B-Instruct)
- 78.69 — Nemotron 3 Super — [card](https://huggingface.co/nvidia/NVIDIA-Nemotron-3-Super-120B-A12B-NVFP4)
- 75.77 — granite-4.2-30b — [card](https://huggingface.co/ibm-granite/granite-4.2-30b)
- 67.87 — NousCoder-14B — [card](https://huggingface.co/NousResearch/NousCoder-14B)
- 58.93 — Qwen3-Coder-Next — [arXiv](https://arxiv.org/html/2603.00729v1)

**BFCL (function calling)**
- **The official BFCL V4 board is stale for 2026 models.** The page says "last updated 2026-04-12", but its CSV's open entries end with GLM-4.6 (72.38%), Kimi-K2-Instruct (59.06%) and DeepSeek-V3.2-Exp (56.73%).
- **Open tool-use specialists on the official board:**
  - xLAM-2-32b-fc-r: 54.66% (#18)
  - xLAM-2-70b-fc-r: 53.07% (#22). Both CC-BY-NC-4.0.
  - Qwen3-32B (FC): 48.71%
  - Arch-Agent-32B: 45.37% (katanemo-research licence)
  - ToolACE-2-8B: 42.44%
  - Qwen3-30B-A3B-Instruct-2507 (FC): 41.39%
  - No Qwen3.5/3.6/3.8, Granite 4.x, Hermes 4 or Nemotron 3.x rows.
  - Sources: [BFCL leaderboard](https://gorilla.cs.berkeley.edu/leaderboard.html); [BFCL data_overall.csv](https://gorilla.cs.berkeley.edu/data_overall.csv)
- **Vendor-reported BFCL-V4** (not run by Berkeley):
  - Qwen3.5-122B-A10B 72.2, Qwen3.5-27B 68.5, Qwen3.5-35B-A3B 67.3 — [Qwen3.5-122B-A10B card](https://huggingface.co/Qwen/Qwen3.5-122B-A10B)
  - granite-4.2-30b: 61.39 — [card](https://huggingface.co/ibm-granite/granite-4.2-30b)
  - Nemotron-3-Nano 53.8; Nemotron-Cascade-2 52.9 — [Cascade-2 card](https://huggingface.co/nvidia/Nemotron-Cascade-2-30B-A3B)
- **Other tool-use evidence:**
  - xLAM-2-70b-fc-r: 56.2% on τ-bench; its card cites BFCL v3 as of 2025-04-02 — [xLAM-2-70b card](https://huggingface.co/Salesforce/Llama-xLAM-2-70b-fc-r)
  - Qwen3.6-35B-A3B: MCPMark 37.0, MCP-Atlas 62.8, TAU3 67.2 — [card](https://huggingface.co/Qwen/Qwen3.6-35B-A3B)
  - Qwen3.8-Flash-Next: Toolathlon Verified 73.5 — [card](https://huggingface.co/Qwen/Qwen3.8-Flash-Next)

### Inferences
- The industry is moving off SWE-bench Verified for headline claims. Qwen3.8 cards report SWE-bench Pro, DeepSWE and NL2Repo rather than Verified, and the newest (Oct 2026) MiniMax-M3.1-Flash and DeepSeek-V4.1-Flash-1017 cards report Terminal Bench 4.0. Expect Verified numbers to be absent or saturated for the newest models.
- Treat any cross-vendor comparison as ±10 pp. Same-harness evidence (NVIDIA's NeMo Gym table, Kwaipilot's table, mini-SWE-agent's board) consistently puts Devstral 2/Small 2 and Qwen3.6-35B-A3B below their self-reports.
- For function calling, the non-commercial xLAM-2 models were the last true BFCL specialists. Newer general models self-report much higher BFCL-V4 but have not been independently placed on Berkeley's board.

### Gaps
- No independent 2026 runs exist on SWE-bench Verified/Pro or Aider for Qwen3.8, KAT-Coder-V2.5-Dev, Nemotron 3.5, Step-3.7-Flash or Mistral Small 4 / Medium 3.5.
- No BFCL numbers were found for Hermes 4.3, KAT-Coder, Devstral 2 or Qwen3.6/3.8.
- No Aider numbers were found for any 2026 model except Qwen3-Coder-Next's self-report.
- The open-model rows of tbench.ai could not be extracted.

## 4. Real-world reports of running them locally on 128 GB machines (speed, quant quality)

### Takeaway
Measured reports (NVIDIA DGX Spark forums, Framework/Strix Halo forum, Japanese engineering blogs) show the following:
- 3–12B-active MoEs run at about 20–45 tok/s single-stream on a 128 GB DGX Spark or Strix Halo: Qwen3-Coder-Next FP8 about 43, Mistral Small 4 NVFP4 about 29, Nemotron 3 Super NVFP4 about 23, Qwen3.8-Flash-Next about 17–43 depending on quant, engine and MTP.
- Dense 27B models give about 8–15 tok/s stock and 20–35 with MTP or speculative decoding.
- Dense 123–128B models (Devstral 2, Mistral Medium 3.5) crawl at about 2 tok/s, or 7–9 with EAGLE.
- Vendors' FP4 builds showed little loss in their own tests; independent 3–4-bit quality data is thin.

### Cited Findings
**Qwen3-Coder-Next (FP8, single DGX Spark, vLLM; eugr, 2026-02-03)**
- tg128 ≈ 42.7–44.6 tok/s, falling to about 37–38 tok/s at 32K depth; prefill about 3,000–3,800 tok/s.
- About 92 GB used at 0.8 GPU memory utilization; about 60K context with FLASH_ATTN, while the author claims about 170K with FlashInfer.
- Source: [NVIDIA forum HOW-TO](https://forums.developer.nvidia.com/t/how-to-run-qwen3-coder-next-on-spark/359571)

**Qwen3.8-Flash-Next**
- **Single Spark** (classmethod, 2026-09-06; NVFP4 RadixArk build, vLLM with an n-gram/PLE mmap patch):
  - Resident memory 79.4 GiB with about 646K tokens of KV.
  - Decode 17.51 tok/s without speculation, 28.01 with MTP (k=2); 8 streams 92–103 tok/s.
  - TTFT 63 s at 131K.
  - It **accepted `tool_choice=required` but returned zero tool calls**, so the author recommends Qwen3.8-27B or GLM 3-bit for agent use.
  - Source: [classmethod](https://dev.classmethod.jp/en/articles/dgx-spark-single-node-flash-next-vs-glm-3bit-vs-27b/)
- **2× Spark TP=2** (2026-09-08):
  - NVFP4 29.6 tok/s single-stream; FP8 23.0.
  - Prefill 2,591 tok/s at 131K; max verified context 245K.
  - Source: [NVIDIA forum](https://forums.developer.nvidia.com/t/qwen3-8-flash-next-glm-5-3-flash-deepseek-v4-flash-on-2x-dgx-spark-prefill-ttft-swe-bench-pro/382697)
- **NVIDIA's FP4-Local card:** DGX Spark runs it "with or without PLE offload", with "1M tokens of FP8 KV Cache with PLE offload". It needs community vLLM containers until upstream support lands. — [FP4-Local card](https://huggingface.co/nvidia/Qwen3.8-Flash-Next-FP4-Local)
- **Strix Halo, llama.cpp** (Framework community, Aug–Oct 2026):
  - unsloth UD-IQ4_XS: about 16 tok/s at 32K context and about 21 at 16K (Martin_Roth); pp 405 / tg 27.5 (Guest209); 36–43 tok/s with a 96 GB VRAM carve-out on the "Strata" engine (Frank_Fujita).
  - A Q3 quant (83.80 GiB) under Vulkan at 140 W: pp512 414 / tg 27.9.
  - "Q4 was the largest Flash-Next quant that fit on Strix Halo alone."
  - Source: [Framework forum](https://community.frame.work/t/qwen3-8-please-share-your-t-s-any-quant/84405)
- **llmconfigurator** rates Qwen3.8-Flash-Next Q4_K_M (120 GB) as "fits, tight" on DGX Spark and the Ryzen AI Max+ 395, and says nobody has benchmarked the 4-bit builds users actually run. — [llmconfigurator report, 2026-10-08](https://llmconfigurator.com/en/reports/best-local-coding-models-by-hardware)

**Qwen3.8-27B**
- **Single Spark, NVFP4 (21.9 GB):** 12.37 tok/s without spec, 20.79 with MTP; 8-stream total 135.98 tok/s; about 1.27M tokens of KV room. — [classmethod](https://dev.classmethod.jp/en/articles/dgx-spark-single-node-flash-next-vs-glm-3bit-vs-27b/)
- **Stock llama.cpp** (Hardware Corner measurements as cited): 12.2 tok/s on Spark, 8–11 on Strix Halo. Community-tuned with MTP or DFlash: 33–35 and 24–36. — [llmconfigurator](https://llmconfigurator.com/en/reports/best-local-coding-models-by-hardware)
- **Strix Halo Q8_0:** about 7.6–14.8 tok/s across users. — [Framework forum](https://community.frame.work/t/qwen3-8-please-share-your-t-s-any-quant/84405)

**Mistral Small 4 (official NVFP4, single Spark)**
- **vLLM 0.21** (0rand, 2026-05-20): 28.76 tok/s at c=1, 74.44 at c=4; pp512 1,365 tok/s; about 75 GB of weights. "Tools calls and reasoning excellent" in an open-source coding agent.
- **SGLang with EAGLE:** 26.8–27.7 tok/s.
- Source: [NVIDIA forum](https://forums.developer.nvidia.com/t/running-mistral-small-4-119b-moe-on-dgx-spark-with-sglang-full-setup-benchmarks/364763)

**Mistral Medium 3.5 128B (dense; community NVFP4 `zdy1995love/Mistral-Medium-3.5-128B-NVFP4`, single Spark, 2026-05-04)**
- About 2 tok/s without EAGLE on llama.cpp; 6.9–9.1 tok/s with EAGLE in vLLM.
- 72.91 GiB of weights; prefill about 190–300 tok/s.
- Source: [NVIDIA forum](https://forums.developer.nvidia.com/t/performance-report-mistral-medium-3-5-128b-nvfp4-eagle/368917)

**Devstral 2 123B**
- Only calculator estimates were found, and they conflict: "~12 tok/s (est.)" vs "roughly 3 tok/s (estimated)" on DGX Spark at Q4_K_M, and about 6–8 tok/s on 128 GB Macs. — [llmconfigurator Devstral-2 on Spark](https://llmconfigurator.com/en/can-i-run/devstral-2-123b-on-nvidia-dgx-spark); [willitrunai M4 Max](https://willitrunai.com/zh/can-run/devstral-2-123b-on-m4-max-128gb)

**Nemotron 3 Super NVFP4 (single Spark)**
- A thread title reports 23.45 tok/s (spark-arena/llama-benchy). Search summaries cite 16.6–16.7 tok/s on Marlin dequant paths. — [NVIDIA forum thread](https://forums.developer.nvidia.com/t/nemotron-3-super-120b-a12b-nvfp4-on-single-dgx-spark-23-45-tok-s-spark-arena-com-benhmarks/370070)

**DeepSeek-V4-Flash-0731 (just-miss model, see Q5)**
- **2× Spark NVFP4:** 41.0 tok/s with DSpark speculation, but the slowest long-context prefill (about 1,134 tok/s at 131K) and about 5.7 h for the 40-task SWE-bench Pro run vs about 2 h for Qwen. — [NVIDIA forum](https://forums.developer.nvidia.com/t/qwen3-8-flash-next-glm-5-3-flash-deepseek-v4-flash-on-2x-dgx-spark-prefill-ttft-swe-bench-pro/382697)
- **Single Spark:** a REAP-pruned 3.0 bpw EXL3 build (106.82 GB) is "validated on one physical NVIDIA DGX Spark" through a pinned SparkInfer Docker image, with a 262,144-token limit. — [0xSero spark build card](https://huggingface.co/0xSero/deepseek-v4-flash-0731-spark); [HF API](https://huggingface.co/api/models/0xSero/deepseek-v4-flash-0731-spark?blobs=true)

**GLM-5.3-Flash UD-IQ3_XXS (120.4 GB, single Spark)**
- 15.19 tok/s with only 3 GiB free memory; needed `--cache-ram 0 --ctx-checkpoints 0`.
- TTFT 197.8 s at 32K. The author suggests 2-bit over 3-bit.
- Source: [classmethod](https://dev.classmethod.jp/en/articles/dgx-spark-single-node-flash-next-vs-glm-3bit-vs-27b/)

**MiniMax Q3_K_XL on a 128 GB Strix Halo (Bosgame M5)**
- Reported at about 30 tok/s generation, but the source does not say which MiniMax version. Aggregator article, low confidence. — [ai-radar.it](https://ai-radar.it/article/strix-halo-prestazioni-sorprendenti-con-minimax-q3-k-xl)

**Quantization-quality evidence**
- **Nemotron 3.5 Super VL NVFP4 vs BF16:** SWE-bench Verified 71.07 vs 71.10 and TB 2.1 62.78 vs 60.67, but DeepSWE [offline] 18.2 vs 24.65. — [card](https://huggingface.co/nvidia/NVIDIA-Nemotron-3.5-Super-VL-120B-A12B-NVFP4)
- **Nemotron 3 Super:** LiveCodeBench v6 78.44 for both NVFP4 and FP8, vs 78.69 for BF16. — [card](https://huggingface.co/nvidia/NVIDIA-Nemotron-3-Super-120B-A12B-NVFP4)
- **Nemotron 3.5 Bolt:** the IQ2-class "24GB" build scores TB 2.1 50.98 vs 53.93 for BF16. — [card](https://huggingface.co/nvidia/NVIDIA-Nemotron-3.5-Bolt-74B-A9B-BF16)
- **Qwen3.8-Flash-Next FP4-Local:** GPQA 91.5 vs 92.2, ToolEvalBench 88.7 vs 89.4. — [card](https://huggingface.co/nvidia/Qwen3.8-Flash-Next-FP4-Local)
- **ISTA GSQ-RCO-Coder** (50% experts pruned, 3.5 bpw) keeps 95.7% of SWE-bench Verified (79.20 vs 82.80). Only about 29.6 GB must be resident, because the n-gram shard can be served from disk. — [ISTA Coder card](https://huggingface.co/ISTA-DASLab/Qwen3.8-Flash-Next-GSQ-RCO-Coder-GGUF)
- **Ollama on DGX Spark:** one blog reported q4 vs q8 differences for qwen3-coder-next as "nearly invisible" (36.1 tok/s at q8_0 vs 47.0 at q4_K_M). This comes via a search summary and was not read directly. — [ai-muninn DGX Spark part 1](https://ai-muninn.com/en/blog/dgx-spark-ollama-benchmark-8-models)

### Inferences
- **Decode speed tracks active parameters.**
  - For interactive agentic coding on 128 GB, 3–12B-active MoEs (Qwen3-Coder-Next, Mistral Small 4, Nemotron 3.5 Super VL, Qwen3.8-Flash-Next, KAT-Coder/Qwen3.6-35B-A3B) are the practical sweet spot.
  - Dense 120B+ (Devstral 2, Medium 3.5) are usable only for batch or overnight work unless speculative decoding is used.
  - Dense 27B sits in between and is fast mainly with MTP.
- **Prefill differs sharply between platforms:** vLLM on Spark measured about 2,000–3,800 tok/s for Qwen MoEs, against about 260–415 tok/s pp512 in the Strix Halo llama.cpp reports. Long agent contexts (100K+) therefore cost minutes of TTFT on Strix, and this is measured across different setups, not same-model controlled.
- **A tight build risks failure modes**, as seen with GLM 3-bit's 3 GiB free memory and Flash-Next's tool_choice bug. Use the vendor FP4/NVFP4 builds that come with validated recipes when they exist.

### Gaps
- No measured local reports were found for KAT-Coder-V2.5-Dev, Step-3.7-Flash on 128 GB, Nemotron 3.5 Super VL or Bolt on a single Spark, MiniMax-M2.7 at Q3 on a verified setup, or Devstral 2 (measured rather than estimated).
- No Apple M-series 128 GB measurements were found for any of these 2026 models; the Mac figures found were estimates.
- Independent agentic-quality evaluations of 3–4-bit GGUFs (vs the vendor FP4 builds) are largely missing, as llmconfigurator itself notes.

## 5. Which notable coding models just miss (about 110–170 GB), and what quant would make them fit?

### Takeaway
The notable near-misses are Qwen3.8-Flash-Next, DeepSeek-V4-Flash-0731, Mistral Medium 3.5 and Devstral 2, Step-3.7-Flash, Qwen3-Coder-Next BF16, Nemotron 3.5 Bolt, Mistral Small 4 FP8, Qwen3.5-122B FP8, Nemotron 3 Super FP8, Kimi-Dev or KAT-Dev-72B BF16, LongCat-Flash-Lite BF16 and GLM-4.5-Air FP8. Each has a measured ≤110 GB build:
- **Vendor FP4 builds**, the safest choice: Qwen3.8-Flash-Next FP4-Local 102.26 GB, Mistral Small 4 NVFP4 70.81 GB, Nemotron NVFP4 75–80 GB, Qwen3.5 GPTQ-Int4 78.86 GB.
- **Q5/Q6 GGUF** for the dense Mistrals: 88–103 GB.
- **IQ4_XS/Q3 GGUF** for the 200–300B MoEs: about 90–105 GB.

MiMo-V2.6-Flash and GLM-5.3-Flash only fit at 2-bit or not at all.

### Cited Findings
- **Qwen3.8-Flash-Next:** FP8 185.52 GB; NVIDIA NVFP4 132.68 GB.
  - Fits: **NVIDIA FP4-Local 102.26 GB** (QAD; DGX Spark validated), unsloth UD-IQ4_XS 93.67 GB, UD-Q3_K_XL 89.46 GB, ISTA GSQ-RCO IQ3_S 83.62 GB, GSQ-RCO-Coder about 59 GB (expert-pruned).
  - Sources: [HF API FP8](https://huggingface.co/api/models/Qwen/Qwen3.8-Flash-Next-FP8?blobs=true); [HF API FP4-Local](https://huggingface.co/api/models/nvidia/Qwen3.8-Flash-Next-FP4-Local?blobs=true); [HF API unsloth](https://huggingface.co/api/models/unsloth/Qwen3.8-Flash-Next-GGUF?blobs=true); [HF API ISTA](https://huggingface.co/api/models/ISTA-DASLab/Qwen3.8-Flash-Next-GSQ-RCO-GGUF?blobs=true)
- **DeepSeek-V4-Flash-0731** (284B/13B active per the V4-Flash card, 1M context, MIT; scores in Q3): official mixed FP4/FP8 weights 166.89 GB.
  - Fits: unsloth **UD-IQ3_XXS 104.21 GB** or UD-Q2_K_XL 96.83 GB; 0xSero REAP-pruned EXL3 for a single Spark 106.82 GB. UD-IQ3_S at 116.07 GB is over budget.
  - Sources: [HF API official](https://huggingface.co/api/models/deepseek-ai/DeepSeek-V4-Flash-0731?blobs=true); [HF API unsloth](https://huggingface.co/api/models/unsloth/DeepSeek-V4-Flash-0731-GGUF?blobs=true); [HF API 0xSero](https://huggingface.co/api/models/0xSero/deepseek-v4-flash-0731-spark?blobs=true); [V4-Flash card](https://huggingface.co/deepseek-ai/DeepSeek-V4-Flash)
- **Mistral Medium 3.5 128B dense:** FP8 133.61 GB.
  - Fits: unsloth **Q6_K 102.58 GB**, Q5_K_M 88.32 GB, Q4_K_M 74.90 GB; a community NVFP4 loads as 72.91 GiB.
  - Sources: [HF API](https://huggingface.co/api/models/mistralai/Mistral-Medium-3.5-128B?blobs=true); [HF API unsloth](https://huggingface.co/api/models/unsloth/Mistral-Medium-3.5-128B-GGUF?blobs=true); [NVIDIA forum](https://forums.developer.nvidia.com/t/performance-report-mistral-medium-3-5-128b-nvfp4-eagle/368917)
- **Devstral 2 123B dense:** FP8 128.25 GB.
  - Fits: unsloth **Q6_K 102.58 GB**, UD-Q5_K_XL 88.25 GB, Q4_K_M 74.90 GB.
  - Source: [HF API unsloth](https://huggingface.co/api/models/unsloth/Devstral-2-123B-Instruct-2512-GGUF?blobs=true)
- **Step-3.7-Flash:** NVFP4 129.24 GB; FP8 212.52 GB.
  - Fits: official **IQ4_XS 104.99 GB** or Q3_K_M 93.80 GB; unsloth UD-IQ4_XS 95.34 GB.
  - Sources: [HF API NVFP4](https://huggingface.co/api/models/stepfun-ai/Step-3.7-Flash-NVFP4?blobs=true); [HF API GGUF](https://huggingface.co/api/models/stepfun-ai/Step-3.7-Flash-GGUF?blobs=true); [HF API unsloth](https://huggingface.co/api/models/unsloth/Step-3.7-Flash-GGUF?blobs=true)
- **Qwen3-Coder-Next:** BF16 159.36 GB.
  - Fits: **official FP8 80.38 GB** or official Q8_0 GGUF 84.81 GB.
  - Sources: [HF API BF16](https://huggingface.co/api/models/Qwen/Qwen3-Coder-Next?blobs=true); [HF API FP8](https://huggingface.co/api/models/Qwen/Qwen3-Coder-Next-FP8?blobs=true)
- **Nemotron 3.5 Bolt 74B-A9B:** BF16 149.77 GB.
  - Fits: unsloth **Q8_0 78.85 GB** or UD-Q6_K_XL 72.39 GB.
  - Source: [HF API unsloth](https://huggingface.co/api/models/unsloth/NVIDIA-Nemotron-3.5-Bolt-74B-A9B-GGUF?blobs=true)
- **Mistral Small 4:** FP8 120.92 GB.
  - Fits: **official NVFP4 70.81 GB** or unsloth UD-Q6_K 99.43 GB.
  - Sources: [HF API NVFP4](https://huggingface.co/api/models/mistralai/Mistral-Small-4-119B-2603-NVFP4?blobs=true); [HF API unsloth](https://huggingface.co/api/models/unsloth/Mistral-Small-4-119B-2603-GGUF?blobs=true)
- **Qwen3.5-122B-A10B:** FP8 127.16 GB.
  - Fits: **official GPTQ-Int4 78.86 GB**.
  - Source: [HF API GPTQ](https://huggingface.co/api/models/Qwen/Qwen3.5-122B-A10B-GPTQ-Int4?blobs=true)
- **Nemotron 3 Super:** FP8 128.35 GB.
  - Fits: **official NVFP4 80.32 GB**. Nemotron 3.5 Super VL NVFP4 is 75.01 GB.
  - Sources: [HF API FP8](https://huggingface.co/api/models/nvidia/NVIDIA-Nemotron-3-Super-120B-A12B-FP8?blobs=true); [HF API 3.5 Super VL NVFP4](https://huggingface.co/api/models/nvidia/NVIDIA-Nemotron-3.5-Super-VL-120B-A12B-NVFP4?blobs=true)
- **Kimi-Dev-72B / KAT-Dev-72B-Exp:** BF16 145.41 GB each.
  - Fits: unsloth Kimi-Dev Q8_0 77.26 GB; **official KAT-Dev-72B-Exp-FP8 75.21 GB**. Both models are superseded.
  - Sources: [HF API unsloth Kimi-Dev](https://huggingface.co/api/models/unsloth/Kimi-Dev-72B-GGUF?blobs=true); [HF API KAT FP8](https://huggingface.co/api/models/Kwaipilot/KAT-Dev-72B-Exp-FP8?blobs=true)
- **LongCat-Flash-Lite:** BF16 138.18 GB.
  - Fits: **official FP8 101.76 GB**.
  - Source: [HF API FP8](https://huggingface.co/api/models/meituan-longcat/LongCat-Flash-Lite-FP8?blobs=true)
- **GLM-4.5-Air** (Jul 2025, superseded): FP8 112.56 GB.
  - Fits: unsloth Q6_K 99.01 GB; cerebras REAP-82B-A12B-FP8 83.26 GB.
  - Sources: [HF API FP8](https://huggingface.co/api/models/zai-org/GLM-4.5-Air-FP8?blobs=true); [HF API unsloth](https://huggingface.co/api/models/unsloth/GLM-4.5-Air-GGUF?blobs=true); [HF API REAP FP8](https://huggingface.co/api/models/cerebras/GLM-4.5-Air-REAP-82B-A12B-FP8?blobs=true)
- **MiniMax-M2.5 / M2.7** (230B class; FP8 230.13 GB, above the band but notable):
  - Fits: unsloth **UD-Q3_K_XL about 101–102 GB** or UD-IQ3_XXS 80–93 GB.
  - Cerebras' REAP-139B-A10B FP8 pruning of M2.5 is still 140.43 GB.
  - Sources: [HF API unsloth M2.7](https://huggingface.co/api/models/unsloth/MiniMax-M2.7-GGUF?blobs=true); [HF API unsloth M2.5](https://huggingface.co/api/models/unsloth/MiniMax-M2.5-GGUF?blobs=true); [HF API REAP-139B](https://huggingface.co/api/models/cerebras/MiniMax-M2.5-REAP-139B-A10B?blobs=true)
- **Ling-3.2-air** (inclusionAI, general model, MIT, created 2026-11-23): FP8 129.28 GB. No official smaller build was measured.
  - Source: [HF API](https://huggingface.co/api/models/inclusionAI/Ling-3.2-air?blobs=true)
- **MiMo-V2.6-Flash-RL** (309B/15B; TB 2.1 87.6): official weights 172.93 GB.
  - ggml-org GGUF: MXFP4 167.37 GB and **Q2_K 126.37 GB, so even 2-bit does not fit**.
  - Sources: [HF API](https://huggingface.co/api/models/XiaomiMiMo/MiMo-V2.6-Flash-RL?blobs=true); [HF API ggml-org GGUF](https://huggingface.co/api/models/ggml-org/MiMo-V2.6-Flash-RL-GGUF?blobs=true)
- **GLM-5.3-Flash** (320B/18B): NVIDIA FP4-Local 177.52 GB.
  - unsloth: UD-IQ3_XXS 120.37 GB (over budget), UD-Q2_K_XL 108.72 GB, UD-IQ2_XXS 101.84 GB.
  - Sources: [HF API FP4-Local](https://huggingface.co/api/models/nvidia/GLM-5.3-Flash-FP4-Local?blobs=true); [HF API unsloth](https://huggingface.co/api/models/unsloth/GLM-5.3-Flash-GGUF?blobs=true)
- **MiniMax-M3.1-Flash** (about 428B/23B): the smallest unsloth GGUF, UD-IQ1_M, is 129.36 GB, so it **does not fit**. — [HF API unsloth](https://huggingface.co/api/models/unsloth/MiniMax-M3.1-Flash-GGUF?blobs=true)

### Inferences
- **Best "just-miss" upgrades, in order:**
  1. Qwen3.8-Flash-Next via NVIDIA FP4-Local. QAD-trained, about 1 pt loss on NVIDIA's evals, a Spark recipe exists. Check the licence.
  2. DeepSeek-V4-Flash-0731 at UD-IQ3_XXS or the 0xSero Spark build. Strong TB 2.1/DeepSWE self-reports, but 3-bit quality is unverified and it fills nearly all memory.
  3. Mistral Medium 3.5 at Q5/Q6 if a dense Western-lab model is preferred. Slow; $20M-revenue licence cap.
- **Prefer vendor FP4 over community quants.** Vendor FP4 builds (NVFP4/FP4-Local/GPTQ) report minimal benchmark loss, while community 2–3-bit GGUFs of 200–300B MoEs carry undocumented agentic-quality risk. That matters most for long-horizon SWE tasks; ISTA's own data shows SWE-bench drops more than LiveCodeBench under compression.

### Gaps
- No same-harness agentic benchmark exists for any 2–3-bit GGUF of DeepSeek-V4-Flash, MiniMax-M2.x, Step-3.7-Flash or GLM-5.3-Flash.
- No measured AWQ/FP8 community builds were checked for Kimi-Dev-72B or xLAM-2-70b.
- Cerebras REAP-pruned FP8 sizes for Step-3.5/3.7 were not measured. Only Step-3.5-Flash-REAP-121B BF16 was measured, at 241.93 GB ([HF API](https://huggingface.co/api/models/cerebras/Step-3.5-Flash-REAP-121B-A11B?blobs=true)).
