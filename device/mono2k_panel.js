// SPDX-License-Identifier: GPL-3.0-only
// MONO2K for Max for Live: the FM-1's panel (8 encoders, 14 buttons, 27 keys) for the mono2k~ object.
// Click: press, release on letting go. Shift-click: latch (held until clicked again) - for the held combos
// (GLO + a key, a held step + a knob for a lock). Encoders: drag up / down (Shift: finer).
// In: "leds" + 41 values (27 keys, 14 buttons). Out: "btn i down", "key i down", "enc i delta".
autowatch = 0;
inlets = 1;
outlets = 1;
mgraphics.init();
mgraphics.relative_coords = 0;
mgraphics.autofill = 0;

var W = 560, H = 165;
var ENC = ["SELECT", "ALGO", "PRESETS", "K1", "K2", "K3", "K4", "MASTER"];
var EROLE = ["TRACK", "MACHINE", "PATTERN", "", "", "", "", "VOLUME"];
var ECX = [28, 84, 140, 222, 278, 334, 390, 474];
var BTN = ["FX", "FILT", "AMP", "LFO", "SYN", "GLO", "HOME", "SAVE", "ARP", "GRID", "PLAY", "REC", "OCT-", "OCT+"];
var BLACK_ROLE = ["T1", "T2", "T3", "T4", "T5", "T6", "P1", "P2", "P3", "P4", "MUTE"];

var leds = [];
var held = {};      // "b3" / "k5": 1 pressed, 2 latched
var encAng = [];
var drag = null;    // {kind, i, y, acc}
var keys = [];      // [x, y, w, h, black, label]
var btns = [];      // [x, y, w, h]
for (var i = 0; i < 41; i++) leds.push(0);
for (var i = 0; i < 8; i++) encAng.push(0);

function isBlack(k) { return (0x54a >> ((k + 5) % 12)) & 1; }

(function layout() {
    var w = 0, b = 0, KW = 35;
    for (var k = 0; k < 27; k++) {
        if (isBlack(k)) { keys.push([2 + w * KW - 14, 82, 26, 32, 1, BLACK_ROLE[b]]); b++; }
        else { keys.push([2 + w * KW, 117, KW - 3, 46, 0, "" + (w + 1)]); w++; }
    }
    for (var i = 0; i < 14; i++) btns.push([2 + i * 39.8, 57, 36, 19]);
})();

function lit(v) { return v / 255; }

function rrect(x, y, w, h, r) { mgraphics.rectangle_rounded(x, y, w, h, r, r); }

function text_c(s, cx, y, size, col) {
    mgraphics.select_font_face("Arial Bold");
    mgraphics.set_font_size(size);
    var m = mgraphics.text_measure(s);
    mgraphics.set_source_rgba(col[0], col[1], col[2], col[3]);
    mgraphics.move_to(cx - m[0] / 2, y);
    mgraphics.show_text(s);
}

function orange(a) { return [1.0, 0.48 + 0.1 * a, 0.12, 1]; }

function paint() {
    var bw = box.rect[2] - box.rect[0], bh = box.rect[3] - box.rect[1];
    mgraphics.scale(bw / W, bh / H);
    // the body
    mgraphics.set_source_rgba(0.115, 0.118, 0.125, 1);
    rrect(0, 0, W, H, 6);
    mgraphics.fill();
    // the encoders
    for (var i = 0; i < 8; i++) {
        var cx = ECX[i], cy = 21, r = i >= 3 && i <= 6 ? 15 : 13;
        mgraphics.set_source_rgba(0.06, 0.06, 0.065, 1);
        mgraphics.arc(cx, cy, r + 2, 0, Math.PI * 2);
        mgraphics.fill();
        mgraphics.set_source_rgba(0.24, 0.245, 0.26, 1);
        mgraphics.arc(cx, cy, r, 0, Math.PI * 2);
        mgraphics.fill();
        var a = encAng[i] - Math.PI / 2;
        mgraphics.set_source_rgba(0.93, 0.92, 0.88, 1);
        mgraphics.set_line_width(2);
        mgraphics.move_to(cx + Math.cos(a) * r * 0.35, cy + Math.sin(a) * r * 0.35);
        mgraphics.line_to(cx + Math.cos(a) * r * 0.9, cy + Math.sin(a) * r * 0.9);
        mgraphics.stroke();
        text_c(EROLE[i] || ENC[i], cx, 48, 7.5, [0.62, 0.62, 0.6, 1]);
    }
    // groups' rules
    mgraphics.set_source_rgba(1, 1, 1, 0.08);
    mgraphics.set_line_width(1);
    mgraphics.move_to(181, 6); mgraphics.line_to(181, 50); mgraphics.stroke();
    mgraphics.move_to(432, 6); mgraphics.line_to(432, 50); mgraphics.stroke();
    // the brand
    text_c("MONO2K", 520, 18, 9, [0.93, 0.92, 0.88, 1]);
    text_c("M4L", 520, 30, 7, [1.0, 0.5, 0.15, 1]);
    // the buttons
    for (var i = 0; i < 14; i++) {
        var r = btns[i], v = lit(leds[27 + i]), h = held["b" + i];
        mgraphics.set_source_rgba(0.27 + 0.1 * (h ? 1 : 0), 0.275 + 0.1 * (h ? 1 : 0), 0.29 + 0.1 * (h ? 1 : 0), 1);
        rrect(r[0], r[1], r[2], r[3], 3);
        mgraphics.fill();
        if (v > 0) {
            var o = orange(v);
            mgraphics.set_source_rgba(o[0], o[1], o[2], 0.25 + 0.75 * v);
            rrect(r[0] + 2, r[1] + r[3] - 4, r[2] - 4, 2.5, 1);
            mgraphics.fill();
        }
        if (h == 2) {
            mgraphics.set_source_rgba(1, 0.5, 0.15, 1);
            mgraphics.set_line_width(1.5);
            rrect(r[0] + 0.75, r[1] + 0.75, r[2] - 1.5, r[3] - 1.5, 3);
            mgraphics.stroke();
        }
        text_c(BTN[i], r[0] + r[2] / 2, r[1] + 11.5, 7.5, [0.92, 0.91, 0.87, 1]);
    }
    // the keys: white, then black over them
    for (var pass = 0; pass < 2; pass++) {
        for (var k = 0; k < 27; k++) {
            var r = keys[k];
            if (r[4] != pass) continue;
            var v = lit(leds[k]), h = held["k" + k];
            if (r[4]) mgraphics.set_source_rgba(0.17 + (h ? 0.12 : 0), 0.17 + (h ? 0.12 : 0), 0.185 + (h ? 0.12 : 0), 1);
            else mgraphics.set_source_rgba(0.86 - (h ? 0.14 : 0), 0.85 - (h ? 0.14 : 0), 0.81 - (h ? 0.14 : 0), 1);
            rrect(r[0], r[1], r[2], r[3], 3);
            mgraphics.fill();
            if (v > 0) {
                var o = orange(v);
                mgraphics.set_source_rgba(o[0], o[1], o[2], 0.3 + 0.7 * v);
                rrect(r[0] + 3, r[1] + 3, r[2] - 6, 4, 2);
                mgraphics.fill();
            }
            if (h == 2) {
                mgraphics.set_source_rgba(1, 0.5, 0.15, 1);
                mgraphics.set_line_width(1.5);
                rrect(r[0] + 0.75, r[1] + 0.75, r[2] - 1.5, r[3] - 1.5, 3);
                mgraphics.stroke();
            }
            if (r[4]) text_c(r[5], r[0] + r[2] / 2, r[1] + r[3] - 6, 7, [0.75, 0.75, 0.72, 1]);
            else text_c(r[5], r[0] + r[2] / 2, r[1] + r[3] - 6, 7.5, [0.25, 0.25, 0.26, 1]);
        }
    }
}

