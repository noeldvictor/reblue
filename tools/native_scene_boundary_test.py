"""Scene ownership source guards, not an independent GPU or ABI comparison."""
from pathlib import Path
import unittest


class NativeSceneBoundaryTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        root = Path(__file__).resolve().parents[1]
        cls.bridge = (root / "src/gpu/scene/native_scene_pass_bridge.cpp").read_text(encoding="utf-8")
        source = (root / "src/gpu/resolve.cpp").read_text(encoding="utf-8")
        start = source.index("bool Video::PublishSceneOutput(")
        cls.output = source[start:source.index("void Video::ResolveRtToTexture(", start)]

    def test_whole_native_pair_does_not_use_console_allocation_or_resolve(self):
        native = self.bridge[self.bridge.index("bool Begin("):self.bridge.index("REX_HOOK_RAW(")]
        for name in ("__imp__", "hcgD3DCreateSurface", "bdRenderTargetRelease",
                     "ClassifyHostTarget", "bdResolveToTexture", "D3DDevice_Resolve",
                     "D3DDevice_BeginTiling", "D3DDevice_EndTiling",
                     "kStack + 232", "kStack + 236", "kStack + 240"):
            self.assertNotIn(name, native)
        self.assertIn("HostTargetClass::SceneColor", native)
        self.assertIn("HostTargetClass::SceneDepth", native)
        self.assertIn("PublishSceneOutput(pass.depth, depth_output, 1.0f)", native)
        self.assertIn("PublishSceneOutput(pass.color, color_output, exposure)", native)
        self.assertLess(native.index("LeaveNativePass(result)"),
                        native.index("ReleaseResourceAdapter(pass.color->selfVa)"))

    def test_explicit_output_never_selects_a_source_by_binding_or_dimensions(self):
        for name in ("TrackResolveSource", "ResolveSourceForFlagsLocked",
                     "last_drawn", "s.render_target", "s.depth_stencil",
                     "resolveClearToFar = true", "HostResourceHeap", "ResolveGuestTexture"):
            self.assertNotIn(name, self.output)
        self.assertIn("dst->sourceSurface = src", self.output)
        self.assertIn("dst->resolveScale = exposure", self.output)
        # Keep the remaining downstream compatibility dependency visible.
        self.assertIn("NoteTileContentLocked", self.output)


if __name__ == "__main__":
    unittest.main()
