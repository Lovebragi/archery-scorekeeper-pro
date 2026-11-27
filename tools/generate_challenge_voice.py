#!/usr/bin/env python3
"""Generate local 16 kHz PCM challenge prompt clips with Volcengine TTS.

Credentials are read from VOLC_TTS_API_KEY; never hard-code or commit them.
The generated files are application assets, not runtime network calls.
"""
import argparse
import base64
import http.client
import json
import os
import struct
from pathlib import Path

ENDPOINT = "https://openspeech.bytedance.com/api/v3/tts/unidirectional"
RESOURCE_ID = "seed-tts-2.0"
SPEAKER = "zh_female_tianmeitaozi_uranus_bigtts"

MAIN = [
    "十环、九环、八环各两支，共六支箭。",
    "六环、七环、八环、九环、十环、内十各一支，共六支箭。",
    "连续三支箭命中十环。",
    "最后一支箭必须命中十环。",
    "六支箭全部八环以上。",
    "六支箭不允许脱靶。",
    "六支箭总分五十二环以上。",
    "六支箭总分五十四环以上。",
    "十环、十环、九环、九环、八环、八环，共六支箭。",
    "十环、九环、八环、十环、九环、八环，共六支箭。",
    "八环、九环、十环、八环、九环、十环，共六支箭。",
    "十环、九环、八环各一支，共三支箭。",
    "十环、十环、九环，共三支箭。",
    "九环、九环、八环、八环，共四支箭。",
    "六支箭中至少三支命中十环。",
    "六支箭中至少四支命中九环以上。",
    "第一支箭必须命中十环。",
    "前两支箭必须命中十环。",
    "单箭决胜，一支箭必须命中十环。",
    "六支箭全部九环以上。",
    "九支箭至少七支九环以上。",
    "九支箭全部八环以上。",
    "最多一支八环，其余九环以上。",
    "六支箭总分四十八环以上。",
    "六支箭总分五十六环以上。",
    "三支箭总分二十八环以上。",
    "三支箭总分二十九环以上。",
    "九支箭总分七十八环以上。",
    "九支箭总分八十环以上。",
]
MODS = [
    "六支箭两分钟内完成。",
    "每支箭三十秒内完成。",
    "全部射完前不看靶。",
    "最后一支箭必须十环。",
    "第一支箭按八环计算，后面追回。",
    "后退一步，不调整瞄具。",
    "六支箭三分钟内完成。",
    "九支箭五分钟内完成。",
    "每支箭二十秒内完成。",
    "箭与箭之间不超过三十秒。",
    "不允许中途休息或调整心态。",
    "每支箭前深呼吸三次再射。",
]
DIFF = ["难度，一颗星。", "难度，两颗星。", "难度，三颗星。", "难度，四颗星。",
        "难度，五颗星。", "难度，六颗星。", "难度，七颗星。", "难度，八颗星。",
        "难度，九颗星。", "难度，十颗星。"]


def tts(text: str, key: str, reqid: str) -> bytes:
    body = json.dumps({
        "user": {"uid": "ai-passport"},
        "req_params": {
            "text": text,
            "speaker": SPEAKER,
            "audio_params": {"format": "wav", "sample_rate": 16000},
        },
    }).encode()
    conn = http.client.HTTPSConnection("openspeech.bytedance.com", timeout=60)
    conn.request("POST", "/api/v3/tts/unidirectional", body, {
        "Content-Type": "application/json",
        "X-Api-Key": key,
        "X-Api-Resource-Id": RESOURCE_ID,
        "X-Api-Request-Id": reqid,
    })
    resp = conn.getresponse()
    raw = resp.read()
    conn.close()
    if resp.status != 200:
        raise RuntimeError(f"HTTP {resp.status}: {raw[:200]!r}")
    chunks = []
    for line in raw.decode("utf-8", "ignore").splitlines():
        if not line.strip():
            continue
        obj = json.loads(line)
        if obj.get("data"):
            chunks.append(base64.b64decode(obj["data"]))
        if obj.get("code") not in (None, 0, 20000000):
            raise RuntimeError(f"TTS {obj.get('code')}: {obj.get('message')}")
    return b"".join(chunks)


def wav_pcm16(wav: bytes) -> bytes:
    if wav[:4] != b"RIFF" or wav[8:12] != b"WAVE":
        raise ValueError("not a WAV response")
    pos = 12
    while pos + 8 <= len(wav):
        cid = wav[pos:pos+4]
        size = struct.unpack("<I", wav[pos+4:pos+8])[0]
        if cid == b"fmt ":
            fmt = struct.unpack("<HHIIHH", wav[pos+8:pos+24])
            if fmt[0] != 1 or fmt[1] != 1 or fmt[2] != 16000 or fmt[5] != 16:
                raise ValueError(f"unexpected WAV format {fmt}")
        if cid == b"data":
            return wav[pos+8:]
        if size == 0xFFFFFFFF:
            pos += 8
        else:
            pos += 8 + size
    raise ValueError("missing data chunk")


def trim(pcm: bytes) -> bytes:
    if len(pcm) < 4:
        return pcm
    samples = struct.unpack("<%dh" % (len(pcm)//2), pcm)
    threshold = 180
    first = 0
    while first < len(samples) and abs(samples[first]) < threshold:
        first += 1
    last = len(samples) - 1
    while last >= first and abs(samples[last]) < threshold:
        last -= 1
    if first > last:
        return b""
    pad = 800  # 50 ms
    first = max(0, first - pad)
    last = min(len(samples) - 1, last + pad)
    return struct.pack("<%dh" % (last-first+1), *samples[first:last+1])


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="assets/voice/challenge")
    args = ap.parse_args()
    key = os.environ.get("VOLC_TTS_API_KEY", "").strip()
    if not key:
        raise SystemExit("VOLC_TTS_API_KEY is not set")
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    manifest = {}
    jobs = [(f"main_{i:02d}", text) for i, text in enumerate(MAIN)]
    jobs += [(f"mod_{i:02d}", text) for i, text in enumerate(MODS)]
    jobs += [(f"diff_{i+1:02d}", text) for i, text in enumerate(DIFF)]
    for name, text in jobs:
        wav = tts(text, key, f"challenge-{name}")
        pcm = trim(wav_pcm16(wav))
        (out / f"{name}.pcm").write_bytes(pcm)
        manifest[name] = text
        print(f"{name}: {len(pcm)} bytes")
    (out / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
