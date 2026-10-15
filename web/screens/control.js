"use strict";

/* Control center (screen 1): alerts with actions, key numbers, speed and
   spend charts (uPlot), campaign and fleet summaries, recent activity.
   The stream client (EventSource) feeds live numbers; charts re-fetch on
   range change and every 15 s. */

const SPEED_RANGES = [["1h", 3600], ["24h", 86400], ["7d", 604800]];
const SPEND_RANGES = [["24h", 86400], ["7d", 604800]];

function usd(value) {
  return "$" + Number(value || 0).toFixed(4);
}

const ControlScreen = () => {
  const [summary, setSummary] = preactHooks.useState(null);
  const [alerts, setAlerts] = preactHooks.useState([]);
  const [events, setEvents] = preactHooks.useState([]);
  const [streamState, setStreamState] = preactHooks.useState("connecting");
  const [speedRange, setSpeedRange] = preactHooks.useState("24h");
  const [spendRange, setSpendRange] = preactHooks.useState("24h");
  const speedChart = preactHooks.useRef(null);
  const speedDom = preactHooks.useRef(null);
  const spendChart = preactHooks.useRef(null);
  const spendDom = preactHooks.useRef(null);

  const loadCharts = preactHooks.useCallback(async () => {
    try {
      const speed = await fetch(
        "/api/v1/metrics/speed?range=" + speedRange,
      ).then((r) => r.json());
      const rows = (speed.series || []).filter((p) => p.ts);
      if (speedDom.current) {
        const times = rows.map((p) => p.ts);
        const answers = rows.map((p) => p.answers_ps);
        const limit = rows.map((p) => p.limit);
        const inflight = rows.map((p) => p.inflight);
        const options = {
          width: speedDom.current.clientWidth || 600,
          height: 180,
          cursor: { show: false },
          axes: [
            {},
            { size: 60 },
          ],
          series: [
            {},
            { label: "answers/s", stroke: "#b45229", width: 2 },
            { label: "limit", stroke: "#426d96", width: 1.5 },
            { label: "inflight", stroke: "#65636d", width: 1 },
          ],
          scales: { x: { time: false } },
          legend: { show: true },
        };
        if (speedChart.current) speedChart.current.destroy();
        speedChart.current = new uPlot(
          options,
          [times, answers, limit, inflight],
          speedDom.current,
        );
      }
      const spend = await fetch(
        "/api/v1/metrics/spend?range=" + spendRange,
      ).then((r) => r.json());
      const hours = (spend.hours || []).filter((h) => h.ts_hour);
      if (spendDom.current) {
        const times = hours.map((h) => h.ts_hour);
        const peak = hours.map((h) => Number(h.peak_usd || 0));
        const offpeak = hours.map((h) => Number(h.offpeak_usd || 0));
        const options = {
          width: spendDom.current.clientWidth || 600,
          height: 160,
          cursor: { show: false },
          axes: [{}, { size: 60, values: (u, vals) => vals.map((v) => "$" + v.toFixed(4)) }],
          series: [
            {},
            { label: "peak", stroke: "#b45229", width: 2, fill: "rgba(180,82,41,0.15)" },
            { label: "off-peak", stroke: "#426d96", width: 2, fill: "rgba(66,109,150,0.12)" },
          ],
          scales: { x: { time: false } },
          legend: { show: true },
        };
        if (spendChart.current) spendChart.current.destroy();
        spendChart.current = new uPlot(options, [times, peak, offpeak], spendDom.current);
      }
    } catch (error) {
      /* charts are best-effort; the KPIs carry the live numbers */
    }
  }, [speedRange, spendRange]);

  preactHooks.useEffect(() => {
    const source = new EventSource("/api/v1/stream");
    source.addEventListener("summary", (message) => {
      try {
        setSummary(JSON.parse(message.data));
        setStreamState("live");
      } catch (error) {
        setStreamState("parse error");
      }
    });
    source.addEventListener("alerts", (message) => {
      try {
        setAlerts(JSON.parse(message.data).items || []);
      } catch (error) {
        /* ignored */
      }
    });
    source.addEventListener("events", (message) => {
      try {
        const incoming = JSON.parse(message.data).items || [];
        setEvents((old) =>
          incoming.concat(old).slice(0, 12).sort((a, b) => b.seq - a.seq),
        );
      } catch (error) {
        /* ignored */
      }
    });
    source.onerror = () => setStreamState("reconnecting");
    loadCharts();
    const timer = setInterval(loadCharts, 15000);
    return () => {
      source.close();
      clearInterval(timer);
    };
  }, [loadCharts]);

  const counts = (summary && summary.counts) || {};
  const sync = (summary && summary.sync) || {};

  return preact.h(
    "div",
    null,
    preact.h(
      "div",
      { className: "kpi-row stats-kpis" },
      preact.h("div", { className: "kpi" },
        preact.h("span", { className: "kpi-value" },
          (summary ? summary.inflight : "–") + "/" + (summary ? summary.limit : "–")),
        preact.h("span", { className: "muted" }, "requests / limit")),
      preact.h("div", { className: "kpi" },
        preact.h("span", { className: "kpi-value" }, counts.nodes ?? "–"),
        preact.h("span", { className: "muted" }, "nodes")),
      preact.h("div", { className: "kpi" },
        preact.h("span", { className: "kpi-value" }, counts.settled ?? "–"),
        preact.h("span", { className: "muted" }, "settled")),
      preact.h("div", { className: "kpi" },
        preact.h("span", { className: "kpi-value" },
          summary ? usd(summary.spend_today_usd) : "–"),
        preact.h("span", { className: "muted" }, "spend today / " +
          (summary ? usd(summary.budget_usd) : "–"))),
      preact.h("div", { className: "kpi" },
        preact.h("span", { className: "kpi-value" }, counts.review ?? "–"),
        preact.h("span", { className: "muted" }, "in review")),
      preact.h("div", { className: "kpi" },
        preact.h("span", { className: "kpi-value" }, sync.rounds_completed ?? "–"),
        preact.h("span", { className: "muted" }, "sync rounds")),
    ),
    preact.h(
      "div",
      { className: "kpi-row" },
      preact.h("span", { className: "conn " + (streamState === "live" ? "ok" : "err") },
        streamState),
      preact.h("span", { className: "muted" },
        "last sync " + (sync.seconds_since_last_round ?? "–") + " s ago"),
    ),
    preact.h(
      "section",
      null,
      preact.h("h2", null, "Alerts"),
      alerts.length === 0 &&
        preact.h("p", { className: "muted" }, "no alerts"),
      preact.h(
        "ul",
        { className: "alert-list" },
        alerts.map((alertItem) =>
          preact.h("li", { key: alertItem.id, className: "alert " + alertItem.severity },
            preact.h("strong", null, alertItem.title),
            preact.h("span", { className: "muted" }, " " + alertItem.detail),
            preact.h(
              "span",
              { className: "alert-actions" },
              (alertItem.actions || []).map((action) =>
                preact.h(
                  "button",
                  {
                    key: action.path,
                    onClick: () => {
                      if (action.confirm &&
                          !confirm(action.label + "?")) return;
                      fetch(action.path, {
                        method: action.method,
                        headers: { "Content-Type": "application/json" },
                        body: JSON.stringify({}),
                      });
                    },
                  },
                  action.label,
                ),
              ),
            ))),
      ),
    ),
    preact.h(
      "section",
      null,
      preact.h("h2", null, "Speed"),
      preact.h(
        "div",
        { className: "range-switch" },
        SPEED_RANGES.map(([label]) =>
          preact.h("button", {
            key: label,
            className: speedRange === label ? "active" : "",
            onClick: () => setSpeedRange(label),
          }, label)),
      ),
      preact.h("div", { ref: speedDom, className: "chart" }),
    ),
    preact.h(
      "section",
      null,
      preact.h("h2", null, "Spend"),
      preact.h(
        "div",
        { className: "range-switch" },
        SPEND_RANGES.map(([label]) =>
          preact.h("button", {
            key: label,
            className: spendRange === label ? "active" : "",
            onClick: () => setSpendRange(label),
          }, label)),
      ),
      preact.h("div", { ref: spendDom, className: "chart" }),
    ),
    preact.h(
      "div",
      { className: "stats-tables" },
      preact.h(
        "section",
        null,
        preact.h("h2", null, "Campaigns"),
        preact.h(
          "table",
          { className: "data-table" },
          preact.h("thead", null,
            preact.h("tr", null,
              preact.h("th", null, "id"), preact.h("th", null, "state"),
              preact.h("th", null, "reason"), preact.h("th", null, "reserved"))),
          preact.h("tbody", null,
            (summary ? summary.campaigns : []).map((campaign) =>
              preact.h("tr", { key: campaign.id },
                preact.h("td", { className: "mono" }, campaign.id),
                preact.h("td", null, campaign.state),
                preact.h("td", null, campaign.reason || ""),
                preact.h("td", null, campaign.reserved)),
            ))),
      ),
      preact.h(
        "section",
        null,
        preact.h("h2", null, "Fleet"),
        preact.h(
          "table",
          { className: "data-table" },
          preact.h("thead", null,
            preact.h("tr", null,
              preact.h("th", null, "device"), preact.h("th", null, "state"))),
          preact.h("tbody", null,
            (summary ? summary.members : []).map((member) =>
              preact.h("tr", { key: member.device_id },
                preact.h("td", { className: "mono" }, member.device_id),
                preact.h("td", null, member.state)),
            ))),
      ),
    ),
    preact.h(
      "section",
      null,
      preact.h("h2", null, "Recent activity"),
      preact.h(
        "ul",
        { className: "activity-list" },
        events.map((event) =>
          preact.h("li", { key: event.seq },
            preact.h("span", { className: "mono muted" },
              new Date(event.ts * 1000).toLocaleTimeString()),
            preact.h("span", null, " " + event.kind),
            preact.h("span", { className: "muted" }, " " + event.detail)),
        ),
      ),
    ),
  );
};
