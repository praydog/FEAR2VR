"use strict";
/*
 * FEAR2 Inspector -- vanilla JS, no build step, no dependencies.
 *
 * Talks directly to the injected DLL's HTTP server (default
 * http://127.0.0.1:8798). Every fetch goes through apiGet(), which flips a
 * global "unreachable" banner on network/parse failure and clears it on the
 * next success -- views never blank themselves on a failed poll, they just
 * keep showing the last good render underneath the banner.
 */

// ------------------------------------------------------------------ config

const DEFAULT_BASE_URL = "http://127.0.0.1:8798";
const LS_BASE_URL = "fear2-inspector-base-url";
const LS_LIVE_INTERVAL = "fear2-inspector-live-interval";
const LS_ACTIVE_TAB = "fear2-inspector-active-tab";
const MAX_LIMIT = 500; // the API's paging maximum -- never request more

const TABS = ["live", "subsystems", "database", "find", "console", "vars", "raw"];

// ------------------------------------------------------------------ small helpers

function $(id) {
  return document.getElementById(id);
}

function dash(v) {
  return v === null || v === undefined || v === "" ? "\u2014" : v;
}

function fmtBool(v) {
  if (v === null || v === undefined) return "\u2014";
  return v ? "yes" : "no";
}

function fmtNum(v, digits) {
  if (v === null || v === undefined || typeof v !== "number" || Number.isNaN(v)) return "\u2014";
  return typeof digits === "number" ? v.toFixed(digits) : String(v);
}

function fmtRatio(v, max) {
  if (v === null || v === undefined || max === null || max === undefined) return "\u2014";
  return `${v} / ${max}`;
}

function fmtVec3(v) {
  if (!Array.isArray(v) || v.length < 3) return "\u2014";
  return `(${fmtNum(v[0], 2)}, ${fmtNum(v[1], 2)}, ${fmtNum(v[2], 2)})`;
}

function fmtPair(v) {
  if (!Array.isArray(v) || v.length < 2) return "\u2014";
  return `${fmtNum(v[0], 3)} \u2026 ${fmtNum(v[1], 3)}`;
}

function clampLimit(n) {
  n = Number(n);
  if (!Number.isFinite(n) || n <= 0) return 1;
  return Math.min(MAX_LIMIT, Math.floor(n));
}

function debounce(fn, ms) {
  let t = null;
  return (...args) => {
    clearTimeout(t);
    t = setTimeout(() => fn(...args), ms);
  };
}

/** Build a <td>, defaulting null/undefined/empty to an em dash. */
function cell(text, cls) {
  const td = document.createElement("td");
  if (cls) td.className = cls;
  td.textContent = text === null || text === undefined || text === "" ? "\u2014" : String(text);
  return td;
}

/** Build a <td> containing a clickable link, for cross-view navigation. */
function linkCell(text, onClick, cls) {
  const td = document.createElement("td");
  if (cls) td.className = cls;
  const a = document.createElement("a");
  a.href = "#";
  a.textContent = text;
  a.addEventListener("click", (e) => {
    e.preventDefault();
    onClick();
  });
  td.appendChild(a);
  return td;
}

function emptyRow(colspan, message) {
  const tr = document.createElement("tr");
  const td = document.createElement("td");
  td.colSpan = colspan;
  td.className = "muted empty-row";
  td.textContent = message || "No data.";
  tr.appendChild(td);
  return tr;
}

/** Replace a <tbody>'s rows with one row per item, or an empty-row placeholder. */
function renderTable(tbody, items, colspan, rowBuilder, emptyMessage) {
  tbody.replaceChildren();
  if (!items || items.length === 0) {
    tbody.appendChild(emptyRow(colspan, emptyMessage));
    return;
  }
  for (const item of items) {
    tbody.appendChild(rowBuilder(item));
  }
}

// ------------------------------------------------------------------ API layer

function getBaseUrl() {
  return (localStorage.getItem(LS_BASE_URL) || DEFAULT_BASE_URL).trim();
}

function setBaseUrl(url) {
  const trimmed = (url || "").trim() || DEFAULT_BASE_URL;
  localStorage.setItem(LS_BASE_URL, trimmed);
}

