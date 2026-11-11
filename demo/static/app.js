"use strict";
const $ = (id) => document.getElementById(id);
let createdLink = "";
let currentStats = null;
let refreshing = false;

async function api(path, options = {}) {
  const response = await fetch(path, {
    ...options,
    signal: AbortSignal.timeout(6000),
  });
  const data = await response.json();
  if (!response.ok)
    throw new Error(data.error || `Request failed (${response.status}).`);
  return data;
}

function showStats(data) {
  const previousHits = currentStats?.cache_hits;
  currentStats = data;
  const total = data.cache_hits + data.cache_misses;
  const rate = total ? Math.round((data.cache_hits / total) * 100) : 0;
  $("hit-rate").textContent = total ? `${rate}%` : "—";
  $("hit-bar").style.width = `${rate}%`;
  $("cache-hits").textContent = data.cache_hits.toLocaleString();
  $("db-reads").textContent = data.database_reads.toLocaleString();
  if (previousHits !== undefined && previousHits !== data.cache_hits) {
    $("cache-hits").classList.remove("metric-changed");
    requestAnimationFrame(() =>
      $("cache-hits").classList.add("metric-changed"),
    );
  }
  $("entries").textContent = data.cache
    ? data.cache.entries.toLocaleString()
    : "—";
  $("capacity").textContent = data.cache
    ? `/ ${data.cache.capacity.toLocaleString()}`
    : "";
  $("ttl-note").textContent = `Copies expire after ${data.ttl}s`;
  $("status").className =
    `status ${data.cache_available ? "online" : "offline"}`;
  $("status-text").textContent = data.cache_available
    ? "Cache connected"
    : "SQLite fallback";
}

function cell(text, className) {
  const element = document.createElement("td");
  element.textContent = text;
  if (className) element.className = className;
  return element;
}

function showLinks(links) {
  const list = $("link-list");
  list.replaceChildren();
  if (!links.length) {
    const row = document.createElement("tr");
    const empty = cell("No links yet. Create your first one above.", "empty");
    empty.colSpan = 4;
    row.append(empty);
    list.append(row);
    return;
  }
  for (const item of links) {
    const row = document.createElement("tr");
    const short = cell("");
    const anchor = document.createElement("a");
    anchor.href = item.short_path;
    anchor.textContent = item.short_path;
    anchor.target = "_blank";
    anchor.rel = "noopener";
    short.append(anchor);
    const destination = cell(item.url);
    destination.title = item.url;
    const date = new Date(item.created_at).toLocaleDateString(undefined, {
      month: "short",
      day: "numeric",
    });
    const action = cell("");
    const button = document.createElement("button");
    button.className = "row-button";
    button.type = "button";
    button.textContent = "Resolve";
    button.setAttribute("aria-label", `Resolve ${item.code}`);
    button.addEventListener("click", () => {
      $("resolve-code").value = item.code;
      resolve(item.code);
    });
    action.append(button);
    row.append(short, destination, cell(date), action);
    list.append(row);
  }
}

async function refresh() {
  if (refreshing) return;
  refreshing = true;
  try {
    const [stats, links] = await Promise.all([
      api("/api/stats"),
      api("/api/links"),
    ]);
    showStats(stats);
    showLinks(links);
  } catch (error) {
    $("status").className = "status offline";
    $("status-text").textContent = "App unreachable";
  } finally {
    refreshing = false;
  }
}

function trace(title, subtitle, steps, destination = null) {
  $("trace-title").textContent = title;
  $("trace-subtitle").textContent = subtitle;
  $("trace-steps").replaceChildren();
  steps.forEach(([label, state], index) => {
    const li = document.createElement("li");
    li.className = state;
    const badge = document.createElement("span");
    badge.textContent = state === "done" ? "✓" : String(index + 1);
    li.append(badge, document.createTextNode(label));
    $("trace-steps").append(li);
  });
  $("trace-destination").hidden = !destination;
  if (destination) {
    $("trace-url").href = destination;
    $("trace-url").textContent = destination;
  }
}

let resolving = false;
async function resolve(code) {
  if (resolving) return;
  resolving = true;
  $("resolve-button").disabled = true;
  $("request-route").className = "request-route running";
  trace("Resolving…", `Looking up ${code}`, [["Ask VoltCache", "idle"]]);
  try {
    const result = await api(`/api/resolve/${encodeURIComponent(code)}`);
    if (result.source === "cache") {
      $("request-route").className = "request-route hit";
      $("trace-symbol").textContent = "✓";
      trace(
        "Cache hit",
        "Destination returned from VoltCache. No database read.",
        [
          ["Found in VoltCache", "done"],
          ["SQLite lookup skipped", "idle"],
          ["Destination ready", "done"],
        ],
        result.url,
      );
    } else {
      $("request-route").className = "request-route miss";
      $("trace-symbol").textContent = "↳";
      trace(
        "Database lookup",
        "The app read SQLite; cache population is best effort.",
        [
          ["Cache unavailable or key missing", "idle"],
          ["Destination read from SQLite", "done"],
          ["Cache update attempted when connected", "idle"],
        ],
        result.url,
      );
    }
  } catch (error) {
    $("request-route").className = "request-route";
    $("trace-symbol").textContent = "!";
    trace("Lookup failed", error.message, [
      ["No destination returned", "error"],
    ]);
  } finally {
    $("resolve-button").disabled = false;
    resolving = false;
    refresh();
  }
}

$("create-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  $("create-button").disabled = true;
  $("form-message").textContent = "";
  try {
    const result = await api("/api/links", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        url: $("destination").value.trim(),
        alias: $("alias").value.trim(),
      }),
    });
    createdLink = new URL(result.short_path, location.origin).href;
    $("short-url").href = result.short_path;
    $("short-url").textContent = createdLink;
    $("result").hidden = false;
    $("copy-button").textContent = "Copy";
    $("resolve-code").value = result.code;
    await refresh();
  } catch (error) {
    $("form-message").textContent = error.message;
  } finally {
    $("create-button").disabled = false;
  }
});
$("resolve-form").addEventListener("submit", (event) => {
  event.preventDefault();
  resolve($("resolve-code").value.trim());
});
$("copy-button").addEventListener("click", async () => {
  try {
    await navigator.clipboard.writeText(createdLink);
    $("copy-button").textContent = "Copied";
  } catch {
    $("copy-button").textContent = "Select link";
    const range = document.createRange();
    range.selectNodeContents($("short-url"));
    const selection = window.getSelection();
    selection.removeAllRanges();
    selection.addRange(range);
  }
});
$("refresh-button").addEventListener("click", refresh);
refresh();
setInterval(() => {
  if (!document.hidden) refresh();
}, 5000);
