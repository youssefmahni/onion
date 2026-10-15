"use strict";

/* Tree Explorer (screen 5): lazy tree, node details, children table,
   history. Children load 50 at a time through /api/v1/tree/children. */

const TreeScreen = ({ initialPath }) => {
  const [path, setPath] = preactHooks.useState(initialPath || "");
  const [stack, setStack] = preactHooks.useState(initialPath ? [initialPath] : []);
  const [children, setChildren] = preactHooks.useState([]);
  const [cursor, setCursor] = preactHooks.useState("");
  const [hasMore, setHasMore] = preactHooks.useState(false);
  const [loading, setLoading] = preactHooks.useState(false);
  const [detail, setDetail] = preactHooks.useState(null);
  const [error, setError] = preactHooks.useState("");

  const load = preactHooks.useCallback(async (targetPath, reset) => {
    setLoading(true);
    setError("");
    try {
      const body = await fetch(
        "/api/v1/tree/children?" +
          new URLSearchParams({
            path: targetPath || "",
            limit: "50",
            ...(reset ? {} : { cursor }),
          }),
      ).then((r) => r.json());
      if (body.error) throw new Error(body.error);
      setChildren((old) => (reset ? body.items : old.concat(body.items)));
      setHasMore(body.has_more);
      setCursor(body.next_cursor || "");
      const detailResponse = await fetch(
        "/api/v1/tree/node?path=" + encodeURIComponent(targetPath),
      ).then((r) => r.json());
      setDetail(detailResponse.error ? null : detailResponse);
    } catch (e) {
      setError(String(e.message || e));
    } finally {
      setLoading(false);
    }
  }, [cursor]);

  preactHooks.useEffect(() => {
    load(path, true);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [path]);

  const enter = (childPath) => {
    setStack((s) => s.concat(childPath));
    setPath(childPath);
  };
  const up = () => {
    const next = stack.slice(0, -1);
    setStack(next);
    setPath(next.length ? next[next.length - 1] : "");
  };

  return preact.h(
    "div",
    { className: "tree-layout" },
    preact.h(
      "aside",
      { className: "tree-panel" },
      preact.h(
        "div",
        { className: "tree-toolbar" },
        preact.h("button", { onClick: up, disabled: stack.length === 0 }, "↑ up"),
        preact.h("span", { className: "muted mono" }, path || "/"),
        loading && preact.h("span", { className: "muted" }, "…"),
      ),
      error && preact.h("p", { className: "err-text" }, error),
      preact.h(
        VList,
        {
          items: children,
          height: 34,
          render: (child) =>
            preact.h(
              "button",
              {
                className: "tree-row",
                onClick: () => enter(child.path),
              },
              preact.h("span", { className: "mono" }, child.code),
              preact.h("span", { className: "tree-title" }, child.title),
              preact.h("span", { className: "badge " + child.status }, child.status),
              preact.h("span", { className: "muted" }, child.subtree),
            ),
        },
      ),
      hasMore &&
        preact.h("button", { className: "load-more", disabled: loading,
          onClick: () => load(path, false) },
          loading ? "loading…" : "load more"),
    ),
    preact.h(
      "section",
      { className: "tree-detail" },
      detail
        ? preact.h(
            preact.Fragment,
            null,
            preact.h("h2", null, detail.title),
            preact.h("p", { className: "mono muted" }, detail.path),
            preact.h(
              "div",
              { className: "kpi-row" },
              preact.h("span", { className: "badge " + detail.status }, detail.status),
              preact.h("span", { className: "muted" }, "layer " + detail.layer),
              preact.h("span", { className: "muted" }, "children " + detail.children),
              preact.h("span", { className: "muted" }, "subtree " + detail.subtree),
              detail.settle &&
                preact.h("span", { className: "muted" },
                  "score " + detail.settle.score + " · " + detail.settle.end),
            ),
            preact.h(TreeHistory, { detail }),
            preact.h(
              "div",
              { className: "kpi-row" },
              preact.h(
                "a",
                { href: "/api/v1/tree/export.csv?path=" + encodeURIComponent(detail.path) },
                "export subtree csv",
              ),
            ),
          )
        : preact.h("p", { className: "muted" }, "select a node"),
    ),
  );
};

const TreeHistory = ({ detail }) => {
  const [tab, setTab] = preactHooks.useState("children");
  const attempts = detail.attempts || [];
  return preact.h(
    preact.Fragment,
    null,
    preact.h(
      "div",
      { className: "tabs" },
      preact.h(
        "button",
        { className: tab === "children" ? "active" : "", onClick: () => setTab("children") },
        "attempts (" + attempts.length + ")",
      ),
      preact.h(
        "button",
        { className: tab === "critiques" ? "active" : "", onClick: () => setTab("critiques") },
        "critiques (" + (detail.critiques || []).length + ")",
      ),
    ),
    tab === "children" &&
      preact.h(
        "table",
        { className: "data-table" },
        preact.h(
          "thead",
          null,
          preact.h("tr", null,
            preact.h("th", null, "n"), preact.h("th", null, "score"),
            preact.h("th", null, "passed"), preact.h("th", null, "tokens"),
            preact.h("th", null, "cost"), preact.h("th", null, "origin")),
        ),
        preact.h(
          "tbody",
          null,
          attempts.map((attempt) =>
            preact.h("tr", { key: attempt.n },
              preact.h("td", null, attempt.n),
              preact.h("td", null, attempt.score),
              preact.h("td", null, attempt.passed),
              preact.h("td", null, attempt.tokens_in + "/" + attempt.tokens_out),
              preact.h("td", null, attempt.cost),
              preact.h("td", { className: "mono" }, attempt.origin)),
          ),
        ),
      ),
    tab === "critiques" &&
      preact.h(
        "table",
        { className: "data-table" },
        preact.h(
          "thead",
          null,
          preact.h("tr", null,
            preact.h("th", null, "score"), preact.h("th", null, "issues"),
            preact.h("th", null, "remove"), preact.h("th", null, "add")),
        ),
        preact.h(
          "tbody",
          null,
          (detail.critiques || []).map((critique, index) =>
            preact.h("tr", { key: index },
              preact.h("td", null, critique.score),
              preact.h("td", null, critique.issues),
              preact.h("td", null, critique.remove),
              preact.h("td", null, critique.add)),
          ),
        ),
      ),
  );
};
