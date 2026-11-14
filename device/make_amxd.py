#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Writes MONO2K.amxd (a Max for Live instrument) around the mono2k~ object and the panel jsui.
import json, struct, sys

VERSION = "1.0.0"
OUT = sys.argv[1] if len(sys.argv) > 1 else "MONO2K.amxd"
DW, DH = 830, 169

boxes, lines = [], []
_n = [0]


def box(maxclass, rect, text=None, pres=None, ins=1, outs=0, otype=None, **kw):
    _n[0] += 1
    b = {"id": "obj-%d" % _n[0], "maxclass": maxclass, "numinlets": ins, "numoutlets": outs,
         "patching_rect": [float(v) for v in rect]}
    if otype is not None: b["outlettype"] = otype
    elif outs: b["outlettype"] = [""] * outs
    if text is not None: b["text"] = text
    if pres is not None:
        b["presentation"] = 1
        b["presentation_rect"] = [float(v) for v in pres]
    b.update(kw)
    boxes.append({"box": b})
    return b["id"]


def obj(text, x, y, ins=1, outs=1, otype=None, w=None, **kw):
    return box("newobj", [x, y, w or max(40, 7 * len(text) + 12), 22], text, ins=ins, outs=outs, otype=otype, **kw)


def msg(text, x, y):
    return box("message", [x, y, max(40, 7 * len(text) + 12), 22], text, ins=2, outs=1, otype=[""])


def wire(a, ao, b, bi=0):
    lines.append({"patchline": {"source": [a, ao], "destination": [b, bi]}})


def param(longname, shortname, ptype, **v):
    d = {"parameter_longname": longname, "parameter_shortname": shortname, "parameter_type": ptype}
    d.update(v)
    return {"valueof": d}


DARK = [0.115, 0.118, 0.125, 1.0]
TXT = [0.62, 0.62, 0.60, 1.0]
ORANGE = [1.0, 0.5, 0.15, 1.0]

# the device's background
box("panel", [10, 600, 120, 40], pres=[0, 0, DW, DH], ins=1, outs=0, bgcolor=DARK, background=1, rounded=0, border=0)

# the engine
eng = obj("mono2k~", 300, 300, ins=1, outs=6, otype=["signal", "signal", "", "", "", ""], w=320)
pha = obj("plugphasor~", 300, 200, outs=1, otype=["signal"])
mid = obj("midiin", 420, 200, outs=1, otype=["int"])
out = obj("plugout~", 300, 420, ins=2, outs=0)
wire(pha, 0, eng); wire(mid, 0, eng)
wire(eng, 0, out, 0); wire(eng, 1, out, 1)

# the screen in the device: 160 x 160 (the object draws it down from 240 itself)
scr = box("jit.pwindow", [40, 420, 160, 160], pres=[4, 4, 160, 160], ins=1, outs=2, otype=["jit_matrix", ""],
          border=0.0, sync=1)
wire(eng, 2, scr)
sbig = obj("s ---m2kbig", 420, 380, ins=1, outs=0); wire(eng, 3, sbig)

# the panel
pnl = box("jsui", [240, 460, 560, 165], pres=[170, 2, 560, 165], ins=1, outs=1, otype=[""],
          filename="mono2k_panel.js", parameter_enable=0, border=0)
wire(eng, 4, pnl)
sled = obj("s ---m2kleds", 560, 380, ins=1, outs=0); wire(eng, 4, sled)
wire(pnl, 0, eng)
rcmd = obj("r ---m2kcmd", 640, 240, ins=0, outs=1); wire(rcmd, 0, eng)

# the state, kept in the Live set (a blob parameter: saved, never automated)
pat = obj("pattr m2kstate", 700, 380, ins=1, outs=3, w=110, parameter_enable=1,
          saved_attribute_attributes=param("m2kstate", "m2kstate", 3, parameter_invisible=1, parameter_initial_enable=0))
wire(eng, 5, pat)
pst = obj("prepend state", 700, 420); wire(pat, 0, pst); wire(pst, 0, eng)

