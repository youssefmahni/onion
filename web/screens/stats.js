"use strict";

/* Tree Stats (screen 7): per-level counts, endings, campaign settles. */

const StatsScreen = () => {
  const [stats, setStats] = preact.useState(null);
  const [error, setError] = preact.useState("");

  const load = preact.useCallback(async () => {
    try {
      const body = await fetch("/api/v1/tree/stats").then((r) => r.json());
      if (body.error) throw new Error(body.error);
      setStats(body);
    } catch (e) {
      setError(String(e.message || e));
    }
  }, []);

  preact.useEffect(() => {
    load();
    const timer = setInterval(load, 15000);
    return () => clearInterval(timer);
  }, [load]);

  if (error) return preact.h("p", { className: "err-text" }, error);
  if (!stats) return preact.h("p", { className: "muted" }, "loading…");

  const levels = stats.levels || [];
  const endings = stats.endings || [];
  const campaigns = stats.campaigns || [];

  return preact.h(
    "div",
    null,
    preact.h(
      "div",
      { className: "kpi-row stats-kpis" },
      preact.h("div", { className: "kpi" },
        preact.h("span", { className: "kpi-value" }, stats.nodes_total),
        preact.h("span", { className: "muted" }, "nodes")),
      preact.h("div", { className: "kpi" },
        preact.h("span", { className: "kpi-value" }, stats.settled_total),
        preact.h("span", { className: "muted" }, "settled")),
      preact.h("div", { className: "kpi" },
        preact.h("span", { className: "kpi-value" }, stats.leaves_total),
        preact.h("span", { className: "muted" }, "leaves")),
      preact.h("div", { className: "kpi" },
        preact.h("span", { className: "kpi-value" }, stats.atomic_total),
        preact.h("span", { className: "muted" }, "atomic")),
      preact.h("div", { className: "kpi" },
        preact.h("span", { className: "kpi-value" }, stats.review_open),
        preact.h("span", { className: "muted" }, "in review")),
    ),
    preact.h(
      "section",
      null,
      preact.h("h2", null, "Nodes per level"),
      preact.h(
        "table",
        { className: "data-table" },
        preact.h(
          "thead",
          null,
          preact.h("tr", null, preact.h("th", null, "layer"), preact.h("th", null, "nodes")),
        ),
        preact.h(
          "tbody",
          null,
          levels.map((level) =>
            preact.h("tr", { key: level.layer },
              preact.h("td", null, level.layer),
              preact.h("td", null, level.nodes)),
          ),
        ),
      ),
    ),
    preact.h(
      "section",
      null,
      preact.h("h2", null, "How nodes ended"),
      preact.h(
        "table",
        { className: "data-table" },
        preact.h(
          "thead",
          null,
          preact.h("tr", null, preact.h("th", null, "end"), preact.h("th", null, "nodes")),
        ),
        preact.h(
          "tbody",
          null,
          endings.map((ending) =>
            preact.h("tr", { key: ending.end },
              preact.h("td", null, ending.end),
              preact.h("td", null, ending.n)),
          ),
        ),
      ),
    ),
    campaigns.length > 0 &&
      preact.h(
        "section",
        null,
        preact.h("h2", null, "Settles per campaign"),
        preact.h(
          "table",
          { className: "data-table" },
          preact.h(
            "thead",
            null,
            preact.h("tr", null,
              preact.h("th", null, "campaign"), preact.h("th", null, "settles")),
          ),
          preact.h(
            "tbody",
            null,
            campaigns.map((campaign) =>
              preact.h("tr", { key: campaign.campaign },
                preact.h("td", { className: "mono" }, campaign.campaign),
                preact.h("td", null, campaign.settles)),
            ),
          ),
        ),
      ),
  );
};
