"use strict";

/* Review queue (screen 4): items by reason, attempts and proposed
   children, accept a selection, retry with a note, leaf. Keyboard:
   a = accept the selection, r = retry, l = leaf (plan §7). */

const ReviewScreen = () => {
  const [items, setItems] = preactHooks.useState([]);
  const [cursor, setCursor] = preactHooks.useState("");
  const [reasonFilter, setReasonFilter] = preactHooks.useState("");
  const [selectedPath, setSelectedPath] = preactHooks.useState(null);
  const [detail, setDetail] = preactHooks.useState(null);
  const [picked, setPicked] = preactHooks.useState(null);  // set of indices
  const [note, setNote] = preactHooks.useState("");
  const [message, setMessage] = preactHooks.useState("");
  const [settings, setSettings] = preactHooks.useState(null);
  const [autoDays, setAutoDays] = preactHooks.useState("");
  const [autoReasons, setAutoReasons] = preactHooks.useState("");

  const load = preactHooks.useCallback(async () => {
    const query = new URLSearchParams({ limit: "50" });
    if (reasonFilter) query.set("reason", reasonFilter);
    if (cursor) query.set("cursor", cursor);
    const body = await fetch("/api/v1/reviews?" + query).then((r) => r.json());
    setItems((old) => (cursor ? old.concat(body.items || []) : body.items || []));
    setCursor(body.next_cursor || "");
  }, [reasonFilter, cursor]);

  const loadSettings = preactHooks.useCallback(async () => {
    const body = await fetch("/api/v1/review-settings").then((r) => r.json());
    setSettings(body);
    setAutoDays(String(body.auto_accept_after_days || "0"));
    setAutoReasons(Array.isArray(body.reasons) ? body.reasons.join(",") : (body.reasons || ""));
  }, []);

  preactHooks.useEffect(() => {
    load();
    loadSettings();
    const timer = setInterval(load, 10000);
    return () => clearInterval(timer);
  }, [load, loadSettings]);

  const open = async (path) => {
    setSelectedPath(path);
    setPicked(new Set());
    setNote("");
    const body = await fetch("/api/v1/reviews/" + path).then((r) => r.json());
    setDetail(body);
  };

  const act = async (action, body) => {
    const response = await fetch("/api/v1/reviews/" + selectedPath + "/" + action, {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        "X-Onion-Request": "1",
        "Idempotency-Key": crypto.randomUUID(),
      },
      body: JSON.stringify(body || {}),
    });
    const json = await response.json().catch(() => ({}));
    if (response.status === 202) {
      setMessage("submitted: " + json.op_id);
      setDetail(null);
      setSelectedPath(null);
      load();
    } else {
      setMessage(json.error || "action failed");
    }
  };

  const toggle = (index) => {
    const next = new Set(picked);
    if (next.has(index)) next.delete(index);
    else next.add(index);
    setPicked(next);
  };

  const saveSettings = async () => {
    const response = await fetch("/api/v1/review-settings", {
      method: "PUT",
      headers: {
        "Content-Type": "application/json",
        "X-Onion-Request": "1",
        "Idempotency-Key": crypto.randomUUID(),
      },
      body: JSON.stringify({
        auto_accept_after_days: Number(autoDays) || 0,
        reasons: autoReasons.split(",").map((s) => s.trim()).filter(Boolean),
      }),
    });
    const json = await response.json().catch(() => ({}));
    setMessage(json.error || (json.ok ? "settings saved" : "save failed"));
  };

  preactHooks.useEffect(() => {
    const handler = (event) => {
      if (!selectedPath || event.target.closest("input,textarea,select")) return;
      if (event.key === "a") act("accept", { children: Array.from(picked || []) });
      if (event.key === "r") act("retry", { note });
      if (event.key === "l") act("leaf", {});
    };
    document.addEventListener("keydown", handler);
    return () => document.removeEventListener("keydown", handler);
  }, [selectedPath, picked, note]);

  return preact.h(
    "div",
    { className: "campaigns-layout" },
    preact.h(
      "section",
      { className: "campaigns-list" },
      preact.h("h2", null, "Review queue"),
      preact.h(
        "div",
        { className: "search-bar" },
        preact.h("input", {
          type: "text",
          placeholder: "filter by reason",
          value: reasonFilter,
          onInput: (e) => { setReasonFilter(e.target.value); setCursor(""); },
          "aria-label": "reason filter",
        }),
      ),
      preact.h(
        "table",
        { className: "data-table" },
        preact.h("thead", null,
          preact.h("tr", null,
            preact.h("th", null, "path"), preact.h("th", null, "title"),
            preact.h("th", null, "reason"), preact.h("th", null, "attempts"))),
        preact.h("tbody", null,
          items.map((item) =>
            preact.h("tr", {
              key: item.path,
              className: selectedPath === item.path ? "selected" : "",
              onClick: () => open(item.path),
            },
              preact.h("td", { className: "mono" }, item.path),
              preact.h("td", null, item.title || ""),
              preact.h("td", null, item.reason),
              preact.h("td", null, item.attempts))),
        ),
      ),
      cursor && preact.h("button", { className: "load-more", onClick: load }, "load more"),
      preact.h("h3", null, "Auto-accept settings"),
      preact.h("label", null, preact.h("span", { className: "muted" }, "after days (0 = off)")),
      preact.h("input", { type: "number", min: 0, value: autoDays,
        onInput: (e) => setAutoDays(e.target.value), "aria-label": "auto accept days" }),
      preact.h("label", null, preact.h("span", { className: "muted" }, "reasons (comma list)")),
      preact.h("input", { type: "text", value: autoReasons,
        onInput: (e) => setAutoReasons(e.target.value), "aria-label": "auto accept reasons" }),
      preact.h("p", null, preact.h("button", { onClick: saveSettings }, "save settings")),
    ),
    preact.h(
      "section",
      { className: "campaigns-detail" },
      detail
        ? preact.h(
            preact.Fragment,
            null,
            preact.h("h2", null, detail.title || detail.path),
            preact.h("p", { className: "mono muted" }, detail.path + " · " + detail.reason),
            preact.h("h3", null, "Proposed children (select to keep)"),
            preact.h(
              "table",
              { className: "data-table" },
              preact.h("thead", null,
                preact.h("tr", null,
                  preact.h("th", null, "keep"), preact.h("th", null, "code"),
                  preact.h("th", null, "title"), preact.h("th", null, "layer"))),
              preact.h("tbody", null,
                (detail.children || []).map((child) =>
                  preact.h("tr", { key: child.path },
                    preact.h("td", null,
                      preact.h("input", {
                        type: "checkbox",
                        checked: picked && picked.has(child.index),
                        onChange: () => toggle(child.index),
                        "aria-label": "keep " + child.path,
                      })),
                    preact.h("td", { className: "mono" }, child.code),
                    preact.h("td", null, child.title),
                    preact.h("td", null, child.layer))),
              ),
            ),
            preact.h("h3", null, "Attempts (" + (detail.attempts || []).length + ")"),
            preact.h(
              "table",
              { className: "data-table" },
              preact.h("thead", null,
                preact.h("tr", null,
                  preact.h("th", null, "n"), preact.h("th", null, "score"),
                  preact.h("th", null, "passed"), preact.h("th", null, "tokens"),
                  preact.h("th", null, "cost"))),
              preact.h("tbody", null,
                (detail.attempts || []).map((attempt) =>
                  preact.h("tr", { key: attempt.n },
                    preact.h("td", null, attempt.n),
                    preact.h("td", null, attempt.score),
                    preact.h("td", null, attempt.passed),
                    preact.h("td", null, attempt.tokens_in + "/" + attempt.tokens_out),
                    preact.h("td", null, attempt.cost))),
              ),
            ),
            preact.h("h3", null, "Check results"),
            preact.h("pre", { className: "plan-report" },
              (detail.critiques || []).map((c) => c.issues + "\\n").join("") ||
                "(none)"),
            preact.h(
              "div",
              { className: "kpi-row" },
              preact.h("input", { type: "text", placeholder: "retry note", value: note,
                onInput: (e) => setNote(e.target.value), "aria-label": "retry note" }),
              preact.h("button", { onClick: () => act("accept", { children: Array.from(picked || []) }) },
                "accept (a)"),
              preact.h("button", { onClick: () => act("retry", { note }) }, "retry (r)"),
              preact.h("button", { onClick: () => act("leaf", {}) }, "leaf (l)"),
            ),
          )
        : preact.h("p", { className: "muted" }, "select an item"),
      message && preact.h("p", { className: "message" }, message),
    ),
  );
};
