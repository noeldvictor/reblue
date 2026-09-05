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
        cls.view = (root / "src/gpu/scene/native_view_bridge.cpp").read_text(encoding="utf-8")
        cls.view_math = (root / "src/gpu/scene/native_view.h").read_text(encoding="utf-8")
        cls.shadow = (root / "src/gpu/scene/native_shadow_pass_bridge.cpp").read_text(encoding="utf-8")

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

    def test_view_producer_does_not_delegate_its_math(self):
        native = self.view[self.view.index("bool Produce("):self.view.index("__imp__sub_82186840(ctx, base);")]
        for name in ("__imp__", "sub_822873E0", "sub_82287478", "sub_821CCC78",
                     "sub_82491748", "bdMatrixInverse4x4", "sub_82277198", "sub_8217A8D0"):
            self.assertNotIn(name, native)
        self.assertIn("GetNativeRenderTransforms()", native)
        self.assertIn("BuildViewFrustumShape", native)
        self.assertIn("views.Get(view)", native)
        self.assertIn("PublishCachedViewFrustum(ctx, 1)", self.bridge)
        # No address-based memory access, PPC context or GPU SDK in the core.
        for name in ("PPCContext", "bd::mem::", "REX_", "plume::", "kCache"):
            self.assertNotIn(name, self.view_math)

    def test_view_comparison_precedes_native_publication(self):
        native = self.view[self.view.index("bool Produce("):self.view.index("} // namespace")]
        self.assertLess(native.index("__imp__sub_82186840(ctx, base)"),
                        native.index("CompareWords(kShape"))
        self.assertLess(native.index("CompareWords(kShape"), native.index("Publish(shape, frustum)"))
        self.assertIn('CompareWords(slot + 4, Pack(shape), "cache")', native)

    def test_shadow_lifecycle_has_explicit_depth_and_output_ownership(self):
        native = self.shadow[self.shadow.index("bool Begin("):self.shadow.index("REX_HOOK_RAW(")]
        for name in ("__imp__", "hcgD3DCreateSurface", "bdRenderTargetRelease",
                     "ClassifyHostTarget", "bdSurfaceSetMSAA", "bdDestroySurface",
                     "D3DDevice_Resolve", "TrackResolveSource", "surfaceDrawn"):
            self.assertNotIn(name, native)
        self.assertIn("HostTargetClass::Shadow", native)
        self.assertIn("EnterNativePass(nullptr, depth, result)", native)
        self.assertIn("shadows.push_back({source, depth, output, NativePassDepth()})", native)
        self.assertIn("RetainResourceAdapter(output->selfVa)", native)
        end = native[native.index("bool End("):]
        self.assertIn("Video::BindDrawFramebuffer()", end)
        self.assertIn("PublishSceneOutput(pass.depth, pass.output, 1.0f, false)", end)
        self.assertLess(end.index("LeaveNativePass(result)"),
                        end.index("ReleaseResourceAdapter(pass.depth->selfVa)"))
        self.assertNotIn("bd_native_shadow_passes", end)  # scopes outlive setting changes

    def test_shadow_output_does_not_publish_a_post_chain(self):
        self.assertIn("if (publish_post_chain)\n    NoteTileContentLocked", self.output)
        self.assertIn("PublishSceneOutput(pass.depth, pass.output, 1.0f, false)", self.shadow)
        # These two producers are deliberately still counted, never called native.
        self.assertIn("++stats.camera_snapshots", self.shadow)
        self.assertIn("++stats.light_fits", self.shadow)


if __name__ == "__main__":
    unittest.main()
