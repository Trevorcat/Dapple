"""Appends a Sony-style Non-Real Time Metadata 'meta' box (handler 'nrtm') to an MP4, as XAVC S files carry it.
usage: add_nrt_meta.py in.mp4 out.mp4 <CaptureGammaEquation> <CaptureColorPrimaries>"""
import struct, sys
src, dst, gamma, prim = sys.argv[1:5]
xml = ('<?xml version="1.0" encoding="UTF-8"?>\n'
       '<NonRealTimeMeta xmlns="urn:schemas-professionalDisc:nonRealTimeMeta:ver.2.20" lastUpdate="2026-01-01T00:00:00+08:00">\n'
       '\t<AcquisitionRecord>\n\t\t<Group name="CameraUnitMetadataSet">\n'
       '\t\t\t<Item name="CaptureGammaEquation" value="%s"/>\n'
       '\t\t\t<Item name="CaptureColorPrimaries" value="%s"/>\n'
       '\t\t</Group>\n\t</AcquisitionRecord>\n</NonRealTimeMeta>\n' % (gamma, prim)).encode()
def box(t, payload): return struct.pack('>I4s', 8 + len(payload), t) + payload
hdlr = box(b'hdlr', b'\0' * 8 + b'nrtm' + b'\0' * 12 + b'Non-Real Time Metadata\0')
xmlb = box(b'xml ', b'\0\0\0\0' + xml)
meta = box(b'meta', b'\0\0\0\0' + hdlr + xmlb)
open(dst, 'wb').write(open(src, 'rb').read() + meta)
