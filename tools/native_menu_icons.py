#!/usr/bin/env python3
"""Rebuild original native menu XDS icons from the checked-in SVG primitives.
Developer-only: requires Pillow; launchers stage the prebuilt XDS assets.
The restricted SVG vocabulary is rect/circle/line/polyline/polygon, outlined
white at 12 units. Rasterize at 4x then downsample for smooth native UI edges.
"""
from pathlib import Path
import xml.etree.ElementTree as E,struct
from PIL import Image,ImageDraw
from xdc_decode import tiled_offset


def main():
    root = Path(__file__).parent / "native_menu/icons"
    for source in sorted(root.glob('*.svg')):
        name=source.stem
        svg=source.read_text()
        im=Image.new('RGBA',(1024,1024));d=ImageDraw.Draw(im)
        for e in E.fromstring(svg):
            tag=e.tag.split('}')[-1];a=e.attrib;white=(255,255,255,255);sw=48
            if tag in ['polygon','polyline']:
                pts=[tuple(float(v)*4 for v in p.split(',')) for p in a['points'].split()]
                if tag=='polygon':pts.append(pts[0])
                d.line(pts,fill=white,width=sw,joint='curve')
                for x,y in pts:d.ellipse((x-sw/2,y-sw/2,x+sw/2,y+sw/2),fill=white)
            elif tag=='line':
                pts=[(float(a['x1'])*4,float(a['y1'])*4),(float(a['x2'])*4,float(a['y2'])*4)]
                d.line(pts,fill=white,width=sw)
                for x,y in pts:d.ellipse((x-sw/2,y-sw/2,x+sw/2,y+sw/2),fill=white)
            elif tag=='rect':
                x,y,w,h=[float(a[v])*4 for v in ['x','y','width','height']];d.rounded_rectangle((x,y,x+w,y+h),radius=float(a.get('rx',0))*4,outline=white,width=sw)
            elif tag=='circle':
                x,y,r=[float(a[v])*4 for v in ['cx','cy','r']];d.ellipse((x-r,y-r,x+r,y+r),outline=white,width=sw)
        im=im.resize((256,256),Image.Resampling.LANCZOS)
        rgba=im.tobytes();pixels=bytearray(256*256*4)
        for y in range(256):
            for x in range(256):
                i=(y*256+x)*4;o=tiled_offset(x,y,256);alpha=rgba[i+3]
                pixels[o:o+4]=bytes((alpha,255,255,255))
        # Original art in a native XDS v3, tiled 8_8_8_8 surface; no game pixels.
        header=struct.pack('>13I',3,1,0,0,0,0xffff0000,0xffff0000,0x820000fe,0xb2,0x1fe0ff,0xc14,0,0xa00)
        (root/(name+'.xds')).write_bytes(header+pixels)
        print(name,len(header+pixels))


if __name__ == "__main__":
    main()
