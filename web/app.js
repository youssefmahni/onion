"use strict";

/* Router, store skeleton, connectivity probe. Screens arrive per phase:
   D1 wires the tree explorer, search and stats (5–7); control, campaigns,
   fleet and review land in D2–D5 and stay stubs until then. */

let readOnly = false;
let versionInfo = "";

const root = document.getElementById("screen");

const SCREEN_DEFS = [
  ["control", "Control center", ""],
  ["campaigns", "Campaigns", ""],
  ["fleet", "Fleet", ""],
  ["review", "Review queue", ""],
];

function stub(key, title, note) {
  const wrap = document.createElement("div");
  const h1 = document.createElement("h1");
  h1.textContent = title;
  const p = document.createElement("p");
  p.className = "muted";
  p.textContent = note;
  wrap.appendChild(h1);
  wrap.appendChild(p);
  root.replaceChildren(wrap);
  document.title = "Onion — " + title;
}

function render(preactNode) {
  preact.render(preactNode, root);
}

function route() {
  const hash = location.hash.replace(/^#\/?/, "");
  const [key, rest] = hash.split("/");
  const query = key.includes("?") ? key.slice(key.indexOf("?") + 1) : "";
  const screen = key.split("?")[0] || (readOnly ? "tree" : "control");

  for (const link of document.querySelectorAll("[data-nav]")) {
    const active = link.dataset.nav === screen;
    link.setAttribute("aria-current", active ? "page" : "false");
  }

  switch (screen) {
    case "control":
      render(preact.h(ControlScreen, null));
      break;
    case "campaigns":
      render(preact.h(CampaignsScreen, null));
      break;
    case "review":
      render(preact.h(ReviewScreen, null));
      break;
    case "fleet":
      render(preact.h(FleetScreen, null));
      break;
    case "tree": {
      const path = rest ? decodeURIComponent(rest) : "";
      render(preact.h(TreeScreen, { initialPath: path }));
      break;
    }
    case "search":
      render(preact.h(SearchScreen, null));
      break;
    case "stats":
      render(preact.h(StatsScreen, null));
      break;
    default: {
      const def = SCREEN_DEFS.find((entry) => entry[0] === screen);
      if (def) stub(def[0], def[1], def[2]);
      else stub("control", "Onion", "unknown route — pick a screen above");
      break;
    }
  }
  document.title = "Onion — Control dashboard";
}

async function probe() {
  const conn = document.getElementById("conn");
  const version = document.getElementById("version");
  try {
    const response = await fetch("/api/v1/version", {
      headers: { Accept: "application/json" },
    });
    if (!response.ok) throw new Error(String(response.status));
    const body = await response.json();
    readOnly = body.read_only === true;
    versionInfo = body.version + "/" + body.phase;
    conn.textContent = readOnly ? "read-only" : "connected";
    conn.className = "conn ok";
    version.textContent = "api " + versionInfo;
  } catch (error) {
    conn.textContent = "offline";
    conn.className = "conn err";
    version.textContent = "";
  }
  for (const link of document.querySelectorAll("[data-nav]")) {
    if (readOnly && !["tree", "search", "stats"].includes(link.dataset.nav))
      link.style.display = "none";
  }
  route();
}

window.addEventListener("hashchange", route);
window.addEventListener("load", probe);

/* '/' focuses the search box (plan §7); from anywhere else it opens
   the search screen. */
document.addEventListener("keydown", (event) => {
  if (event.key !== "/") return;
  if (event.target.closest("input,textarea,select")) return;
  event.preventDefault();
  const searchInput = document.querySelector(
    'input[type="search"], input[aria-label="search"]',
  );
  if (searchInput) {
    searchInput.focus();
    searchInput.select();
  } else {
    location.hash = "#/search";
  }
});
