// Map marks: pins (icon, colour, name) and named areas, shared with every dashboard and
// the Tab5 through /api/marks and the "marks" socket event (marks.py has the format).
// Pin: the toolbar's Pin button, then click the map. Area: Area, click the corners, Finish.
// Click any mark to rename, recolour, change its icon or delete it; drag a pin to move it.
(function () {
    const { map, api, esc, $ } = window.OUISPY;
    // Plain stroke icons on a 24x24 grid, TAK-style basics. The Tab5 draws the same set.
    const ICONS = {
        pin:     '<path d="M12 21s-6-6.2-6-11a6 6 0 0 1 12 0c0 4.8-6 11-6 11z"/><circle cx="12" cy="10" r="2.2"/>',
        flag:    '<path d="M6 21V4M6 4h11l-2.5 4L17 12H6"/>',
        target:  '<circle cx="12" cy="12" r="7"/><circle cx="12" cy="12" r="2.5"/><path d="M12 2v4M12 18v4M2 12h4M18 12h4"/>',
        house:   '<path d="M4 11 12 4l8 7M6 10v10h12V10M10 20v-5h4v5"/>',
        car:     '<path d="M4 16v-4l2-5h12l2 5v4zM4 16v2M20 16v2M4 12h16"/><circle cx="8" cy="14" r="1"/><circle cx="16" cy="14" r="1"/>',
        person:  '<circle cx="12" cy="6" r="2.5"/><path d="M12 9v6M8 12h8M12 15l-3 6M12 15l3 6"/>',
        drone:   '<circle cx="6" cy="6" r="2.5"/><circle cx="18" cy="6" r="2.5"/><circle cx="6" cy="18" r="2.5"/><circle cx="18" cy="18" r="2.5"/><path d="M8 8l8 8M16 8l-8 8"/><rect x="10" y="10" width="4" height="4"/>',
        camera:  '<rect x="3" y="7" width="18" height="12" rx="2"/><circle cx="12" cy="13" r="3.5"/><path d="M8 7l2-3h4l2 3"/>',
        warning: '<path d="M12 3 2 20h20zM12 10v4M12 17v.5"/>',
        star:    '<path d="m12 3 2.7 5.6 6.1.8-4.5 4.2 1.1 6.1L12 16.8 6.6 19.7l1.1-6.1L3.2 9.4l6.1-.8z"/>',
        camp:    '<path d="M3 20 12 5l9 15zM12 5v15M9 20l3-6 3 6"/>',
        radio:   '<path d="M12 10v11M8 21h8M6 6a8 8 0 0 0 0 8M18 6a8 8 0 0 1 0 8M9 8a4 4 0 0 0 0 4M15 8a4 4 0 0 1 0 4"/><circle cx="12" cy="10" r="1.3"/>',
    };
    const col = (h) => `hsl(${h} 100% 60%)`;
    const svg = (icon) => `<svg viewBox="0 0 24 24">${ICONS[icon] || ICONS.pin}</svg>`;
    const layer = L.layerGroup().addTo(map);
    const draftLayer = L.layerGroup().addTo(map);
    let marks = [], mode = null, draftPts = [], popup = null;

    // ---- drawing ----
    function render() {
        layer.clearLayers();
        for (const m of marks) {
            if (m.kind === "area" && m.pts && m.pts.length >= 3) {
                const pg = L.polygon(m.pts, { color: col(m.hue), weight: 2.5, fillColor: col(m.hue), fillOpacity: 0.14,
                                              className: "mk-area" }).addTo(layer);
                if (m.name) pg.bindTooltip(esc(m.name), { permanent: true, direction: "center", className: "mk-area-label" });
                pg.on("click", (e) => { L.DomEvent.stop(e); if (!mode) edit(m, e.latlng); });
            } else if (m.kind === "pin" && m.lat != null) {
                const icon = L.divIcon({ className: "", iconSize: [0, 0],
                    html: `<div class="mk-pin" style="--mc:${col(m.hue)}">${svg(m.icon)}${m.name ? `<span class="mk-label">${esc(m.name)}</span>` : ""}</div>` });
                const mk = L.marker([m.lat, m.lon], { icon, draggable: true, zIndexOffset: 700, keyboard: false }).addTo(layer);
                mk.on("click", (e) => { L.DomEvent.stop(e); if (!mode) edit(m, mk.getLatLng()); });
                mk.on("dragend", () => {
                    const p = mk.getLatLng();
                    save({ id: m.id, kind: "pin", lat: p.lat, lon: p.lng });
                });
            }
        }
    }
    function renderDraft() {
        draftLayer.clearLayers();
        if (mode !== "area" || !draftPts.length) return;
        L.polyline(draftPts.length >= 3 ? [...draftPts, draftPts[0]] : draftPts,
                   { color: "#22d3ee", weight: 2, dashArray: "6 6", interactive: false }).addTo(draftLayer);
        draftPts.forEach((p) => L.circleMarker(p, { radius: 5, color: "#22d3ee", fillColor: "#06080b", fillOpacity: 1,
                                                    weight: 2, interactive: false }).addTo(draftLayer));
    }

    // ---- server ----
    async function save(m) {
        const r = await api("/api/marks", { method: "POST", body: m });
        if (!r.ok) alert(r.error || "could not save the mark");
        return r;
    }

    // ---- editor (a popup on the mark) ----
    function edit(m, at, isNew) {
        const d = { ...m };
        const el = document.createElement("div");
        el.className = "mk-edit";
        el.innerHTML = `
            <div class="mk-edit-head">${isNew ? "New" : "Edit"} ${d.kind === "area" ? "area" : "pin"}</div>
            <input class="mk-name" maxlength="40" placeholder="Name" value="${esc(d.name || "")}">
            <div class="mk-hue-row"><span class="mk-swatch"></span><input class="mk-hue" type="range" min="0" max="359" value="${d.hue ?? 190}"></div>
            ${d.kind === "pin" ? `<div class="mk-icons">${Object.keys(ICONS).map((k) =>
                `<button type="button" data-icon="${k}" title="${k}" class="${k === (d.icon || "pin") ? "on" : ""}">${svg(k)}</button>`).join("")}</div>` : ""}
            <div class="mk-btns">
                <button type="button" class="mk-save">Save</button>
                ${isNew ? "" : `<button type="button" class="mk-del">Delete</button>`}
                <button type="button" class="mk-cancel">Cancel</button>
            </div>`;
        const swatch = () => {
            el.style.setProperty("--mc", col(d.hue ?? 190));
        };
        swatch();
        el.querySelector(".mk-hue").oninput = (e) => { d.hue = +e.target.value; swatch(); };
        el.querySelectorAll(".mk-icons button").forEach((b) => b.onclick = () => {
            d.icon = b.dataset.icon;
            el.querySelectorAll(".mk-icons button").forEach((x) => x.classList.toggle("on", x === b));
        });
        el.querySelector(".mk-save").onclick = async () => {
            d.name = el.querySelector(".mk-name").value.trim();
            const r = await save(d);
            if (r.ok) map.closePopup();
        };
        const del = el.querySelector(".mk-del");
        if (del) del.onclick = async () => {
            if (!confirm(`Delete ${d.name || "this mark"}?`)) return;
            await api("/api/marks", { method: "POST", body: { id: d.id, delete: true } });
            map.closePopup();
        };
        el.querySelector(".mk-cancel").onclick = () => map.closePopup();
        L.DomEvent.disableClickPropagation(el);
        popup = L.popup({ className: "mk-popup", minWidth: 260, autoPanPadding: [40, 40] }).setLatLng(at).setContent(el).openOn(map);
        setTimeout(() => el.querySelector(".mk-name").focus(), 50);
    }

    // ---- tools ----
    const pinBtn = $("mapPin"), areaBtn = $("mapArea");
    const bar = L.control({ position: "bottomleft" });
    bar.onAdd = () => {
        const div = L.DomUtil.create("div", "mk-bar");
        div.hidden = true;
        L.DomEvent.disableClickPropagation(div);
        return div;
    };
    bar.addTo(map);
    function setMode(m) {
        mode = m;
        draftPts = [];
        pinBtn.classList.toggle("on", m === "pin");
        areaBtn.classList.toggle("on", m === "area");
        pinBtn.setAttribute("aria-pressed", m === "pin");
        areaBtn.setAttribute("aria-pressed", m === "area");
        map.getContainer().classList.toggle("mk-placing", !!m);
        if (m === "area") map.doubleClickZoom.disable(); else map.doubleClickZoom.enable();
        renderDraft();
        renderBar();
    }
    function renderBar() {
        const div = bar.getContainer();
        if (!mode) { div.hidden = true; return; }
        div.hidden = false;
        if (mode === "pin") {
            div.innerHTML = `<span>Click the map to drop a pin</span><button type="button" data-a="cancel">Cancel</button>`;
        } else {
            div.innerHTML = `<span>Area: click the corners (${draftPts.length} point${draftPts.length === 1 ? "" : "s"})</span>
                <button type="button" data-a="undo" ${draftPts.length ? "" : "disabled"}>Undo</button>
                <button type="button" data-a="finish" ${draftPts.length >= 3 ? "" : "disabled"}>Finish</button>
                <button type="button" data-a="cancel">Cancel</button>`;
        }
        div.querySelectorAll("button").forEach((b) => b.onclick = () => {
            if (b.dataset.a === "cancel") setMode(null);
            else if (b.dataset.a === "undo") { draftPts.pop(); renderDraft(); renderBar(); }
            else if (b.dataset.a === "finish") finishArea();
        });
    }
    function finishArea() {
        if (draftPts.length < 3) return;
        const pts = draftPts.map((p) => [p.lat, p.lng]);
        const c = L.polygon(draftPts).getBounds().getCenter();
        setMode(null);
        edit({ id: newId(), kind: "area", name: "", hue: 300, pts }, c, true);
    }
    const newId = () => "w" + Date.now().toString(36) + Math.random().toString(36).slice(2, 6);
    pinBtn.onclick = () => setMode(mode === "pin" ? null : "pin");
    areaBtn.onclick = () => setMode(mode === "area" ? null : "area");
    map.on("click", (e) => {
        if (mode === "pin") {
            setMode(null);
            edit({ id: newId(), kind: "pin", name: "", hue: 190, icon: "pin", lat: e.latlng.lat, lon: e.latlng.lng }, e.latlng, true);
        } else if (mode === "area") {
            draftPts.push(e.latlng);
            renderDraft();
            renderBar();
        }
    });
    map.on("dblclick", () => { if (mode === "area" && draftPts.length >= 3) finishArea(); });
    document.addEventListener("keydown", (e) => {
        if (e.key === "Escape" && mode) setMode(null);
        if (e.key === "Enter" && mode === "area" && document.activeElement === document.body) finishArea();
    });

    window.marksApply = (list) => { marks = list || []; render(); };
})();