function setUnreachable(bad) {
  $("banner").hidden = !bad;
}

/**
 * GET `path` (must start with "/") against the configured base URL, parsed
 * as JSON. Network failure or a non-JSON body flips the global banner and
 * rethrows; callers that want to leave stale data on screen should just
 * catch-and-ignore. A parsed body -- even one carrying {"ok":false,...} --
 * is a successful round trip and clears the banner.
 */
async function apiGet(path) {
  const base = getBaseUrl().replace(/\/+$/, "");
  const url = base + path;
  let res;
  try {
    res = await fetch(url, { cache: "no-store", mode: "cors" });
  } catch (err) {
    setUnreachable(true);
    throw err;
  }
  let json;
  try {
    json = await res.json();
  } catch (err) {
    setUnreachable(true);
    throw err;
  }
  setUnreachable(false);
  return json;
}

// ------------------------------------------------------------------ tabs

let liveLoadedOnce = false;
let subsystemsLoadedOnce = false;
let dbLoadedOnce = false;
let consoleLoadedOnce = false;
let varsLoadedOnce = false;

function activateTab(name) {
  if (!TABS.includes(name)) name = "live";
  for (const t of TABS) {
    $("view-" + t).hidden = t !== name;
    $("tab-" + t).classList.toggle("active", t === name);
  }
  localStorage.setItem(LS_ACTIVE_TAB, name);

  if (name === "live" && !liveLoadedOnce) {
    liveLoadedOnce = true;
    liveTick();
  } else if (name === "subsystems" && !subsystemsLoadedOnce) {
    subsystemsLoadedOnce = true;
    subsystemsLoad();
  } else if (name === "database" && !dbLoadedOnce) {
    dbLoadedOnce = true;
    dbLoadCategories();
  } else if (name === "console" && !consoleLoadedOnce) {
    consoleLoadedOnce = true;
    consoleLoad();
  } else if (name === "vars" && !varsLoadedOnce) {
    varsLoadedOnce = true;
    varsLoad();
  }
}

// ================================================================== LIVE STATE

let livePollTimer = null;
let liveInFlight = false;

function liveIntervalMs() {
  return Number($("live-interval").value) || 0;
}

function scheduleLivePoll() {
  if (livePollTimer) {
    clearInterval(livePollTimer);
    livePollTimer = null;
  }
  const ms = liveIntervalMs();
  localStorage.setItem(LS_LIVE_INTERVAL, String(ms));
  if (ms <= 0) return;
  liveTick();
  livePollTimer = setInterval(liveTick, ms);
}

async function liveTick() {
  if (liveInFlight) return;
  liveInFlight = true;
  try {
    const data = await apiGet("/api/state");
    renderLiveState(data);
  } catch (err) {
    // banner already raised by apiGet; keep the last good render on screen.
  } finally {
    liveInFlight = false;
  }
}

function setBar(id, value, max) {
  const fill = $(id);
  fill.classList.remove("bar-good", "bar-warn", "bar-bad");
  if (typeof value !== "number" || typeof max !== "number" || max <= 0) {
    fill.style.width = "0%";
    return;
  }
  const ratio = Math.max(0, Math.min(1, value / max));
  fill.style.width = (ratio * 100).toFixed(1) + "%";
  fill.classList.add(ratio > 0.5 ? "bar-good" : ratio > 0.25 ? "bar-warn" : "bar-bad");
}

