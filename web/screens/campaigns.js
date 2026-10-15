"use strict";

/* Campaigns (screen 2): list with details, plan-before-start, pause,
   resume, budget, settings, stop. Actions POST with Idempotency-Key and
   the §5.3 headers; responses are 202 {op_id} and the op event lands on
   the stream. */

const CAMPAIGN_HEADERS = {
  "Content-Type": "application/json",
  "X-Onion-Request": "1",
};

async function v1(method, path, body) {
  const response = await fetch(path, {
    method,
    headers: body
      ? { ...CAMPAIGN_HEADERS, "Idempotency-Key": crypto.randomUUID() }
      : undefined,
    body: body ? JSON.stringify(body) : undefined,
  });
  const json = await response.json().catch(() => ({}));
  return { status: response.status, json };
}

const CampaignsScreen = () => {
  const [campaigns, setCampaigns] = preact.useState([]);
  const [selected, setSelected] = preact.useState(null);
  const [detail, setDetail] = preact.useState(null);
  const [planSpec, setPlanSpec] = preact.useState(
    '{"id": "c1", "scope": [""], "loop_budget": 100}',
  );
  const [plan, setPlan] = preact.useState(null);
  const [planId, setPlanId] = preact.useState("");
  const [message, setMessage] = preact.useState("");
  const [confirmText, setConfirmText] = preact.useState("");
  const [events, setEvents] = preact.useState([]);

  const load = preact.useCallback(async () => {
    const response = await fetch("/api/v1/campaigns").then((r) => r.json());
    setCampaigns((response.items || []).sort((a, b) =>
      (a.state === "running" ? 0 : 1) - (b.state === "running" ? 0 : 1) ||
      a.id.localeCompare(b.id),
    ));
  }, []);

  preact.useEffect(() => {
    load();
    const timer = setInterval(load, 5000);
    const source = new EventSource("/api/v1/stream");
    source.addEventListener("op", (message) => {
      try {
        const op = JSON.parse(message.data);
        setEvents((old) =>
          [op, ...old].slice(0, 20),
        );
        setMessage("op " + op.op_id + " → " + op.status);
        load();
      } catch (error) {
        /* ignored */
      }
    });
    return () => {
      clearInterval(timer);
      source.close();
    };
  }, [load]);

  const open = preact.useCallback(async (id) => {
    setSelected(id);
    const detail = await fetch("/api/v1/campaigns/" + id).then((r) => r.json());
    setDetail(detail);
    setConfirmText("");
  }, []);

  const act = async (id, action, body) => {
    const result = await v1("POST", "/api/v1/campaigns/" + id + "/" + action, body);
    if (result.status !== 202)
      setMessage(result.json.error || "action failed (" + result.status + ")");
    else setMessage("submitted: " + result.json.op_id);
  };

  const runPlan = async () => {
    setMessage("");
    let spec;
    try {
      spec = JSON.parse(planSpec);
    } catch (error) {
      setMessage("the spec is not valid JSON");
      return;
    }
    const result = await v1("POST", "/api/v1/campaigns/plan", spec);
    if (result.status === 200) {
      setPlan(result.json);
      setPlanId(result.json.plan_id);
    } else {
      setMessage(result.json.error || "plan failed");
    }
  };

  const startPlanned = async () => {
    if (!planId) {
      setMessage("run the plan first — you must see the projection");
      return;
    }
    const result = await v1("POST", "/api/v1/campaigns", {
      spec: JSON.parse(planSpec),
      plan_id: planId,
    });
    if (result.status === 202) setMessage("started: " + result.json.op_id);
    else setMessage(result.json.error || "start rejected");
  };

  return preact.h(
    "div",
    { className: "campaigns-layout" },
    preact.h(
      "section",
      { className: "campaigns-list" },
      preact.h("h2", null, "Campaigns"),
      preact.h(
        "table",
        { className: "data-table" },
        preact.h("thead", null,
          preact.h("tr", null,
            preact.h("th", null, "id"), preact.h("th", null, "state"),
            preact.h("th", null, "used"), preact.h("th", null, "reserved"))),
        preact.h("tbody", null,
          campaigns.map((campaign) =>
            preact.h("tr", {
              key: campaign.id,
              className: selected === campaign.id ? "selected" : "",
              onClick: () => open(campaign.id),
            },
              preact.h("td", { className: "mono" }, campaign.id),
              preact.h("td", null, campaign.state +
                (campaign.reason ? " (" + campaign.reason + ")" : "")),
              preact.h("td", null, campaign.budget_used_usd),
              preact.h("td", null, campaign.reserved))),
        ),
      ),
      preact.h("h3", null, "Plan before start"),
      preact.h("textarea", {
        rows: 4,
        value: planSpec,
        onInput: (e) => setPlanSpec(e.target.value),
        "aria-label": "campaign spec",
      }),
      preact.h("p", null,
        preact.h("button", { onClick: runPlan }, "show projection"),
        preact.h("button", { onClick: startPlanned, disabled: !planId },
          "start (needs a fresh plan)")),
      plan && preact.h("pre", { className: "plan-report" }, plan.report || ""),
    ),
    preact.h(
      "section",
      { className: "campaigns-detail" },
      detail
        ? preact.h(
            preact.Fragment,
            null,
            preact.h("h2", null, detail.id),
            preact.h(
              "div",
              { className: "kpi-row" },
              preact.h("span", { className: "badge " + (detail.state === "running" ? "settled" : "") },
                detail.state),
              preact.h("span", { className: "muted" },
                "used $" + detail.budget_used_usd),
              preact.h("span", { className: "muted" },
                "reserved " + detail.reserved),
            ),
            preact.h(
              "div",
              { className: "kpi-row" },
              detail.state === "running" &&
                preact.h("button", { onClick: () => act(detail.id, "pause", {}) }, "pause"),
              detail.state === "paused" &&
                preact.h("button", { onClick: () => act(detail.id, "resume", {}) }, "resume"),
              detail.state !== "stopped" &&
                preact.h("button", {
                  onClick: () => act(detail.id, "budget", { add_attempts: 100 }),
                }, "+100 budget"),
              detail.state !== "stopped" &&
                preact.h(
                  preact.Fragment,
                  null,
                  preact.h("input", {
                    type: "text",
                    placeholder: "type the campaign id to stop",
                    value: confirmText,
                    onInput: (e) => setConfirmText(e.target.value),
                    "aria-label": "stop confirmation",
                  }),
                  preact.h("button", {
                    onClick: () => act(detail.id, "stop", { confirm: confirmText }),
                    disabled: confirmText !== detail.id,
                  }, "stop"),
                ),
            ),
            preact.h("h3", null, "Settles by end"),
            preact.h(
              "table",
              { className: "data-table" },
              preact.h("thead", null,
                preact.h("tr", null,
                  preact.h("th", null, "end"), preact.h("th", null, "n"))),
              preact.h("tbody", null,
                (detail.settles_by_end || []).map((row) =>
                  preact.h("tr", { key: row.end },
                    preact.h("td", null, row.end),
                    preact.h("td", null, row.n)),
                )),
            ),
            preact.h("h3", null, "Settles per level"),
            preact.h(
              "table",
              { className: "data-table" },
              preact.h("thead", null,
                preact.h("tr", null,
                  preact.h("th", null, "layer"), preact.h("th", null, "settled"))),
              preact.h("tbody", null,
                (detail.settles_per_level || []).map((row) =>
                  preact.h("tr", { key: row.layer },
                    preact.h("td", null, row.layer),
                    preact.h("td", null, row.settled)),
                )),
            ),
          )
        : preact.h("p", { className: "muted" }, "select a campaign"),
      message && preact.h("p", { className: "message" }, message),
      preact.h("h3", null, "Operations"),
      preact.h("ul", { className: "activity-list" },
        events.map((op) =>
          preact.h("li", { key: op.op_id + op.status },
            preact.h("span", { className: "mono muted" }, op.op_id),
            preact.h("span", null, " " + op.status)))),
    ),
  );
};