function hit(x, y) {
    for (var k = 0; k < 27; k++) {           // black keys first: they sit over the white
        var r = keys[k];
        if (r[4] && x >= r[0] && x < r[0] + r[2] && y >= r[1] && y < r[1] + r[3]) return ["k", k];
    }
    for (var k = 0; k < 27; k++) {
        var r = keys[k];
        if (!r[4] && x >= r[0] && x < r[0] + r[2] && y >= r[1] && y < r[1] + r[3]) return ["k", k];
    }
    for (var i = 0; i < 14; i++) {
        var r = btns[i];
        if (x >= r[0] && x < r[0] + r[2] && y >= r[1] && y < r[1] + r[3]) return ["b", i];
    }
    for (var i = 0; i < 8; i++) {
        var dx = x - ECX[i], dy = y - 21;
        if (dx * dx + dy * dy < 22 * 22 && y < 52) return ["e", i];
    }
    return null;
}

function send(kind, i, d) {
    if (kind == "b") outlet(0, "btn", i, d);
    else outlet(0, "key", i, d);
}

function local(x, y) {
    var bw = box.rect[2] - box.rect[0], bh = box.rect[3] - box.rect[1];
    return [x * W / bw, y * H / bh];
}

function onclick(x, y, but, cmd, shift, capslock, option, ctrl) {
    var p = local(x, y), h = hit(p[0], p[1]);
    if (!h) return;
    if (h[0] == "e") { drag = { kind: "e", i: h[1], y: p[1], acc: 0, fine: shift }; return; }
    var id = h[0] + h[1];
    if (held[id] == 2) {                     // a latched one: let go
        delete held[id];
        send(h[0], h[1], 0);
    } else if (shift) {                       // latch
        held[id] = 2;
        send(h[0], h[1], 1);
    } else {
        held[id] = 1;
        send(h[0], h[1], 1);
        drag = { kind: h[0], i: h[1] };
    }
    mgraphics.redraw();
}
onclick.local = 1;

function ondrag(x, y, but, cmd, shift, capslock, option, ctrl) {
    if (!drag) return;
    var p = local(x, y);
    if (drag.kind == "e") {
        if (but) {
            var step = drag.fine ? 10 : 4;
            drag.acc += drag.y - p[1];
            drag.y = p[1];
            var n = drag.acc > 0 ? Math.floor(drag.acc / step) : Math.ceil(drag.acc / step);
            if (n != 0) {
                drag.acc -= n * step;
                encAng[drag.i] += n * 0.18;
                outlet(0, "enc", drag.i, n);
                mgraphics.redraw();
            }
        }
        if (!but) drag = null;
        return;
    }
    if (!but) {
        var id = drag.kind + drag.i;
        if (held[id] == 1) { delete held[id]; send(drag.kind, drag.i, 0); }
        drag = null;
        mgraphics.redraw();
    }
}
ondrag.local = 1;

function leds_in() {
    var a = arrayfromargs(arguments);
    var changed = false;
    for (var i = 0; i < 41 && i < a.length; i++) if (leds[i] != a[i]) { leds[i] = a[i]; changed = true; }
    if (changed) mgraphics.redraw();
}

function anything() {
    if (messagename == "leds") leds_in.apply(this, arrayfromargs(arguments));
}

function list() { leds_in.apply(this, arrayfromargs(arguments)); }

// all latched controls let go (e.g. on the device's reset)
function release() {
    for (var id in held) send(id.charAt(0), parseInt(id.substring(1)), 0);
    held = {};
    mgraphics.redraw();
}

function onresize(w, h) { mgraphics.redraw(); }
onresize.local = 1;
