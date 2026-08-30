"""Ollama escalation for the borderline band ONLY.

MODEL CHOICE: qwen3:14b. Measured on this host's RTX 5090 against five real
fleet/title pairs with ollama's JSON-schema `format` enforcement:

    model                      strict JSON   agreed   median latency
    qwen3:14b                       5/5        4/5        0.4 s   <- chosen
    gemma3:12b                      5/5        4/5        0.7 s
    gemma4:26b                      5/5        4/5       45.6 s
    qwen3.6:27b                     0/5        0/5        1.0 s
    qwen2.5-coder:7b-instruct       5/5        1/5        0.2 s

qwen3.6:27b returned an EMPTY response every time under `format` - unusable, and
worth recording so nobody re-tries it. gemma4:26b matched qwen3:14b's accuracy
at a hundred times the latency and emitted nonsense confidences (0.0, and once
-1.0) plus one word-salad reason. qwen2.5-coder is fast and confidently wrong -
it called 511 MB "insufficient RAM" against a 128 MB minimum. qwen3:14b was
right on four of five, and its one miss (UT2004 on a Pentium III 845 MHz, "no"
where a human said "marginal") is a defensible reading rather than a
misunderstanding of the inputs.

THE MALFORMED-REPLY RULE. A reply that will not parse, or carries a verdict
outside the enum, is retried; if it still will not parse, the DETERMINISTIC
verdict stands. A malformed reply must never become "run" - that would make a
broken model the most permissive possible gate, and it would be invisible.
"""

from __future__ import annotations

import json
import urllib.error
import urllib.request

from . import rules

DEFAULT_MODEL = "qwen3:14b"
DEFAULT_HOST = "http://localhost:11434"

#: ollama enforces this server-side, which is why every model tested returned
#: parseable JSON. It is not a substitute for validating the reply - a
#: schema-valid object can still carry a verdict we do not accept.
RESPONSE_SCHEMA = {
    "type": "object",
    "properties": {
        "verdict": {"type": "string", "enum": ["run", "marginal", "no"]},
        "confidence": {"type": "number"},
        "limiting_factor": {"type": "string"},
        "reason": {"type": "string"},
    },
    "required": ["verdict", "confidence", "limiting_factor", "reason"],
}

SYSTEM = (
    "You are a retro-PC compatibility judge for a fleet of period machines "
    "running Windows 98 to Windows 7. You are given ONE machine's hardware and "
    "ONE game's requirements.\n"
    "Deterministic rules have ALREADY decided every clear-cut case - you are "
    "only ever asked about borderline ones, so do not simply restate that a "
    "number is below a minimum. Judge whether the game is worth installing on "
    "this machine and playable at low settings.\n"
    "Reply with ONE JSON object and nothing else. Keys: verdict (\"run\", "
    "\"marginal\" or \"no\"), confidence (0.0-1.0), limiting_factor (one of: "
    "cpu_mhz, ram_mb, vram_mb, gpu_feature_level, cpu_features, os, disk, "
    "none), reason (at most 200 characters, plain ASCII).\n"
    "\"run\" = playable. \"marginal\" = runs but poorly; still install it. "
    "\"no\" = do not install it on this machine.\n"
    "Period minimum specs were often conservative, and a title with a software "
    "or lower-detail renderer is usually still worth installing."
)

GPU_GLOSS = {
    rules.GPU_FIXED: "fixed-function rasteriser, NO hardware T&L",
    rules.GPU_TNL: "DX7 hardware T&L, no programmable shaders",
    rules.GPU_SM1: "DX8, shader model 1.x",
    rules.GPU_SM2: "DX9, shader model 2.0",
    rules.GPU_SM3: "shader model 3.0 or better",
}


class LLMError(Exception):
    pass


