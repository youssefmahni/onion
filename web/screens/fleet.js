"use strict";

/* Fleet (screen 3): members with live states, invitations (the token is
   shown once), eviction proposals with sign/reject, and this device's
   actions (LLM key — never echoed — and restart with a typed confirm). */

const FleetScreen = () => {
  const [members, setMembers] = preactHooks.useState([]);
  const [invites, setInvites] = preactHooks.useState([]);
  const [evictions, setEvictions] = preactHooks.useState([]);
  const [pubkey, setPubkey] = preactHooks.useState("");
  const [ttlHours, setTtlHours] = preactHooks.useState("24");
  const [issuedToken, setIssuedToken] = preactHooks.useState("");
  const [evictDevice, setEvictDevice] = preactHooks.useState("");
  const [llmKey, setLlmKey] = preactHooks.useState("");
  const [restartConfirm, setRestartConfirm] = preactHooks.useState("");
  const [message, setMessage] = preactHooks.useState("");

  const load = preactHooks.useCallback(async () => {
    const [m, i, e] = await Promise.all([
      fetch("/api/v1/members").then((r) => r.json()),
      fetch("/api/v1/invites").then((r) => r.json()),
      fetch("/api/v1/evictions").then((r) => r.json()),
    ]);
    setMembers(m.items || []);
    setInvites(i.items || []);
    setEvictions(e.items || []);
  }, []);

  preactHooks.useEffect(() => {
    load();
    const timer = setInterval(load, 10000);
    return () => clearInterval(timer);
  }, [load]);

  const post = async (path, body, okStatus) => {
    const response = await fetch(path, {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        "X-Onion-Request": "1",
        "Idempotency-Key": crypto.randomUUID(),
      },
      body: JSON.stringify(body || {}),
    });
    const json = await response.json().catch(() => ({}));
    if (response.status === okStatus) return json;
    setMessage(json.error || "request failed (" + response.status + ")");
    return null;
  };

  const mintInvite = async () => {
    if (!/^[0-9a-f]{64}$/.test(pubkey.trim())) {
      setMessage("the pubkey must be 64 hex characters");
      return;
    }
    const result = await post("/api/v1/invites",
      { pubkey: pubkey.trim(), ttl_hours: Number(ttlHours) || 24 }, 200);
    if (result) {
      setIssuedToken(result.token);
      load();
    }
  };

  const proposeEviction = async () => {
    const result = await post("/api/v1/evictions",
      { device_id: evictDevice.trim() }, 202);
    if (result) {
      setMessage("proposed: " + result.op_id);
      load();
    }
  };

  const evictionAction = async (id, action) => {
    const result = await post("/api/v1/evictions/" + id + "/" + action, {}, 202);
    if (result) {
      setMessage("ok: " + result.op_id);
      load();
    }
  };

  const setKey = async () => {
    const result = await post("/api/v1/device/llm-key", { key: llmKey }, 200);
    if (result && result.set) {
      setLlmKey("");
      setMessage("key stored (never echoed)");
    }
  };

  const restart = async () => {
    const result = await post("/api/v1/device/restart-service",
      { confirm: restartConfirm }, 202);
    if (result) setMessage("restart accepted — the service is going down");
  };

  return preact.h(
    "div",
    null,
    preact.h(
      "section",
      null,
      preact.h("h2", null, "Members"),
      preact.h(
        "table",
        { className: "data-table" },
        preact.h("thead", null,
          preact.h("tr", null,
            preact.h("th", null, "device"), preact.h("th", null, "state"),
            preact.h("th", null, "live"), preact.h("th", null, "lane"),
            preact.h("th", null, "slot"), preact.h("th", null, "sync url"))),
        preact.h("tbody", null,
          members.map((member) =>
            preact.h("tr", { key: member.device_id },
              preact.h("td", { className: "mono" }, member.device_id),
              preact.h("td", null, member.state),
              preact.h("td", null, member.live_state || "—"),
              preact.h("td", null, member.lane_weight),
              preact.h("td", null, member.slot),
              preact.h("td", { className: "mono" }, member.sync_url || ""))),
        ),
      ),
    ),
    preact.h(
      "div",
      { className: "stats-tables" },
      preact.h(
        "section",
        null,
        preact.h("h2", null, "Invitations"),
        preact.h("label", null, preact.h("span", { className: "muted" }, "pubkey (64 hex)")),
        preact.h("input", { type: "text", value: pubkey,
          onInput: (e) => setPubkey(e.target.value), "aria-label": "invite pubkey" }),
        preact.h("label", null, preact.h("span", { className: "muted" }, "ttl hours")),
        preact.h("input", { type: "number", value: ttlHours,
          onInput: (e) => setTtlHours(e.target.value), "aria-label": "invite ttl" }),
        preact.h("p", null, preact.h("button", { onClick: mintInvite }, "issue invitation")),
        issuedToken && preact.h("pre", { className: "plan-report" },
          "show the joiner ONCE:\\n" + issuedToken),
        preact.h(
          "table",
          { className: "data-table" },
          preact.h("thead", null,
            preact.h("tr", null,
              preact.h("th", null, "id"), preact.h("th", null, "expires"),
              preact.h("th", null, "expired"))),
          preact.h("tbody", null,
            invites.map((invite) =>
              preact.h("tr", { key: invite.id },
                preact.h("td", { className: "mono" }, invite.id),
                preact.h("td", null, new Date(invite.expires_at * 1000).toLocaleString()),
                preact.h("td", null, invite.expired ? "yes" : "no"))),
          ),
        ),
      ),
      preact.h(
        "section",
        null,
        preact.h("h2", null, "Evictions"),
        preact.h("label", null, preact.h("span", { className: "muted" }, "device id to propose")),
        preact.h("input", { type: "text", value: evictDevice,
          onInput: (e) => setEvictDevice(e.target.value), "aria-label": "eviction device" }),
        preact.h("p", null, preact.h("button", { onClick: proposeEviction }, "propose eviction")),
        preact.h(
          "table",
          { className: "data-table" },
          preact.h("thead", null,
            preact.h("tr", null,
              preact.h("th", null, "device"), preact.h("th", null, "sigs"),
              preact.h("th", null, "actions"))),
          preact.h("tbody", null,
            evictions.map((proposal) =>
              preact.h("tr", { key: proposal.id },
                preact.h("td", { className: "mono" }, proposal.device_id),
                preact.h("td", null, (proposal.sigs || []).length),
                preact.h("td", null,
                  preact.h("button", {
                    onClick: () => evictionAction(proposal.id, "sign"),
                  }, "sign"),
                  preact.h("button", {
                    onClick: () => evictionAction(proposal.id, "reject"),
                  }, "reject")))),
          ),
        ),
      ),
      preact.h(
        "section",
        null,
        preact.h("h2", null, "This device"),
        preact.h("label", null, preact.h("span", { className: "muted" }, "DeepSeek API key (stored, never shown)")),
        preact.h("input", { type: "password", value: llmKey,
          onInput: (e) => setLlmKey(e.target.value), "aria-label": "llm key" }),
        preact.h("p", null, preact.h("button", { onClick: setKey }, "store key")),
        preact.h("label", null, preact.h("span", { className: "muted" }, "type \"restart\" to restart the service")),
        preact.h("input", { type: "text", value: restartConfirm,
          onInput: (e) => setRestartConfirm(e.target.value), "aria-label": "restart confirm" }),
        preact.h("p", null, preact.h("button", {
          onClick: restart,
          disabled: restartConfirm !== "restart",
        }, "restart service")),
      ),
    ),
    message && preact.h("p", { className: "message" }, message),
  );
};
