"""dng_tags.py <file.dng> : print a JSON summary of a DNG's tags and decoded OpcodeList1/2/3 (read-only parser; run with python -I).

Parses TIFF IFD0 and SubIFDs, then the opcode lists (always big-endian inside the tag).
"""
import json
import struct
import sys

TYPES = {1: ("B", 1), 2: ("c", 1), 3: ("H", 2), 4: ("I", 4), 5: ("II", 8), 6: ("b", 1), 7: ("B", 1), 8: ("h", 2),
         9: ("i", 4), 10: ("ii", 8), 11: ("f", 4), 12: ("d", 8), 13: ("I", 4), 16: ("Q", 8)}
OP = {1: "WarpRectilinear", 2: "WarpFisheye", 3: "FixVignetteRadial", 4: "FixBadPixelsConstant", 5: "FixBadPixelsList",
      6: "TrimBounds", 7: "MapTable", 8: "MapPolynomial", 9: "GainMap", 10: "DeltaPerRow", 11: "DeltaPerColumn",
      12: "ScalePerRow", 13: "ScalePerColumn"}
NAMES = {274: "Orientation", 254: "NewSubfileType", 256: "ImageWidth", 257: "ImageLength", 258: "BitsPerSample", 259: "Compression",
         262: "Photometric", 271: "Make", 272: "Model", 277: "SamplesPerPixel", 330: "SubIFDs", 33421: "CFARepeatPatternDim",
         33422: "CFAPattern", 50706: "DNGVersion", 50707: "DNGBackwardVersion", 50708: "UniqueCameraModel",
         50710: "CFAPlaneColor", 50711: "CFALayout", 50714: "BlackLevel", 50717: "WhiteLevel", 50719: "DefaultCropOrigin",
         50720: "DefaultCropSize", 50721: "ColorMatrix1", 50722: "ColorMatrix2", 50728: "AsShotNeutral", 50729: "AsShotWhiteXY", 50964: "ForwardMatrix1",
         50730: "BaselineExposure", 50778: "CalibrationIlluminant1", 50779: "CalibrationIlluminant2",
         50829: "ActiveArea", 51008: "OpcodeList1", 51009: "OpcodeList2", 51022: "OpcodeList3", 50964: "ForwardMatrix1",
         50965: "ForwardMatrix2", 51041: "NoiseProfile", 50934: "AsShotProfileName", 52525: "ProfileGainTableMap",
         52544: "ProfileGainTableMap2"}


def read_ifd(f, off, e, depth=0):
    f.seek(off)
    n = struct.unpack(e + "H", f.read(2))[0]
    ents = []
    for _ in range(n):
        tag, typ, cnt, val = struct.unpack(e + "HHI4s", f.read(12))
        ents.append((tag, typ, cnt, val))
    nxt = struct.unpack(e + "I", f.read(4))[0]
    out = {}
    for tag, typ, cnt, val in ents:
        fmt, sz = TYPES.get(typ, ("B", 1))
        total = sz * cnt
        if total > 64 * 1024 * 1024:
            continue
        if total <= 4:
            raw = val[:total]
        else:
            o = struct.unpack(e + "I", val)[0]
            here = f.tell()
            f.seek(o)
            raw = f.read(total)
            f.seek(here)
        out[tag] = (typ, cnt, raw)
    return out, nxt


def decode_val(typ, cnt, raw, e):
    if typ == 2:
        return raw.decode("latin1", "replace").rstrip("\0")
    if typ in (1, 7):
        return list(raw[:64]) if cnt <= 64 else "<%d bytes>" % cnt
    fmt, sz = TYPES[typ]
    if typ in (5, 10):
        vals = struct.unpack(e + fmt[0] * (2 * cnt), raw)
        vals = [vals[i] / vals[i + 1] if vals[i + 1] else 0 for i in range(0, len(vals), 2)]
    else:
        vals = list(struct.unpack(e + fmt * cnt, raw))
    vals = [round(v, 5) if isinstance(v, float) else v for v in vals]
    return vals if len(vals) <= 32 else vals[:32] + ["...(%d)" % len(vals)]