function renderLiveState(data) {
  if (!data || data.ok === false) return;

  const p = data.player || {};
  $("live-player-badge").hidden = !!p.resolved;
  $("live-player-inconsistent").hidden = p.consistent !== false;
  setBar("live-health-bar", p.health, p.max_health);
  $("live-health-num").textContent = fmtRatio(p.health, p.max_health);
  setBar("live-armor-bar", p.armor, p.max_armor);
  $("live-armor-num").textContent = fmtRatio(p.armor, p.max_armor);
  // air is a FRACTION in [0,1] per the SDK -- convert to a percentage for display.
  $("live-air").textContent = typeof p.air === "number" ? fmtNum(p.air * 100, 1) + "%" : "\u2014";
  $("live-health-lost").textContent = dash(p.health_lost);
  $("live-alive").textContent = fmtBool(p.alive);
  $("live-consistent").textContent = fmtBool(p.consistent);

  const m = data.movement || {};
  $("live-move-flags").textContent = typeof m.flags === "number" ? "0x" + (m.flags >>> 0).toString(16).padStart(8, "0") : dash(m.flags);
  $("live-crouching").textContent = fmtBool(m.crouching);
  $("live-moving").textContent = fmtBool(m.moving);
  $("live-speed").textContent = fmtNum(m.speed, 2);
  $("live-velocity").textContent = fmtVec3(m.velocity);
  $("live-water").textContent = fmtBool(m.water_affects_speed);
  $("live-spectator").textContent = fmtNum(m.spectator_speed_mul, 2);

  const cam = data.camera || {};
  $("live-cam-clamp-record").textContent = dash(cam.clamp_record);
  $("live-cam-state").textContent = dash(cam.state);
  $("live-cam-chase").textContent = fmtBool(cam.is_chase);
  $("live-cam-predicted").textContent = dash(cam.predicted_clamp);
  $("live-cam-slide-kick").textContent = fmtBool(cam.slide_kick_unchecked);

  renderClampsTable(cam.clamps, cam.predicted_clamp);

  const before = cam.pitch_before;
  const after = cam.pitch_after;
  const timer = cam.timer || {};
  const dormant = before === 0 && after === 0 && timer.active !== true;
  $("live-pitch-dormant").hidden = !dormant;
  $("live-pitch-values").hidden = dormant;
  if (!dormant) {
    $("live-pitch-before").textContent = fmtNum(before, 3);
    $("live-pitch-after").textContent = fmtNum(after, 3);
    $("live-pitch-corrected").textContent = fmtBool(cam.pitch_corrected);
    $("live-timer-active").textContent = fmtBool(timer.active);
    $("live-timer-duration").textContent = fmtNum(timer.duration, 3);
    $("live-timer-source").textContent = timer.use_cached === null || timer.use_cached === undefined ? "\u2014" : timer.use_cached ? "cached" : "live";
  }

  const aim = cam.aim_tracking || {};
  $("live-aim-normal").textContent = typeof aim.normal_deg === "number" ? fmtNum(aim.normal_deg, 2) + "\u00b0" : "\u2014";
  $("live-aim-zoomed").textContent = typeof aim.zoomed_deg === "number" ? fmtNum(aim.zoomed_deg, 2) + "\u00b0" : "\u2014";
  $("live-aim-flag").textContent = fmtBool(aim.flag);
}

function renderClampsTable(clamps, predicted) {
  const tbody = $("live-clamps-tbody");
  const entries = clamps && typeof clamps === "object" ? Object.entries(clamps) : [];
  renderTable(tbody, entries, 3, ([name, c]) => {
    const tr = document.createElement("tr");
    if (name === predicted) tr.classList.add("row-highlight");
    tr.appendChild(cell(name, "mono"));
    tr.appendChild(cell(fmtPair(c && c.deg), "mono"));
    tr.appendChild(cell(fmtPair(c && c.rad), "mono"));
    return tr;
  }, "No clamp data.");
}

// ================================================================== SUBSYSTEMS

async function subsystemsLoad() {
  let data;
  try {
    data = await apiGet("/api/subsystems");
  } catch (err) {
    return;
  }
  if (!data || data.ok === false) return;
  renderSubsystems(data);
}

function renderSubsystems(data) {
  $("subsystems-player").textContent = dash(data.player);
  renderTable($("subsystems-tbody"), data.items, 8, (item) => {
    const tr = document.createElement("tr");
    const anomaly = item.is_class_instance === false || item.owner_is_player === false;
    if (anomaly) tr.classList.add("row-warn");
    tr.appendChild(cell(item.offset, "mono"));
    if (item.name === null || item.name === undefined) {
      tr.appendChild(cell("\u2039unnamed\u203a", "muted"));
    } else {
      tr.appendChild(cell(item.name));
    }
    tr.appendChild(cell(item.object, "mono"));
    tr.appendChild(cell(item.vtable, "mono"));
    tr.appendChild(cell(item.ctor, "mono"));
    tr.appendChild(cell(item.size_lower_bound));
    tr.appendChild(cell(item.delegate_nodes));
    const flagsTd = document.createElement("td");
    if (anomaly) {
      const badges = [];
      if (item.is_class_instance === false) badges.push("not a class instance");
      if (item.owner_is_player === false) badges.push("owner \u2260 player");
      flagsTd.textContent = badges.join("; ");
      flagsTd.className = "warn-text";
    } else {
      flagsTd.textContent = "\u2014";
      flagsTd.className = "muted";
    }
    tr.appendChild(flagsTd);
    return tr;
  }, "No subsystems reported.");
}

