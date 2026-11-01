// Mesh status: both meshes at a glance, from /api/meshstatus. Top of the Fleet tab: the
// ESP-NOW fleet (who is up, channel, encryption, who hears whom and how well), the
// Meshtastic link to a Tab5 in the field, and what is on this computer's USB. Two pills in
// the top bar (MESH, LORA) say the same in a word and open it.
(function () {
    const { api, esc, $, nodeName, showTab, on } = window.OUISPY;
    const LINK_OK_S = 900;   // the field Tab5 says hello every 5 min: three missed is trouble

    const ago = (s) => s == null ? "never" : s < 60 ? `${Math.round(s)}s ago` : s < 3600 ? `${Math.round(s / 60)}m ago`
        : s < 86400 ? `${Math.round(s / 3600)}h ago` : `${Math.round(s / 86400)}d ago`;
    const sig = (r) => r == null ? "" : r >= -60 ? "s4" : r >= -72 ? "s3" : r >= -85 ? "s2" : "s1";
    const bars = (r) => {
        const n = r == null ? 0 : r >= -60 ? 4 : r >= -72 ? 3 : r >= -85 ? 2 : 1;
        return `<span class="ms-bars ${sig(r)}">${[1, 2, 3, 4].map((i) => `<i class="${i <= n ? "on" : ""}"></i>`).join("")}</span>`;
    };

    function verdicts(d) {
        const e = d.espnow, m = d.meshtastic;
        const homeUp = e.home_age != null && e.home_age < 20;
        const mesh = !homeUp ? ["bad", "home silent"] : e.auth_fail ? ["bad", `${e.auth_fail} wrong-key frames`]
            : e.online === e.total ? ["good", `${e.online}/${e.total}`] : ["warn", `${e.online}/${e.total}`];
        let lora;
        if (!m.enabled) lora = ["off", "off"];
        else if (m.state !== "ready") lora = ["bad", m.state || "down"];
        else if (m.link && m.link.age < LINK_OK_S) lora = ["good", m.link.wifi ? "ready, Tab5 on Wi-Fi" : "linked"];
        else lora = ["warn", "ready, no field link"];
        return { mesh, lora, homeUp };
    }

    function pills(d) {
        const v = verdicts(d);
        $("meshPills").innerHTML = [["MESH", v.mesh], ["LORA", v.lora]].map(([k, [cls, word]]) =>
            `<button type="button" class="src-pill ms-pill" title="Mesh status (Fleet tab)"><span class="dot ${cls === "good" ? "on" : cls === "off" ? "" : cls}"></span><b>${k}</b> <span class="src-detail">${esc(word)}</span></button>`).join("");
        $("meshPills").querySelectorAll("button").forEach((b) => b.onclick = () => {
            showTab("fleet");
            $("meshStatus").scrollIntoView({ block: "start", behavior: "smooth" });
        });
    }

    function panel(d) {
        const e = d.espnow, m = d.meshtastic, v = verdicts(d);
        const rate = { lr250k: "LR 250 kbps", lr500k: "LR 500 kbps" }[e.rate] || e.rate || "?";
        const tile = (cls, title, big, lines) => `<div class="ms-tile ${cls}"><div class="ms-k">${title}</div>
            <div class="ms-big">${big}</div>${lines.filter(Boolean).map((l) => `<div class="ms-l">${l}</div>`).join("")}</div>`;
        const link = m.link;
        const linkLine = !link ? "field Tab5: not heard since the dashboard started"
            : `field Tab5${link.node ? ` (on ${esc(nodeName(link.node))})` : ""}, radio ${esc(link.from || "?")}: ${ago(link.age)}` +
              (link.rssi != null ? `, ${link.rssi} dBm` : "") + (link.snr != null ? `, SNR ${link.snr}` : "") +
              (link.wifi ? ", on Wi-Fi (no LoRa traffic needed)" : ", over LoRa");
        const usb = d.usb || {};
        const u = (k, label) => {
            const s = usb[k];
            if (!s) return "";
            // Nothing plugged in is normal for a wardriver; a missing home or bridge is not.
            const dot = s.connected ? "on" : k === "wardrive-usb" ? "" : "warn";
            return `<span class="dot ${dot}"></span> <b>${label}</b> ${esc(s.connected ? (s.port || s.detail || "") : s.detail || "")}`;
        };
        const tiles = [
            tile(v.mesh[0], "ESP-NOW mesh", `${e.online}<small> / ${e.total} up</small>`, [
                `channel ${esc(String(e.ch ?? "?"))}, ${esc(rate)}, ${e.enc ? "encrypted with the fleet key" : "<b class=bad>NOT encrypted</b>"}`,
                `wrong key ${e.auth_fail}, replays ${e.replays}, undelivered ${e.lost}`,
                v.homeUp ? `home up ${ago(e.home_up).replace(" ago", "")}` : `<b class=bad>home silent ${ago(e.home_age)}</b>`,
            ]),
            tile(v.lora[0], "Meshtastic", esc((m.enabled ? m.state || "?" : "off").toUpperCase()), [
                m.enabled ? `${esc(m.radio || "radio ?")} on ${esc(m.channel || "?")}` : "turn it on in Settings",
                m.enabled ? `sent ${m.sent ?? 0}, failed ${m.failed ?? 0}, waiting ${m.queued ?? 0}, nodes heard ${m.heard ?? 0}` : "",
                m.error ? `<b class=bad>${esc(m.error)}</b>` : m.enabled ? linkLine : "",
            ]),
            tile("plain", "USB on this computer", "", [u("serial", "Home node"), u("bridge", "Tab5 bridge"),
                m.port ? `<span class="dot ${m.state === "ready" ? "on" : "warn"}"></span> <b>Meshtastic radio</b> ${esc(m.port)}` : "",
                u("wardrive-usb", "Wardriver")]),
        ];
        // Who hears whom: each row is what that node last reported hearing (signal of each
        // neighbour). A node that is down shows its last report, dimmed.
        const ids = e.nodes.map((n) => n.id);
        const head = `<tr><th>hears &rarr;</th>${e.nodes.map((n) => `<th title="${esc(n.id)}">${esc(n.name)}</th>`).join("")}</tr>`;
        const rows = e.nodes.map((n) => {
            const h = Object.fromEntries(n.hears.map((x) => [x.id, x.rssi]));
            const state = n.online ? `<span class="dot on"></span>` : `<span class="dot bad"></span>`;
            const sub = n.online ? (n.home ? "home" : `${n.peers ?? 0} peers`) : `down, last ${ago(n.age)}`;
            return `<tr class="${n.online ? "" : "down"}"><th>${state} ${esc(n.name)}<small>${esc(sub)}${n.key === false ? ", <b class=bad>other key</b>" : ""}</small></th>` +
                ids.map((id) => id === n.id ? `<td class="self"></td>` : h[id] != null
                    ? `<td class="${sig(h[id])}" title="${esc(n.name)} hears ${esc(nodeName(id))} at ${h[id]} dBm">${bars(h[id])}<span>${h[id]}</span></td>`
                    : `<td class="none">-</td>`).join("") + `</tr>`;
        }).join("");
        $("meshStatus").innerHTML = `<div class="ms-tiles">${tiles.join("")}</div>
            <div class="ms-mx-wrap"><table class="ms-mx">${head}${rows}</table></div>
            <div class="ms-foot">Rows: what each node last reported hearing. Strong &ge; -60 dBm, good &ge; -72, weak &ge; -85, poor below.</div>`;
    }

    async function tick() {
        try {
            const d = await api("/api/meshstatus");
            pills(d);
            if (document.body.dataset.mv === "fleet") panel(d);
            else tick.last = d;
        } catch (e) { /* the next tick tries again */ }
    }
    // Instant: any change to the fleet, the USB feeds or the Meshtastic connector re-reads it
    // (coalesced); the 5 s tick only keeps the "ago" times moving.
    let soon = null;
    const refresh = () => { clearTimeout(soon); soon = setTimeout(tick, 250); };
    window.meshStatusRefresh = refresh;
    ["fleet", "sources", "meshtastic", "node_event"].forEach((ev) => on(ev, refresh));
    document.querySelector('.tab[data-tab="fleet"]').addEventListener("click", () => { if (tick.last) panel(tick.last); tick(); });
    tick();
    setInterval(tick, 5000);
})();
