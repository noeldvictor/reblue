#!/usr/bin/env python3
"""Outline the render passes of a RenderDoc capture, from its XML export.

This exists because every remaining question in this port has the shape "which
draw wrote nothing, and into what", and inference has lost that argument
repeatedly - the multiview resolve had ten causes eliminated by measurement and
none of them right, three of the wrong answers coming from bounded log counters
that report what happened *first* rather than what happens.

A capture is the record that cannot lie about it. Khronos ships no Windows
validation binaries, so on the desktop RenderDoc is the instrument available,
and its XML export is the whole command stream as text.

    # in the app: bd_renderdoc = true, bd_renderdoc_after_s = 150
    renderdoccmd convert -f cap.rdc -c zip.xml -o frame.zip.xml
    python tools/rdc_outline.py frame.zip.xml

Prints one line per render pass - target size, layer count, pipelines bound,
viewports set and draws issued - which is enough to see a pass that runs into
the wrong target, a pass that draws nothing, or a stereo pair that collapses to
one layer.
"""

import argparse
import io
import re


def chunks(text):
    """Yield (index, name, body) per chunk, in capture order."""
    for m in re.finditer(r'<chunk id="\d+" chunkIndex="(\d+)" name="([^"]+)"[^>]*>(.*?)</chunk>',
                         text, re.S):
        yield int(m.group(1)), m.group(2), m.group(3)


def uint(body, name, default=None):
    m = re.search(r'<(?:uint|int) name="%s"[^>]*>(-?\d+)<' % name, body)
    return int(m.group(1)) if m else default


def rid(body, name):
    m = re.search(r'<ResourceId name="%s"[^>]*>(\d+)<' % name, body)
    return int(m.group(1)) if m else None


def renderpass_viewmasks(text):
    """Map render pass id -> viewMask.

    Multiview is configured through a VkRenderPassMultiviewCreateInfo chained
    on pNext, not through a field of VkRenderPassCreateInfo, so a pass that
    looks ordinary in every other respect may still be rendering two views -
    and one that was *meant* to will look identical to a mono pass without
    this. Reading it back from the capture is the only way to tell which the
    driver was actually given.
    """
    out = {}
    for _, name, body in chunks(text):
        if name != "vkCreateRenderPass":
            continue
        rp = None
        ids = re.findall(r'<ResourceId name="RenderPass"[^>]*>(\d+)<', body)
        if ids:
            rp = int(ids[-1])
        if rp is None:
            continue
        mask = 0
        m = re.search(r'typename="VkRenderPassMultiviewCreateInfo".*?'
                      r'<array name="pViewMasks">\s*<uint[^>]*>(\d+)<', body, re.S)
        if m:
            mask = int(m.group(1))
        out[rp] = mask
    return out


def framebuffers(text):
    """Map framebuffer id -> (w, h, layers, attachment view ids)."""
    out = {}
    for _, name, body in chunks(text):
        if name != "vkCreateFramebuffer":
            continue
        fb = rid(body, "Framebuffer")
        if fb is None:
            # Older exports name the output differently; take the last id.
            ids = re.findall(r'<ResourceId name="\w*[Ff]ramebuf\w*"[^>]*>(\d+)<', body)
            fb = int(ids[-1]) if ids else None
        if fb is None:
            continue
        views = [int(v) for v in
                 re.findall(r'<ResourceId typename="VkImageView"[^>]*>(\d+)<', body)]
        out[fb] = (uint(body, "width"), uint(body, "height"),
                   uint(body, "layers"), views)
    return out


def viewports(body):
    return [(float(x), float(y), float(w), float(h)) for x, y, w, h in
            re.findall(r'<float name="x"[^>]*>([-\d.e+]+)<.*?'
                       r'<float name="y"[^>]*>([-\d.e+]+)<.*?'
                       r'<float name="width"[^>]*>([-\d.e+]+)<.*?'
                       r'<float name="height"[^>]*>([-\d.e+]+)<',
                       body, re.S)]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("xml")
    ap.add_argument("--mask", type=int, default=None,
                    help="only show passes whose render pass has this view mask")
    args = ap.parse_args()

    text = io.open(args.xml, encoding="utf-8", errors="replace").read()
    fbs = framebuffers(text)
    masks = renderpass_viewmasks(text)
    # VkFramebufferCreateInfo::layers must be 1 when the render pass has a view
    # mask - the layer count comes from the mask - so a layered framebuffer
    # reads as layers=1 and only the mask says whether it is stereo.
    print("%d framebuffers created; %d render passes, %d with a view mask"
          % (len(fbs), len(masks), sum(1 for v in masks.values() if v)))

    cur = None
    passes = []
    for idx, name, body in chunks(text):
        if name == "vkCmdBeginRenderPass":
            fb = rid(body, "framebuffer")
            w, h, layers, views = fbs.get(fb, (None, None, None, []))
            rp = rid(body, "renderPass")
            cur = dict(idx=idx, fb=fb, rp=rp, mask=masks.get(rp, 0),
                       w=w, h=h, layers=layers, views=views,
                       pipes=[], vps=[], draws=0, verts=[])
        elif cur is None:
            continue
        elif name == "vkCmdBindPipeline":
            cur["pipes"].append(rid(body, "pipeline"))
        elif name == "vkCmdSetViewport":
            cur["vps"].extend(viewports(body))
        elif name in ("vkCmdDraw", "vkCmdDrawIndexed"):
            cur["draws"] += 1
            cur["verts"].append(uint(body, "vertexCount") or uint(body, "indexCount"))
        elif name == "vkCmdEndRenderPass":
            passes.append(cur)
            cur = None

    print("%d render passes in the frame\n" % len(passes))
    for i, p in enumerate(passes):
        if args.mask is not None and p["mask"] != args.mask:
            continue
        vp = ""
        if p["vps"]:
            uniq = sorted(set(p["vps"]))
            vp = " vp=" + ",".join("%gx%g@%g" % (v[2], v[3], v[0]) for v in uniq[:4])
        print("  [%3d] fb=%-6s %sx%s mask=%d %s pipes=%d draws=%3d%s"
              % (i, p["fb"], p["w"], p["h"], p["mask"],
                 "STEREO" if p["mask"] else "mono  ",
                 len(set(p["pipes"])), p["draws"], vp))


if __name__ == "__main__":
    main()
