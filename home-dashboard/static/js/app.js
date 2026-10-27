// OUI-SPY Mesh client. Server pushes everything over Socket.IO:
//   fleet (roster + home stats), det (one detection), console (raw line),
//   node_event (online/offline), sources (feed status), mac_aliases.
(() => {
"use strict";

const S = {
    fleet: { home: {}, nodes: [], ts: 0 },
    skew: 0,                  // client clock - server clock (s)
    settings: {},
    sources: {},
    dets: [],                 // live feed, newest first
    selected: null,
    placing: false,
    markers: {},
    hist: { offset: 0, rows: [] },
    statsBase: null,
    hourly: [],               // [{h, c}] detections per hour, last 24 h
    macAliases: {},           // MAC -> {alias, notes}, dashboard-local
    links: null,              // Leaflet layer group for home-to-remote lines
    device: null,             // MAC open in the device drawer
    booted: false,
    prevHomeAlive: null,
    prevHeal: 0,
    jobs: [],                 // remote-management command log (newest first)
    trails: {},               // node -> [[ts, lat, lon, acc]] GPS breadcrumbs (last 30 min)
    gpsLive: {},              // node -> ts of its latest phone fix
    gps: null,                // this phone carrying a node: {node, watch, lastSent, last, lock}
    jobState: {},             // job id -> last seen state (for finish toasts)
    filterMeta: null,         // types, hex lengths, presets
    editing: null,            // filter rows being edited in the drawer
    editFor: null,            // node the editor belongs to
    audioDirty: null,         // node whose buzzer/LED switches the user touched
    loadHomePending: false,
};
const $ = (id) => document.getElementById(id);
const esc = (s) => String(s ?? "").replace(/[&<>"']/g, (c) =>
    ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
const now = () => Date.now() / 1000 - S.skew;

async function api(path, opts = {}) {
    const r = await fetch(path, {
        headers: opts.body ? { "Content-Type": "application/json" } : {},
        ...opts,
        body: opts.body ? JSON.stringify(opts.body) : undefined,
    });
    return r.json();
}

// ------------------------------------------------------------------
// helpers
// ------------------------------------------------------------------
function nodeById(id) { return S.fleet.nodes.find((n) => n.id === id); }
function nodeName(id) {
    const n = nodeById(id);
    return (n && n.alias) || id || "?";
}
function devAlias(mac, fallback) {
    const a = S.macAliases[mac];
    return (a && a.alias) || fallback || "";
}
function metres(a, b) {
    return map.distance([a.lat, a.lon], [b.lat, b.lon]);
}
function fmtDist(m) {
    return m >= 1000 ? `${(m / 1000).toFixed(2)} km` : `${Math.round(m)} m`;
}
function ago(ts) {
    if (!ts) return "never";
    const s = Math.max(0, now() - ts);
    if (s < 2) return "now";
    if (s < 60) return `${Math.floor(s)}s ago`;
    if (s < 3600) return `${Math.floor(s / 60)}m ago`;
    if (s < 86400) return `${Math.floor(s / 3600)}h ago`;
    return `${Math.floor(s / 86400)}d ago`;
}
function hms(ts) {
    return new Date(ts * 1000).toLocaleTimeString([], { hour12: false });
}
function fullTime(ts) {
    const d = new Date(ts * 1000);
    return d.toLocaleDateString([], { month: "short", day: "2-digit" }) + " " + hms(ts);
}
function uptime(s) {
    if (s == null) return "-";
    const d = Math.floor(s / 86400), h = Math.floor((s % 86400) / 3600), m = Math.floor((s % 3600) / 60);
    return d ? `${d}d ${h}h` : h ? `${h}h ${m}m` : `${m}m ${s % 60}s`;
}
function nodeState(n) {
    if (!n.online) return "offline";
    const age = now() - (n.last_seen || 0);
    return age > 6 ? "stale" : "online";
}
function bars(rssi) {
    if (!rssi) return `<span class="bars"><i></i><i></i><i></i><i></i></span>`;
    const lvl = rssi >= -60 ? 4 : rssi >= -70 ? 3 : rssi >= -80 ? 2 : 1;
    const cls = lvl === 2 ? "weak" : lvl === 1 ? "poor" : "";
    return `<span class="bars ${cls}">` + [1, 2, 3, 4].map((i) => `<i class="${i <= lvl ? "lit" : ""}"></i>`).join("") + "</span>";
}

// ------------------------------------------------------------------
// header: sources, toggles, clock
// ------------------------------------------------------------------
// Serial health is about data, not just an open port: every line blinks
// the dot; open-but-silent for 7 s (TLM is every 5 s) turns it amber (stuck or rebooting board).
S.serialLast = 0;
function serialPillState(s) {
    if (!s.connected) return ["", "DISCONNECTED"];
    if (now() - S.serialLast > 7) return ["warn", "PORT OPEN, NO DATA"];   // TLM every 5 s
    return ["on", "LIVE"];
}
function updateSerialPill() {
    const s = S.sources.serial;
    const el = document.querySelector('.src-pill[data-kind="serial"]');
    if (!s || !el) return;
    const [cls, word] = serialPillState(s);
    el.querySelector(".dot").className = `dot ${cls}`;
    el.querySelector(".state").textContent = word;
}
function serialBlink() {
    S.serialLast = now();
    const el = document.querySelector('.src-pill[data-kind="serial"] .dot');
    if (!el) return;
    el.className = "dot on";
    el.classList.remove("rx"); void el.offsetWidth; el.classList.add("rx");
    const st = document.querySelector('.src-pill[data-kind="serial"] .state');
    if (st) st.textContent = "LIVE";
}
setInterval(updateSerialPill, 500);

function renderSources() {
    if (S.booted) { renderFleetCtl(); renderRemote(); }
    const box = $("sources");
    const entries = Object.values(S.sources);
    if (!entries.length) {
        box.innerHTML = `<span class="src-pill"><span class="dot"></span><b>NO FEED</b> serial off, MQTT not configured</span>`;
        return;
    }
    box.innerHTML = entries.map((s) => {
        if (s.kind === "serial") {
            const [cls, word] = serialPillState(s);
            return `<span class="src-pill" data-kind="serial" title="${esc(s.detail)}"><span class="dot ${cls}"></span>` +
                `<b>USB</b> <span class="state">${word}</span> <span class="src-detail">${esc(s.connected ? s.port || "" : s.detail || "")}</span></span>`;
        }
        let extra = "";
        if (s.kind === "mqtt" && s.availability) extra = ` | ${esc(s.device)}: ${esc(s.availability)}`;
        return `<span class="src-pill" title="${esc(s.detail)}"><span class="dot ${s.connected ? "on" : "warn"}"></span>` +
            `<b>${esc(s.kind.toUpperCase())}</b> <span class="src-detail">${esc(s.detail || "")}${extra}</span></span>`;
    }).join("");
}

function renderToggles() {
    $("soundBtn").classList.toggle("on", !!S.settings.sound);
    $("flashBtn").classList.toggle("on", !!S.settings.flash);
}
$("soundBtn").onclick = () => {
    S.settings.sound = !S.settings.sound;
    renderToggles();
    api("/api/settings", { method: "POST", body: { sound: S.settings.sound } });
    if (S.settings.sound) beep(true);
};
$("flashBtn").onclick = () => {
    S.settings.flash = !S.settings.flash;
    renderToggles();
    api("/api/settings", { method: "POST", body: { flash: S.settings.flash } });
};

// ------------------------------------------------------------------
// fleet panel
// ------------------------------------------------------------------
function renderHome() {
    const h = S.fleet.home || {};
    const alive = h.alive;
    const fail = h.tx ? ((h.txf || 0) / h.tx * 100) : 0;
    const cells = [
        ["Home", h.id ? `${esc(nodeName(h.id))}` : "-", alive ? "good" : "bad", true],
        ["State", alive ? (h.mode === "config" ? "CONFIG" : "SCANNING") : "SILENT", alive ? "good" : "bad", true],
        ["Chan", h.ch ?? "-"],
        ["Radio", h.rate ? (h.rate.startsWith("lr") && h.rate !== "lr-failed" ? `LR ${h.rate.slice(2)}` : h.rate.toUpperCase()) : "-",
         h.rate && h.rate.startsWith("lr") && h.rate !== "lr-failed" ? "good" : h.rate ? "warn" : ""],
        ["Peers", h.n ?? "-", h.n ? "good" : "warn"],
        ["lRSSI", h.lrssi ? `${h.lrssi}` : "-"],
        ["Uptime", uptime(h.up)],
        ["RX", h.rx ?? "-"],
        ["TX", h.tx ?? "-"],
        ["TX fail", h.tx != null ? `${fail.toFixed(1)}%` : "-", fail > 10 ? "bad" : fail > 3 ? "warn" : ""],
        ["Dup", h.dup ?? "-"],
        ["MQTT", h.mqtt == null ? "-" : h.mqtt ? "UP" : "DOWN", h.mqtt ? "good" : ""],
        ["TX q", h.pend ?? "-"],
        ["Heap", h.heap ? `${Math.round(h.heap / 1024)}K` : "-"],
        ["Heal", h.heal ? "SWEEP" : "-", h.heal ? "warn" : ""],
    ];
    // Mesh duty: how much of the time the radio is actually listening on the
    // mesh channel. Time shared with a BLE scan mostly goes to BLE.
    if (h.park != null) {
        const clear = h.park - (h.pble || 0);
        cells.push(["On mesh", `${h.park}%`, h.park >= 50 ? "good" : h.park >= 25 ? "warn" : "bad"],
                   ["Mesh, no BLE", `${clear}%`, clear >= 30 ? "good" : clear >= 15 ? "warn" : "bad"]);
    }
    // Acked delivery (firmware 35d27bc+): detections this node originated.
    if (h.dsent != null) {
        cells.push(["Dets sent", h.dsent], ["Acked", h.dack, h.dack ? "good" : ""],
                   ["Lost", h.dlost, h.dlost ? "bad" : ""], ["Retries", `${h.dretry}${h.dpend ? ` / ${h.dpend} pend` : ""}`]);
    }
    if (h.enc != null) {
        cells.push(["Encryption", h.enc ? "AES-256 ON" : "OFF", h.enc ? "good" : "bad", true]);
        if (h.enc && (h.eauth || h.ereplay || h.eplain)) {
            cells.push(["Rejected", `${h.eauth || 0} auth / ${h.ereplay || 0} replay / ${h.eplain || 0} plain`, "warn", true]);
        }
    }
    if (h.legacy) cells.push(["Firmware", "legacy (no TLM)", "warn", true]);
    if (h.strays && h.strays.length) {
        cells.push(["Wrong channel", h.strays.map((x) => `${esc(nodeName(x.id))} on ${x.ch}`).join(", "), "bad", true]);
    }
    $("homeStats").innerHTML = cells.map(([k, v, cls, wide]) =>
        `<div class="hs${wide ? " wide" : ""}"><div class="k">${k}</div><div class="v ${cls || ""}">${v}</div></div>`).join("");
}

function renderNodes() {
    const list = $("nodeList");
    const nodes = S.fleet.nodes;
    const online = nodes.filter((n) => n.online).length;
    $("fleetCount").textContent = `${online} / ${nodes.length} online`;
    if (!nodes.length) {
        list.innerHTML = `<div class="empty">No nodes yet. Plug in the home node or start an MQTT feed.</div>`;
        return;
    }
    const perNode = (S.statsBase && S.statsBase.per_node) || {};
    list.innerHTML = nodes.map((n) => {
        const st = nodeState(n);
        const role = n.is_home ? "home" : (n.role || "remote");
        const dc = perNode[n.id] ? perNode[n.id].count : 0;
        return `<div class="node ${st === "offline" ? "offline" : ""} ${S.selected === n.id ? "sel" : ""}" data-id="${esc(n.id)}">
            <div class="node-top">
                <span class="dot ${st === "online" ? "on" : st === "stale" ? "warn" : ""}"></span>
                <span class="node-name">${esc(n.alias || n.id)}</span>
                ${n.alias ? `<span class="node-id">${esc(n.id)}</span>` : ""}
                <span class="role ${role}">${role}</span>
            </div>
            <div class="node-meta">
                <span>${n.is_home ? "" : bars(n.rssi)}${n.is_home ? "local" : n.rssi ? `<b>${n.rssi}</b> dBm` : "no rssi"}</span>
                <span data-age="${n.last_seen || 0}">${st === "offline" ? "last " : ""}${ago(n.last_seen)}</span>
                ${n.is_home ? "" : `<span><b>${n.pkts || 0}</b> pkts</span>`}
                <span><b>${dc}</b> det</span>
                ${n.cfg && n.cfg.buzzer === 0 ? `<span class="ntag muted" title="Buzzer muted">MUTED</span>` : ""}
                ${syncState(n) === "drift" ? `<span class="ntag drift" title="Filter list differs from the home node">DRIFT</span>` : ""}
            </div>
        </div>`;
    }).join("");
    list.querySelectorAll(".node").forEach((el) => el.onclick = () => selectNode(el.dataset.id, true));

    const sel = $("hNode");
    const cur = sel.value;
    sel.innerHTML = `<option value="">All nodes</option>` +
        nodes.map((n) => `<option value="${esc(n.id)}">${esc(n.alias ? `${n.alias} (${n.id})` : n.id)}</option>`).join("");
    sel.value = cur;
    const vs = $("vNode"), vcur = vs.value;
    vs.innerHTML = `<option value="">Seen by any node</option>` + sel.innerHTML.replace(/^<option value="">All nodes<\/option>/, "");
    vs.value = vcur;
}

// ------------------------------------------------------------------
// map
// ------------------------------------------------------------------
// maxZoom 22 everywhere: past a layer's last real tile level (maxNativeZoom)
// Leaflet enlarges its final tiles, so you can always zoom in close.
const MAP_MAX_ZOOM = 22;
const map = L.map("map", { zoomControl: true, attributionControl: true, worldCopyJump: true, maxZoom: MAP_MAX_ZOOM });
let tileLayer = null;
map.attributionControl.setPrefix('<a href="https://leafletjs.com">Leaflet</a>');
// Free base maps that need no API key. Each keeps its provider's
// attribution. `dark` runs the light OSM tiles through the CSS invert filter;
// `labels` lays a transparent place-name layer over imagery.
const A_OSM = '&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a> contributors';
const A_CARTO = A_OSM + ' &copy; <a href="https://carto.com/attributions">CARTO</a>';
const A_ESRI = 'Tiles &copy; <a href="https://www.esri.com">Esri</a>';
const ESRI = (svc) => `https://server.arcgisonline.com/ArcGIS/rest/services/${svc}/MapServer/tile/{z}/{y}/{x}`;
const CARTO = (style) => `https://{s}.basemaps.cartocdn.com/${style}/{z}/{x}/{y}{r}.png`;
const TILES = {
    "osm-dark":     { name: "OpenStreetMap, dark", url: "https://tile.openstreetmap.org/{z}/{x}/{y}.png", attr: A_OSM, max: 19, dark: true },
    "carto-dark":   { name: "CARTO Dark Matter", url: CARTO("dark_all"), attr: A_CARTO, max: 20 },
    "carto-dark-nl":{ name: "CARTO Dark Matter, no labels", url: CARTO("dark_nolabels"), attr: A_CARTO, max: 20 },
    "esri-dark":    { name: "Esri Dark Gray", url: ESRI("Canvas/World_Dark_Gray_Base"), attr: A_ESRI, max: 16,
                      labels: ESRI("Canvas/World_Dark_Gray_Reference") },
    "esri-sat":     { name: "Satellite (Esri)", url: ESRI("World_Imagery"), attr: A_ESRI + ", Maxar, Earthstar Geographics", max: 19 },
    "esri-hybrid":  { name: "Satellite + labels (Esri)", url: ESRI("World_Imagery"), attr: A_ESRI + ", Maxar, Earthstar Geographics", max: 19,
                      labels: ESRI("Reference/World_Boundaries_and_Places") },
    "usgs-sat":     { name: "USGS imagery + topo (US only)", url: "https://basemap.nationalmap.gov/arcgis/rest/services/USGSImageryTopo/MapServer/tile/{z}/{y}/{x}",
                      attr: 'Tiles courtesy of the <a href="https://usgs.gov/">U.S. Geological Survey</a>', max: 16 },
    "osm":          { name: "OpenStreetMap", url: "https://tile.openstreetmap.org/{z}/{x}/{y}.png", attr: A_OSM, max: 19 },
    "osm-hot":      { name: "OpenStreetMap Humanitarian", url: "https://{s}.tile.openstreetmap.fr/hot/{z}/{x}/{y}.png",
                      attr: A_OSM + ', tiles by <a href="https://www.hotosm.org/">HOT</a>', max: 19 },
    "carto-voyager":{ name: "CARTO Voyager", url: CARTO("rastertiles/voyager"), attr: A_CARTO, max: 20 },
    "carto-light":  { name: "CARTO Positron (light)", url: CARTO("light_all"), attr: A_CARTO, max: 20 },
    "esri-street":  { name: "Esri World Street", url: ESRI("World_Street_Map"), attr: A_ESRI, max: 19 },
    "esri-topo":    { name: "Esri World Topo", url: ESRI("World_Topo_Map"), attr: A_ESRI, max: 19 },
    "opentopo":     { name: "OpenTopoMap (terrain)", url: "https://{s}.tile.opentopomap.org/{z}/{x}/{y}.png",
                      attr: A_OSM + ', <a href="http://viewfinderpanoramas.org">SRTM</a> | style &copy; <a href="https://opentopomap.org">OpenTopoMap</a> (CC-BY-SA)', max: 17 },
    "cyclosm":      { name: "CyclOSM", url: "https://{s}.tile-cyclosm.openstreetmap.fr/cyclosm/{z}/{x}/{y}.png",
                      attr: A_OSM + ', style <a href="https://www.cyclosm.org">CyclOSM</a>', max: 20 },
};
const TILE_OTHER = { "custom-dark": "Custom / local tile server, dark", "custom": "Custom / local tile server", "none": "None - offline grid" };
let labelLayer = null;
function applyTiles() {
    if (tileLayer) { map.removeLayer(tileLayer); tileLayer = null; }
    if (labelLayer) { map.removeLayer(labelLayer); labelLayer = null; }
    const mode = (TILES[S.settings.tiles] || TILE_OTHER[S.settings.tiles]) ? S.settings.tiles : "osm-dark";
    const t = TILES[mode];
    const el = map.getContainer();
    el.classList.toggle("grid-bg", mode === "none");
    el.classList.toggle("dark-tiles", (t && t.dark) || mode === "custom-dark");
    if (t) {
        tileLayer = L.tileLayer(t.url, { maxZoom: MAP_MAX_ZOOM, maxNativeZoom: t.max, attribution: t.attr }).addTo(map);
        if (t.labels) labelLayer = L.tileLayer(t.labels, { maxZoom: MAP_MAX_ZOOM, maxNativeZoom: t.max, pane: "overlayPane" }).addTo(map);
    } else if (mode.startsWith("custom") && S.settings.tiles_custom) {
        tileLayer = L.tileLayer(S.settings.tiles_custom, { maxZoom: MAP_MAX_ZOOM, maxNativeZoom: 19 }).addTo(map);
    }
    if ($("mapLayer") && document.activeElement !== $("mapLayer")) $("mapLayer").value = mode;
}
// Maximize: the map panel fills the whole window (desktop and phone; a CSS
// overlay, since phones have no element fullscreen). Esc or the button exits.
function setMapMax(on) {
    const panel = document.querySelector(".map-panel");
    panel.classList.toggle("map-max", on);
    document.body.classList.toggle("map-maxed", on);
    $("mapMax").classList.toggle("on", on);
    $("mapMax").title = on ? "Exit full screen map (Esc)" : "Full screen map";
    $("mapMax").setAttribute("aria-pressed", on ? "true" : "false");
    setTimeout(() => map.invalidateSize({ pan: false }), 60);
}
$("mapMax").onclick = () => setMapMax(!document.querySelector(".map-panel").classList.contains("map-max"));
document.addEventListener("keydown", (e) => {
    if (e.key === "Escape" && document.querySelector(".map-panel.map-max")) setMapMax(false);
});
// Picking a layer on the map saves it, so every open dashboard switches too.
$("mapLayer").onchange = async () => {
    const tiles = $("mapLayer").value;
    S.settings.tiles = tiles;
    applyTiles();
    if ($("sTiles")) $("sTiles").value = tiles;
    await api("/api/settings", { method: "POST", body: { tiles } });
};
// Map layer picker (the map's top-right corner and Settings share one list).
function tileOptions(sel) {
    const opt = (k, n) => `<option value="${k}">${esc(n)}</option>`;
    const dark = ["osm-dark", "carto-dark", "carto-dark-nl", "esri-dark"];
    const sat = ["esri-sat", "esri-hybrid", "usgs-sat"];
    const light = Object.keys(TILES).filter((k) => !dark.includes(k) && !sat.includes(k));
    sel.innerHTML =
        `<optgroup label="Dark">${dark.map((k) => opt(k, TILES[k].name)).join("")}</optgroup>` +
        `<optgroup label="Satellite">${sat.map((k) => opt(k, TILES[k].name)).join("")}</optgroup>` +
        `<optgroup label="Street and terrain">${light.map((k) => opt(k, TILES[k].name)).join("")}</optgroup>` +
        `<optgroup label="Private / offline">${Object.entries(TILE_OTHER).map(([k, n]) => opt(k, n)).join("")}</optgroup>`;
}
let viewSet = false;
function initView() {
    if (viewSet) return;
    const v = S.settings.map_view;
    const placed = S.fleet.nodes.filter((n) => n.lat != null);
    if (v && v.length === 3) map.setView([v[0], v[1]], v[2]);
    else if (placed.length) map.fitBounds(L.latLngBounds(placed.map((n) => [n.lat, n.lon])).pad(0.4), { maxZoom: 16 });
    else map.setView([39.5, -98.35], 4);
    viewSet = true;
}
let viewTimer = null;
map.on("moveend", () => {
    if (!viewSet) return;
    clearTimeout(viewTimer);
    viewTimer = setTimeout(() => {
        const c = map.getCenter();
        const mv = [+c.lat.toFixed(6), +c.lng.toFixed(6), map.getZoom()];
        S.settings.map_view = mv;
        api("/api/settings", { method: "POST", body: { map_view: mv } });
    }, 800);
});
map.on("click", (e) => {
    if (!S.placing || !S.selected) return;
    setNodePos(S.selected, e.latlng.lat, e.latlng.lng);
    setPlacing(false);
});

function markerParts(n) {
    const st = nodeState(n);
    const sub = n.is_home ? "home" : (n.rssi ? `${n.rssi} dBm` : st);
    const recent = S.dets.filter((d) => d.node === n.id && now() - d.ts < 900).length;
    const gpsOn = S.gpsLive[n.id] && now() - S.gpsLive[n.id] < 60;
    return {
        cls: `nmark ${st} ${n.is_home ? "home" : ""} ${S.selected === n.id ? "sel" : ""}`,
        badge: recent ? String(recent) : "",
        label: `${esc(n.alias || n.id)}${gpsOn ? `<i class="gpstag">GPS</i>` : ""}<small>${esc(sub)}</small>`,
    };
}
function markerHtml(n) {
    const p = markerParts(n);
    return `<div class="${p.cls}">
        <span class="ping"></span><span class="burst"></span><span class="core"></span>
        ${p.badge ? `<span class="badge" title="Detections in the last 15 min">${p.badge}</span>` : ""}
        <span class="label">${p.label}</span></div>`;
}
// Update a marker that's already on the map without rebuilding it: only the
// pieces that changed are touched, so its CSS animations (ping, pulse) keep
// running. Rebuilding the icon every second made the map flicker on phones.
function patchMarker(m, n) {
    const root = m.getElement() && m.getElement().querySelector(".nmark");
    if (!root) return;
    const p = markerParts(n);
    const cls = p.cls + (root.classList.contains("hit") ? " hit" : "");
    if (root.className !== cls) root.className = cls;
    let badge = root.querySelector(".badge");
    if (p.badge && !badge) {
        badge = document.createElement("span");
        badge.className = "badge";
        badge.title = "Detections in the last 15 min";
        root.insertBefore(badge, root.querySelector(".label"));
    }
    if (badge && !p.badge) badge.remove();
    else if (badge && badge.textContent !== p.badge) badge.textContent = p.badge;
    const label = root.querySelector(".label");
    if (label && label.innerHTML !== p.label) label.innerHTML = p.label;
}
function renderMap() {
    const seen = new Set();
    for (const n of S.fleet.nodes) {
        if (n.lat == null || n.lon == null) continue;
        seen.add(n.id);
        let m = S.markers[n.id];
        if (!m) {
            m = L.marker([n.lat, n.lon], {
                draggable: true, riseOnHover: true,
                icon: L.divIcon({ className: "", html: markerHtml(n), iconSize: [0, 0] }),
            }).addTo(map);
            m.on("dragend", (e) => { const p = e.target.getLatLng(); setNodePos(n.id, p.lat, p.lng); });
            m.on("click", () => selectNode(n.id, true));
            S.markers[n.id] = m;
        } else {
            const ll = m.getLatLng();
            if (!m.dragging || !m._dragActive) {
                if (Math.abs(ll.lat - n.lat) > 1e-9 || Math.abs(ll.lng - n.lon) > 1e-9) m.setLatLng([n.lat, n.lon]);
            }
            patchMarker(m, n);
        }
    }
    for (const id of Object.keys(S.markers)) {
        if (!seen.has(id)) { map.removeLayer(S.markers[id]); delete S.markers[id]; }
    }
    renderLinks();
    setTimeout(declutterLabels, 0);
    const unplaced = S.fleet.nodes.filter((n) => n.lat == null);
    $("unplaced").innerHTML = unplaced.length
        ? `<span>Not on map:</span>` + unplaced.map((n) => `<button data-id="${esc(n.id)}">${esc(n.alias || n.id)}</button>`).join("")
        : "";
    $("unplaced").querySelectorAll("button").forEach((b) => b.onclick = () => {
        selectNode(b.dataset.id, false);
        setPlacing(true);
    });
}
// Home-to-remote links: the roster RSSI is what the HOME hears from each
// remote, so a line per placed remote coloured by that RSSI, with distance.
// One polyline per remote, kept between updates and moved / restyled in
// place (clearing and redrawing them all every second flickered too).
function renderLinks() {
    if (!S.links) S.links = L.layerGroup().addTo(map);
    if (!S.linkLines) S.linkLines = {};
    const keep = new Set();
    const home = map._loaded && S.settings.links !== false
        ? S.fleet.nodes.find((n) => n.is_home && n.lat != null) : null;
    if (home) {
        for (const n of S.fleet.nodes) {
            if (n.is_home || n.lat == null || !n.online) continue;
            keep.add(n.id);
            const st = nodeState(n);
            const r = n.rssi;
            const color = !r ? "#5b6879" : r >= -70 ? "#4ecdc4" : r >= -82 ? "#fbbf24" : "#f87171";
            const style = { color, weight: 2, opacity: st === "stale" ? 0.35 : 0.75, dashArray: st === "stale" ? "4 6" : "6 4" };
            const pts = [[home.lat, home.lon], [n.lat, n.lon]];
            // Label only the selected node's link permanently, and only when
            // the line is long enough on screen to hold it without covering
            // the markers; otherwise the label appears on hover/tap.
            const px = map.latLngToContainerPoint(pts[0]).distanceTo(map.latLngToContainerPoint(pts[1]));
            const pinned = (S.selected === n.id || S.selected === home.id) && px > 160;
            const tip = `<b>${r ? r + " dBm" : "no rssi"}</b> | ${fmtDist(metres(home, n))}`;
            let line = S.linkLines[n.id];
            if (!line) {
                line = S.linkLines[n.id] = L.polyline(pts, style);
                S.links.addLayer(line);
            } else {
                const cur = line.getLatLngs();
                if (cur[0].lat !== pts[0][0] || cur[0].lng !== pts[0][1] || cur[1].lat !== pts[1][0] || cur[1].lng !== pts[1][1]) line.setLatLngs(pts);
                const o = line.options;
                if (o.color !== style.color || o.opacity !== style.opacity || o.dashArray !== style.dashArray) line.setStyle(style);
            }
            if (line._pinned !== pinned) {
                line.unbindTooltip();
                line.bindTooltip(tip, { permanent: pinned, sticky: !pinned, direction: "center", className: "link-label" });
                line._pinned = pinned;
            } else if (line.getTooltip() && line._tip !== tip) {
                line.setTooltipContent(tip);
            }
            line._tip = tip;
        }
    }
    for (const id of Object.keys(S.linkLines)) {
        if (!keep.has(id)) { S.links.removeLayer(S.linkLines[id]); delete S.linkLines[id]; }
    }
}
// ------------------------------------------------------------------
// GPS: a phone carrying a node feeds its position; every viewer sees the
// marker move and a breadcrumb trail.
// ------------------------------------------------------------------
// Trail length slider: steps in seconds (0 = off). Per viewer, remembered
// in this browser; longer windows are fetched from the stored history.
const TRAIL_STEPS = [0, 300, 900, 1800, 3600, 10800, 21600, 43200, 86400, 604800];
const TRAIL_NAMES = ["Off", "5 min", "15 min", "30 min", "1 h", "3 h", "6 h", "12 h", "24 h", "7 days"];
S.trailSeconds = 1800;
S.trailLoaded = 1800;
try {
    const v = localStorage.getItem("ouispy.trail");
    if (v !== null && TRAIL_STEPS.includes(Number(v))) S.trailSeconds = Number(v);
} catch (e) { /* storage blocked: default */ }
function syncTrailSlider() {
    const i = Math.max(0, TRAIL_STEPS.indexOf(S.trailSeconds));
    $("trailRange").value = i;
    $("trailLabel").textContent = TRAIL_NAMES[i];
}
async function loadTrails(seconds) {
    const tr = await api(`/api/tracks?seconds=${Math.max(seconds, 60)}`);
    S.trails = tr.tracks || {};
    S.trailLoaded = tr.seconds || seconds;
    for (const [k, v] of Object.entries(tr.live || {})) S.gpsLive[k] = v.ts;
    renderTrails();
}
$("trailRange").addEventListener("input", () => {
    S.trailSeconds = TRAIL_STEPS[Number($("trailRange").value)];
    $("trailLabel").textContent = TRAIL_NAMES[Number($("trailRange").value)];
    try { localStorage.setItem("ouispy.trail", String(S.trailSeconds)); } catch (e) { /* ignore */ }
    renderTrails();
});
$("trailRange").addEventListener("change", () => {
    if (S.trailSeconds > S.trailLoaded) loadTrails(S.trailSeconds);   // need older points
});

function renderTrails() {
    if (!S.trailLayer) S.trailLayer = L.layerGroup().addTo(map);
    S.trailLayer.clearLayers();
    if (!S.trailSeconds) return;
    const cutoff = now() - S.trailSeconds;
    for (const [nid, pts] of Object.entries(S.trails)) {
        const ll = pts.filter((p) => p[0] >= cutoff).map((p) => [p[1], p[2]]);
        if (ll.length < 2) continue;
        S.trailLayer.addLayer(L.polyline(ll, { color: "#f0abfc", weight: 3, opacity: 0.7, dashArray: "2 6", interactive: false }));
    }
}
function onGps(g) {
    S.gpsLive[g.node] = g.ts;
    (S.trails[g.node] = S.trails[g.node] || []).push([g.ts, g.lat, g.lon, g.acc]);
    if (S.trails[g.node].length > 20000) S.trails[g.node].splice(0, 5000);
    const n = nodeById(g.node);
    if (n) { n.lat = g.lat; n.lon = g.lon; }
    renderMap();
    renderTrails();
}
function gpsInfo(text) { $("dGpsInfo").textContent = text; }
function renderGpsButton() {
    const mine = S.gps && S.gps.node === S.selected;
    $("dGps").textContent = mine ? "Stop carrying" : "Carry with this phone";
    $("dGps").classList.toggle("armed", !!mine);
    if (!mine) {
        const ts = S.gpsLive[S.selected];
        gpsInfo(ts && now() - ts < 60 ? `GPS live from another device, ${ago(ts)}` : "");
    }
}
async function stopCarry(clearTrail) {
    if (!S.gps) return;
    navigator.geolocation.clearWatch(S.gps.watch);
    try { if (S.gps.lock) await S.gps.lock.release(); } catch (e) { /* ignore */ }
    const node = S.gps.node;
    S.gps = null;
    await api(`/api/nodes/${node}/gps${clearTrail ? "?clear=1" : ""}`, { method: "DELETE" });
    renderGpsButton();
}
async function startCarry(node) {
    if (!window.isSecureContext) {
        toast("warn", "Location needs HTTPS", "Phones only share GPS with https:// pages: serve the dashboard over HTTPS (your own reverse proxy or VPN)");
        gpsInfo("needs the https:// address");
        return;
    }
    if (!("geolocation" in navigator)) { toast("bad", "No GPS", "This browser has no location support"); return; }
    if (S.gps) await stopCarry(false);
    S.gps = { node, lastSent: 0, last: null, lock: null };
    try { if (navigator.wakeLock) S.gps.lock = await navigator.wakeLock.request("screen"); } catch (e) { /* optional */ }
    S.gps.watch = navigator.geolocation.watchPosition(async (pos) => {
        if (!S.gps) return;
        const c = pos.coords, t = Date.now() / 1000;
        const moved = S.gps.last ? map.distance([S.gps.last.lat, S.gps.last.lon], [c.latitude, c.longitude]) : 1e9;
        if (t - S.gps.lastSent >= 2 || moved >= 3) {
            S.gps.lastSent = t;
            S.gps.last = { lat: c.latitude, lon: c.longitude };
            await api(`/api/nodes/${S.gps.node}/gps`, { method: "POST",
                body: { lat: c.latitude, lon: c.longitude, acc: c.accuracy } });
        }
        if (S.selected === S.gps.node) gpsInfo(`GPS live, accuracy ${Math.round(c.accuracy)} m`);
    }, (err) => {
        gpsInfo(err.code === 1 ? "location permission denied" : "no GPS fix yet");
        if (err.code === 1) stopCarry(false);
    }, { enableHighAccuracy: true, maximumAge: 2000, timeout: 20000 });
    renderGpsButton();
    gpsInfo("waiting for a GPS fix...");
    toast("good", `Carrying ${nodeName(node)}`, "This phone's GPS now positions the node. Keep the page open.");
}
$("dGps").onclick = () => (S.gps && S.gps.node === S.selected) ? stopCarry(false) : startCarry(S.selected);
document.addEventListener("visibilitychange", async () => {
    // wake locks drop when the page is hidden; take it back on return
    if (S.gps && !document.hidden && navigator.wakeLock) {
        try { S.gps.lock = await navigator.wakeLock.request("screen"); } catch (e) { /* ignore */ }
    }
});

// Marker labels: each picks the side (right, left, above, below) that
// doesn't overlap a label already placed or another marker, so nodes a few
// metres apart stay readable. Re-run after every render, zoom and pan.
const LABEL_SIDES = ["lab-r", "lab-l", "lab-t", "lab-b"];
function declutterLabels() {
    if (!map._loaded) return;
    const marks = Object.values(S.markers).map((m) => m.getElement() && m.getElement().querySelector(".nmark"))
        .filter(Boolean);
    const placed = [];
    const cores = marks.map((el) => el.querySelector(".core").getBoundingClientRect());
    const hit = (a, b) => a.left < b.right && a.right > b.left && a.top < b.bottom && a.bottom > b.top;
    // selected node first, so it gets the best spot
    marks.sort((a, b) => (b.classList.contains("sel") ? 1 : 0) - (a.classList.contains("sel") ? 1 : 0));
    for (const el of marks) {
        const lab = el.querySelector(".label");
        if (!lab) continue;
        let best = LABEL_SIDES[0];
        for (const side of LABEL_SIDES) {
            el.classList.remove(...LABEL_SIDES);
            el.classList.add(side);
            const r = lab.getBoundingClientRect();
            const box = { left: r.left - 3, right: r.right + 3, top: r.top - 3, bottom: r.bottom + 3 };
            if (!placed.some((p) => hit(box, p)) && !cores.some((c) => hit(box, c))) { best = side; break; }
        }
        el.classList.remove(...LABEL_SIDES);
        el.classList.add(best);
        placed.push(lab.getBoundingClientRect());
    }
}
map.on("zoomend moveend", () => { renderLinks(); declutterLabels(); });

async function setNodePos(id, lat, lon) {
    const n = nodeById(id);
    if (n) { n.lat = lat; n.lon = lon; }
    renderMap();
    await api(`/api/nodes/${id}`, { method: "POST", body: { lat, lon } });
}
function setPlacing(on) {
    S.placing = on;
    document.querySelector(".map-panel").classList.toggle("placing", on);
    $("mapHint").textContent = on
        ? `Click the map to place ${nodeName(S.selected)}. Esc to cancel.`
        : `Drag markers to move nodes. Select a node, then "Place on map".`;
}
document.addEventListener("keydown", (e) => {
    if (e.key === "Escape") { if (S.placing) setPlacing(false); else { closeDrawer(); closeDevice(); hideFleetKey(); } }
});

function hitNode(id) {
    const card = document.querySelector(`.node[data-id="${CSS.escape(id)}"]`);
    if (card) { card.classList.remove("hit"); void card.offsetWidth; card.classList.add("hit"); }
    const m = S.markers[id];
    if (m) {
        const el = m.getElement() && m.getElement().querySelector(".nmark");
        if (el) {
            m._hitting = true;
            el.classList.remove("hit"); void el.offsetWidth; el.classList.add("hit");
            setTimeout(() => { m._hitting = false; el.classList.remove("hit"); }, 1650);
        }
    }
}

// ------------------------------------------------------------------
// live feed
// ------------------------------------------------------------------
function detHtml(d, fresh) {
    const node = nodeById(d.node);
    const home = node && node.is_home;
    return `<div class="det ${d.is_new ? "new" : ""} ${fresh ? "fresh" : ""}" data-node="${esc(d.node)}">
        <div class="det-top">
            <span class="det-vendor">${esc(d.descr || d.match || "UNKNOWN")}</span>
            ${d.is_new ? `<span class="tag newtag">NEW</span>` : ""}
            <span class="tag ${esc(d.dom)}">${d.dom === "wifi" ? "WIFI" : "BLE"}</span>
            <span class="tag">${esc(d.type || "")}</span>
            <span class="det-time" title="${esc(fullTime(d.ts))}">${hms(d.ts)}</span>
        </div>
        <div class="det-mid">
            <span class="det-mac" data-mac="${esc(d.mac)}">${esc(d.mac)}</span>
            ${devAlias(d.mac, d.alias) ? `<span class="mal">${esc(devAlias(d.mac, d.alias))}</span>` : ""}
            <span class="rssi-v">${d.rssi ?? "?"} dBm</span>
            <span class="det-node ${home ? "home" : ""}">via ${esc(nodeName(d.node))}</span>
        </div>
    </div>`;
}
function renderFeed() {
    const f = $("feed");
    if (!S.dets.length) { f.innerHTML = `<div class="empty">Waiting for detections...</div>`; return; }
    f.innerHTML = S.dets.slice(0, 120).map((d) => detHtml(d)).join("");
    f.querySelectorAll(".det").forEach(wireDet);
}
function wireDet(el) {
    el.onclick = (e) => {
        const m = e.target.closest(".det-mac");
        if (m) openDevice(m.dataset.mac);
        else selectNode(el.dataset.node, true);
    };
}
function renderKpis() {
    const st = S.statsBase || { total: 0, day: 0, macs: 0 };
    const lastHour = S.dets.filter((d) => now() - d.ts < 3600).length;
    $("detCount").textContent = st.total;
    $("kpis").innerHTML = `
        <div class="kpi"><div class="k">Last hour</div><div class="v ${lastHour ? "alert" : ""}">${lastHour}</div></div>
        <div class="kpi"><div class="k">24 h</div><div class="v">${st.day}</div></div>
        <div class="kpi"><div class="k">Unique MACs</div><div class="v">${st.macs}</div></div>`;
    renderActivity();
}
function bumpHourly(ts) {
    const h = Math.floor(ts / 3600) * 3600;
    const last = S.hourly[S.hourly.length - 1];
    if (last && last.h === h) last.c++;
    else if (!last || h > last.h) { S.hourly.push({ h, c: 1 }); S.hourly = S.hourly.slice(-24); }
}
function renderActivity() {
    const hrs = S.hourly;
    if (!hrs.length) { $("activity").innerHTML = ""; return; }
    const max = Math.max(1, ...hrs.map((x) => x.c));
    $("activity").innerHTML = hrs.map((x, i) => {
        const cls = i === hrs.length - 1 ? "now" : x.c >= max * 0.6 ? "hot" : "";
        const label = new Date(x.h * 1000).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", hour12: false });
        return `<i class="${cls}" style="height:${Math.max(4, (x.c / max) * 100)}%" title="${label}  ${x.c} detections"></i>`;
    }).join("");
}

// ------------------------------------------------------------------
// toasts + desktop notifications
// ------------------------------------------------------------------
function toast(kind, title, body, onClick) {
    const el = document.createElement("div");
    el.className = `toast ${kind}`;
    el.innerHTML = `<b>${esc(title)}</b><span>${esc(body || "")}</span>`;
    el.onclick = () => { if (onClick) onClick(); el.remove(); };
    const box = $("toasts");
    box.prepend(el);
    while (box.children.length > 5) box.lastElementChild.remove();
    setTimeout(() => { el.classList.add("out"); setTimeout(() => el.remove(), 350); }, kind === "bad" ? 9000 : 5000);
    if (S.settings.notify && document.hidden && "Notification" in window && Notification.permission === "granted") {
        try { new Notification(`OUI-SPY: ${title}`, { body: body || "", tag: title, silent: true }); } catch (e) { /* ignore */ }
    }
}
function watchHealth() {
    const h = S.fleet.home || {};
    if (!S.booted) { S.prevHomeAlive = h.alive; S.prevHeal = h.heal || 0; return; }
    if (S.prevHomeAlive && !h.alive) toast("bad", "Home node silent", `No telemetry from ${nodeName(h.id)} for 15 s`);
    if (S.prevHomeAlive === false && h.alive) toast("good", "Home node back", nodeName(h.id));
    if (h.heal && !S.prevHeal) toast("warn", "Channel heal sweep", "Home lost the fleet and is scanning channels");
    S.prevHomeAlive = h.alive;
    S.prevHeal = h.heal || 0;
}

let audioCtx = null;
function beep(test) {
    try {
        audioCtx = audioCtx || new (window.AudioContext || window.webkitAudioContext)();
        const t = audioCtx.currentTime;
        const n = test ? 1 : 3;
        for (let i = 0; i < n; i++) {
            const o = audioCtx.createOscillator(), g = audioCtx.createGain();
            o.type = "square";
            o.frequency.value = 1760;
            g.gain.setValueAtTime(0.0001, t + i * 0.16);
            g.gain.exponentialRampToValueAtTime(0.08, t + i * 0.16 + 0.01);
            g.gain.exponentialRampToValueAtTime(0.0001, t + i * 0.16 + 0.11);
            o.connect(g).connect(audioCtx.destination);
            o.start(t + i * 0.16);
            o.stop(t + i * 0.16 + 0.12);
        }
    } catch (e) { /* audio blocked until first user gesture */ }
}
function flashScreen() {
    const o = $("flashOverlay");
    o.classList.remove("go"); void o.offsetWidth; o.classList.add("go");
}

const ALERT_QUIET_S = 300, ALERT_GAP_MS = 2000;
S.macSeen = {};
S.lastAlert = 0;
function onDetection(d) {
    S.dets.unshift(d);
    S.dets.length = Math.min(S.dets.length, 300);
    if (S.statsBase) {
        S.statsBase.total++; S.statsBase.day++;
        if (d.is_new) S.statsBase.macs++;
        const pn = S.statsBase.per_node[d.node] = S.statsBase.per_node[d.node] || { count: 0 };
        pn.count++; pn.last = d.ts;
    }
    const f = $("feed");
    if (f.querySelector(".empty")) f.innerHTML = "";
    f.insertAdjacentHTML("afterbegin", detHtml(d, true));
    wireDet(f.firstElementChild);
    while (f.children.length > 120) f.lastElementChild.remove();
    renderKpis();
    hitNode(d.node);
    bumpHourly(d.ts);
    renderActivity();
    if (d.is_new) {
        toast("bad", `New device: ${devAlias(d.mac, d.descr || d.match)}`,
              `${d.mac}  ${d.rssi} dBm  via ${nodeName(d.node)}`, () => openDevice(d.mac));
    }
    // Sound and screen flash are for news: a new device, or one back after
    // ALERT_QUIET_S of silence, at most once per ALERT_GAP_MS. A device a node
    // keeps re-reporting would otherwise flash the screen every few seconds.
    const prev = S.macSeen[d.mac];
    S.macSeen[d.mac] = d.ts;
    if ((d.is_new || !prev || d.ts - prev >= ALERT_QUIET_S) && Date.now() - S.lastAlert >= ALERT_GAP_MS) {
        S.lastAlert = Date.now();
        if (S.settings.sound) beep(false);
        if (S.settings.flash) flashScreen();
    }
    if (!histFiltered()) {
        $("hBody").insertAdjacentHTML("afterbegin", histRow(d));
        S.hist.total = (S.hist.total || 0) + 1;
        $("hTotal").textContent = `${S.hist.total} detections`;
    }
    if (S.selected === d.node && $("drawer").classList.contains("open")) {
        $("dDets").insertAdjacentHTML("afterbegin", detHtml(d, true));
    }
}

// ------------------------------------------------------------------
// drawer
// ------------------------------------------------------------------
function selectNode(id, open) {
    if (!id) return;
    S.selected = id;
    renderNodes();
    renderMap();
    const n = nodeById(id);
    if (n && n.lat != null && open) map.panTo([n.lat, n.lon], { animate: true });
    if (open || $("drawer").classList.contains("open")) openDrawer(id);
}
function closeDrawer() { $("drawer").classList.remove("open"); }
$("dClose").onclick = closeDrawer;

async function openDrawer(id) {
    $("vDrawer").classList.remove("open");
    $("drawer").classList.add("open");
    const r = await fetch(`/api/nodes/${id}`);
    if (!r.ok) return;
    const data = await r.json();
    const n = data.node;
    $("dTitle").textContent = n.alias || n.id;
    $("dSub").textContent = `${n.id} | ${n.is_home ? "home" : n.role} | first seen ${n.first_seen ? fullTime(n.first_seen) : "-"}`;
    if (document.activeElement !== $("dAlias")) $("dAlias").value = n.alias || "";
    if (document.activeElement !== $("dNotes")) $("dNotes").value = n.notes || "";
    const st = nodeState(n);
    const samples = data.sightings.filter((s) => s.rssi);
    const avg = samples.length ? Math.round(samples.reduce((a, s) => a + s.rssi, 0) / samples.length) : null;
    const cells = [
        ["Status", st.toUpperCase(), st === "online" ? "good" : st === "stale" ? "warn" : "bad"],
        ["Last seen", ago(n.last_seen)],
        ["Link RSSI", n.is_home ? "local" : n.rssi ? `${n.rssi} dBm` : "-"],
        ["Packets", n.is_home ? "-" : n.pkts || 0],
        ["Detections", data.stats.count || 0, data.stats.count ? "bad" : ""],
        ["Avg RSSI 24h", avg ? `${avg} dBm` : "-"],
    ];
    $("dStats").innerHTML = cells.map(([k, v, c]) =>
        `<div class="hs"><div class="k">${k}</div><div class="v ${c || ""}">${esc(v)}</div></div>`).join("");
    $("dSpark").innerHTML = spark(samples);
    $("dDets").innerHTML = data.detections.length
        ? data.detections.map((d) => detHtml(d)).join("")
        : `<div class="empty">No detections from this node.</div>`;
    const evs = data.sightings.filter((s) => s.event !== "sample").slice(-30).reverse();
    $("dEvents").innerHTML = evs.length
        ? evs.map((e) => `<div><span class="${e.event === "online" ? "on" : "off"}">${e.event.toUpperCase()}</span> ${fullTime(e.ts)}</div>`).join("")
        : `<div>No link events in 24 h.</div>`;
    renderGpsButton();
    if (S.editFor && S.editFor !== id) { S.editing = null; S.editFor = null; }
    if (S.audioDirty && S.audioDirty !== id) S.audioDirty = null;
    $("dFilters").innerHTML = "";
    renderRemote();
    $("dPlace").textContent = n.lat != null ? "Move on map" : "Place on map";
    $("dUnplace").style.display = n.lat != null ? "" : "none";
    $("dForget").style.display = n.online ? "none" : "";
}
function spark(samples) {
    if (samples.length < 2) return `<div class="none">Not enough samples yet</div>`;
    const W = 380, H = 70, lo = -100, hi = -30;
    const t0 = samples[0].ts, t1 = samples[samples.length - 1].ts || t0 + 1;
    const pts = samples.map((s) => {
        const x = ((s.ts - t0) / Math.max(1, t1 - t0)) * (W - 8) + 4;
        const y = H - 4 - ((Math.min(hi, Math.max(lo, s.rssi)) - lo) / (hi - lo)) * (H - 8);
        return `${x.toFixed(1)},${y.toFixed(1)}`;
    }).join(" ");
    return `<svg viewBox="0 0 ${W} ${H}" preserveAspectRatio="none">
        <defs><linearGradient id="sg" x1="0" x2="0" y1="0" y2="1"><stop offset="0" stop-color="#4ecdc4" stop-opacity=".35"/><stop offset="1" stop-color="#4ecdc4" stop-opacity="0"/></linearGradient></defs>
        <polygon points="4,${H} ${pts} ${W - 4},${H}" fill="url(#sg)" stroke="none"/>
        <polyline points="${pts}" fill="none" stroke="#4ecdc4" stroke-width="2" style="filter:drop-shadow(0 0 4px #4ecdc4)"/>
    </svg>`;
}
$("dSave").onclick = async () => {
    await api(`/api/nodes/${S.selected}`, { method: "POST", body: { alias: $("dAlias").value, notes: $("dNotes").value } });
    $("dTitle").textContent = $("dAlias").value || S.selected;
};
$("dAlias").addEventListener("keydown", (e) => { if (e.key === "Enter") $("dSave").click(); });
$("dNotes").addEventListener("change", () => api(`/api/nodes/${S.selected}`, { method: "POST", body: { notes: $("dNotes").value } }));
$("dPlace").onclick = () => setPlacing(true);
$("dUnplace").onclick = async () => {
    await api(`/api/nodes/${S.selected}`, { method: "POST", body: { lat: null, lon: null } });
    openDrawer(S.selected);
};
let forgetArmed = false;
$("dForget").onclick = async () => {
    // Two-tap confirm, same pattern as the device portal's burn-in button.
    if (!forgetArmed) {
        forgetArmed = true;
        $("dForget").textContent = "Tap again to forget";
        setTimeout(() => { forgetArmed = false; $("dForget").textContent = "Forget node"; }, 3000);
        return;
    }
    forgetArmed = false;
    $("dForget").textContent = "Forget node";
    await api(`/api/nodes/${S.selected}`, { method: "DELETE" });
    closeDrawer();
};

// ------------------------------------------------------------------
// history
// ------------------------------------------------------------------
function histParams() {
    const p = new URLSearchParams();
    if ($("hQ").value.trim()) p.set("q", $("hQ").value.trim());
    if ($("hNode").value) p.set("node", $("hNode").value);
    if ($("hDom").value) p.set("dom", $("hDom").value);
    if ($("hRange").value) p.set("since", String(Math.floor(now() - Number($("hRange").value))));
    return p;
}
function histFiltered() { return [...histParams().keys()].length > 0; }
function histRow(d) {
    return `<tr class="${d.is_new ? "new" : ""}"><td>${fullTime(d.ts)}</td><td>${esc(nodeName(d.node))}</td>
        <td>${esc(d.mac)}</td><td class="v">${devAlias(d.mac, d.alias) ? `<span class="mal">${esc(devAlias(d.mac, d.alias))}</span> ` : ""}${esc(d.descr || d.match || "")}</td>
        <td>${d.dom === "wifi" ? "WiFi" : "BLE"} ${esc(d.type || "")}</td><td>${d.rssi ?? ""}</td><td>${esc(d.source || "")}</td></tr>`;
}
async function loadHistory(append) {
    if (!append) S.hist.offset = 0;
    const p = histParams();
    p.set("limit", "200");
    p.set("offset", String(S.hist.offset));
    const r = await api(`/api/detections?${p}`);
    S.hist.total = r.total;
    const html = r.rows.map(histRow).join("");
    if (append) $("hBody").insertAdjacentHTML("beforeend", html);
    else $("hBody").innerHTML = html || `<tr><td colspan="7" style="text-align:center;padding:20px">No matching detections</td></tr>`;
    S.hist.offset += r.rows.length;
    $("hTotal").textContent = `${r.total} detections`;
    $("hMore").style.display = S.hist.offset < r.total ? "" : "none";
    const ep = histParams().toString();
    $("expCsv").href = `/api/export/detections.csv${ep ? "?" + ep : ""}`;
    $("expJson").href = `/api/export/detections.json${ep ? "?" + ep : ""}`;
}
let histTimer = null;
$("hQ").addEventListener("input", () => { clearTimeout(histTimer); histTimer = setTimeout(() => loadHistory(false), 250); });
["hNode", "hDom", "hRange"].forEach((id) => $(id).addEventListener("change", () => loadHistory(false)));
$("hMore").onclick = () => loadHistory(true);

// ------------------------------------------------------------------
// remote settings (commands go home -> mesh -> node)
// ------------------------------------------------------------------
function canCommand() {
    return Object.values(S.sources).some((x) => (x.kind === "serial" && x.connected) || x.kind === "demo");
}
function homeNode() { return S.fleet.nodes.find((n) => n.is_home); }
function syncState(n) {
    const h = homeNode();
    if (!n || !n.cfg || !n.cfg.fcrc || !h || !h.cfg || !h.cfg.fcrc || n.is_home) return null;
    return n.cfg.fcrc === h.cfg.fcrc ? "ok" : "drift";
}
function twoTap(btn, fn) {
    if (btn._armed) {
        btn._armed = false;
        btn.classList.remove("armed");
        btn.textContent = btn._label;
        fn();
        return;
    }
    btn._label = btn.textContent;
    btn._armed = true;
    btn.classList.add("armed");
    btn.textContent = "Tap again to confirm";
    setTimeout(() => {
        if (btn._armed) { btn._armed = false; btn.classList.remove("armed"); btn.textContent = btn._label; }
    }, 3500);
}
async function command(target, op, args) {
    const url = target === "all" ? "/api/fleet/cmd" : `/api/nodes/${target}/cmd`;
    const r = await api(url, { method: "POST", body: { op, args: args || {} } });
    if (!r.ok) toast("bad", "Command not sent", r.error);
    else onJob(r.job);
    return r;
}
function targetName(t) { return t === "all" ? "all nodes" : nodeName(t); }
const JOB_RANK = { queued: 0, sending: 1, sent: 2, done: 3, failed: 3, partial: 3 };
S.jobWaiters = {};
// Resolves when the job reaches done / failed / partial.
function waitJob(id) {
    const j = S.jobs.find((x) => x.id === id);
    if (j && ["done", "failed", "partial"].includes(j.state)) return Promise.resolve(j);
    return new Promise((res) => { S.jobWaiters[id] = res; });
}

// ------------------------------------------------------------------
// firmware updates over the mesh
// ------------------------------------------------------------------
S.firmware = null;
S.fwSel = new Set();
S.fwRun = null;          // {queue: [ids], current, results: {id: text}}
function fwBuildOf(n) { return (n.cfg && n.cfg.fw) || ""; }
function renderFirmware() {
    const m = S.firmware;
    $("fwInfo").innerHTML = m
        ? `<b>${esc(m.filename || "firmware.bin")}</b> build ${esc(m.build || m.date + " " + m.time)} | ${(m.size / 1024).toFixed(0)} KB | ${esc(m.sha256.slice(0, 12))}`
        : "No image uploaded";
    const nodes = S.fleet.nodes.slice().sort((a, b) => (a.is_home - b.is_home) || String(a.alias || a.id).localeCompare(b.alias || b.id));
    const job = (id) => S.jobs.find((j) => j.op && j.op.startsWith("fw_") && (j.target === id) && !["done", "failed", "partial"].includes(j.state))
        || (S.fwRun && S.fwRun.lastJob[id]);
    $("fwNodes").innerHTML = nodes.map((n) => {
        const j = job(n.id);
        const res = S.fwRun && S.fwRun.results[n.id];
        const pct = j && j.progress != null ? j.progress : null;
        const stage = j ? (j.op === "fw_stage" ? "staging on home" : j.ota && j.ota.state ? j.ota.state : j.state) : "";
        return `<label class="fw-node ${n.online ? "" : "off"}">
            <input type="checkbox" data-id="${esc(n.id)}" ${S.fwSel.has(n.id) ? "checked" : ""} ${n.online && !S.fwRun ? "" : "disabled"}>
            <span class="fw-name">${esc(n.alias || n.id)}${n.is_home ? " <i>home</i>" : ""}</span>
            <span class="fw-cur" title="Firmware build it runs now">${esc(fwBuildOf(n) || "build unknown")}</span>
            ${pct != null || res ? `<span class="fw-bar"><i style="width:${pct != null ? pct : 100}%"></i></span>
            <span class="fw-st">${esc(res || `${stage}${pct != null ? " " + pct + "%" : ""}${j && j.ota && j.ota.kbps ? " | " + j.ota.kbps + " kbps" : ""}`)}</span>` : ""}
        </label>`;
    }).join("");
    $("fwNodes").querySelectorAll("input[type=checkbox]").forEach((c) => c.onchange = () => {
        if (c.checked) S.fwSel.add(c.dataset.id); else S.fwSel.delete(c.dataset.id);
        $("fwGo").disabled = !S.firmware || !S.fwSel.size || !!S.fwRun;
    });
    $("fwGo").disabled = !S.firmware || !S.fwSel.size || !!S.fwRun;
    $("fwGo").textContent = S.fwRun ? "Updating..." : "Update selected";
}
$("fwFile").onchange = async () => {
    const f = $("fwFile").files[0];
    if (!f) return;
    const r = await fetch("/api/firmware", { method: "POST", body: f, headers: { "X-Filename": f.name, "Content-Type": "application/octet-stream" } });
    const d = await r.json();
    if (!d.ok) { toast("bad", "Firmware not accepted", d.error); return; }
    S.firmware = d.firmware;
    toast("good", "Firmware uploaded", `build ${d.firmware.build || d.firmware.date}, ${(d.firmware.size / 1024).toFixed(0)} KB`);
    renderFirmware();
    $("fwFile").value = "";
};
// Stage on the home once, then each selected remote in turn, the home last.
$("fwGo").onclick = () => twoTap($("fwGo"), async () => {
    const h = homeNode();
    if (!h || !S.firmware) return;
    const remotes = [...S.fwSel].filter((id) => id !== h.id);
    const home = S.fwSel.has(h.id);
    S.fwRun = { results: {}, lastJob: {} };
    renderFirmware();
    const run = async (target, op) => {
        const r = await command(target, op);
        if (!r.ok) return { state: "failed", error: r.error };
        S.fwRun.lastJob[target] = r.job;
        return waitJob(r.job.id);
    };
    const st = await run(h.id, "fw_stage");
    if (st.state !== "done") {
        S.fwRun.results[h.id] = "staging failed: " + (st.error || "");
    } else {
        for (const id of remotes) {
            const j = await run(id, "fw_send");
            S.fwRun.results[id] = j.state === "done" ? "updated, rebooting" : "failed: " + (j.error || Object.values(j.replies || {}).map((x) => x.text).join(", "));
            renderFirmware();
        }
        if (home) {
            const j = await run(h.id, "fw_self");
            S.fwRun.results[h.id] = j.state === "done" ? "installing, rebooting" : "failed: " + (j.error || "");
        }
    }
    const results = S.fwRun.results;
    S.fwRun = null;
    S.fwSel.clear();
    renderFirmware();
    const bad = Object.values(results).filter((t) => !/^updated|^installing/.test(t)).length;
    toast(bad ? "warn" : "good", "Firmware update finished", bad ? `${bad} node(s) need attention` : "all selected nodes updated");
});

function onJob(j) {
    const i = S.jobs.findIndex((x) => x.id === j.id);
    // The socket can deliver "done" before the POST that queued the job
    // returns: never let a stale update move a job backwards.
    if (i >= 0 && (JOB_RANK[S.jobs[i].state] ?? 0) > (JOB_RANK[j.state] ?? 0)) return;
    if (i >= 0) S.jobs[i] = j; else S.jobs.unshift(j);
    S.jobs.length = Math.min(S.jobs.length, 150);
    const prev = S.jobState[j.id];
    S.jobState[j.id] = j.state;
    const final = ["done", "failed", "partial"].includes(j.state);
    if (final && S.jobWaiters[j.id]) { S.jobWaiters[j.id](j); delete S.jobWaiters[j.id]; }
    if (j.op && j.op.startsWith("fw_")) renderFirmware();
    if (final && prev !== j.state && !j.auto && S.booted) {
        const reps = Object.entries(j.replies || {});
        const okN = reps.filter(([, r]) => r.st === 0).length;
        const body = j.state === "failed" ? j.error
            : j.target === "all" ? `${okN} of ${reps.length} nodes confirmed${j.error ? ", " + j.error : ""}`
            : (reps[0] ? reps[0][1].text : "done");
        toast(j.state === "done" ? "good" : j.state === "partial" ? "warn" : "bad",
              `${j.label}: ${targetName(j.target)}`, body);
    }
    renderJobs();
}
function jobHtml(j) {
    const reps = Object.entries(j.replies || {}).map(([n, r]) =>
        `<span class="${r.st === 0 ? "ok" : "bad"}" title="${esc(r.text)}">${esc(nodeName(n))}: ${esc(r.text)}</span>`).join("");
    const st = j.state === "sent" ? (j.target === "all" ? "collecting" : "waiting") : j.state;
    return `<div class="job ${esc(j.state)}">
        <div class="job-top"><span class="jstate">${esc(st)}</span><b>${esc(j.label)}</b>
            <span>${esc(targetName(j.target))}</span>${j.auto ? `<span class="cfg-age">auto</span>` : ""}
            <span class="jt">${hms(j.created)}</span></div>
        ${reps ? `<div class="job-rep">${reps}</div>` : ""}
        ${j.state === "failed" && j.error ? `<div class="job-err">${esc(j.error)}</div>` : ""}
    </div>`;
}
function renderJobs() {
    if ($("drawer").classList.contains("open") && S.selected) {
        const mine = S.jobs.filter((j) => (j.target === S.selected || j.target === "all") && !j.auto).slice(0, 12);
        $("dJobs").innerHTML = mine.map(jobHtml).join("") || `<div class="cfg-empty">No commands sent to this node yet.</div>`;
    }
    const all = S.jobs.filter((j) => $("fAuto").checked || !j.auto).slice(0, 60);
    $("fJobs").innerHTML = all.map(jobHtml).join("") || `<div class="cfg-empty">No commands yet.</div>`;
}
$("fAuto").onchange = renderJobs;

// One signature library (filters.library() on the server): the firmware
// presets and the portal's OUI database merged by vendor, so each vendor is
// listed once with every signature it has.
function vendorOptions(sel) {
    sel.innerHTML = allGroups().map((g) => `<option value="${esc(g.key)}" title="${esc(g.sources.join(" + "))}">${esc(g.label)} (${g.rows.length})</option>`).join("");
}

function cfgCells(n) {
    const c = n.cfg;
    const sync = syncState(n);
    return [
        ["Firmware", c.fw ? c.fw.replace(/^(\w+ \d+) \d{4}/, "$1") : "-"],
        ["Mode", (c.mode || "-").toUpperCase(), c.mode === "scan" ? "good" : "warn"],
        ["Burned in", c.locked ? "YES" : "NO", c.locked ? "good" : "warn"],
        ["Fleet key", keyState(n).txt, keyState(n).cls],
        ["Filters", `${c.filters}${sync ? `<span class="sync ${sync}">${sync === "ok" ? "SAME AS HOME" : "DIFFERS"}</span>` : ""}`, "", true],
        ["Uptime", uptime(c.up)],
        ["Heap", c.heap ? `${Math.round(c.heap / 1024)}K` : "-"],
        ["Delivered", c.dsent != null ? `${c.dacked}/${c.dsent}${c.dexp ? ` (${c.dexp} lost)` : ""}` : "-"],
        ["Channel", c.ch ?? "-", homeNode() && homeNode().cfg && c.ch !== homeNode().cfg.ch ? "bad" : ""],
    ];
}
function renderRemote() {
    const n = nodeById(S.selected);
    if (!n || !$("drawer").classList.contains("open")) return;
    $("dRemote").classList.toggle("no-command", !canCommand());
    if (n.cfg) {
        $("dCfg").innerHTML = cfgCells(n).map(([k, v, cls, raw]) =>
            `<div class="hs"><div class="k">${k}</div><div class="v ${cls || ""}">${raw ? v : esc(v)}</div></div>`).join("");
        $("dCfgAge").textContent = n.cfg_ts ? `read ${ago(n.cfg_ts)}` : "";
        if (S.audioDirty !== n.id) {
            $("dBuzzer").checked = !!n.cfg.buzzer;
            $("dLed").checked = !!n.cfg.led;
        }
    } else {
        $("dCfg").innerHTML = `<div class="cfg-empty">Settings not read yet${canCommand() ? ". Press Refresh." : "."}</div>`;
        $("dCfgAge").textContent = "";
    }
    renderFilters(n);
    renderJobs();
}
// ------------------------------------------------------------------
// Predefined filter groups: the firmware presets + the portal's OUI
// database. Each can be added to or removed from the node being edited;
// changes are staged in the editor and go out with Push.
// ------------------------------------------------------------------
function allGroups() {
    return ((S.filterMeta && S.filterMeta.library) || []).map((g) => ({ ...g, section: "Signature library" }));
}
// Rows a group adds. The BLE / WiFi switches pick which radio's signatures
// go in: BLE off drops the BLE rows (OUI, MAC, CID, UUID, name), WiFi off
// drops the probe/beacon rows.
const WIFI_T = ["PROBE", "BEACON"];
function groupRows(g) {
    return g.rows.filter((r) => (WIFI_T.includes(r.t) ? S.groupWifi !== false : S.groupBle !== false))
        .map((r) => ({ t: r.t, id: r.id, d: r.d }));
}
function normId(id) { return String(id || "").replace(/[:\-\s]/g, "").toUpperCase(); }
// Does an installed row belong to this group? Exact type + identifier.
function rowInGroup(g, f) {
    return g.rows.some((r) => r.t === f.t && normId(r.id) === normId(f.id));
}
function groupState(g, list) {
    const want = groupRows(g);
    const key = (r) => r.t + ":" + String(r.id).replace(/[:\-\s]/g, "").toUpperCase();
    const have = new Set(list.map(key));
    const got = want.filter((r) => have.has(key(r))).length;
    const any = list.some((f) => rowInGroup(g, f));
    return { got, total: want.length, any };
}
function renderGroups() {
    const box = $("feGroups");
    if (!box || !S.editing) return;
    let html = "", section = "";
    for (const g of allGroups()) {
        if (g.section !== section) { section = g.section; html += `<div class="gsec">${esc(section)}</div>`; }
        const st = groupState(g, S.editing);
        const full = st.total > 0 && st.got === st.total;
        const chip = full ? `<span class="gst on">INSTALLED</span>`
            : st.any ? `<span class="gst part">PARTIAL</span>` : `<span class="gst">-</span>`;
        const info = `${g.rows.length} signature${g.rows.length === 1 ? "" : "s"}${g.meta && g.meta["Category"] ? " | " + g.meta["Category"] : ""}`;
        html += `<div class="grow" data-key="${esc(g.key)}">
            ${chip}<span class="gname" title="${esc(info)}">${esc(g.label)}</span><span class="ginfo">${esc(info)}</span>
            ${full ? "" : `<button class="btn ghost mini gadd">Add</button>`}
            ${st.any ? `<button class="btn ghost mini danger grm">Remove</button>` : ""}
        </div>`;
    }
    box.innerHTML = html;
    box.querySelectorAll(".gadd").forEach((b) => b.onclick = () => {
        readEditor();
        const g = allGroups().find((x) => x.key === b.closest(".grow").dataset.key);
        const key = (r) => r.t + ":" + String(r.id).replace(/[:\-\s]/g, "").toUpperCase();
        const have = new Set(S.editing.map(key));
        const add = groupRows(g).filter((r) => !have.has(key(r)));
        S.editing = S.editing.filter((f) => f.id).concat(add);
        rerenderEditor();
        toast("good", `Staged: +${add.length} filter${add.length === 1 ? "" : "s"}`, `${g.label} (press Push to apply)`);
    });
    box.querySelectorAll(".grm").forEach((b) => b.onclick = () => {
        readEditor();
        const g = allGroups().find((x) => x.key === b.closest(".grow").dataset.key);
        const before = S.editing.length;
        S.editing = S.editing.filter((f) => !rowInGroup(g, f));
        rerenderEditor();
        toast("warn", `Staged: -${before - S.editing.length} filter${before - S.editing.length === 1 ? "" : "s"}`, `${g.label} (press Push to apply)`);
    });
}

function renderFilters(n) {
    const box = $("dFilters");
    $("dFiltInfo").textContent = n.filters ? `${n.filters.length} installed, read ${ago(n.filters_ts)}` : "";
    if (S.editing && S.editFor === n.id) {
        if (box.querySelector(".fedit")) return;   // keep focus while typing
        const types = (S.filterMeta && S.filterMeta.types) || {};
        box.innerHTML = `<div class="fedit"><div class="flist" id="fRows">${S.editing.map((f, i) => `
            <div class="frow" data-i="${i}">
                <select class="field ftype">${Object.keys(types).map((t) => `<option value="${t}" ${t === f.t ? "selected" : ""}>${t}</option>`).join("")}</select>
                <input class="field fi" value="${esc(f.id)}" placeholder="identifier">
                <input class="field fdesc" value="${esc(f.d)}" placeholder="description" maxlength="48">
                <button class="rm" title="Remove"><svg viewBox="0 0 24 24"><path d="M6 6l12 12M18 6 6 18"/></svg></button>
            </div>`).join("")}</div>
            <div class="fvalid" id="fValid"></div>
            <div class="ctl-row">
                <button class="btn ghost" id="feAdd">Add row</button>
                <button class="btn ghost" id="feHome">Copy home's list</button>
            </div>
            <div class="rb-head"><h4>Predefined filters</h4>
                <label class="switch" title="OUI-database adds: each OUI as a BLE filter"><input type="checkbox" id="feBle" ${S.groupBle !== false ? "checked" : ""}><span></span>BLE</label>
                <label class="switch" title="OUI-database adds: each OUI as a WiFi probe filter"><input type="checkbox" id="feWifi" ${S.groupWifi !== false ? "checked" : ""}><span></span>WiFi</label>
            </div>
            <div class="groups" id="feGroups"></div>
            <div class="ctl-row">
                <button class="btn" id="fePush">Push to ${esc(n.alias || n.id)}</button>
                <button class="btn ghost" id="feCancel">Cancel</button>
            </div>
            <div class="help-text">Pushing replaces the node's whole filter list, live: the node keeps scanning. CIDs and service UUIDs are 4 hex digits, OUIs 6, full MACs 12, names 1-16 characters.</div>
        </div>`;
        renderGroups();
        wireEditor(n);
        validateEdit();
        return;
    }
    if (!n.filters) {
        box.innerHTML = `<div class="cfg-empty">Filter list not read yet.</div>`;
        return;
    }
    box.innerHTML = n.filters.length
        ? `<div class="flist">${n.filters.map((f) => `<div class="frow"><span class="ft ${esc(f.t)}">${esc(f.t)}</span>
            <span class="fid">${esc(f.id)}</span><span class="fd" title="${esc(f.d)}">${esc(f.d)}</span>
            ${n.filters.length > 1 ? `<button class="rm-live" data-t="${esc(f.t)}" data-id="${esc(f.id)}" title="Remove from this node (live)"><svg viewBox="0 0 24 24"><path d="M6 6l12 12M18 6 6 18"/></svg></button>` : ""}</div>`).join("")}</div>`
        : `<div class="cfg-empty">No filters: this node has nothing to scan for.</div>`;
    // Two taps: the first arms the button, the second (within 3.5 s) removes.
    box.querySelectorAll(".rm-live").forEach((b) => b.onclick = () => {
        if (!b.classList.contains("armed")) {
            b.classList.add("armed");
            b.title = "Tap again to remove";
            setTimeout(() => { b.classList.remove("armed"); b.title = "Remove from this node (live)"; }, 3500);
            return;
        }
        command(n.id, "filters_del", { filters: [{ t: b.dataset.t, id: b.dataset.id, d: "" }] });
    });
}

// Live add / remove of one identifier, from the quick row in the node
// drawer ("d") or the fleet tab ("f"). Applied on the node at once, no
// reboot. Auto picks the type from the hex length: 6 = OUI, 12 = full MAC.
function quickTypeOptions(sel) {
    const types = (S.filterMeta && S.filterMeta.types) || {};
    sel.innerHTML = `<option value="AUTO">Auto</option>` + Object.entries(types)
        .filter(([t]) => t !== "META")
        .map(([t, label]) => `<option value="${t}">${esc(label)}</option>`).join("");
}
function quickResolve(pre) {
    const raw = $(pre + "QId").value.trim();
    const hex = raw.replace(/[:\-\s.]/g, "").replace(/^0x/i, "").toUpperCase();
    let t = $(pre + "QType").value;
    if (t === "AUTO") t = /^[0-9A-F]{6}$/.test(hex) ? "OUI" : /^[0-9A-F]{12}$/.test(hex) ? "MAC" : "";
    const hexLen = (S.filterMeta && S.filterMeta.hex) || {};
    return { t, id: hexLen[t] ? hex : raw, raw };
}
function quickRecords(pre, removing) {
    const { t, id, raw } = quickResolve(pre);
    if (!raw) { toast("bad", "Nothing to send", "Type an OUI, MAC or other identifier first"); return null; }
    if (!t) { toast("bad", "Pick a type", "Auto knows 6 hex digits (an OUI) and 12 (a full MAC)"); return null; }
    const d = removing ? "" : ($(pre + "QDesc").value.trim() || "Added live");
    const recs = [{ t, id, d }];
    if (t === "OUI" && $(pre + "QWifi").checked) {
        recs.push({ t: "PROBE", id, d });
        if (removing) recs.push({ t: "BEACON", id, d });
    }
    return recs;
}
function wireQuick(pre, send) {
    const input = $(pre + "QId");
    const check = () => {
        const { t, raw } = quickResolve(pre);
        input.classList.toggle("bad", !!raw && !t);
    };
    input.addEventListener("input", check);
    $(pre + "QType").addEventListener("change", check);
    input.addEventListener("keydown", (e) => { if (e.key === "Enter") $(pre + "QAdd").click(); });
    $(pre + "QAdd").onclick = async () => {
        const recs = quickRecords(pre, false);
        if (!recs) return;
        if (await send("filters_add", recs)) { input.value = ""; $(pre + "QDesc").value = ""; check(); }
    };
    $(pre + "QDel").onclick = () => {
        const recs = quickRecords(pre, true);
        if (recs) twoTap($(pre + "QDel"), () => send("filters_del", recs));
    };
}
wireQuick("d", async (op, recs) => (await command(S.selected, op, { filters: recs })).ok);
// Fleet-wide: every remote over the mesh, and the home itself over USB.
wireQuick("f", async (op, recs) => {
    const h = homeNode();
    const r = await command("all", op, { filters: recs });
    const rh = h ? await command(h.id, op, { filters: recs }) : { ok: false };
    return r.ok || rh.ok;
});
function readEditor() {
    S.editing = [...document.querySelectorAll("#fRows .frow")].map((r) => ({
        t: r.querySelector(".ftype").value, id: r.querySelector(".fi").value.trim(), d: r.querySelector(".fdesc").value,
    }));
}
function rerenderEditor() {
    $("dFilters").innerHTML = "";
    renderFilters(nodeById(S.editFor));
}
function wireEditor(n) {
    const rows = $("fRows");
    rows.addEventListener("input", () => { readEditor(); validateEdit(); });
    rows.addEventListener("change", () => { readEditor(); validateEdit(); renderGroups(); });
    rows.querySelectorAll(".rm").forEach((b) => b.onclick = () => {
        readEditor();
        S.editing.splice(Number(b.closest(".frow").dataset.i), 1);
        rerenderEditor();
    });
    $("feAdd").onclick = () => { readEditor(); S.editing.push({ t: "OUI", id: "", d: "" }); rerenderEditor(); };
    $("feBle").onchange = () => { S.groupBle = $("feBle").checked; renderGroups(); };
    $("feWifi").onchange = () => { S.groupWifi = $("feWifi").checked; renderGroups(); };
    $("feHome").onclick = () => {
        const h = homeNode();
        if (h && h.filters) {
            S.editing = h.filters.map((f) => ({ t: f.t, id: f.id, d: f.d }));
            rerenderEditor();
        } else if (h) {
            S.loadHomePending = true;
            command(h.id, "filters_get");
            toast("warn", "Reading the home's filters", "The list fills in when it arrives");
        }
    };
    $("feCancel").onclick = () => { S.editing = null; S.editFor = null; renderFilters(n); };
    $("fePush").onclick = () => twoTap($("fePush"), async () => {
        readEditor();
        const r = await command(n.id, "filters_set", { filters: S.editing });
        if (r.ok) { S.editing = null; S.editFor = null; renderFilters(n); }
    });
}
let validTimer = null;
function validateEdit() {
    clearTimeout(validTimer);
    validTimer = setTimeout(async () => {
        if (!S.editing) return;
        const hexLen = (S.filterMeta && S.filterMeta.hex) || {};
        document.querySelectorAll("#fRows .frow").forEach((r) => {
            const t = r.querySelector(".ftype").value;
            const v = r.querySelector(".fi").value.replace(/[:\-\s]/g, "").replace(/^0x/i, "");
            const bad = hexLen[t] ? !(new RegExp(`^[0-9a-fA-F]{${hexLen[t]}}$`).test(v)) : !(v.length >= 1 && v.length <= 16);
            r.querySelector(".fi").classList.toggle("bad", bad);
        });
        const r = await api("/api/filters/validate", { method: "POST", body: { filters: S.editing } });
        const el = $("fValid");
        if (!el) return;
        const h = homeNode();
        const same = r.ok && h && h.cfg && h.cfg.fcrc === r.crc;
        el.className = `fvalid ${r.ok ? "" : "bad"}`;
        el.textContent = r.ok ? `${r.count} filters, ${r.bytes} bytes, CRC ${r.crc}${same ? "  (same as home)" : ""}` : r.error;
        $("fePush").disabled = !r.ok;
    }, 200);
}

$("dRefresh").onclick = () => command(S.selected, "status");
$("dBuzzer").onchange = $("dLed").onchange = () => { S.audioDirty = S.selected; };
$("dAudio").onclick = async () => {
    await command(S.selected, "audio", { buzzer: $("dBuzzer").checked, led: $("dLed").checked });
    S.audioDirty = null;
};
$("dIdent").onclick = () => command(S.selected, "identify", { secs: 15 });
// Install: the chosen library entry goes to the node live (only the
// signatures it doesn't have yet are new; the node keeps scanning).
$("dPresetGo").onclick = () => {
    const g = allGroups().find((x) => x.key === $("dPreset").value);
    if (g) twoTap($("dPresetGo"), () => command(S.selected, "filters_add", { filters: groupRows(g) }));
};
$("dReboot").onclick = () => twoTap($("dReboot"), () => command(S.selected, "reboot"));
$("dFiltRead").onclick = () => command(S.selected, "filters_get");
$("dFiltEdit").onclick = () => {
    const n = nodeById(S.selected);
    if (n && !n.filters) {
        // Never start from an empty list: a push replaces the node's whole
        // list, so read what it has first and open the editor on that.
        S.editAfterRead = S.selected;
        command(S.selected, "filters_get");
        toast("warn", "Reading the node's filters first", "The editor opens on its current list");
        return;
    }
    S.editFor = S.selected;
    S.editing = (n && n.filters ? n.filters : []).map((f) => ({ t: f.t, id: f.id, d: f.d }));
    if (!S.editing.length) S.editing.push({ t: "OUI", id: "", d: "" });
    $("dFilters").innerHTML = "";
    renderFilters(n);
};

// fleet control tab
function renderFleetCtl() {
    renderFirmware();
    $("tab-fleetctl").classList.toggle("no-command", !canCommand());
    const nodes = S.fleet.nodes;
    $("fBody").innerHTML = nodes.map((n) => {
        const c = n.cfg;
        const sync = syncState(n);
        if (!c) {
            return `<tr data-id="${esc(n.id)}"><td class="v">${esc(n.alias || n.id)}</td><td colspan="11">${n.online ? "not read yet" : "offline"}</td></tr>`;
        }
        const yn = (v, goodWhen = 1) => `<td class="${v === goodWhen ? "on" : "off"}">${v ? "on" : "off"}</td>`;
        return `<tr data-id="${esc(n.id)}"><td class="v">${esc(n.alias || n.id)}${n.is_home ? ` <span class="cfg-age">home</span>` : ""}</td>
            <td>${esc(c.fw || "-")}</td><td>${esc(c.mode || "")}</td><td>${c.locked ? "yes" : "no"}</td>
            ${yn(c.buzzer)}${yn(c.led)}<td class="key-${keyState(n).cls}" title="${esc(keyState(n).tip || "")}">${keyState(n).txt}</td>
            <td>${c.filters}${sync ? `<span class="sync ${sync}">${sync === "ok" ? "SAME" : "DIFFERS"}</span>` : ""}</td>
            <td>${uptime(c.up)}</td><td>${c.heap ? Math.round(c.heap / 1024) + "K" : ""}</td>
            <td>${c.dsent != null ? `${c.dacked}/${c.dsent}` : ""}</td><td>${ago(n.cfg_ts)}</td></tr>`;
    }).join("");
    $("fBody").querySelectorAll("tr[data-id]").forEach((tr) => tr.onclick = () => selectNode(tr.dataset.id, true));
    const ch = $("fChan");
    if (!ch.options.length) {
        ch.innerHTML = Array.from({ length: 11 }, (_, i) => `<option value="${i + 1}">Channel ${i + 1}</option>`).join("");
    }
    if (document.activeElement !== ch && S.fleet.home && S.fleet.home.ch) ch.value = S.fleet.home.ch;
}
$("fStatus").onclick = () => { command("all", "status"); const h = homeNode(); if (h) command(h.id, "status"); };
$("fFilters").onclick = () => { command("all", "filters_get"); const h = homeNode(); if (h) command(h.id, "filters_get"); };
$("fIdent").onclick = () => command("all", "identify", { secs: 10 });
$("fAudio").onclick = () => command("all", "audio", { buzzer: $("fBuzzer").checked, led: $("fLed").checked });
$("fPush").onclick = () => twoTap($("fPush"), () => command("all", "meshpush"));
$("fPresetGo").onclick = () => {
    const g = allGroups().find((x) => x.key === $("fPreset").value);
    if (!g) return;
    twoTap($("fPresetGo"), async () => {
        const rows = { filters: groupRows(g) };
        await command("all", "filters_add", rows);
        const h = homeNode();
        if (h) await command(h.id, "filters_add", rows);
    });
};
$("fRechan").onclick = () => twoTap($("fRechan"), () => command("all", "rechan", { ch: Number($("fChan").value) }));
$("fReboot").onclick = () => twoTap($("fReboot"), () => command("all", "reboot"));

// ------------------------------------------------------------------
// devices (one row per MAC) + device drawer
// ------------------------------------------------------------------
async function loadDevices() {
    const p = new URLSearchParams();
    if ($("vQ").value.trim()) p.set("q", $("vQ").value.trim());
    if ($("vNode").value) p.set("node", $("vNode").value);
    if ($("vDom").value) p.set("dom", $("vDom").value);
    const r = await api(`/api/devices?${p}`);
    $("vTotal").textContent = `${r.total} devices${r.total > r.rows.length ? ` (showing ${r.rows.length})` : ""}`;
    $("vBody").innerHTML = r.rows.map((d) => {
        const al = devAlias(d.mac);
        return `<tr data-mac="${esc(d.mac)}"><td class="v">${al ? `<span class="mal">${esc(al)}</span> ` : ""}${esc(d.descr || "")}</td>
            <td>${esc(d.mac)}</td><td>${d.dom === "wifi" ? "WiFi" : "BLE"} ${esc(d.type || "")}</td><td>${d.hits}</td>
            <td>${fullTime(d.first)}</td><td>${ago(d.last)}</td><td>${d.best ?? ""}</td>
            <td class="nodes">${d.nodes.map((n) => esc(nodeName(n))).join(", ")}</td></tr>`;
    }).join("") || `<tr><td colspan="8" style="text-align:center;padding:20px">No devices</td></tr>`;
    $("vBody").querySelectorAll("tr[data-mac]").forEach((tr) => tr.onclick = () => openDevice(tr.dataset.mac));
}
let devTimer = null;
$("vQ").addEventListener("input", () => { clearTimeout(devTimer); devTimer = setTimeout(loadDevices, 250); });
["vNode", "vDom"].forEach((id) => $(id).addEventListener("change", loadDevices));

async function openDevice(mac) {
    if (!mac) return;
    S.device = mac;
    closeDrawer();
    $("vDrawer").classList.add("open");
    const r = await api(`/api/detections?mac=${encodeURIComponent(mac)}&limit=200`);
    const rows = r.rows;
    const a = S.macAliases[mac] || {};
    const vendor = rows.length ? (rows[0].descr || rows[0].match || "") : "";
    $("vTitle").textContent = a.alias || vendor || mac;
    $("vSub").textContent = `${mac}${vendor ? " | " + vendor : ""}`;
    if (document.activeElement !== $("vAlias")) $("vAlias").value = a.alias || "";
    if (document.activeElement !== $("vNotes")) $("vNotes").value = a.notes || "";
    const rssis = rows.map((x) => x.rssi).filter((x) => x != null);
    const first = rows.length ? rows[rows.length - 1].ts : null;
    const cells = [
        ["Hits", r.total, r.total ? "bad" : ""],
        ["First seen", first ? ago(first) : "-"],
        ["Last seen", rows.length ? ago(rows[0].ts) : "-"],
        ["Best RSSI", rssis.length ? `${Math.max(...rssis)} dBm` : "-"],
        ["Avg RSSI", rssis.length ? `${Math.round(rssis.reduce((x, y) => x + y, 0) / rssis.length)} dBm` : "-"],
        ["Domain", rows.length ? (rows[0].dom === "wifi" ? "WiFi" : "BLE") : "-"],
    ];
    $("vStats").innerHTML = cells.map(([k, v, c]) =>
        `<div class="hs"><div class="k">${k}</div><div class="v ${c || ""}">${esc(v)}</div></div>`).join("");
    const byNode = {};
    rows.forEach((x) => { byNode[x.node] = (byNode[x.node] || 0) + 1; });
    $("vSeenBy").innerHTML = Object.entries(byNode).sort((x, y) => y[1] - x[1])
        .map(([n, c]) => `<span data-node="${esc(n)}">${esc(nodeName(n))}<b>${c}</b></span>`).join("") || "-";
    $("vSeenBy").querySelectorAll("span[data-node]").forEach((el) => el.onclick = () => {
        $("vDrawer").classList.remove("open");
        selectNode(el.dataset.node, true);
    });
    $("vDets").innerHTML = rows.slice(0, 60).map((d) => detHtml(d)).join("") || `<div class="empty">No sightings.</div>`;
}
function closeDevice() { $("vDrawer").classList.remove("open"); S.device = null; }
$("vClose").onclick = closeDevice;
$("vSave").onclick = async () => {
    const r = await api(`/api/devices/${encodeURIComponent(S.device)}`, {
        method: "POST", body: { alias: $("vAlias").value, notes: $("vNotes").value } });
    S.macAliases = r.mac_aliases;
    $("vTitle").textContent = $("vAlias").value || S.device;
    renderFeed();
    if ($("tab-devices").classList.contains("active")) loadDevices();
    if ($("tab-history").classList.contains("active")) loadHistory(false);
};
$("vAlias").addEventListener("keydown", (e) => { if (e.key === "Enter") $("vSave").click(); });
$("vHistory").onclick = () => {
    $("hQ").value = S.device;
    closeDevice();
    document.querySelector('.tab[data-tab="history"]').click();
    loadHistory(false);
};

// ------------------------------------------------------------------
// console
// ------------------------------------------------------------------
function consoleLine(ts, source, line) {
    let cls = "";
    if (line.startsWith("TLM ")) cls = "tlm";
    else if (line.startsWith("{") || line.startsWith("DET ")) cls = "djs";
    else if (line.startsWith("[MESH]")) cls = "mesh";
    else if (line.startsWith("[HEARTBEAT]")) cls = "hb";
    if (source === "you") cls = "you";
    return `<span class="${cls}">${hms(ts)} ${esc(line)}</span>\n`;
}
function appendConsole(ts, source, line) {
    const c = $("console");
    const stick = c.scrollTop + c.clientHeight >= c.scrollHeight - 30;
    c.insertAdjacentHTML("beforeend", consoleLine(ts, source, line));
    while (c.childElementCount > 400) c.firstElementChild.remove();
    if (stick) c.scrollTop = c.scrollHeight;
}
$("consoleForm").onsubmit = async (e) => {
    e.preventDefault();
    const line = $("consoleLine").value.trim();
    if (!line) return;
    const r = await api("/api/console", { method: "POST", body: { line } });
    if (!r.ok) appendConsole(now(), "you", `! ${r.error}`);
    else $("consoleLine").value = "";
};

// ------------------------------------------------------------------
// settings
// ------------------------------------------------------------------
async function loadSettingsForm() {
    const s = S.settings;
    const ports = await api("/api/ports");
    const cur = s.serial_port || "auto";
    const opts = [["auto", "Auto-detect (Espressif VID 0x303A)"], ["off", "Off"]]
        .concat(ports.map((p) => [p.device, `${p.device}${p.espressif ? "  [ESP32]" : ""} ${p.desc}`]));
    if (!opts.some(([v]) => v === cur)) opts.push([cur, cur]);
    $("sPort").innerHTML = opts.map(([v, t]) => `<option value="${esc(v)}">${esc(t)}</option>`).join("");
    $("sPort").value = cur;
    const m = s.mqtt || {};
    $("mEn").checked = !!m.enabled;
    $("mHost").value = m.host || "";
    $("mPort").value = m.port || 1883;
    $("mUser").value = m.user || "";
    $("mPass").value = m.password || "";
    $("mTopic").value = m.topic || "+/detection";
    $("sTiles").value = s.tiles || "osm-dark";
    $("sNotify").checked = !!s.notify;
    $("sLinks").checked = s.links !== false;
    $("sRetention").value = s.retention_days || 0;
    $("sPoll").value = s.status_poll ?? 120;
    $("sTrusted").value = s.trusted_nets || "";
    $("sKeyNets").value = s.key_nets || "";
    $("sTilesCustom").value = s.tiles_custom || "";
    $("sTilesCustom").style.display = $("sTiles").value.startsWith("custom") ? "" : "none";
}
$("sNotify").onchange = async () => {
    if ($("sNotify").checked && "Notification" in window && Notification.permission !== "granted") {
        const p = await Notification.requestPermission();
        if (p !== "granted") {
            $("sNotify").checked = false;
            $("savedMsg").textContent = "The browser blocked notifications for this page.";
        }
    }
};
$("sTiles").onchange = () => { $("sTilesCustom").style.display = $("sTiles").value.startsWith("custom") ? "" : "none"; };
$("saveSettings").onclick = async () => {
    const body = {
        tiles: $("sTiles").value, tiles_custom: $("sTilesCustom").value.trim(),
        serial_port: $("sPort").value,
        notify: $("sNotify").checked, links: $("sLinks").checked,
        retention_days: Math.max(0, parseInt($("sRetention").value, 10) || 0),
        status_poll: Math.max(0, parseInt($("sPoll").value, 10) || 0),
        trusted_nets: $("sTrusted").value.trim(),
        key_nets: $("sKeyNets").value.trim(),
        mqtt: { enabled: $("mEn").checked, host: $("mHost").value.trim(), port: Number($("mPort").value) || 1883,
                user: $("mUser").value, password: $("mPass").value, topic: $("mTopic").value.trim() || "+/detection" },
    };
    const res = await api("/api/settings", { method: "POST", body });
    if (res.ok === false) { toast("bad", "Settings not saved", res.error); return; }
    S.settings = res;
    applyTiles();
    renderLinks();
    $("savedMsg").textContent = window.OUISPY_MODE === "live" ? "Saved. Feeds restarted." : "Saved (feeds are fixed in demo/replay mode).";
    setTimeout(() => $("savedMsg").textContent = "", 3000);
};
let clearArmed = false;
$("clearBtn").onclick = async () => {
    if (!clearArmed) {
        clearArmed = true;
        $("clearBtn").textContent = "Tap again to delete all";
        setTimeout(() => { clearArmed = false; $("clearBtn").textContent = "Clear detection history"; }, 3000);
        return;
    }
    clearArmed = false;
    $("clearBtn").textContent = "Clear detection history";
    await api("/api/detections/clear", { method: "POST" });
    S.dets = [];
    await afterDetsCleared();
};

// Clear one node's detections (drawer). The server tells every open
// dashboard (dets_cleared) so their feeds drop them too.
async function afterDetsCleared() {
    S.statsBase = await api("/api/stats");
    S.hourly = S.statsBase.hourly || [];
    renderFeed(); renderKpis(); renderNodes(); loadHistory(false);
    if (S.selected && $("drawer").classList.contains("open")) openDrawer(S.selected);
    if (S.device && $("vDrawer").classList.contains("open")) openDevice(S.device);
    if (typeof loadDevices === "function") loadDevices();
}
$("vClearDets").onclick = () => twoTap($("vClearDets"), async () => {
    const mac = S.device;
    const r = await api("/api/detections/clear", { method: "POST", body: { mac } });
    if (r.ok) toast("good", `Cleared ${r.n} detection${r.n === 1 ? "" : "s"}`, (S.macAliases[mac] || {}).alias || mac);
});
$("dClearDets").onclick = () => twoTap($("dClearDets"), async () => {
    const id = S.selected;
    const r = await api("/api/detections/clear", { method: "POST", body: { node: id } });
    if (r.ok) toast("good", `Cleared ${r.n} detection${r.n === 1 ? "" : "s"}`, nodeName(id));
});

// ------------------------------------------------------------------
// fleet key as QR (this computer + the key_nets networks only; server enforces it)
// ------------------------------------------------------------------
// Per-node key state: compares the fingerprint each node reports with the
// fingerprint of the key on this computer (neither reveals the key).
function keyState(n) {
    const c = n && n.cfg;
    if (!c) return { txt: "-", cls: "" };
    if (!c.key) return { txt: "NO KEY", cls: "bad" };
    if (!c.kfp) return { txt: "SET", cls: "warn", tip: "firmware too old to verify the key matches" };
    if (!S.keyFp) return { txt: "SET", cls: "warn", tip: "no key on this computer to compare" };
    return c.kfp === S.keyFp ? { txt: "KEY OK", cls: "good" } : { txt: "DIFFERS", cls: "bad", tip: "this node holds a different key: it can't talk to the fleet" };
}
async function loadFleetKeyStatus() {
    const st = await api("/api/fleetkey/status");
    S.keyFp = st.fp || null;
    const fp = st.fp ? `${st.fp.slice(0, 4)}-${st.fp.slice(4)}` : "";
    $("fkStatus").innerHTML = !st.present ? "No fleet key on this computer yet."
        : `Key fingerprint <b class="mono">${esc(fp)}</b>${st.viewable ? "" : " (the key itself is viewable only on the computer running OUI-SPY Mesh, or networks allowed under Access)"}`;
    $("fkShow").style.display = st.present && st.viewable ? "" : "none";
    $("fkCreate").style.display = !st.present && st.viewable ? "" : "none";
    const h = homeNode();
    const hs = keyState(h);
    $("fkHome").style.display = st.present && st.viewable && h && hs.txt !== "KEY OK" ? "" : "none";
    renderFleetCtl();
    renderRemote();
}
let fkTimer = null;
function hideFleetKey() {
    $("fkModal").classList.remove("open");
    $("fkQr").innerHTML = "";
    $("fkText").value = "";
    $("fkCopied").textContent = "";
    clearTimeout(fkTimer);
}
$("fkShow").onclick = () => twoTap($("fkShow"), async () => {
    const r = await fetch("/api/fleetkey/reveal", { method: "POST", cache: "no-store" });
    const d = await r.json();
    if (!d.ok) { toast("bad", "Fleet key", d.error); return; }
    const q = qrcode(0, "M");
    q.addData(d.key);
    q.make();
    $("fkQr").innerHTML = q.createSvgTag({ cellSize: 6, margin: 3, scalable: true });
    $("fkText").value = d.key.match(/.{1,4}/g).join(" ");   // grouped for reading; Copy and QR use it raw
    $("fkText").dataset.raw = d.key;
    $("fkSub").textContent = `Fingerprint ${d.fp.slice(0, 4)}-${d.fp.slice(4)}. Hides itself in 60 s`;
    $("fkModal").classList.add("open");
    clearTimeout(fkTimer);
    fkTimer = setTimeout(hideFleetKey, 60000);
});
$("fkClose").onclick = hideFleetKey;
$("fkModal").onclick = (e) => { if (e.target === $("fkModal")) hideFleetKey(); };
document.addEventListener("visibilitychange", () => { if (document.hidden) hideFleetKey(); });
$("fkCopy").onclick = async () => {
    const raw = $("fkText").dataset.raw || "";
    try {
        await navigator.clipboard.writeText(raw);       // needs a secure context
    } catch (e) {
        const t = document.createElement("textarea");   // plain-http fallback
        t.value = raw; document.body.appendChild(t); t.select();
        document.execCommand("copy"); t.remove();
    }
    $("fkCopied").textContent = "Copied (no spaces)";
};
$("fkCreate").onclick = () => twoTap($("fkCreate"), async () => {
    const r = await api("/api/fleetkey/create", { method: "POST" });
    if (!r.ok) { toast("bad", "Fleet key", r.error); return; }
    toast("good", "Fleet key created", `Fingerprint ${r.fp.slice(0, 4)}-${r.fp.slice(4)}. Now set it on every node.`);
    loadFleetKeyStatus();
});
$("fkHome").onclick = () => twoTap($("fkHome"), async () => {
    const r = await api("/api/fleetkey/provision_home", { method: "POST" });
    if (!r.ok) { toast("bad", "Fleet key", r.error); return; }
    onJob(r.job);
});

// tabs
document.querySelectorAll(".tab").forEach((t) => t.onclick = () => {
    document.querySelectorAll(".tab").forEach((x) => x.classList.toggle("active", x === t));
    document.querySelectorAll(".tab-body").forEach((b) => b.classList.toggle("active", b.id === `tab-${t.dataset.tab}`));
    if (t.dataset.tab === "settings") { loadSettingsForm(); loadFleetKeyStatus(); }
    if (t.dataset.tab === "devices") loadDevices();
    if (t.dataset.tab === "console") $("console").scrollTop = $("console").scrollHeight;
});

// ------------------------------------------------------------------
// boot + live updates
// ------------------------------------------------------------------
function applyFleet(f) {
    S.fleet = f;
    S.skew = Date.now() / 1000 - f.ts;
    renderHome();
    renderNodes();
    renderMap();
    watchHealth();
    renderRemote();
    renderFleetCtl();
    if (S.editAfterRead) {
        const en = nodeById(S.editAfterRead);
        if (en && en.filters) {
            const id = S.editAfterRead;
            S.editAfterRead = null;
            if (S.selected === id && $("drawer").classList.contains("open")) {
                $("dFiltEdit").click();
            }
        }
    }
    const h = homeNode();
    if (S.loadHomePending && h && h.filters && S.editing) {
        S.loadHomePending = false;
        S.editing = h.filters.map((f) => ({ t: f.t, id: f.id, d: f.d }));
        rerenderEditor();
    }
}

async function boot() {
    const st = await api("/api/state");
    S.settings = st.settings;
    S.sources = st.sources;
    S.statsBase = st.stats;
    S.dets = st.recent;
    S.dets.forEach((d) => { if (!S.macSeen[d.mac] || d.ts > S.macSeen[d.mac]) S.macSeen[d.mac] = d.ts; });
    S.hourly = st.hourly || [];
    S.macAliases = st.mac_aliases || {};
    S.filterMeta = st.filter_meta;
    syncTrailSlider();
    try { await loadTrails(Math.max(S.trailSeconds, 1800)); } catch (e) { /* older server */ }
    S.jobs = st.jobs || [];
    S.jobs.forEach((j) => { S.jobState[j.id] = j.state; });
    tileOptions($("mapLayer"));
    tileOptions($("sTiles"));
    vendorOptions($("dPreset"));
    vendorOptions($("fPreset"));
    quickTypeOptions($("dQType"));
    quickTypeOptions($("fQType"));
    try { S.firmware = (await api("/api/firmware")).firmware; } catch (e) { /* older server */ }
    loadFleetKeyStatus();
    const badge = $("modeBadge");
    badge.textContent = st.mode.toUpperCase();
    badge.className = `mode-badge ${st.mode}`;
    applyFleet(st.fleet);
    applyTiles();
    initView();
    renderTrails();
    renderSources();
    renderToggles();
    renderFeed();
    renderKpis();
    st.console.forEach(([ts, src, line]) => appendConsole(ts, src, line));
    loadHistory(false);
    S.booted = true;
    renderJobs();
    renderFleetCtl();

    const sock = io({ transports: ["websocket", "polling"] });
    sock.on("fleet", applyFleet);
    sock.on("det", onDetection);
    sock.on("sources", (s) => { S.sources = s; renderSources(); });
    sock.on("console", (c) => { appendConsole(c.ts, c.source, c.line); if (c.source === "serial") serialBlink(); });
    sock.on("mac_aliases", (m) => {
        S.macAliases = m;
        renderFeed();
        if ($("tab-devices").classList.contains("active")) loadDevices();
        if (S.device && $("vDrawer").classList.contains("open")) openDevice(S.device);
    });
    sock.on("settings", (s) => {
        // Reload tiles only when the layer itself changed (re-adding the tile
        // layer blanks the map until the tiles come back), and keep this
        // screen's own map position.
        const layerChanged = s.tiles !== S.settings.tiles || s.tiles_custom !== S.settings.tiles_custom;
        S.settings = { ...s, map_view: S.settings.map_view };
        if (layerChanged) applyTiles();
        renderLinks(); renderToggles();
        // Refill the form unless someone is typing in it right now.
        if (!$("tab-settings").contains(document.activeElement)) loadSettingsForm();
    });
    sock.on("fleetkey", () => loadFleetKeyStatus());
    sock.on("firmware", (m) => { S.firmware = m; renderFirmware(); });
    sock.on("job", onJob);
    sock.on("dets_cleared", (e) => {
        S.dets = S.dets.filter((d) => (e.node && d.node !== e.node) || (e.mac && d.mac !== e.mac));
        afterDetsCleared();
    });
    sock.on("gps", onGps);
    sock.on("gps_stop", (g) => {
        delete S.gpsLive[g.node];
        if (g.cleared) delete S.trails[g.node];
        renderMap(); renderTrails();
        if (S.selected === g.node) renderGpsButton();
    });
    sock.on("node_event", (e) => {
        const n = nodeById(e.id);
        if (S.booted && e.id !== S.fleet.home.id && !(n && n.is_home)) {
            if (e.event === "offline") toast("bad", `${nodeName(e.id)} dropped off the mesh`, `${e.id} | last ${n && n.rssi ? n.rssi + " dBm" : "no rssi"}`, () => selectNode(e.id, true));
            else toast("good", `${nodeName(e.id)} joined`, e.id, () => selectNode(e.id, true));
        }
        if (S.selected === e.id && $("drawer").classList.contains("open")) openDrawer(e.id);
    });
    sock.on("disconnect", () => { S.sources = { ui: { kind: "dashboard", connected: false, detail: "server connection lost" } }; renderSources(); });
    // (Re)connect: catch up on everything that happened while the socket
    // was down (phone asleep, Wi-Fi blip, server restart), not just the fleet.
    sock.on("connect", async () => {
        const st2 = await api("/api/state");
        S.sources = st2.sources; renderSources(); applyFleet(st2.fleet);
        S.dets = st2.recent; S.statsBase = st2.stats; S.hourly = st2.hourly || [];
        S.macAliases = st2.mac_aliases || {};
        S.settings = st2.settings;
        (st2.jobs || []).forEach((j) => { S.jobState[j.id] = j.state; });
        S.jobs = st2.jobs || S.jobs;
        renderFeed(); renderKpis(); renderNodes(); renderJobs(); renderFleetCtl();
        loadHistory(false);
        if ($("tab-devices").classList.contains("active")) loadDevices();
        if (S.selected && $("drawer").classList.contains("open")) openDrawer(S.selected);
        if (S.device && $("vDrawer").classList.contains("open")) openDevice(S.device);
        loadFleetKeyStatus();
    });
}

setInterval(() => {
    $("clock").textContent = new Date().toLocaleTimeString([], { hour12: false });
    document.querySelectorAll("[data-age]").forEach((el) => {
        const ts = Number(el.dataset.age);
        const off = el.textContent.startsWith("last ");
        el.textContent = (off ? "last " : "") + ago(ts);
    });
}, 1000);

boot();
})();
