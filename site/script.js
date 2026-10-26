// The road follows the sections: it is drawn from the layout, the car drives it as you scroll,
// and the sky goes from sunset to night.
(function () {
  "use strict";
  var doc = document.documentElement;
  var svg = document.querySelector(".road");
  var paths = svg.querySelectorAll("path");
  var car = document.querySelector(".car");
  var stops = Array.prototype.slice.call(document.querySelectorAll(".stop"));
  var reduce = window.matchMedia("(prefers-reduced-motion: reduce)").matches;
  var path = null, length = 0, ys = [];

  // Sky colors at the start, the middle and the end of the drive: [top, bottom, far ridge, near ridge].
  var SKY = [
    [[36, 23, 71], [255, 157, 60], [58, 39, 112], [43, 29, 85]],
    [[43, 29, 85], [122, 42, 110], [50, 33, 98], [36, 24, 72]],
    [[20, 16, 46], [58, 39, 112], [38, 27, 80], [26, 19, 58]],
    [[11, 7, 34], [28, 19, 64], [28, 20, 62], [18, 13, 44]]
  ];
  function mix(a, b, t) { return [0, 1, 2].map(function (i) { return Math.round(a[i] + (b[i] - a[i]) * t); }); }
  function css(c) { return "rgb(" + c.join(",") + ")"; }
  function sky(t) {
    var f = t * (SKY.length - 1), i = Math.min(SKY.length - 2, Math.floor(f)), k = f - i;
    var a = SKY[i], b = SKY[i + 1], s = doc.style;
    s.setProperty("--sky-top", css(mix(a[0], b[0], k)));
    s.setProperty("--sky-bottom", css(mix(a[1], b[1], k)));
    s.setProperty("--ridge-far", css(mix(a[2], b[2], k)));
    s.setProperty("--ridge-near", css(mix(a[3], b[3], k)));
    s.setProperty("--stars", String(Math.max(0, (t - 0.45) / 0.55)));
    s.setProperty("--sun-y", String(Math.round(t * 260)));
    s.setProperty("--sun-o", String(Math.max(0, Math.min(1, 1 - (t - 0.6) / 0.25))));
    if (!reduce) {
      s.setProperty("--far-y", Math.round(t * 40) + "px");
      s.setProperty("--near-y", Math.round(t * 90) + "px");
    }
  }

  // Lay the road out: it passes each section on the side opposite its panel.
  function build() {
    var w = document.body.clientWidth, h = document.body.scrollHeight;
    var narrow = w <= 820;
    svg.setAttribute("width", w);
    svg.setAttribute("height", h);
    svg.setAttribute("viewBox", "0 0 " + w + " " + h);
    svg.style.height = h + "px";
    var pts = [{ x: narrow ? 24 : w * 0.5, y: 0 }];
    ys = [];
    stops.forEach(function (s, i) {
      var top = s.offsetTop, mid = top + s.offsetHeight * 0.5;
      var x;
      if (narrow) x = 22 + (i % 2) * 6;
      else x = s.classList.contains("left") ? w * 0.82 : w * 0.18;
      pts.push({ x: x, y: mid });
      ys.push(mid);
    });
    pts.push({ x: narrow ? 24 : w * 0.5, y: h - 120 });
    var d = "M" + pts[0].x + " " + pts[0].y;
    for (var i = 1; i < pts.length; i++) {
      var p = pts[i - 1], q = pts[i], my = (p.y + q.y) / 2;
      d += " C" + p.x + " " + my + " " + q.x + " " + my + " " + q.x + " " + q.y;
    }
    for (var j = 0; j < paths.length; j++) paths[j].setAttribute("d", d);
    path = paths[paths.length - 1];
    length = path.getTotalLength();
    path.style.strokeDasharray = length + " " + length;
    update();
  }

  // The length along the road at which it is at page height y (the road only goes down).
  function lengthAtY(y) {
    var lo = 0, hi = length;
    for (var i = 0; i < 22; i++) {
      var m = (lo + hi) / 2;
      if (path.getPointAtLength(m).y < y) lo = m; else hi = m;
    }
    return (lo + hi) / 2;
  }

  var ticking = false;
  function update() {
    ticking = false;
    if (!path) return;
    var max = Math.max(1, doc.scrollHeight - window.innerHeight);
    var t = Math.min(1, Math.max(0, window.scrollY / max));
    sky(t);
    var y = window.scrollY + window.innerHeight * 0.6;
    var len = Math.min(length, lengthAtY(y));
    var p = path.getPointAtLength(len), p2 = path.getPointAtLength(Math.min(length, len + 2));
    var ang = Math.atan2(p2.x - p.x, p2.y - p.y) * 180 / Math.PI;  // 0 = heading down the page
    var sc = svg.clientWidth <= 820 ? 0.7 : 1;
    car.style.transform = "translate(" + (p.x - 12 * sc) + "px," + (p.y - 22 * sc) + "px) rotate(" + (-ang + 180) + "deg) scale(" + sc + ")";
    car.style.transformOrigin = (12 * sc) + "px " + (22 * sc) + "px";
    path.style.strokeDashoffset = String(length - len);
  }
  function onScroll() { if (!ticking) { ticking = true; requestAnimationFrame(update); } }

  window.addEventListener("scroll", onScroll, { passive: true });
  window.addEventListener("resize", build);
  window.addEventListener("load", build);
  if (document.fonts && document.fonts.ready) document.fonts.ready.then(build);
  build();

  // Screenshot lightbox.
  var box = document.getElementById("lightbox");
  if (box && box.showModal) {
    document.querySelectorAll(".shots button, .cards button").forEach(function (b) {
      b.addEventListener("click", function () {
        box.querySelector("img").src = b.dataset.full;
        box.querySelector("img").alt = b.querySelector("img").alt;
        box.querySelector("p").textContent = b.dataset.cap || "";
        box.showModal();
      });
    });
    box.addEventListener("click", function (e) { if (e.target === box) box.close(); });
  }
})();
