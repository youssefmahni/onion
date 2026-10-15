"use strict";

/* Tree Search (screen 6): full-text search with level/status filters and
   the path jump. '/' focuses the query box (wired in app.js). */

const SearchScreen = () => {
  const [query, setQuery] = preact.useState("");
  const [levels, setLevels] = preact.useState("");
  const [status, setStatus] = preact.useState("");
  const [items, setItems] = preact.useState([]);
  const [error, setError] = preact.useState("");
  const [loading, setLoading] = preact.useState(false);
  const input = preact.useRef(null);

  preact.useEffect(() => {
    if (input.current) input.current.focus();
  }, []);

  const run = preact.useCallback(async () => {
    if (!query.trim()) {
      setItems([]);
      return;
    }
    setLoading(true);
    setError("");
    try {
      const params = new URLSearchParams({ q: query, levels, status });
      const body = await fetch("/api/v1/tree/search?" + params).then((r) =>
        r.json(),
      );
      if (body.error) throw new Error(body.error);
      setItems(body.items || []);
    } catch (e) {
      setError(String(e.message || e));
      setItems([]);
    } finally {
      setLoading(false);
    }
  }, [query, levels, status]);

  preact.useEffect(() => {
    const timer = setTimeout(run, 250);
    return () => clearTimeout(timer);
  }, [run]);

  return preact.h(
    "div",
    null,
    preact.h(
      "div",
      { className: "search-bar" },
      preact.h("input", {
        ref: input,
        type: "search",
        placeholder: "search titles and paths (prefix matches)",
        value: query,
        onInput: (e) => setQuery(e.target.value),
        "aria-label": "search",
      }),
      preact.h("input", {
        type: "text",
        placeholder: "levels (e.g. 0,1,2)",
        value: levels,
        onInput: (e) => setLevels(e.target.value),
        "aria-label": "level filter",
      }),
      preact.h(
        "select",
        { value: status, onChange: (e) => setStatus(e.target.value),
          "aria-label": "status filter" },
        preact.h("option", { value: "" }, "any status"),
        preact.h("option", { value: "open" }, "open"),
        preact.h("option", { value: "settled" }, "settled"),
        preact.h("option", { value: "leaf" }, "leaf"),
        preact.h("option", { value: "review" }, "review"),
      ),
      loading && preact.h("span", { className: "muted" }, "…"),
    ),
    error && preact.h("p", { className: "err-text" }, error),
    preact.h(
      VList,
      {
        items,
        height: 36,
        render: (item) =>
          preact.h(
            "a",
            {
              className: "search-row",
              href: "#/tree/" + encodeURIComponent(item.path),
            },
            preact.h("span", { className: "mono" }, item.path),
            preact.h("span", null, item.title),
            preact.h("span", { className: "badge " + item.status }, item.status),
            preact.h("span", { className: "muted" }, "L" + item.layer),
          ),
      },
    ),
  );
};
