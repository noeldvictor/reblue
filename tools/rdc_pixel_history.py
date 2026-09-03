"""Which draw painted this pixel, and with what?
Run under RenderDoc's own interpreter:
    RDC_CAPTURE=C:/abs/cap.rdc RDC_XY=960,700 "C:/Program Files/RenderDoc/qrenderdoc.exe" --python tools/rdc_pixel_history.py
RDC_XY names one or more pixels ("x,y;x,y"). For each, the pixel history on
the scene pass's colour target (the largest colour+depth target with the
most draws) is listed: every event that touched the pixel, the colour it
wrote, whether it passed the depth test, and for the last writer the
shaders and the textures bound. The report lands in ~/rdc_pixel_history.log.
Written for the cyan skirt (2026-09-03): after the replay verifier found the
host replay composing every scene draw the same as the interpreter, the
question left was which draw paints the flat colour.
"""
import os
import sys
import traceback

import renderdoc as rd

ui = globals().get("pyrenderdoc")
out = open(os.path.join(os.path.expanduser("~"), "rdc_pixel_history.log"), "w")
path = os.environ.get("RDC_CAPTURE", "")
xy = os.environ.get("RDC_XY", "960,540")
out.write("ui=%r path=%r xy=%r\n" % (ui is not None, path, xy))
out.flush()


def say(s):
    out.write(s + "\n")
    out.flush()