def parse_ops(raw):
    ops = []
    if len(raw) < 4:
        return ops
    n = struct.unpack(">I", raw[:4])[0]
    p = 4
    for _ in range(n):
        if p + 16 > len(raw):
            break
        oid, ver, flags, plen = struct.unpack(">IIII", raw[p:p + 16])
        p += 16
        body = raw[p:p + plen]
        p += plen
        d = {"op": OP.get(oid, "id%d" % oid), "id": oid, "flags": flags, "bytes": plen}
        try:
            if oid == 9:  # GainMap
                top, left, bot, right, plane, planes, rp, cp, pv, ph = struct.unpack(">10I", body[:40])
                sv, sh, ov, oh = struct.unpack(">4d", body[40:72])
                mp = struct.unpack(">I", body[72:76])[0]
                cnt = pv * ph * mp
                vals = struct.unpack(">%df" % cnt, body[76:76 + 4 * cnt]) if cnt and len(body) >= 76 + 4 * cnt else ()
                d.update(area=[top, left, bot, right], plane=plane, planes=planes, rowPitch=rp, colPitch=cp,
                         mapPointsV=pv, mapPointsH=ph, mapPlanes=mp, spacingV=round(sv, 6), spacingH=round(sh, 6),
                         originV=round(ov, 6), originH=round(oh, 6))
                if vals:
                    d.update(gainMin=round(min(vals), 4), gainMax=round(max(vals), 4), gainMean=round(sum(vals) / len(vals), 4))
                    # corner / centre gains of the first map plane
                    g = vals[0::mp]
                    c = g[(pv // 2) * ph + ph // 2]
                    d.update(gainTopLeft=round(g[0], 4), gainTopRight=round(g[ph - 1], 4), gainBottomLeft=round(g[(pv - 1) * ph], 4),
                             gainBottomRight=round(g[-1], 4), gainCentre=round(c, 4))
            elif oid == 1:  # WarpRectilinear
                planes = struct.unpack(">I", body[:4])[0]
                co = []
                q = 4
                for _p in range(planes):
                    co.append([round(x, 8) for x in struct.unpack(">6d", body[q:q + 48])])
                    q += 48
                cx, cy = struct.unpack(">2d", body[q:q + 16])
                d.update(planes=planes, coeffs_kr0_kr3_kt0_kt1=co, centre=[round(cx, 6), round(cy, 6)])
            elif oid == 3:  # FixVignetteRadial
                k = struct.unpack(">5d", body[:40])
                cx, cy = struct.unpack(">2d", body[40:56])
                d.update(k=[round(x, 8) for x in k], centre=[round(cx, 6), round(cy, 6)])
            elif oid == 6:
                d.update(trim=list(struct.unpack(">2I", body[:8])))
        except Exception as ex:  # malformed payload: keep what we have
            d["parse_error"] = str(ex)
        ops.append(d)
    return ops


def main():
    path = sys.argv[1]
    res = {"file": path}
    with open(path, "rb") as f:
        head = f.read(8)
        if head[:2] not in (b"II", b"MM"):
            res["error"] = "not a TIFF-structured file (magic %r)" % head[:4]
            print(json.dumps(res))
            return
        e = "<" if head[:2] == b"II" else ">"
        magic = struct.unpack(e + "H", head[2:4])[0]
        if magic != 42:
            res["error"] = "TIFF magic %d (not classic TIFF)" % magic
            print(json.dumps(res))
            return
        off = struct.unpack(e + "I", head[4:8])[0]
        ifds = []
        seen = set()
        todo = [("IFD0", off)]
        while todo and len(ifds) < 16:
            name, o = todo.pop(0)
            if o in seen or o == 0:
                continue
            seen.add(o)
            try:
                ents, nxt = read_ifd(f, o, e)
            except Exception:
                continue
            ifds.append((name, ents))
            if name == "IFD0" and nxt:
                todo.append(("IFD1", nxt))
            if 330 in ents:
                typ, cnt, raw = ents[330]
                for i, so in enumerate(struct.unpack(e + "I" * cnt, raw[:4 * cnt])):
                    todo.append(("SubIFD%d" % i, so))
        res["ifds"] = []
        for name, ents in ifds:
            d = {"ifd": name}
            for tag, (typ, cnt, raw) in ents.items():
                nm = NAMES.get(tag)
                if nm and tag not in (51008, 51009, 51022, 330):
                    d[nm] = decode_val(typ, cnt, raw, e)
                if tag in (51008, 51009, 51022):
                    d[nm] = parse_ops(raw)
                if tag >= 65000 or nm is None:
                    pass
            res["ifds"].append(d)
        have = set()
        for d in res["ifds"]:
            for k in ("OpcodeList1", "OpcodeList2", "OpcodeList3"):
                if k in d:
                    have.add(k + ":" + "+".join(o["op"] for o in d[k]))
        res["opcodes"] = sorted(have)
    print(json.dumps(res))


main()
