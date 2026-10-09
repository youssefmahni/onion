#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
vision.py — image understanding for the Onion data depot.

Uses the OpenRouter vision-capable model `google/gemini-2.5-flash-lite`
(vision + text via the standard OpenAI-style image_url content parts).
Extracts knowledge items from diagrams, screenshots, tables, and photos and
can emit TSV lines ready for `data_depot ingest`.

Usage:
  python3 vision.py ask <image> "question"
  python3 vision.py describe <image>
  python3 vision.py items <image> [--td "Map current processes"] [--out file.tsv]

Key: OPENROUTER_API_KEY env var (legacy: DEEPSEEK_API_KEY), or edit
API_KEY_PLACEHOLDER below. Endpoint: DEPOT_API_URL env var to override.
Stdlib only. Images must be PNG/JPEG/WebP/GIF; keep them under ~5 MB.
"""
import argparse, base64, json, os, re, sys
from urllib import request as urlreq

API_URL = os.environ.get(
    "DEPOT_API_URL", "https://openrouter.ai/api/v1/chat/completions")


def _provider():
    """Key-driven routing, identical to the C++ layer: sk-or-* keys belong to
    OpenRouter, anything else to DeepSeek (unless DEPOT_API_URL overrides)."""
    key = (os.environ.get("OPENROUTER_API_KEY")
           or os.environ.get("DEEPSEEK_API_KEY") or "")
    url = os.environ.get("DEPOT_API_URL")
    if url:
        return url, key
    if key.startswith("sk-or"):
        return ("https://openrouter.ai/api/v1/chat/completions",
                "google/gemini-2.5-flash-lite")
    return ("https://api.deepseek.com/chat/completions", "deepseek-flash")


MODEL = "google/gemini-2.5-flash-lite"   # vision-capable, cheapest flash tier
# >>> PASTE YOUR OPENROUTER API KEY HERE (used only if OPENROUTER_API_KEY is unset) <<<
API_KEY_PLACEHOLDER = "PASTE-YOUR-OPENROUTER-API-KEY-HERE"

MIME = {".png": "image/png", ".jpg": "image/jpeg", ".jpeg": "image/jpeg",
        ".webp": "image/webp", ".gif": "image/gif"}


def load_image_b64(path):
    ext = os.path.splitext(path)[1].lower()
    if ext not in MIME:
        sys.exit(f"unsupported image type {ext} (use: {', '.join(MIME)})")
    size = os.path.getsize(path)
    if size > 20 * 1024 * 1024:
        sys.exit(f"image too large ({size/1e6:.1f} MB > 20 MB)")
    if size > 5 * 1024 * 1024:
        print(f"warning: image is {size/1e6:.1f} MB — API may reject large payloads", file=sys.stderr)
    with open(path, "rb") as f:
        return base64.b64encode(f.read()).decode(), MIME[ext]


def ask_vision(image_path, question, max_tokens=1200):
    key = (os.environ.get("OPENROUTER_API_KEY")
           or os.environ.get("DEEPSEEK_API_KEY")   # legacy fallback
           or API_KEY_PLACEHOLDER)
    if not key or key.startswith("PASTE"):
        sys.exit("no API key: set OPENROUTER_API_KEY or edit API_KEY_PLACEHOLDER in vision.py")
    url, model = _provider()
    b64, mime = load_image_b64(image_path)
    body = {
        "model": model,
        "messages": [{"role": "user", "content": [
            {"type": "text", "text": question},
            {"type": "image_url", "image_url": {"url": f"data:{mime};base64,{b64}"}},
        ]}],
        "max_tokens": max_tokens,
        "temperature": 0,
        "stream": False,
    }
    if "deepseek" in url:
        body["reasoning_effort"] = "none"
    payload = json.dumps(body).encode()
    req = urlreq.Request(url, data=payload, headers={
        "Content-Type": "application/json",
        "Authorization": "Bearer " + key,
    })
    try:
        with urlreq.urlopen(req, timeout=120) as r:
            body = json.loads(r.read())
        msg = body["choices"][0]["message"]
        if not msg.get("content"):
            sys.exit("provider returned empty content (wrong key for the "
                     "endpoint, rate limit, or reasoning budget?)")
        return msg["content"]
    except urlreq.HTTPError as e:
        sys.exit(f"API error HTTP {e.code}: {e.read()[:400]}")
    except (KeyError, IndexError, json.JSONDecodeError) as e:
        sys.exit(f"unexpected API response: {e}")


def extract_items(image_path):
    q = ("Extract every distinct knowledge item visible in this image "
         "(diagram nodes, table rows, list entries, process steps). "
         "Return ONLY a JSON object: {\"items\": [\"Title Case item\", ...]} "
         "with concise Title Case noun phrases, no duplicates, no questions.")
    txt = ask_vision(image_path, q, max_tokens=4000)
    m = re.search(r"\{.*\}", txt, re.S)
    if m:
        try:
            d = json.loads(m.group(0))
            items = d.get("items") or []
        except json.JSONDecodeError:
            items = []
    else:
        items = []
    if not items:
        # fallback: one item per non-empty line
        items = [l.strip(" -•*\"'") for l in txt.splitlines() if l.strip() and len(l.strip()) > 2]
    return [i for i in dict.fromkeys(items) if i]


def main():
    ap = argparse.ArgumentParser(description="Image understanding via google/gemini-2.5-flash-lite (OpenRouter)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p_ask = sub.add_parser("ask", help="free-form question about an image")
    p_ask.add_argument("image")
    p_ask.add_argument("question")
    p_desc = sub.add_parser("describe", help="summarize an image")
    p_desc.add_argument("image")
    p_items = sub.add_parser("items", help="extract knowledge items as TSV")
    p_items.add_argument("image")
    p_items.add_argument("--td", default="", help="target TD title for the TSV lines")
    p_items.add_argument("--out", default="", help="write TSV to this file instead of stdout")
    a = ap.parse_args()

    if a.cmd == "ask":
        print(ask_vision(a.image, a.question))
    elif a.cmd == "describe":
        print(ask_vision(a.image,
              "Describe this image concisely: what it shows, its structure, and any text you can read."))
    elif a.cmd == "items":
        items = extract_items(a.image)
        if not items:
            sys.exit("no items extracted")
        lines = [f"{a.td}\t{i}" if a.td else i for i in items]
        out = "\n".join(lines) + "\n"
        if a.out:
            with open(a.out, "w") as f:
                f.write(out)
            print(f"wrote {len(items)} items -> {a.out}", file=sys.stderr)
        else:
            print(out, end="")


if __name__ == "__main__":
    main()