# Live's transport: is_playing, and the song time at the moment it starts
dev = obj("live.thisdevice", 40, 40, ins=1, outs=3)
tbb = obj("t b b", 40, 70, ins=1, outs=2)
pth = obj("live.path live_set", 40, 100, ins=1, outs=3, w=120)
prp = msg("property is_playing", 200, 100)
obs = obj("live.observer", 40, 140, ins=2, outs=2)
lob = obj("live.object", 200, 200, ins=2, outs=1)
tib = obj("t i b", 40, 170, ins=1, outs=2)
get = msg("get current_song_time", 120, 200)
rte = obj("route current_song_time", 200, 230, ins=2, outs=2, w=150)
ppos = obj("prepend pos", 200, 260)
ptr = obj("prepend transport", 40, 260, w=110)
wire(dev, 0, tbb); wire(tbb, 1, prp); wire(prp, 0, obs, 0); wire(tbb, 0, pth)
wire(pth, 0, obs, 1); wire(pth, 0, lob, 1)
wire(obs, 0, tib); wire(tib, 1, get); wire(get, 0, lob, 0); wire(lob, 0, rte); wire(rte, 0, ppos); wire(ppos, 0, eng)
wire(tib, 0, ptr); wire(ptr, 0, eng)

# the right column: MIDI TO, SYNC, SCREEN, credits
X = 738
box("comment", [860, 40, 60, 18], "MIDI TO", pres=[X, 3, 84, 14], ins=1, outs=0, textcolor=TXT,
    fontname="Arial Bold", fontsize=8.0)
men = box("live.menu", [860, 60, 84, 16], pres=[X, 17, 84, 16], ins=1, outs=3, otype=["", "", "float"],
          parameter_enable=1, activebgcolor=[0.27, 0.275, 0.29, 1.0], textcolor=[0.93, 0.92, 0.88, 1.0],
          saved_attribute_attributes=param("MIDI To", "MIDI To", 2, parameter_enum=["SEL", "T1", "T2", "T3", "T4", "T5", "T6"],
                                           parameter_mmax=6, parameter_initial_enable=1, parameter_initial=[0],
                                           parameter_info="Which track Live's MIDI plays: SEL the track in view"))
prt = obj("prepend route", 860, 90); wire(men, 0, prt); wire(prt, 0, eng)

syn = box("live.text", [960, 60, 84, 18], pres=[X, 40, 84, 18], ins=1, outs=2, otype=["", ""], mode=1,
          text="SYNC OFF", texton="SYNC LIVE", parameter_enable=1,
          bgcolor=[0.27, 0.275, 0.29, 1.0], bgoncolor=ORANGE, textcolor=[0.93, 0.92, 0.88, 1.0], textoncolor=[0.1, 0.1, 0.1, 1.0],
          saved_attribute_attributes=param("Sync", "Sync", 2, parameter_enum=["off", "on"], parameter_mmax=1,
                                           parameter_initial_enable=1, parameter_initial=[1],
                                           parameter_info="On: Live's transport and tempo play the sequencer. Off: its own PLAY and tempo"))
psy = obj("prepend sync", 960, 90); wire(syn, 0, psy); wire(psy, 0, eng)

big = box("live.text", [1060, 60, 84, 18], pres=[X, 62, 84, 18], ins=1, outs=2, otype=["", ""], mode=0,
          text="BIG SCREEN", texton="BIG SCREEN", parameter_enable=0,
          bgcolor=[0.27, 0.275, 0.29, 1.0], bgoncolor=ORANGE, textcolor=[0.93, 0.92, 0.88, 1.0])
tb = obj("t b", 1060, 90); opn = msg("open", 1060, 120); pc = obj("pcontrol", 1060, 150)
wire(big, 0, tb); wire(tb, 0, opn); wire(opn, 0, pc)

for i, line in enumerate(["MONO2K core by", "Inigo Hernandez", "GPL-3.0 · free", "M4L port v" + VERSION,
                          "ticcoross.gumroad.com"]):
    box("comment", [860, 200 + 18 * i, 140, 16], line, pres=[X - 2, 92 + 14 * i, 92, 14], ins=1, outs=0,
        textcolor=ORANGE if i == 4 else TXT, fontname="Arial", fontsize=7.5)

# Push's knobs: eight dials, the encoders (parameters only, not on the panel)
ENC = ["Track", "Machine", "Pattern", "Knob 1", "Knob 2", "Knob 3", "Knob 4", "Master"]
for i, nm in enumerate(ENC):
    d = box("live.dial", [40 + 70 * i, 700, 44, 48], ins=1, outs=2, otype=["", "float"], parameter_enable=1,
            saved_attribute_attributes=param("Enc " + nm, nm, 1, parameter_mmin=0, parameter_mmax=1000,
                                             parameter_initial_enable=1, parameter_initial=[500],
                                             parameter_unitstyle=0, parameter_invisible=1,
                                             parameter_info="Turns MONO2K's " + nm + " encoder (for Push)"))
    p = obj("prepend encabs %d" % i, 40 + 70 * i, 760, w=66)
    wire(d, 0, p); wire(p, 0, eng)

# the big window: the screen at 2x and the panel
sub_boxes, sub_lines = [], []


