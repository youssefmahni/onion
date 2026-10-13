#!/usr/bin/env python3
"""Tests for the llm_proxy budget caps (owner rule: the proxy enforces
the 4,000-call / $5.00 lifetime caps itself and refuses calls beyond).

In-process and platform-independent: the proxy module is imported, its
UPSTREAM is never contacted (the refusal path returns before any
network I/O), and the pass-through accounting path runs against a
mocked urlopen. Standard library only.
"""

import importlib.util
import io
import json
import sys
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent.parent
PROXY = ROOT / "tools" / "llm_proxy.py"

spec = importlib.util.spec_from_file_location("llm_proxy", str(PROXY))
llm_proxy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(llm_proxy)


class ShimHandler:
    """Minimal stand-in for BaseHTTPRequestHandler for _forward."""

    budget = None  # filled from Proxy.budget below

    def __init__(self, body=""):
        self.headers = {}
        self.rfile = io.BytesIO(body.encode())
        self.status = None
        self.body = b""

    def send_response(self, code, *_args, **_kwargs):
        self.status = code

    def send_header(self, _name, _value):
        pass

    def end_headers(self):
        pass

    @property
    def wfile(self):
        return self

    def write(self, data):
        self.body += data


USAGE = {"prompt_tokens": 1000, "completion_tokens": 1000}
OK_BODY = json.dumps({"choices": [], "usage": USAGE})


class BudgetTests(unittest.TestCase):
    def setUp(self):
        self.old_caps = (llm_proxy.CALL_CAP, llm_proxy.USD_CAP)

    def tearDown(self):
        llm_proxy.CALL_CAP, llm_proxy.USD_CAP = self.old_caps

    def fresh(self):
        return llm_proxy.Budget()

    def test_usage_usd_uses_peak_prices(self):
        # 1000 in at 0.27/1M + 1000 out at 1.10/1M = 0.00137.
        self.assertAlmostEqual(llm_proxy.Budget.usage_usd(OK_BODY), 0.00137)

    def test_usage_usd_tolerates_garbage(self):
        self.assertEqual(llm_proxy.Budget.usage_usd("not json"), 0.0)
        self.assertEqual(llm_proxy.Budget.usage_usd("{}"), 0.0)

    def test_refused_at_the_call_cap(self):
        budget = self.fresh()
        llm_proxy.CALL_CAP = 2
        self.assertFalse(budget.refused())
        budget.account(OK_BODY)
        budget.account(OK_BODY)
        self.assertTrue(budget.refused())

    def test_refused_at_the_usd_cap(self):
        budget = self.fresh()
        llm_proxy.USD_CAP = 0.002
        budget.account(OK_BODY)  # 0.00137
        self.assertFalse(budget.refused())
        budget.account(OK_BODY)  # 0.00274
        self.assertTrue(budget.refused())

    def test_history_counts_toward_the_lifetime_cap(self):
        budget = self.fresh()
        record = Path("/tmp") if sys.platform != "win32" else Path(".") \
            if False else None
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "cassette.jsonl"
            path.write_text(json.dumps({
                "ts": 1, "method": "POST", "path": "/v1/chat/completions",
                "headers": {}, "body": "{}", "status": 200,
                "response": OK_BODY,
            }) + "\n")
            budget.load_history(path)
        self.assertEqual(budget.calls, 1)
        self.assertAlmostEqual(budget.usd, 0.00137)


class RefusalTests(unittest.TestCase):
    def setUp(self):
        import tempfile
        self._tmp = tempfile.TemporaryDirectory()
        self.old_caps = (llm_proxy.CALL_CAP, llm_proxy.USD_CAP)
        self.old_upstream = llm_proxy.UPSTREAM
        self.old_recorder = llm_proxy.Proxy.recorder
        llm_proxy.Proxy.recorder = llm_proxy.Recorder(
            Path(self._tmp.name) / "cassette.jsonl")

    def tearDown(self):
        (llm_proxy.CALL_CAP, llm_proxy.USD_CAP) = self.old_caps
        llm_proxy.UPSTREAM = self.old_upstream
        llm_proxy.Proxy.recorder = self.old_recorder
        self._tmp.cleanup()

    def test_forward_refuses_before_any_network_io(self):
        llm_proxy.CALL_CAP = 1
        llm_proxy.USD_CAP = 5.0
        llm_proxy.Proxy.budget = llm_proxy.Budget()
        llm_proxy.Proxy.budget.account(OK_BODY)  # already at the cap
        handler = ShimHandler(body="{}")
        handler.path = "/v1/chat/completions"
        handler.budget = llm_proxy.Proxy.budget
        handler.budget = llm_proxy.Proxy.budget
        llm_proxy.UPSTREAM = "http://127.0.0.1:1"  # would fail if contacted
        with mock.patch("urllib.request.urlopen") as urlopen:
            llm_proxy.Proxy._forward(handler, "POST")
        urlopen.assert_not_called()
        self.assertEqual(handler.status, 402)
        self.assertIn(b"budget exhausted", handler.body)

    def test_forward_accounts_a_successful_completion(self):
        llm_proxy.CALL_CAP = 4000
        llm_proxy.USD_CAP = 5.0
        budget = llm_proxy.Budget()
        llm_proxy.Proxy.budget = budget
        handler = ShimHandler(body="{}")
        handler.path = "/v1/chat/completions"
        handler.budget = llm_proxy.Proxy.budget
        handler.budget = llm_proxy.Proxy.budget
        with mock.patch("urllib.request.urlopen") as urlopen:
            urlopen.return_value.__enter__.return_value.status = 200
            urlopen.return_value.__enter__.return_value.read.return_value = \
                OK_BODY.encode()
            llm_proxy.Proxy._forward(handler, "POST")
        self.assertEqual(budget.calls, 1)
        self.assertAlmostEqual(budget.usd, 0.00137)

    def test_models_discovery_never_counts(self):
        llm_proxy.CALL_CAP = 1
        llm_proxy.USD_CAP = 5.0
        budget = llm_proxy.Budget()
        llm_proxy.Proxy.budget = budget
        handler = ShimHandler()
        handler.path = "/models"
        handler.budget = llm_proxy.Proxy.budget
        with mock.patch("urllib.request.urlopen") as urlopen:
            urlopen.return_value.__enter__.return_value.status = 200
            urlopen.return_value.__enter__.return_value.read.return_value = \
                b"{}"
            llm_proxy.Proxy._forward(handler, "GET")
        self.assertEqual(budget.calls, 0)


if __name__ == "__main__":
    unittest.main()