// ================================================================== DATABASE

const db = {
  catOffset: 0,
  catTotal: 0,
  categories: [],
  recCategory: null,
  recOffset: 0,
  recTotal: 0,
};

function dbCatLimit() {
  return clampLimit($("db-cat-pagesize").value);
}
function dbRecLimit() {
  return clampLimit($("db-rec-pagesize").value);
}

async function dbLoadCategories() {
  const limit = dbCatLimit();
  const params = new URLSearchParams({ offset: String(db.catOffset), limit: String(limit) });
  const filter = $("db-cat-filter").value.trim();
  if (filter) params.set("filter", filter);
  let data;
  try {
    data = await apiGet("/api/db/categories?" + params.toString());
  } catch (err) {
    return;
  }
  if (!data || data.ok === false) {
    renderTable($("db-cat-tbody"), [], 3, () => null, (data && data.error) || "Request failed.");
    return;
  }
  db.categories = data.items || [];
  db.catTotal = data.total || 0;
  renderDbCategories();
}

function renderDbCategories() {
  renderTable($("db-cat-tbody"), db.categories, 3, (c) => {
    const tr = document.createElement("tr");
    if (c.name === db.recCategory) tr.classList.add("row-selected");
    tr.appendChild(linkCell(c.name, () => dbSelectCategory(c.name)));
    tr.appendChild(cell(c.record_count));
    const keyedTd = document.createElement("td");
    keyedTd.textContent = fmtBool(c.keyed);
    if (c.keyed === false) keyedTd.className = "warn-text";
    tr.appendChild(keyedTd);
    return tr;
  }, "No categories.");
  updatePager("db-cat-page-label", db.catOffset, db.categories.length, db.catTotal, "db-cat-prev", "db-cat-next", dbCatLimit());
}

function updatePager(labelId, offset, count, total, prevId, nextId, limit) {
  const from = total === 0 ? 0 : offset + 1;
  const to = offset + count;
  $(labelId).textContent = `${from}\u2013${to} of ${total}`;
  $(prevId).disabled = offset <= 0;
  $(nextId).disabled = offset + limit >= total;
}

function dbSelectCategory(name) {
  db.recCategory = name;
  db.recOffset = 0;
  $("db-rec-filter").value = "";
  $("db-rec-filter").disabled = false;
  $("db-rec-category-label").textContent = name;
  const info = db.categories.find((c) => c.name === name);
  $("db-rec-keyed-note").hidden = !(info && info.keyed === false);
  renderDbCategories(); // refresh selection highlight
  dbLoadRecords();
}

async function dbLoadRecords() {
  if (!db.recCategory) return;
  const limit = dbRecLimit();
  const params = new URLSearchParams({ category: db.recCategory, offset: String(db.recOffset), limit: String(limit) });
  const filter = $("db-rec-filter").value.trim();
  if (filter) params.set("filter", filter);
  let data;
  try {
    data = await apiGet("/api/db/records?" + params.toString());
  } catch (err) {
    return;
  }
  if (!data || data.ok === false) {
    renderTable($("db-rec-tbody"), [], 3, () => null, (data && data.error) || "Category not found.");
    $("db-rec-page-label").textContent = "\u2014";
    return;
  }
  renderDbRecordsList(data);
}

function renderDbRecordsList(data) {
  const items = data.items || [];
  renderTable($("db-rec-tbody"), items, 3, (r) => {
    const tr = document.createElement("tr");
    tr.appendChild(linkCell(r.name, () => dbSelectRecordByName(db.recCategory, r.name)));
    tr.appendChild(cell(r.name_hash, "mono"));
    tr.appendChild(cell(r.attribute_count));
    return tr;
  }, "No records.");
  updatePager("db-rec-page-label", db.recOffset, items.length, data.total || 0, "db-rec-prev", "db-rec-next", dbRecLimit());
}