def sbox(maxclass, rect, text=None, pres=None, ins=1, outs=0, otype=None, **kw):
    b = {"id": "sobj-%d" % (len(sub_boxes) + 1), "maxclass": maxclass, "numinlets": ins, "numoutlets": outs,
         "patching_rect": [float(v) for v in rect]}
    if otype is not None: b["outlettype"] = otype
    elif outs: b["outlettype"] = [""] * outs
    if text is not None: b["text"] = text
    if pres is not None: b["presentation"] = 1; b["presentation_rect"] = [float(v) for v in pres]
    b.update(kw)
    sub_boxes.append({"box": b})
    return b["id"]


sbox("panel", [0, 0, 580, 680], pres=[0, 0, 580, 680], ins=1, outs=0, bgcolor=DARK, background=1, border=0)
si = sbox("inlet", [700, 20, 30, 30], ins=0, outs=1, otype=[""])
srb = sbox("newobj", [700, 60, 100, 22], "r ---m2kbig", ins=0, outs=1)
spw = sbox("jit.pwindow", [50, 10, 480, 480], pres=[50, 10, 480, 480], ins=1, outs=2, otype=["jit_matrix", ""], border=0.0, sync=1)
srl = sbox("newobj", [700, 100, 100, 22], "r ---m2kleds", ins=0, outs=1)
sjs = sbox("jsui", [10, 500, 560, 165], pres=[10, 500, 560, 165], ins=1, outs=1, otype=[""],
           filename="mono2k_panel.js", parameter_enable=0, border=0)
ssc = sbox("newobj", [700, 140, 100, 22], "s ---m2kcmd", ins=1, outs=0)
for a, b in ((srb, spw), (srl, sjs), (sjs, ssc)):
    sub_lines.append({"patchline": {"source": [a, 0], "destination": [b, 0]}})

sub = {
    "fileversion": 1, "appversion": {"major": 9, "minor": 0, "revision": 7, "architecture": "x64", "modernui": 1},
    "classnamespace": "box", "rect": [120.0, 120.0, 580.0, 680.0], "openinpresentation": 1, "toolbarvisible": 0,
    "enablehscroll": 0, "enablevscroll": 0, "statusbarvisible": 0, "default_fontsize": 10.0, "default_fontface": 0,
    "default_fontname": "Arial", "gridsize": [8.0, 8.0], "editing_bgcolor": DARK, "locked_bgcolor": DARK,
    "title": "MONO2K", "boxes": sub_boxes, "lines": sub_lines,
}
sp = box("newobj", [1060, 180, 90, 22], "p MONO2K", ins=1, outs=0, patcher=sub)
wire(pc, 0, sp)

patcher = {
    "patcher": {
        "fileversion": 1,
        "appversion": {"major": 9, "minor": 0, "revision": 7, "architecture": "x64", "modernui": 1},
        "classnamespace": "box", "rect": [80.0, 80.0, 1200.0, 820.0], "openrect": [0.0, 0.0, float(DW), float(DH)],
        "openinpresentation": 1, "default_fontsize": 10.0, "default_fontface": 0, "default_fontname": "Arial Bold",
        "gridonopen": 1, "gridsize": [8.0, 8.0], "gridsnaponopen": 1, "objectsnaponopen": 1, "statusbarvisible": 2,
        "toolbarvisible": 1, "boxanimatetime": 200, "enablehscroll": 1, "enablevscroll": 1, "devicewidth": float(DW),
        "description": "MONO2K: six Monomachine-style mono machines, a 64-step sequencer with locks, and a lofi bus. "
                       "Core by Inigo Hernandez (GPL-3.0).",
        "digest": "MONO2K for Max for Live", "tags": "synth sequencer monomachine", "style": "",
        "boxes": boxes, "lines": lines,
        "dependency_cache": [
            {"name": "mono2k_panel.js", "bootpath": "~/MONO2K", "patcherrelativepath": ".", "type": "TEXT", "implicit": 1},
            {"name": "mono2k~.mxo", "type": "iLaX"},
        ],
        "latency": 0, "is_mpe": 0, "minimum_live_version": "", "minimum_max_version": "", "platform_compatibility": 0,
        "autosave": 0,
    }
}

js = json.dumps(patcher, indent=1).encode("utf-8") + b"\n\x00"
data = b"ampf" + struct.pack("<I", 4) + b"iiii" + b"meta" + struct.pack("<I", 4) + b"\x00\x00\x00\x00" + \
       b"ptch" + struct.pack("<I", len(js)) + js
open(OUT, "wb").write(data)
print("wrote", OUT, len(data), "bytes,", len(boxes), "boxes,", len(lines), "lines")
