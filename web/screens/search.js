"use strict";

/* Tree Search (screen 6): full-text search with level/status filters and
   the path jump. '/' focuses the query box (wired in app.js). */

const SearchScreen = () => {
  const [query, setQuery] = preactHooks.useState("");
  const [levels, setLevels] = preactHooks.useState("");
  const [status, setStatus] = preactHooks.useState("");
  const [items, setItems] = preactHooks.useState([]);
  const [error, setError] = preactHooks.useState("");
  const [loading, setLoading] = preactHooks.useState(false);
  const input = preactHooks.useRef(null);

  preactHooks.useEffect(() => {
    if (input.current) input.current.focus();
  }, []);

  const run = preactHooks.useCallback(async () => {
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

  preactHooks.useEffect(() => {
    const timer = setTimeout(run, 250);
    return () => clearTimeout(timer);
  }, [run]);

  return preact.h(
    "div",
    { className: "search-layout" },
    preact.h(
      "aside",
      { className: "search-filters" },
      preact.h("label", null,
        preact.h("span", { className: "muted" }, "query"),
        preact.h("input", {
          ref: input,
          type: "search",
          placeholder: "titles and paths (prefix)",
          value: query,
          onInput: (e) => setQuery(e.target.value),
          "aria-label": "search",
        })),
      preact.h("label", null,
        preact.h("span", { className: "muted" }, "levels"),
        preact.h("input", {
          type: "text",
          placeholder: "0,1,2",
          value: levels,
          onInput: (e) => setLevels(e.target.value),
          "aria-label": "level filter",
        })),
      preact.h("label", null,
        preact.h("span", { className: "muted" }, "status"),
        preact.h(
          "select",
          { value: status, onChange: (e) => setStatus(e.target.value),
            "aria-label": "status filter" },
          preact.h("option", { value: "" }, "any status"),
          preact.h("option", { value: "open" }, "open"),
          preact.h("option", { value: "settled" }, "settled"),
          preact.h("option", { value: "leaf" }, "leaf"),
          preact.h("option", { value: "review" }, "review"),
        )),
      loading && preact.h("span", { className: "muted" }, "…"),
    ),
    preact.h(
      "section",
      { className: "search-results" },
      error && preact.h("p", { className: "err-text" }, error),
      preact.h(
        VList,
        {
          items,
          height: 40,
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
    ),
  );
};
