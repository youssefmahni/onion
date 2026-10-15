"use strict";

/* Hash router: #/control, #/campaigns/<id>, #/fleet, #/review/<id>,
   #/tree/<path>, #/search?q=, #/stats. Screens arrive in D1+. */

const SCREENS = [
  ["control", "Control center", "Alerts, live numbers, charts, activity."],
  ["campaigns", "Campaigns", "List, details, projection, plan-before-start."],
  ["fleet", "Fleet", "Members, lanes, invitations, evictions."],
  ["review", "Review queue", "Items by reason, attempts, accept/retry/leaf."],
  ["tree", "Tree Explorer", "Lazy tree, node details, history."],
  ["search", "Tree Search", "Full-text search, filters, path jump."],
  ["stats", "Tree Stats", "Per-level counts, phases, endings."],
];

function screen_root(route) {
  const main = document.getElementById("screen");
  const key = route.split("/")[0] || "control";
  const def = SCREENS.find((entry) => entry[0] === key) || SCREENS[0];
  const node = document.createElement("div");
  const h1 = document.createElement("h1");
  h1.textContent = def[1];
  const p = document.createElement("p");
  p.className = "muted";
  p.textContent = def[2] + " (arrives in a later phase)";
  node.appendChild(h1);
  node.appendChild(p);
  main.replaceChildren(node);
  for (const link of document.querySelectorAll("[data-nav]")) {
    const active = link.dataset.nav === key;
    link.setAttribute("aria-current", active ? "page" : "false");
  }
  main.focus({ preventScroll: true });
}

function route() {
  const hash = location.hash.replace(/^#\/?/, "");
  screen_root(hash || "control");
  document.title = "Onion — Control dashboard";
}

async function probe() {
  const conn = document.getElementById("conn");
  const version = document.getElementById("version");
  try {
    const response = await fetch("/api/v1/version", { headers: { Accept: "application/json" } });
    if (!response.ok) throw new Error(String(response.status));
    const body = await response.json();
    conn.textContent = "connected";
    conn.className = "conn ok";
    version.textContent = "api " + body.version;
  } catch (error) {
    conn.textContent = "offline";
    conn.className = "conn err";
    version.textContent = "";
  }
}

window.addEventListener("hashchange", route);
window.addEventListener("load", () => {
  route();
  probe();
});

/* '/' focuses search once the search screen exists (D1). */
document.addEventListener("keydown", (event) => {
  if (event.key === "/" && !event.target.closest("input,textarea")) {
    event.preventDefault();
  }
});
