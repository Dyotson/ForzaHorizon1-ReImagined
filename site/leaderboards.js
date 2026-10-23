// The leaderboards page. The public server speaks plain http (a tunnel), which an https page
// may not fetch, so the site publishes a copy of the server's /leaderboards/snapshot as
// data/leaderboards.json (see .github/workflows/pages.yml). For testing, ?api=http://host:port
// reads the server's own /leaderboards/snapshot instead.
(function () {
  "use strict";
  var TOP = 50;
  var boardSel = document.getElementById("board");
  var classSel = document.getElementById("class");
  var meta = document.getElementById("meta");
  var out = document.getElementById("out");
  var data = null;

  function el(tag, text, cls) {
    var e = document.createElement(tag);
    if (text !== undefined) e.textContent = text;
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
  function find(id) {
    return data.boards.filter(function (b) { return b.id === id; })[0];
  }

  function fill() {
    var groups = {}, order = [];
    data.boards.forEach(function (b) {
      if (!groups[b.typeName]) { groups[b.typeName] = el("optgroup"); groups[b.typeName].label = b.typeName; order.push(b.typeName); }
      var o = el("option", b.name + " (" + b.players + (b.players === 1 ? " driver)" : " drivers)"));
      o.value = b.id;
      groups[b.typeName].appendChild(o);
    });
    boardSel.replaceChildren.apply(boardSel, order.map(function (n) { return groups[n]; }));
    boardSel.disabled = classSel.disabled = false;
    var want = location.hash.slice(1);
    if (want && find(want)) boardSel.value = want;
    classes();
  }

  function classes() {
    var b = find(boardSel.value), have = {};
    b.rows.forEach(function (r) { have[r.classId] = true; });
    var keep = classSel.value;
    classSel.replaceChildren(el("option", "All classes"));
    classSel.firstChild.value = "";
    data.classes.forEach(function (c) {
      if (!have[c.id]) return;
      var o = el("option", c.name); o.value = String(c.id); classSel.appendChild(o);
    });
    classSel.value = keep;
    if (classSel.value !== keep) classSel.value = "";
    show();
  }

  function show() {
    var b = find(boardSel.value);
    var cls = classSel.value;
    var rows = b.rows.filter(function (r) { return !cls || String(r.classId) === cls; }).slice(0, TOP);
    meta.textContent = b.typeName + ", " + (b.better === "higher" ? "highest" : "fastest") + " first. " +
      "Showing " + rows.length + " of " + (cls ? "the class" : b.players) + ". Updated " +
      new Date(data.generatedAt * 1000).toLocaleString() + ".";
    if (!rows.length) { state("Nobody has a score here yet."); return; }
    var table = el("table", undefined, "board");
    var head = el("tr");
    [["#"], [b.unit === "s" ? "Time" : "Speed", "num"], ["Driver"], ["Car"], ["Class", "opt"], ["Date", "opt"]].forEach(function (h) {
      var th = el("th", h[0], h[1]); th.scope = "col"; head.appendChild(th);
    });
    table.appendChild(el("thead")).appendChild(head);
    var body = table.appendChild(el("tbody"));
    rows.forEach(function (r, i) {
      var tr = el("tr");
      tr.appendChild(el("td", String(cls ? i + 1 : r.overallRank)));
      tr.appendChild(el("td", r.display, "num"));
      tr.appendChild(el("td", r.gamertag));
      tr.appendChild(el("td", r.car));
      tr.appendChild(el("td", r.class, "opt"));
      tr.appendChild(el("td", r.date, "opt"));
      body.appendChild(tr);
    });
    out.replaceChildren(table);
  }

  boardSel.addEventListener("change", function () { history.replaceState(null, "", "#" + boardSel.value); classes(); });
  classSel.addEventListener("change", show);

  fetch(source(), { cache: "no-cache" }).then(function (r) {
    if (!r.ok) throw new Error("HTTP " + r.status);
    return r.json();
  }).then(function (d) {
    data = d;
    if (!d.boards || !d.boards.length) { boardSel.replaceChildren(el("option", "No boards yet")); state("Nobody has set a score yet. Be the first on the public server."); return; }
    fill();
  }).catch(function () {
    boardSel.replaceChildren(el("option", "Unavailable"));
    state("The leaderboards could not be loaded. Try again in a few minutes.", true);
  });
}());