async function dbSelectRecordByName(category, record) {
  let data;
  try {
    data = await apiGet(`/api/db/record?category=${encodeURIComponent(category)}&record=${encodeURIComponent(record)}`);
  } catch (err) {
    return;
  }
  if (!data || data.ok === false) {
    $("db-record-title").textContent = "Record";
    $("db-record-hash").textContent = (data && data.error) || "Not found.";
    renderTable($("db-attr-tbody"), [], 4, () => null, "Not found.");
    return;
  }
  renderDbRecord(data);
}

function renderDbRecord(data) {
  $("db-record-title").textContent = `${data.category} / ${data.record}`;
  $("db-record-hash").textContent = "hash " + dash(data.name_hash);
  renderTable($("db-attr-tbody"), data.attributes, 4, (a) => {
    const tr = document.createElement("tr");
    if (a.name === null || a.name === undefined) {
      tr.appendChild(cell(a.name_hash, "muted mono"));
    } else {
      tr.appendChild(cell(a.name));
    }
    tr.appendChild(cell(a.type_name));
    tr.appendChild(cell(a.num_values));
    if ((a.type === 9 || a.type === 10) && a.link_target) {
      tr.appendChild(linkCell(a.link_target, () => dbNavigateToLink(a.link_target)));
    } else {
      tr.appendChild(cell(a.rendered));
    }
    return tr;
  }, "No attributes.");
}

/**
 * Navigate to a record-link target: try the current category first (most
 * links stay local), then fall back to _Structures, the shared pool for
 * cross-category links.
 */
async function dbNavigateToLink(target) {
  if (db.recCategory && (await tryOpenRecord(db.recCategory, target))) return;
  if (db.recCategory !== "_Structures" && (await tryOpenRecord("_Structures", target))) return;
  window.alert(`Could not locate linked record "${target}".`);
}

async function tryOpenRecord(category, record) {
  let data;
  try {
    data = await apiGet(`/api/db/record?category=${encodeURIComponent(category)}&record=${encodeURIComponent(record)}`);
  } catch (err) {
    return false;
  }
  if (!data || data.ok === false) return false;
  openDatabaseRecord(category, record, data);
  return true;
}

/** Cross-tab entry point: switch to Database, select category+record. */
function openDatabaseRecord(category, record, prefetched) {
  activateTab("database");
  db.recCategory = category;
  db.recOffset = 0;
  $("db-rec-filter").value = "";
  $("db-rec-filter").disabled = false;
  $("db-rec-category-label").textContent = category;
  const info = db.categories.find((c) => c.name === category);
  $("db-rec-keyed-note").hidden = !(info && info.keyed === false);
  if (db.categories.length > 0) renderDbCategories(); // keep the selection highlight in sync
  dbLoadRecords();
  if (prefetched) {
    renderDbRecord(prefetched);
  } else {
    dbSelectRecordByName(category, record);
  }
}

// ================================================================== ATTRIBUTE SEARCH

async function findRun() {
  const attr = $("find-attribute-input").value.trim();
  if (!attr) return;
  const limit = clampLimit($("find-limit-input").value || 50);
  let data;
  try {
    data = await apiGet(`/api/db/find?attribute=${encodeURIComponent(attr)}&limit=${limit}`);
  } catch (err) {
    return;
  }
  if (!data || data.ok === false) {
    $("find-summary").textContent = (data && data.error) || "Request failed.";
    renderTable($("find-tbody"), [], 4, () => null, "No results.");
    return;
  }
  const count = (data.items || []).length;
  $("find-summary").textContent = `${count} match${count === 1 ? "" : "es"} (scanned ${data.total_scanned})`;
  renderTable($("find-tbody"), data.items, 4, (r) => {
    const tr = document.createElement("tr");
    tr.appendChild(linkCell(r.category, () => openDatabaseRecord(r.category, r.record)));
    tr.appendChild(linkCell(r.record, () => openDatabaseRecord(r.category, r.record)));
    tr.appendChild(cell(r.type));
    tr.appendChild(cell(r.rendered));
    return tr;
  }, "No results.");
}

