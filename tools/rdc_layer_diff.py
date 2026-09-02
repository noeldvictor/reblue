"""Where in the frame does a two-layer target's pair collapse?

Run under RenderDoc's own interpreter, which carries the `renderdoc` module:

    "C:/Program Files/RenderDoc/qrenderdoc.exe" --python tools/rdc_layer_diff.py C:/abs/path/cap.rdc

Give the capture as an ABSOLUTE path. qrenderdoc is a GUI application: its
stdout goes nowhere, its working directory is its install directory, and a
script that fails there fails silently. The first version of this script did
exactly that for an hour - a relative output path under Program Files.

For every run of consecutive actions that render into the same colour target,
replay to the last one and read both array layers back. A target whose layers
differ holds a stereo pair; one whose layers are byte-identical has been
flattened. The first target in the frame that comes out identical after a
source that did not is where multiview loses its second eye - which is what
three sessions of reading views and descriptors could not settle, because a
pipeline, a view and a descriptor can all be correct while the *shader* still
reads one layer.

Prints one line per pass on a two-layer target: event id, target, size, format,
and whether the layers differ. Output goes to <capture>.layers.txt beside the
capture, and progress lines land there as the replay advances.
"""
import os
import sys
import traceback

import renderdoc as rd

# Under qrenderdoc the UI has already opened the capture named on the command
# line and exposes it as `pyrenderdoc`; standalone (renderdoccmd's python or a
# matching CPython) we open it ourselves.
ui = globals().get("pyrenderdoc")
# Log to a fixed absolute location first, before anything that can fail: the
# capture may not be open yet when the script starts under the UI.
out = open(os.path.join(os.path.expanduser("~"), "rdc_layer_diff.log"), "w")
try:
    if ui is not None:
        path = ui.GetCaptureFilename()
    else:
        path = os.path.abspath(sys.argv[-1])
except Exception:
    path = ""
if not path:
    path = os.path.abspath(sys.argv[-1]) if len(sys.argv) > 1 else ""
out.write("argv=%r ui=%r path=%r\n" % (sys.argv, ui is not None, path))
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

    draws = [a for a in walk(controller.GetRootActions())
             if a.flags & (rd.ActionFlags.Drawcall | rd.ActionFlags.Clear |
                           rd.ActionFlags.Copy | rd.ActionFlags.Resolve)]

    groups = []
    for a in draws:
        outs = [o for o in a.outputs if o != rd.ResourceId.Null()]
        target = outs[0] if outs else rd.ResourceId.Null()
        if groups and groups[-1][0] == target:
            groups[-1][1].append(a)
        else:
            groups.append((target, [a]))

    say("%d actions, %d passes" % (len(draws), len(groups)))
    say("%-7s %-6s %-11s %-22s %-6s %s" % ("eid", "draws", "size", "format", "layers", "verdict"))
    for target, actions in groups:
        t = textures.get(target)
        if t is None or t.arraysize < 2:
            continue
        last = actions[-1]
        controller.SetFrameEvent(last.eventId, True)
        layers = []
        for layer in range(2):
            sub = rd.Subresource(0, layer, 0)
            layers.append(bytes(controller.GetTextureData(target, sub)))
        a, b = layers
        if len(a) != len(b) or not a:
            verdict = "readback size mismatch %d/%d" % (len(a), len(b))
        else:
            step = max(1, len(a) // 200000)
            sa = a[::step]
            sb = b[::step]
            diff = sum(1 for x, y in zip(sa, sb) if x != y)
            nonzero = sum(1 for x in sa if x)
            if diff == 0:
                verdict = "IDENTICAL (%.0f%% non-zero)" % (100.0 * nonzero / len(sa))
            else:
                verdict = "differ %.1f%% of sampled bytes" % (100.0 * diff / len(sa))
        say("%-7d %-6d %-11s %-22s %-6d %s" % (
            last.eventId, len(actions), "%dx%d" % (t.width, t.height),
            str(t.format.Name())[:22], t.arraysize, verdict))
    say("done")


try:
    say("capture: " + path)
    # Always open the capture ourselves, even inside qrenderdoc: under
    # `--python` the script starts while the UI is still loading the capture,
    # GetCaptureFilename() is empty, and Replay().BlockInvoke() waits for a
    # replay that never arrives. That is how three launches hung silently.
    if True:
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
        # Headless use: close the UI once the report is written.
        try:
            ui.CloseCapture()
            ui.Quit()
        except Exception:
            pass