class Judge:
    def __init__(self, model=DEFAULT_MODEL, host=DEFAULT_HOST, timeout=180,
                 retries=2):
        self.model = model
        self.host = host.rstrip("/")
        self.timeout = timeout
        self.retries = retries
        self.calls = 0
        self.failures = 0

    def available(self) -> bool:
        try:
            with urllib.request.urlopen(self.host + "/api/tags", timeout=5) as r:
                tags = json.load(r)
            names = {m.get("name", "") for m in tags.get("models", [])}
            return self.model in names
        except Exception:
            return False

    def _machine_block(self, p: rules.Profile) -> dict:
        return {
            "cpu": (f"{p.cpu_brand or p.cpu_vendor} {p.cpu_mhz} MHz, "
                    f"{p.cpu_count} core(s), instructions: "
                    + " ".join(p.feature_names())),
            "ram_mb": p.ram_mb,
            "gpu": (f"{p.gpu_name} ({p.gpu_ven:04X}:{p.gpu_dev:04X}), "
                    f"{p.vram_mb} MB video RAM, feature level "
                    f"{rules.GPU_LEVEL_NAME.get(p.gpu_level, 'unknown')} "
                    f"({GPU_GLOSS.get(p.gpu_level, 'unclassified')})"),
            "os": p.os_product,
            "directx": p.dx_major,
        }

    def judge(self, profile: rules.Profile, req: rules.Requirements,
              fallback: rules.Decision) -> rules.Decision:
        """Ask the model. On ANY failure the deterministic verdict stands.

        The fallback is passed in rather than reconstructed so the caller's
        exact rule result survives - a model that times out must leave the
        answer unchanged, not merely similar.
        """
        prompt = ("MACHINE:\n" + json.dumps(self._machine_block(profile),
                                            indent=1)
                  + "\n\nGAME:\n" + json.dumps(req.describe(), indent=1)
                  + "\n\nThe deterministic rules called this borderline: "
                  + (fallback.reason or "no specific reason")
                  + "\n\nAnswer with the JSON object only.")
        body = {
            "model": self.model,
            "prompt": prompt,
            "system": SYSTEM,
            "stream": False,
            "format": RESPONSE_SCHEMA,
            "options": {"temperature": 0.0, "num_predict": 300},
        }
        last = None
        for attempt in range(self.retries + 1):
            try:
                req_http = urllib.request.Request(
                    self.host + "/api/generate",
                    data=json.dumps(body).encode(),
                    headers={"Content-Type": "application/json"})
                self.calls += 1
                with urllib.request.urlopen(req_http,
                                            timeout=self.timeout) as r:
                    payload = json.load(r)
                raw = payload.get("response", "")
                d = self._parse(raw, fallback)
                if d is not None:
                    return d
                last = f"unparsable reply: {raw[:160]!r}"
            except (urllib.error.URLError, OSError, ValueError) as exc:
                last = repr(exc)
        # Every attempt failed. Keep the rule verdict and SAY SO - a silent
        # downgrade to "run" would make a broken model the most permissive
        # gate on the fleet, and nothing would show it.
        self.failures += 1
        out = rules.Decision(verdict=fallback.verdict,
                             limiting=fallback.limiting,
                             reason=(fallback.reason
                                     + f" [LLM unavailable: {last}]"),
                             missing_caps=fallback.missing_caps,
                             decided_by="rule",
                             confidence=fallback.confidence)
        return out

    @staticmethod
    def _parse(raw, fallback):
        try:
            obj = json.loads(raw)
        except (ValueError, TypeError):
            return None
        if not isinstance(obj, dict):
            return None
        verdict = rules.VERDICT_VALUE.get(str(obj.get("verdict", "")).lower())
        if verdict is None:
            return None                     # NEVER default to run
        try:
            conf = float(obj.get("confidence", 0.0))
        except (TypeError, ValueError):
            conf = 0.0
        conf = min(max(conf, 0.0), 1.0)
        reason = str(obj.get("reason", ""))[:200]
        limiting = str(obj.get("limiting_factor", "")).strip()
        if limiting in ("none", ""):
            limiting = ""
        return rules.Decision(verdict=verdict, limiting=limiting,
                              reason=reason or "(model gave no reason)",
                              missing_caps=fallback.missing_caps,
                              decided_by="llm", confidence=conf)