// ================================================================== CONSOLE

async function consoleLoad() {
  const filter = $("console-filter").value.trim();
  const path = filter ? `/api/console?filter=${encodeURIComponent(filter)}` : "/api/console";
  let data;
  try {
    data = await apiGet(path);
  } catch (err) {
    return;
  }
  if (!data || data.ok === false) return;
  $("console-summary").textContent = `${data.total} command${data.total === 1 ? "" : "s"}`;
  renderTable($("console-tbody"), data.items, 7, (c) => {
    const tr = document.createElement("tr");
    if (c.noop) tr.classList.add("row-warn");
    tr.appendChild(cell(c.name, "mono"));
    tr.appendChild(cell(c.handler, "mono"));
    tr.appendChild(cell(c.module));
    tr.appendChild(cell(fmtBool(c.from_exe)));
    tr.appendChild(cell(fmtBool(c.runtime_registered)));
    tr.appendChild(cell(c.registrar ? `${c.registrar} @ ${dash(c.registrar_offset)}` : "\u2014"));
    const statusTd = document.createElement("td");
    if (c.noop) {
      statusTd.textContent = "NO-OP (does nothing)";
      statusTd.className = "warn-text";
    } else {
      statusTd.textContent = "\u2014";
      statusTd.className = "muted";
    }
    tr.appendChild(statusTd);
    return tr;
  }, "No commands.");
}

// ================================================================== VARS

async function varsLoad() {
  const filter = $("vars-filter").value.trim();
  const limit = clampLimit($("vars-limit").value || 500);
  const params = new URLSearchParams({ limit: String(limit) });
  if (filter) params.set("filter", filter);
  let data;
  try {
    data = await apiGet("/api/vars?" + params.toString());
  } catch (err) {
    return;
  }
  if (!data || data.ok === false) return;
  $("vars-summary").textContent = `${data.total} var${data.total === 1 ? "" : "s"}`;
  renderTable($("vars-tbody"), data.items, 5, (v) => {
    const tr = document.createElement("tr");
    const changed = v.at_default === false;
    if (changed) tr.classList.add("row-warn");
    tr.appendChild(cell(v.name));
    tr.appendChild(cell(v.record, "mono"));
    tr.appendChild(cell(fmtNum(v.value, 3)));
    tr.appendChild(cell(fmtNum(v.default, 3)));
    const statusTd = document.createElement("td");
    if (v.at_default === null || v.at_default === undefined) {
      statusTd.textContent = v.default_source ? v.default_source : "\u2014";
      statusTd.className = "muted";
    } else if (changed) {
      statusTd.textContent = "CHANGED FROM DEFAULT";
      statusTd.className = "warn-text";
    } else {
      statusTd.textContent = v.default_source ? `at default (${v.default_source})` : "at default";
      statusTd.className = "muted";
    }
    tr.appendChild(statusTd);
    return tr;
  }, "No vars.");
}

// ================================================================== RAW

const RAW_ENDPOINTS = [
  { label: "/health", path: "/health" },
  { label: "/sdk/targets", path: "/sdk/targets" },
  { label: "/sdk/database", path: "/sdk/database" },
  { label: "/sdk/objects", path: "/sdk/objects" },
  { label: "/sdk/models", path: "/sdk/models" },
  { label: "/sdk/interfaces", path: "/sdk/interfaces" },
  { label: "/sdk/shader-params", path: "/sdk/shader-params" },
  { label: "/api/state", path: "/api/state" },
  { label: "/api/subsystems", path: "/api/subsystems" },
  { label: "/api/db/categories", path: "/api/db/categories?offset=0&limit=100" },
  { label: "/api/db/records", path: "/api/db/records?category=_Structures&offset=0&limit=100" },
  { label: "/api/db/record", path: "/api/db/record?category=_Structures&record=" },
  { label: "/api/db/find", path: "/api/db/find?attribute=&limit=50" },
  { label: "/api/console", path: "/api/console" },
  { label: "/api/vars", path: "/api/vars?limit=500" },
];

