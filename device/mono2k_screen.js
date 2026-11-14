// SPDX-License-Identifier: GPL-3.0-only
// MONO2K for Max for Live: MONO2K's 240 x 240 1-bit screen, drawn sharp at the display's own pixels.
// In: "scr" + 1920 integers (240 rows of 8 x 30 bits, the leftmost dot the highest bit, 1 ink) from the mono2k object.
// Each run of ink in a row is one rectangle whose edges fall on whole device pixels (Retina: half points), so every dot
// is pure ink or pure paper: nothing smooths it.
autowatch = 0;
inlets = 1;
outlets = 0;
mgraphics.init();
mgraphics.relative_coords = 0;
mgraphics.autofill = 0;

var PAPER = [0.894, 0.882, 0.839, 1];
var INK = [0.090, 0.094, 0.102, 1];
var rows = null;          // 240 x 8 integers
var DPR = 2;              // device pixels a point (Retina); edges snap to 1 / DPR

function scr() {
    var a = arrayfromargs(arguments);
    if (a.length < 1920) return;
    rows = a;
    mgraphics.redraw();
}

function anything() {
    if (messagename == "scr") scr.apply(this, arrayfromargs(arguments));
}

function snap(v) { return Math.round(v * DPR) / DPR; }

function paint() {
    var w = box.rect[2] - box.rect[0], h = box.rect[3] - box.rect[1];
    mgraphics.set_source_rgba(PAPER[0], PAPER[1], PAPER[2], 1);
    mgraphics.rectangle(0, 0, w, h);
    mgraphics.fill();
    if (!rows) return;
    var sx = w / 240, sy = h / 240;
    var xe = [], ye = [];
    for (var i = 0; i <= 240; i++) { xe.push(snap(i * sx)); ye.push(snap(i * sy)); }
    mgraphics.set_source_rgba(INK[0], INK[1], INK[2], 1);
    for (var y = 0; y < 240; y++) {
        var y0 = ye[y], y1 = ye[y + 1], run = -1;
        for (var k = 0; k < 8; k++) {
            var v = rows[y * 8 + k];
            for (var b = 29; b >= 0; b--) {
                var x = k * 30 + 29 - b;
                if ((v >> b) & 1) { if (run < 0) run = x; }
                else if (run >= 0) { mgraphics.rectangle(xe[run], y0, xe[x] - xe[run], y1 - y0); run = -1; }
            }
        }
        if (run >= 0) mgraphics.rectangle(xe[run], y0, xe[240] - xe[run], y1 - y0);
    }
    mgraphics.fill();
}

function onresize(w, h) { mgraphics.redraw(); }
onresize.local = 1;