def analyse(controller):
    textures = {t.resourceId: t for t in controller.GetTextures()}

    def walk(actions):
        for a in actions:
            yield a
            for c in walk(a.children):
                yield c

    acts = [a for a in walk(controller.GetRootActions())
            if a.flags & (rd.ActionFlags.Drawcall | rd.ActionFlags.Clear |
                          rd.ActionFlags.Copy | rd.ActionFlags.Resolve)]
    sf = controller.GetStructuredFile()
    # The scene pass: consecutive draws into one colour target, the largest
    # target with the most draws.
    groups = []
    for a in acts:
        outs = [o for o in a.outputs if o != rd.ResourceId.Null()]
        target = outs[0] if outs else rd.ResourceId.Null()
        if groups and groups[-1][0] == target:
            groups[-1][1].append(a)
        else:
            groups.append((target, [a]))
    best = None
    for target, actions in groups:
        t = textures.get(target)
        if t is None or t.width < 1280:
            continue
        n = sum(1 for a in actions if a.flags & rd.ActionFlags.Drawcall)
        if best is None or n > best[2]:
            best = (target, actions, n)
    if best is None:
        say("no scene pass found")
        return
    target, actions, n = best
    t = textures[target]
    say("scene pass: target %s %dx%d %s samples=%d, %d draws, eids %d-%d" % (
        str(target), t.width, t.height, str(t.format.Name()), t.msSamp, n,
        actions[0].eventId, actions[-1].eventId))
    last_eid = actions[-1].eventId
    controller.SetFrameEvent(last_eid, True)
    for pair in xy.split(";"):
        x, y = [int(v) for v in pair.split(",")]
        say("== pixel (%d, %d)" % (x, y))
        try:
            hist = controller.PixelHistory(target, x, y, rd.Subresource(0, 0, 0),
                                           rd.CompType.Typeless)
        except Exception as e:
            say("PixelHistory failed: %s" % e)
            continue
        say("%d modifications" % len(hist))
        last_pass = None
        for m in hist:
            try:
                a = next(a for a in acts if a.eventId == m.eventId)
                name = a.GetName(sf)
            except StopIteration:
                name = "?"
            passed = not (m.depthTestFailed or m.stencilTestFailed or
                          m.scissorClipped or m.backfaceCulled or
                          m.depthClipped or m.shaderDiscarded or
                          m.unboundPS or m.viewClipped)
            col = m.postMod.col.floatValue
            pre = m.preMod.col.floatValue
            so = m.shaderOut.col.floatValue
            say("  eid %-6d %-40s %s shaderOut=(%.3f %.3f %.3f %.3f) post=(%.3f %.3f %.3f %.3f) pre=(%.3f %.3f %.3f) depth=%.4f%s%s%s%s" % (
                m.eventId, name[:40], "PASS" if passed else "fail",
                so[0], so[1], so[2], so[3], col[0], col[1], col[2], col[3],
                pre[0], pre[1], pre[2], m.postMod.depth,
                " depthFail" if m.depthTestFailed else "",
                " discard" if m.shaderDiscarded else "",
                " culled" if m.backfaceCulled else "",
                " prim=%d" % m.primitiveID if passed else ""))
            if passed:
                last_pass = m
        if last_pass is None:
            say("  nothing passed at this pixel (the clear shows)")
            continue
        controller.SetFrameEvent(last_pass.eventId, True)
        ps_state = controller.GetPipelineState()
        try:
            vs = ps_state.GetShaderEntryPoint(rd.ShaderStage.Vertex)
            ps = ps_state.GetShaderEntryPoint(rd.ShaderStage.Pixel)
        except Exception:
            vs, ps = "?", "?"
        say("  last writer eid %d: vs=%s ps=%s" % (last_pass.eventId, vs, ps))
        try:
            vk = controller.GetVulkanPipelineState()
            say("  blend: %s depth test %s write %s func %s" % (
                [(b.enabled, str(b.colorBlend.source), str(b.colorBlend.destination)) for b in vk.colorBlend.blends[:1]],
                vk.depthStencil.depthTestEnable, vk.depthStencil.depthWriteEnable,
                str(vk.depthStencil.depthFunction)))
        except Exception as e:
            say("  (vk state: %s)" % e)
        try:
            reads = []
            for u in ps_state.GetReadOnlyResources(rd.ShaderStage.Pixel):
                d = getattr(u, "descriptor", u)
                rid = getattr(d, "resource", None)
                rt_ = textures.get(rid)
                if rt_ is None:
                    continue
                acc = getattr(u, "access", None)
                reads.append("bind%s[%s]=%s %dx%d %s" % (
                    getattr(acc, "index", "?"), getattr(acc, "arrayElement", "?"),
                    str(rid)[-8:], rt_.width, rt_.height, str(rt_.format.Name())[:12]))
                if len(reads) >= 12:
                    break
            say("  PS reads: " + ("; ".join(reads) if reads else "none"))
        except Exception as e:
            say("  (reads: %s)" % e)
        # The pixel shader's constant buffer 0 head (the guest PS block):
        # the fog colour and factors live there.
        try:
            for u in ps_state.GetConstantBlocks(rd.ShaderStage.Pixel):
                d = getattr(u, "descriptor", u)
                rid = getattr(d, "resource", None)
                off = getattr(d, "byteOffset", 0)
                sz = getattr(d, "byteSize", 0)
                say("  PS cbuffer: %s offset %d size %d" % (str(rid)[-8:], off, sz))
                break
        except Exception as e:
            say("  (cbuffers: %s)" % e)
    # The presented image: the last colour target written in the frame. Its
    # history at the same pixels says whether the colour was made after the
    # scene pass (post chain, overlay) rather than in it.
    final_target = None
    for a in reversed(acts):
        outs = [o for o in a.outputs if o != rd.ResourceId.Null()]
        for o in outs:
            tt = textures.get(o)
            if tt is not None and tt.width >= 1280 and tt.msSamp <= 1:
                final_target = (o, a.eventId)
                break
        if final_target:
            break
    if final_target:
        ft, feid = final_target
        tt = textures[ft]
        say("final target %s %dx%d %s written last at eid %d" % (str(ft), tt.width, tt.height, str(tt.format.Name()), feid))
        controller.SetFrameEvent(feid, True)
        for pair in xy.split(";"):
            x, y = [int(v) for v in pair.split(",")]
            try:
                hist = controller.PixelHistory(ft, x, y, rd.Subresource(0, 0, 0), rd.CompType.Typeless)
            except Exception as e:
                say("  final PixelHistory failed: %s" % e)
                continue
            say("== final image pixel (%d, %d): %d modifications" % (x, y, len(hist)))
            for m in hist:
                try:
                    a = next(a for a in acts if a.eventId == m.eventId)
                    name = a.GetName(sf)
                except StopIteration:
                    name = "?"
                passed = not (m.depthTestFailed or m.stencilTestFailed or m.scissorClipped or m.backfaceCulled or m.depthClipped or m.shaderDiscarded or m.unboundPS or m.viewClipped)
                col = m.postMod.col.floatValue
                so = m.shaderOut.col.floatValue
                say("  eid %-6d %-44s %s shaderOut=(%.3f %.3f %.3f %.3f) post=(%.3f %.3f %.3f)" % (m.eventId, name[:44], "PASS" if passed else "fail", so[0], so[1], so[2], so[3], col[0], col[1], col[2]))
    say("done")


try:
    say("capture: " + path)
    cap = rd.OpenCaptureFile()
    res = cap.OpenFile(path, "", None)
    if res != rd.ResultCode.Succeeded:
        say("open failed: %s" % res)
        sys.exit(1)
    res, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if res != rd.ResultCode.Succeeded:
        say("replay failed: %s" % res)
        sys.exit(1)
    analyse(controller)
    controller.Shutdown()
    cap.Shutdown()
except Exception:
    say(traceback.format_exc())
finally:
    out.close()
    if ui is not None:
        try:
            ui.Quit()
        except Exception:
            pass
