# qrenderdoc script: find NaN, Inf and out-of-range values in the float render targets of a
# RenderDoc capture (docs/rendering.md > "Screening for driver-dependent bugs").
#
#   RDC_CAP=cap.rdc RDC_OUT=DIR [RDC_STEP=20] [RDC_LIMIT=31.875] \
#       ~/renderdoc/renderdoc_1.46/bin/qrenderdoc --python tools/rdc_float_scan.py
#
# For every R16G16B16A16 / R32 float colour target, after every RDC_STEP-th draw that writes
# it (and the capture's last draw), reads the texture back and counts NaN, +-Inf and finite
# values above RDC_LIMIT (default 31.875, the largest value of the console's 7e3 format, which
# the host emulates with float16). The first draw that leaves bad values in a target is
# written to DIR/float_scan.txt with the texel positions; one line per target. Multisampled
# targets: sample 0 only. A frame of free roam (9,400 draws) takes about 10 minutes at step 20.
import math
import os
import struct
import traceback

OUT = os.environ["RDC_OUT"]
STEP = max(1, int(os.environ.get("RDC_STEP", "20")))
LIMIT = float(os.environ.get("RDC_LIMIT", "31.875"))
os.makedirs(OUT, exist_ok=True)
log = open(os.path.join(OUT, "float_scan.txt"), "w")
try:
    import renderdoc as rd
    cap = rd.OpenCaptureFile()
    cap.OpenFile(os.environ["RDC_CAP"], "", None)
    status, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    if ctrl is None:
        raise RuntimeError(f"cannot replay: {status}")
    textures = {t.resourceId: t for t in ctrl.GetTextures()}

    def walk(actions):
        for a in actions:
            yield a
            yield from walk(a.children)

    draws = [a for a in walk(ctrl.GetRootActions()) if a.flags & rd.ActionFlags.Drawcall]
    writes, flagged = {}, {}
    for i, a in enumerate(draws):
        for o in a.outputs:
            t = textures.get(o)
            if o == rd.ResourceId.Null() or t is None or o in flagged:
                continue
            fmt = t.format.Name()
            if not (fmt.startswith("R16G16B16A16_FLOAT") or fmt.startswith("R32_FLOAT")):
                continue
            writes[o] = writes.get(o, 0) + 1
            if (writes[o] - 1) % STEP and i != len(draws) - 1:
                continue
            ctrl.SetFrameEvent(a.eventId, True)
            data = ctrl.GetTextureData(o, rd.Subresource(0, 0, 0))
            half = fmt.startswith("R16")
            vals = struct.unpack(f"<{len(data) // (2 if half else 4)}{'e' if half else 'f'}", data)
            comps = 4 if half else 1
            bad = [k for k, v in enumerate(vals) if v != v or abs(v) == math.inf or abs(v) > LIMIT]
            if not bad:
                continue
            flagged[o] = a.eventId
            nan = sum(1 for k in bad if vals[k] != vals[k])
            inf = sum(1 for k in bad if abs(vals[k]) == math.inf)
            where = [(k // comps % t.width, k // comps // t.width, "RGBA"[k % comps], vals[k])
                     for k in bad[:4]]
            log.write(f"EID {a.eventId} target {str(o).split('::')[-1]} {fmt} {t.width}x{t.height} "
                      f"ms{t.msSamp}: nan={nan} inf={inf} over_limit={len(bad) - nan - inf} "
                      f"first={where}\n")
            log.flush()
    log.write(f"done: {len(draws)} draws, {len(writes)} float targets, {len(flagged)} with bad values\n")
    ctrl.Shutdown()
    cap.Shutdown()
except Exception:
    log.write(traceback.format_exc())
log.close()
os._exit(0)
