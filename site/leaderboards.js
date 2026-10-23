// The leaderboards page: a map of Colorado with a pin for every speed trap, speed zone and Rivals
// event, and the board beside it. The map is images/colorado-map.svg (our own drawing of the
// roads, built by tools/build_map_roads.py from the game's road layout); the pins are data/map_points.json
// (tools/extract_map_points.py: the in-game names and places). The public server speaks plain
// http (a tunnel), which an https page may not fetch, so the site publishes a copy of the
// server's /leaderboards/snapshot as data/leaderboards.json (see .github/workflows/pages.yml).
// For testing, ?api=http://host:port reads the server's own /leaderboards/snapshot instead.
(function () {
  "use strict";
  var TOP = 50;
  var NS = "http://www.w3.org/2000/svg";
  var MAX_ZOOM = 8;
  var KIND = { "speed-trap": "trap", "speed-zone": "zone", "event": "event", "time-trial": "trial" };
  var KIND_NAME = { trap: "Speed trap", zone: "Speed zone", event: "Rivals event", trial: "Monthly Rivals" };
  var GROUPS = [["trap", "Speed traps"], ["zone", "Speed zones"], ["event", "Events and Rivals"], ["trial", "Monthly Rivals"]];
  // Pins: a constant size on screen (the map scales them with --cm-s); 0,0 is the place.
  var SHAPES = {
    trap: '<circle class="pin-ring" r="14"/><g class="pin-in"><circle class="pin-hit" r="18"/>' +
      '<circle class="pin-shadow" cy="2.5" r="11.5"/><circle class="pin-body" r="11"/>' +
      '<path class="pin-glyph" d="M1.6-7.2-4.8 1.2H-.6L-1.6 7.2 4.8-1.2H.6Z"/></g>',
    zone: '<rect class="pin-ring" x="-14" y="-14" width="28" height="28" rx="8"/><g class="pin-in">' +
      '<circle class="pin-hit" r="18"/><rect class="pin-shadow" x="-11" y="-8.5" width="22" height="22" rx="6"/>' +
      '<rect class="pin-body" x="-11" y="-11" width="22" height="22" rx="6"/>' +
      '<path class="pin-glyph-line" d="M-6.2 4A6.6 6.6 0 1 1 6.2 4"/><path class="pin-glyph-line" d="M0 3 3.6-3"/></g>',
    event: '<path class="pin-ring" d="M0-17 17 0 0 17-17 0Z"/><g class="pin-in"><circle class="pin-hit" r="18"/>' +
      '<path class="pin-shadow" d="M0-10.5 13 2.5 0 15.5-13 2.5Z"/><path class="pin-body" d="M0-13 13 0 0 13-13 0Z"/>' +
      '<path class="pin-glyph" d="M-3.6-6.2V6.4h1.7V1.6h6.3L2.8-2.3 4.4-6.2Z"/></g>'
  };

  function $(id) { return document.getElementById(id); }
  var boardSel = $("board"), classSel = $("class"), meta = $("meta"), out = $("out");
  var titleEl = $("board-title"), kindEl = $("kind"), bestEl = $("best");
  var canvas = $("map"), tip = $("tip"), mapState = $("map-state"), mapBox = canvas.parentNode;
  var reduceMotion = window.matchMedia && matchMedia("(prefers-reduced-motion: reduce)").matches;

  var data = null;        // the snapshot (or null)
  var boards = {};        // board id -> board of the snapshot
  var points = {};        // board or landmark id -> point
  var pins = {};          // board id -> its pin
  var selected = null;
  var svg = null, pinLayer = null, W = 0, H = 0, X0 = 0, Z1 = 0, UNIT = 4;
  var view = null;

  function el(tag, text, cls) {
    var e = document.createElement(tag);
    if (text !== undefined && text !== null) e.textContent = text;
    if (cls) e.className = cls;
    return e;
  }
  function state(text, error) {
    out.replaceChildren(el("p", text, "board-state" + (error ? " error" : "")));
  }
  function source() {
    var api = new URLSearchParams(location.search).get("api");
    return api ? api.replace(/\/+$/, "") + "/leaderboards/snapshot" : "data/leaderboards.json";
  }
  function get(url, as) {
    return fetch(url, { cache: "no-cache" }).then(function (r) {
      if (!r.ok) throw new Error("HTTP " + r.status);
      return as === "text" ? r.text() : r.json();
    });
  }
  function kindOf(id) { return KIND[id.replace(/-\d+$/, "")] || "event"; }
  function nameOf(id) {
    return (points[id] && points[id].name) || (boards[id] && boards[id].name) || id;
  }
  function leader(b) {
    if (!b || !b.rows || !b.rows.length) return null;
    var r = b.rows.filter(function (x) { return x.overallRank === 1; })[0];
    return r || b.rows[0];
  }
  function drivers(n) { return n + (n === 1 ? " driver" : " drivers"); }

  // ---- the list (also the way to boards without a place on the map) ----

  function fillList() {
    var ids = Object.keys(boards);
    Object.keys(points).forEach(function (id) {
      var k = points[id].kind;
      if ((k === "speed-trap" || k === "speed-zone") && ids.indexOf(id) < 0) ids.push(id);
    });
    if (selected && ids.indexOf(selected) < 0) ids.push(selected);
    var groups = {};
    ids.forEach(function (id) {
      var k = kindOf(id);
      (groups[k] = groups[k] || []).push(id);
    });
    var nodes = [];
    GROUPS.forEach(function (g) {
      var list = groups[g[0]];
      if (!list) return;
      var og = el("optgroup");
      og.label = g[1];
      list.sort(function (a, b) { return nameOf(a).localeCompare(nameOf(b)); });
      list.forEach(function (id) {
        var b = boards[id];
        var o = el("option", nameOf(id) + (b ? " (" + drivers(b.players) + ")" : " (no scores yet)"));
        o.value = id;
        og.appendChild(o);
      });
      nodes.push(og);
    });
    boardSel.replaceChildren.apply(boardSel, nodes);
    boardSel.disabled = !nodes.length;
    if (selected) boardSel.value = selected;
  }

  // ---- the board panel ----

  function show(id) {
    var b = boards[id];
    var k = kindOf(id);
    kindEl.replaceChildren(el("span", null, "dot"), document.createTextNode(KIND_NAME[k] || "Board"));
    kindEl.firstChild.style.background = "var(--pin-" + (k === "trial" ? "event" : k) + ")";
    titleEl.textContent = nameOf(id);
    var have = {};
    (b ? b.rows : []).forEach(function (r) { have[r.classId] = true; });
    var keep = classSel.value;
    classSel.replaceChildren(el("option", "All classes"));
    classSel.firstChild.value = "";
    ((data && data.classes) || []).forEach(function (c) {
      if (!have[c.id]) return;
      var o = el("option", c.name);
      o.value = String(c.id);
      classSel.appendChild(o);
    });
    classSel.value = keep;
    if (classSel.value !== keep) classSel.value = "";
    classSel.disabled = !b;
    rows(id);
  }

  function rows(id) {
    var b = boards[id];
    bestEl.replaceChildren();
    if (!data) {
      meta.textContent = "";
      state("The leaderboards could not be loaded. Try again in a few minutes.", true);
      return;
    }
    if (!b || !b.rows.length) {
      meta.textContent = "";
      state("Nobody has set a score here yet. Be the first on the public server.");
      return;
    }
    var top = leader(b);
    bestEl.append(el("strong", top.display), el("span", "#1 " + top.gamertag + " · " + top.car));
    var cls = classSel.value;
    var list = b.rows.filter(function (r) { return !cls || String(r.classId) === cls; }).slice(0, TOP);
    meta.textContent = drivers(b.players) + ", " + (b.better === "higher" ? "highest" : "fastest") + " first. Showing " +
      list.length + (cls ? " in the class" : "") + ". Updated " + new Date(data.generatedAt * 1000).toLocaleString() + ".";
    if (!list.length) { state("Nobody in this class yet."); return; }
    var table = el("table", undefined, "board");
    var head = el("tr");
    [["#"], [b.unit === "s" ? "Time" : "Speed", "num"], ["Driver and car"], ["Class"], ["Date", "opt"]].forEach(function (h) {
      var th = el("th", h[0], h[1]);
      th.scope = "col";
      head.appendChild(th);
    });
    table.appendChild(el("thead")).appendChild(head);
    var body = table.appendChild(el("tbody"));
    list.forEach(function (r, i) {
      var tr = el("tr");
      tr.appendChild(el("td", String(cls ? i + 1 : r.overallRank)));
      tr.appendChild(el("td", r.display, "num"));
      var who = el("td", r.gamertag, "who");
      who.appendChild(el("small", r.car));
      tr.appendChild(who);
      tr.appendChild(el("td", r.class));
      tr.appendChild(el("td", r.date, "opt"));
      body.appendChild(tr);
    });
    out.replaceChildren(table);
  }

  function select(id, how) {
    how = how || {};
    if (!id) return;
    if (selected && pins[selected]) pins[selected].classList.remove("is-sel");
    selected = id;
    var pin = pins[id];
    if (pin) {
      pin.classList.add("is-sel");
      pin.parentNode.appendChild(pin);  // on top of its neighbours
      queueLabels();
    }
    if (!boardSel.querySelector('option[value="' + id + '"]')) fillList();
    boardSel.value = id;
    show(id);
    if (how.hash !== false) history.replaceState(null, "", "#" + id);
    if (how.pan && pin) panTo(points[id]);
    // On phones the board is below the map: the pin's card links down to it.
    if (how.card && pin && narrow()) showTip(pin, true);
  }

  // ---- the map ----

  function toMap(p) { return [(p.x - X0) / UNIT, (Z1 - p.z) / UNIT]; }

  function fitView() {
    var cw = canvas.clientWidth || 1, ch = canvas.clientHeight || 1, a = ch / cw;
    var w = Math.max(W, H / a), h = w * a;
    return { x: (W - w) / 2, y: (H - h) / 2, w: w, h: h, a: a };
  }

  function clampView(v) {
    var f = fitView();
    v.w = Math.min(Math.max(v.w, W / MAX_ZOOM), f.w);
    v.h = v.w * f.a;
    var cx = v.x + v.w / 2, cy = v.y + v.h / 2;
    cx = Math.min(Math.max(cx, Math.min(W / 2, v.w / 2)), Math.max(W / 2, W - v.w / 2));
    cy = Math.min(Math.max(cy, Math.min(H / 2, v.h / 2)), Math.max(H / 2, H - v.h / 2));
    v.x = cx - v.w / 2;
    v.y = cy - v.h / 2;
    return v;
  }

  function setView(v) {
    view = clampView(v);
    svg.setAttribute("viewBox", view.x.toFixed(2) + " " + view.y.toFixed(2) + " " + view.w.toFixed(2) + " " + view.h.toFixed(2));
    var f = fitView();
    svg.style.setProperty("--cm-s", (view.w / (canvas.clientWidth || 1)).toFixed(4));
    svg.style.setProperty("--cm-zr", Math.pow(f.w / view.w, 0.45).toFixed(3));
    svg.classList.toggle("far", view.w / (canvas.clientWidth || 1) > 5.5);
    hideTip();
    var sc = view.w / (canvas.clientWidth || 1);
    if (!placedAt || Math.abs(sc - placedAt) / sc > 0.04) queueLabels();
  }

  // ---- the landmarks' names go where no pin covers them ----
  // Pins and names keep their size on screen, so whether they meet depends on the zoom: the
  // names are placed again (first clear spot around the icon) when the zoom changes.
  var labels = [], placedAt = 0, labelsQueued = false;
  function queueLabels() {
    if (labelsQueued) return;
    labelsQueued = true;
    requestAnimationFrame(function () { labelsQueued = false; placeLabels(); });
  }
  function collectLabels() {
    labels = [];
    svg.querySelectorAll(".cm-lm").forEach(function (g) {
      var t = g.querySelector(".cm-name"), st = g.getAttribute("style") || "";
      var x = /--x:\s*(-?[\d.]+)/.exec(st), y = /--y:\s*(-?[\d.]+)/.exec(st);
      if (!t || !x || !y) return;
      var use = g.querySelector("use");
      labels.push({ t: t, x: +x[1], y: +y[1], half: use ? (+use.getAttribute("width") || 18) / 2 : 9, w: 0,
                    town: g.classList.contains("cm-lm-town") });
    });
  }
  function hitRect(r, cx, cy, rad) {
    var dx = cx - Math.max(r[0], Math.min(cx, r[0] + r[2])), dy = cy - Math.max(r[1], Math.min(cy, r[1] + r[3]));
    return dx * dx + dy * dy < rad * rad;
  }
  function overlap(a, b) {
    return a[0] < b[0] + b[2] && b[0] < a[0] + a[2] && a[1] < b[1] + b[3] && b[1] < a[1] + a[3];
  }
  function placeLabels() {
    if (!svg || !view) return;
    var s = view.w / (canvas.clientWidth || 1), far = svg.classList.contains("far");
    var small = window.matchMedia("(max-width: 600px)").matches;
    placedAt = s;
    // The pins that show, as circles in screen pixels.
    var obstacles = [];
    Object.keys(pins).forEach(function (id) {
      var pin = pins[id], k = kindOf(id), cls = pinLayer.classList;
      if (cls.contains("hide-" + k) || (k === "event" && pin.classList.contains("is-empty") && cls.contains("hide-empty-event"))) return;
      var c = toMap(points[id]);
      var r = (k === "event" ? 12 : 13) * (small ? 0.8 : 1) * (pin.classList.contains("is-sel") ? 1.3 : 1);
      obstacles.push([c[0] / s, c[1] / s, r + 2]);
    });
    labels.forEach(function (l) { obstacles.push([l.x / s, l.y / s, l.half + 1]); });
    var placed = [];
    labels.forEach(function (l) {
      if (l.town && far) return;
      if (!l.w) { try { l.w = l.t.getComputedTextLength(); } catch (e) { l.w = 0; } }
      if (!l.w) return;
      var ax = l.x / s, ay = l.y / s, h = l.half, w = l.w + 4, best = null;
      // baseline offsets and anchors around the icon: below, above, right, left, the corners, further out
      [[0, h + 12, "middle"], [0, -h - 5, "middle"], [h + 4, 4, "start"], [-h - 4, 4, "end"],
       [h - 2, h + 12, "start"], [2 - h, h + 12, "end"], [h - 2, -h - 5, "start"], [2 - h, -h - 5, "end"],
       [0, h + 26, "middle"], [0, -h - 19, "middle"], [h + 18, 4, "start"], [-h - 18, 4, "end"]].some(function (c) {
        var left = c[2] === "middle" ? c[0] - w / 2 : c[2] === "start" ? c[0] - 2 : c[0] - w + 2;
        var rect = [ax + left, ay + c[1] - 10, w, 13.5], score = 0;
        obstacles.forEach(function (o) {
          if (Math.abs(o[0] - ax) < 200 && Math.abs(o[1] - ay) < 200 && hitRect(rect, o[0], o[1], o[2])) score += 10;
        });
        placed.forEach(function (p) { if (overlap(rect, p)) score += 4; });
        if (!best || score < best.score) best = { c: c, rect: rect, score: score };
        return score === 0;
      });
      l.t.setAttribute("x", best.c[0]);
      l.t.setAttribute("y", best.c[1]);
      l.t.setAttribute("text-anchor", best.c[2]);
      placed.push(best.rect);
    });
  }

  function startView() {
    // Whole Colorado; a canvas much taller (or wider) than the map starts closer in.
    var f = fitView();
    var k = Math.min(1.7, 1 / Math.min(W / f.w, H / f.h));
    var w = f.w / k;
    return clampView({ x: W / 2 - w / 2, y: H / 2 - w * f.a / 2, w: w, h: w * f.a });
  }

  function zoomAt(factor, px, py) {
    var cw = canvas.clientWidth, ch = canvas.clientHeight;
    if (px === undefined) { px = cw / 2; py = ch / 2; }
    var sx = view.x + px / cw * view.w, sy = view.y + py / ch * view.h;
    var w = view.w / factor;
    var h = w * view.h / view.w;
    setView({ x: sx - px / cw * w, y: sy - py / ch * h, w: w, h: h });
  }

  var anim = 0;
  function animateTo(target) {
    cancelAnimationFrame(anim);
    if (reduceMotion) { setView(target); return; }
    var from = { x: view.x, y: view.y, w: view.w, h: view.h }, t0 = performance.now();
    (function step(now) {
      var t = Math.min(1, (now - t0) / 380), e = 1 - Math.pow(1 - t, 3);
      setView({ x: from.x + (target.x - from.x) * e, y: from.y + (target.y - from.y) * e,
                w: from.w + (target.w - from.w) * e, h: from.h + (target.h - from.h) * e });
      if (t < 1) anim = requestAnimationFrame(step);
    })(t0);
  }

  function panTo(p) {
    if (!p || !view) return;
    var c = toMap(p), f = fitView();
    var w = Math.min(view.w, f.w / 2.2);
    var inside = c[0] > view.x + view.w * 0.12 && c[0] < view.x + view.w * 0.88 &&
      c[1] > view.y + view.h * 0.15 && c[1] < view.y + view.h * 0.88;
    if (inside && w === view.w) return;
    animateTo(clampView({ x: c[0] - w / 2, y: c[1] - w * f.a / 2, w: w, h: w * f.a }));
  }

  function makePin(p) {
    var k = KIND[p.kind];
    var c = toMap(p), b = boards[p.id], top = leader(b);
    var g = document.createElementNS(NS, "g");
    g.setAttribute("class", "lb-pin lb-pin-" + k + (b ? "" : " is-empty"));
    g.setAttribute("style", "--x:" + c[0].toFixed(1) + ";--y:" + c[1].toFixed(1));
    g.setAttribute("tabindex", "0");
    g.setAttribute("role", "button");
    g.setAttribute("aria-label", p.name + ", " + KIND_NAME[k].toLowerCase() + (top ? ", best " + top.display + " by " + top.gamertag : ", no scores yet"));
    g.dataset.id = p.id;
    g.innerHTML = SHAPES[k];
    return g;
  }

  function buildPins() {
    if (pinLayer) pinLayer.remove();
    pinLayer = document.createElementNS(NS, "g");
    pinLayer.setAttribute("class", "lb-pins hide-empty-event");
    pins = {};
    var counts = { trap: 0, zone: 0, event: 0 };
    // Events below, then zones, then traps on top; west to east within each.
    var list = Object.keys(points).map(function (id) { return points[id]; }).filter(function (p) { return KIND[p.kind]; });
    var order = { event: 0, zone: 1, trap: 2 };
    list.sort(function (a, b) {
      return (order[KIND[a.kind]] - order[KIND[b.kind]]) || (!!boards[a.id] - !!boards[b.id]) || (a.x - b.x);
    });
    list.forEach(function (p) {
      var pin = makePin(p);
      pins[p.id] = pin;
      pinLayer.appendChild(pin);
      if (KIND[p.kind] !== "event" || boards[p.id]) counts[KIND[p.kind]]++;
    });
    svg.appendChild(pinLayer);
    $("n-trap").textContent = counts.trap;
    $("n-zone").textContent = counts.zone;
    $("n-event").textContent = counts.event;
    document.querySelectorAll(".lb-chip").forEach(function (chip) {
      var hidden = chip.getAttribute("aria-pressed") !== "true";
      pinLayer.classList.toggle("hide-" + chip.dataset.kind, hidden);
    });
    if (selected && pins[selected]) {
      pins[selected].classList.add("is-sel");
      pinLayer.appendChild(pins[selected]);
    }
    collectLabels();
    placedAt = 0;
    queueLabels();
  }

  // The tooltip: the name and the #1.
  var tipFor = null;
  function keyboardFocus(e) {
    try { return e.matches(":focus-visible"); } catch (err) { return true; }
  }
  function hashId() {
    try { return decodeURIComponent(location.hash.slice(1)); } catch (err) { return ""; }
  }
  function narrow() { return window.matchMedia("(max-width: 900px)").matches; }

  function showTip(pin, card) {
    var id = pin.dataset.id, b = boards[id], top = leader(b), k = kindOf(id);
    tip.replaceChildren(el("b", nameOf(id)), el("span", KIND_NAME[k] + (b ? " · " + drivers(b.players) : ""), "k"));
    var line = el("div", null, "top");
    if (top) {
      line.append(el("strong", "#1 "), document.createTextNode(top.gamertag + " — " + top.display));
    } else {
      line.textContent = data ? "No scores yet" : "Scores unavailable";
    }
    tip.appendChild(line);
    tip.classList.toggle("card", !!card);
    if (card) {
      var go = el("button", boards[id] ? "See the top " + Math.min(TOP, b.players) + " below" : "Open the board below", "lb-tip-go");
      go.type = "button";
      go.addEventListener("click", function () {
        hideTip();
        $("board-panel").scrollIntoView({ behavior: reduceMotion ? "auto" : "smooth", block: "start" });
        $("board-panel").focus({ preventScroll: true });
      });
      tip.appendChild(go);
    }
    var r = pin.getBoundingClientRect(), m = mapBox.getBoundingClientRect();
    tip.hidden = false;
    var x = r.left + r.width / 2 - m.left, y = r.top - m.top;
    var half = tip.offsetWidth / 2;
    x = Math.min(Math.max(x, half + 6), m.width - half - 6);
    tip.style.left = x + "px";
    tip.style.top = y + "px";
    tip.style.transform = y < tip.offsetHeight + 24 ? "translate(-50%, " + (r.height + 14) + "px)" : "";
    tipFor = pin;
  }
  function hideTip() { if (tipFor) { tip.hidden = true; tipFor = null; } }

  function setupMap(text) {
    canvas.insertAdjacentHTML("beforeend", text);
    svg = canvas.querySelector("svg.cmap");
    if (!svg) throw new Error("no map");
    var vb = svg.getAttribute("viewBox").split(/\s+/).map(Number);
    W = vb[2]; H = vb[3];
    X0 = +svg.dataset.x0; Z1 = +svg.dataset.z1; UNIT = +svg.dataset.unit || 4;
    svg.setAttribute("preserveAspectRatio", "xMidYMid meet");
    svg.removeAttribute("role");
    svg.setAttribute("aria-hidden", "false");
    mapState.remove();
    setView(startView());

    // Drag to pan (capture only once it is a drag, so a tap still reaches the pin), wheel and
    // pinch to zoom.
    var ptrs = {}, down = null, pinch = null, dragging = false, moved = false;
    function count() { return Object.keys(ptrs).length; }
    function grab(e) {
      down = { x: e.clientX, y: e.clientY, view: { x: view.x, y: view.y, w: view.w, h: view.h }, id: e.pointerId };
    }
    function pair() {
      var ids = Object.keys(ptrs), a = ptrs[ids[0]], b = ptrs[ids[1]];
      return { d: Math.max(Math.hypot(a.x - b.x, a.y - b.y), 1), x: (a.x + b.x) / 2, y: (a.y + b.y) / 2 };
    }
    canvas.addEventListener("pointerdown", function (e) {
      if (e.pointerType === "mouse" && e.button !== 0) return;
      ptrs[e.pointerId] = { x: e.clientX, y: e.clientY };
      if (count() === 1) { grab(e); moved = false; }
      if (count() === 2) {
        var p = pair();
        pinch = { d: p.d, x: p.x, y: p.y, view: { x: view.x, y: view.y, w: view.w, h: view.h } };
        Object.keys(ptrs).forEach(function (pid) { try { canvas.setPointerCapture(+pid); } catch (err) { /* gone */ } });
      }
    });
    canvas.addEventListener("pointermove", function (e) {
      if (!ptrs[e.pointerId]) return;
      ptrs[e.pointerId] = { x: e.clientX, y: e.clientY };
      var rect = canvas.getBoundingClientRect();
      if (pinch && count() >= 2) {
        // The map point under the fingers' first middle stays under their middle.
        var p = pair(), v = pinch.view;
        var w = v.w * pinch.d / p.d, h = w * v.h / v.w;
        var sx = v.x + (pinch.x - rect.left) / rect.width * v.w, sy = v.y + (pinch.y - rect.top) / rect.height * v.h;
        setView({ x: sx - (p.x - rect.left) / rect.width * w, y: sy - (p.y - rect.top) / rect.height * h, w: w, h: h });
        moved = true;
        return;
      }
      if (!down || e.pointerId !== down.id) return;
      var dx = e.clientX - down.x, dy = e.clientY - down.y;
      if (!dragging && Math.hypot(dx, dy) > 5) {
        dragging = moved = true;
        canvas.classList.add("dragging");
        try { canvas.setPointerCapture(e.pointerId); } catch (err) { /* gone */ }
      }
      if (dragging) {
        setView({ x: down.view.x - dx * view.w / rect.width, y: down.view.y - dy * view.h / rect.height, w: view.w, h: view.h });
      }
    });
    function up(e) {
      delete ptrs[e.pointerId];
      if (count() < 2) pinch = null;
      if (count() === 1) {
        // One finger left after a pinch: it pans from here.
        var id = Object.keys(ptrs)[0];
        grab({ clientX: ptrs[id].x, clientY: ptrs[id].y, pointerId: +id });
      }
      if (!count()) { dragging = false; down = null; canvas.classList.remove("dragging"); }
    }
    canvas.addEventListener("pointerup", up);
    canvas.addEventListener("pointercancel", up);
    canvas.addEventListener("wheel", function (e) {
      e.preventDefault();
      var rect = canvas.getBoundingClientRect();
      var dy = e.deltaMode === 1 ? e.deltaY * 30 : e.deltaY;
      zoomAt(Math.exp(-dy * 0.0018), e.clientX - rect.left, e.clientY - rect.top);
    }, { passive: false });
    canvas.addEventListener("dblclick", function (e) {
      if (e.target.closest && e.target.closest(".lb-pin")) return;
      var rect = canvas.getBoundingClientRect();
      zoomAt(2, e.clientX - rect.left, e.clientY - rect.top);
    });
    canvas.addEventListener("click", function (e) {
      var pin = e.target.closest && e.target.closest(".lb-pin");
      if (!pin || moved) return;
      select(pin.dataset.id, { card: true });
    });
    canvas.addEventListener("keydown", function (e) {
      var pin = e.target.closest && e.target.closest(".lb-pin");
      if (pin && (e.key === "Enter" || e.key === " ")) {
        e.preventDefault();
        select(pin.dataset.id, { card: true });
        return;
      }
      var step = 0.15, pan = null;
      if (e.key === "+" || e.key === "=") zoomAt(1.5);
      else if (e.key === "-" || e.key === "_") zoomAt(1 / 1.5);
      else if (e.key === "0") animateTo(startView());
      else if (e.key === "ArrowLeft") pan = [-step, 0];
      else if (e.key === "ArrowRight") pan = [step, 0];
      else if (e.key === "ArrowUp") pan = [0, -step];
      else if (e.key === "ArrowDown") pan = [0, step];
      else return;
      e.preventDefault();
      if (pan) setView({ x: view.x + pan[0] * view.w, y: view.y + pan[1] * view.h, w: view.w, h: view.h });
    });
    canvas.addEventListener("pointerover", function (e) {
      var pin = e.target.closest && e.target.closest(".lb-pin");
      if (pin && pin !== tipFor && !dragging && e.pointerType === "mouse") showTip(pin);
    });
    canvas.addEventListener("pointerout", function (e) {
      var pin = e.target.closest && e.target.closest(".lb-pin");
      if (pin && e.pointerType === "mouse" && !tip.classList.contains("card") &&
          (!e.relatedTarget || !pin.contains(e.relatedTarget))) hideTip();
    });
    canvas.addEventListener("focusin", function (e) {
      var pin = e.target.closest && e.target.closest(".lb-pin");
      if (pin && keyboardFocus(pin)) { panTo(points[pin.dataset.id]); showTip(pin); }
    });
    canvas.addEventListener("focusout", function (e) {
      if (!tip.classList.contains("card") && !(e.relatedTarget && tip.contains(e.relatedTarget))) hideTip();
    });
    document.querySelectorAll(".lb-zoom button").forEach(function (b) {
      b.addEventListener("click", function () {
        var z = b.dataset.zoom;
        if (z === "in") animateTo(clampView(zoomed(1.6)));
        else if (z === "out") animateTo(clampView(zoomed(1 / 1.6)));
        else animateTo(clampView(startView()));
      });
    });
    function zoomed(f) {
      var w = view.w / f, h = view.h / f;
      return { x: view.x + (view.w - w) / 2, y: view.y + (view.h - h) / 2, w: w, h: h };
    }
    document.querySelectorAll(".lb-chip").forEach(function (chip) {
      chip.addEventListener("click", function () {
        var on = chip.getAttribute("aria-pressed") !== "true";
        chip.setAttribute("aria-pressed", String(on));
        if (pinLayer) pinLayer.classList.toggle("hide-" + chip.dataset.kind, !on);
        queueLabels();
      });
    });
    if (window.ResizeObserver) {
      var lastW = canvas.clientWidth, lastH = canvas.clientHeight;
      new ResizeObserver(function () {
        if (canvas.clientWidth === lastW && canvas.clientHeight === lastH) return;
        var k = view ? fitView().w / view.w : 1;
        var cx = view.x + view.w / 2, cy = view.y + view.h / 2;
        lastW = canvas.clientWidth; lastH = canvas.clientHeight;
        var f = fitView(), w = f.w / k;
        setView({ x: cx - w / 2, y: cy - w * f.a / 2, w: w, h: w * f.a });
      }).observe(canvas);
    }
  }

  // ---- start ----

  boardSel.addEventListener("change", function () { select(boardSel.value, { pan: true }); });
  window.addEventListener("hashchange", function () {
    var id = hashId();
    if (id && id !== selected && (boards[id] || pins[id])) select(id, { pan: true, hash: false });
  });
  classSel.addEventListener("change", function () { if (selected) rows(selected); });

  Promise.all([
    get(source()).catch(function () { return null; }),
    get("data/map_points.json").catch(function () { return null; }),
    get("images/colorado-map.svg", "text").catch(function () { return null; })
  ]).then(function (got) {
    data = got[0] && Array.isArray(got[0].boards) ? got[0] : null;
    (data ? data.boards : []).forEach(function (b) { if (b.rows && b.rows.length) boards[b.id] = b; });
    ((got[1] && got[1].points) || []).forEach(function (p) { points[p.id] = p; });
    if (got[2]) {
      try { setupMap(got[2]); buildPins(); } catch (e) { mapState.textContent = "The map could not be shown."; }
    } else {
      mapState.textContent = "The map could not be loaded. The list beside it has every board.";
    }
    fillList();
    var want = hashId();
    if (!boards[want] && !(points[want] && KIND[points[want].kind])) {
      // The busiest board with a place on the map, else the busiest.
      var all = Object.keys(boards).sort(function (a, b) { return boards[b].players - boards[a].players; });
      want = all.filter(function (id) { return pins[id]; })[0] || all[0] ||
        Object.keys(pins).filter(function (id) { return kindOf(id) === "trap"; })[0] || null;
    }
    if (want) select(want, { hash: false, pan: !!location.hash });
    else state(data ? "Nobody has set a score yet. Be the first on the public server." :
      "The leaderboards could not be loaded. Try again in a few minutes.", !data);
    if (!data && want) state("The leaderboards could not be loaded. Try again in a few minutes.", true);
  });
}());