async function rawFetch() {
  const path = $("raw-path-input").value.trim();
  if (!path || !path.startsWith("/")) {
    $("raw-output").textContent = "Path must start with /";
    return;
  }
  $("raw-output").textContent = "Loading\u2026";
  let data;
  try {
    data = await apiGet(path);
  } catch (err) {
    $("raw-output").textContent = "Fetch failed: " + err;
    return;
  }
  $("raw-output").textContent = JSON.stringify(data, null, 2);
}

async function rawCopy() {
  const text = $("raw-output").textContent;
  try {
    await navigator.clipboard.writeText(text);
  } catch (err) {
    // Clipboard API unavailable (e.g. insecure context) -- fall back to a
    // hidden textarea + execCommand so the button still works offline.
    const ta = document.createElement("textarea");
    ta.value = text;
    ta.style.position = "fixed";
    ta.style.opacity = "0";
    document.body.appendChild(ta);
    ta.select();
    try {
      document.execCommand("copy");
    } catch (e2) {
      /* best effort */
    }
    document.body.removeChild(ta);
  }
}

// ================================================================== init

function initDatabaseWiring() {
  $("db-cat-filter").addEventListener(
    "input",
    debounce(() => {
      db.catOffset = 0;
      dbLoadCategories();
    }, 300)
  );
  $("db-cat-pagesize").addEventListener("change", () => {
    db.catOffset = 0;
    dbLoadCategories();
  });
  $("db-cat-prev").addEventListener("click", () => {
    db.catOffset = Math.max(0, db.catOffset - dbCatLimit());
    dbLoadCategories();
  });
  $("db-cat-next").addEventListener("click", () => {
    db.catOffset += dbCatLimit();
    dbLoadCategories();
  });

  $("db-rec-filter").addEventListener(
    "input",
    debounce(() => {
      db.recOffset = 0;
      dbLoadRecords();
    }, 300)
  );
  $("db-rec-pagesize").addEventListener("change", () => {
    db.recOffset = 0;
    dbLoadRecords();
  });
  $("db-rec-prev").addEventListener("click", () => {
    db.recOffset = Math.max(0, db.recOffset - dbRecLimit());
    dbLoadRecords();
  });
  $("db-rec-next").addEventListener("click", () => {
    db.recOffset += dbRecLimit();
    dbLoadRecords();
  });
}

function initRawWiring() {
  const select = $("raw-endpoint-select");
  for (const ep of RAW_ENDPOINTS) {
    const opt = document.createElement("option");
    opt.value = ep.path;
    opt.textContent = ep.label;
    select.appendChild(opt);
  }
  select.addEventListener("change", () => {
    $("raw-path-input").value = select.value;
  });
  $("raw-path-input").value = RAW_ENDPOINTS[0].path;
  $("raw-fetch-btn").addEventListener("click", rawFetch);
  $("raw-copy-btn").addEventListener("click", rawCopy);
}

document.addEventListener("DOMContentLoaded", () => {
  const baseInput = $("baseUrl");
  baseInput.value = getBaseUrl();
  baseInput.addEventListener("change", () => setBaseUrl(baseInput.value));

  for (const t of TABS) {
    $("tab-" + t).addEventListener("click", () => activateTab(t));
  }

  const savedInterval = localStorage.getItem(LS_LIVE_INTERVAL);
  if (savedInterval !== null) $("live-interval").value = savedInterval;
  $("live-interval").addEventListener("change", scheduleLivePoll);

  $("subsystems-refresh").addEventListener("click", subsystemsLoad);
  $("console-filter").addEventListener("input", debounce(consoleLoad, 300));
  $("console-refresh").addEventListener("click", consoleLoad);
  $("vars-filter").addEventListener("input", debounce(varsLoad, 300));
  $("vars-limit").addEventListener("change", varsLoad);
  $("vars-refresh").addEventListener("click", varsLoad);
  $("find-button").addEventListener("click", findRun);
  $("find-attribute-input").addEventListener("keydown", (e) => {
    if (e.key === "Enter") findRun();
  });

  initDatabaseWiring();
  initRawWiring();

  const savedTab = localStorage.getItem(LS_ACTIVE_TAB);
  activateTab(TABS.includes(savedTab) ? savedTab : "live");

  scheduleLivePoll();
});
