"""DoF source ownership guards, not independent GPU qualification."""
from pathlib import Path
import unittest


class NativePostBoundaryTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        root = Path(__file__).resolve().parents[1]
        cls.bridge = (root / "src/gpu/native_dof_bridge.cpp").read_text(encoding="utf-8")
        cls.post = (root / "src/gpu/post_chain.cpp").read_text(encoding="utf-8")
        cls.parameters = (root / "src/gpu/post_parameters.h").read_text(encoding="utf-8")
        cls.scheduler = (root / "src/gpu/native_post_bridge.cpp").read_text(encoding="utf-8")
        cls.flare = (root / "src/gpu/lens_flare.h").read_text(encoding="utf-8")
        cls.flare_shader = (root / "src/gpu/shaders/hlsl/lens_flare_ps.hlsl").read_text(encoding="utf-8")

    def test_flare_shader_folds_the_quarter_image_with_tested_mapping(self):
        self.assertIn('#include "src/gpu/lens_flare_uv.h"', self.flare_shader)
        self.assertIn("float2(LensFlareU(uv.x), LensFlareV(uv.y))", self.flare_shader)
        self.assertIn("float3(optical_uv,0)", self.flare_shader)
        self.assertIn("NonUniformResourceIndex(image)", self.flare_shader)

    def test_native_flare_is_one_instanced_draw_into_explicit_output(self):
        body = self.post.split("bool RenderLensFlare(", 1)[1].split("} // namespace", 1)[0]
        self.assertIn("drawInstanced(6, parameters.count, 0, 0)", body)
        self.assertIn("GetFramebuffer(s, output, nullptr)", body)
        for name in ("s.render_target", "GuestPixelConstant", "D3DDevice_", "s.textures[", "device_guest"):
            self.assertNotIn(name, body)
        self.assertNotIn("filter(8660", self.scheduler)
        self.assertIn("std::none_of(tail.adjustments.begin()", self.scheduler)

    def test_flare_recipe_has_no_engine_or_register_dependency(self):
        for name in ("PPCContext", "bd::mem::", "plume::", "REX_", "psFloatConstants"):
            self.assertNotIn(name, self.flare)
        body = self.scheduler.split("bool ReadLensFlare(", 1)[1].split("bool ReadPlan(", 1)[0]
        self.assertIn("GetNativeRenderTransforms()", body)
        self.assertIn("MakeLensFlareParameters(", body)
        for name in ("sub_82183DE8(", "sub_82218140(", "__imp__", "psFloatConstants"):
            self.assertNotIn(name, body)

    def test_direct_frame_has_no_old_draw_trigger_or_target_inference(self):
        body = self.post.split("bool HostPostRender(", 1)[1].split("bool HostPostPrepareDof(", 1)[0]
        for name in ("GuestPixelConstant", "device_guest", "s.textures[", "s.render_target",
                     "HostPostIntercept", "ResolveRtToTexture", "TrackResolveSource"):
            self.assertNotIn(name, body)
        self.assertLess(body.index("DrawQueueFlush("), body.index("BuildDofPyramid("))
        self.assertIn("HostComposite(s, c, source, nullptr, output, bloom)", body)
        self.assertIn("s.draw_framebuffer_bound = false", body)

    def test_composite_consumes_typed_native_parameters(self):
        body = self.post.split("bool HostComposite(", 1)[1].split("} // namespace", 1)[0]
        self.assertNotIn("GuestPixelConstant", body)
        self.assertNotIn("s.render_target", body)
        self.assertIn("parameters.scene_weight[i]", body)
        self.assertIn("parameters.threshold", body)

    def test_native_scheduler_has_explicit_completed_output(self):
        body = self.scheduler.split("if (HostPostRender(", 1)[1].split("++stats.inputs", 1)[0]
        self.assertIn("Video::PublishSceneOutput(target, scene, 1.0f)", body)
        for name in ("__imp__", "sub_8221E758", "sub_8221CB38", "sub_822166E8"):
            self.assertNotIn(name, body)

    def test_native_prepare_has_no_console_parameter_or_resource_producer(self):
        body = self.post.split("bool HostPostPrepareDof(", 1)[1].split("bool HostPostProducerSkip(", 1)[0]
        for name in ("GuestPixelConstant", "device_guest", "s.textures[", "D3DDevice_", "ResolveRtToTexture"):
            self.assertNotIn(name, body)
        self.assertIn("BuildDofPyramid(s, c, scene, depth, parameters)", body)
        self.assertLess(body.index("DrawQueueFlush("), body.index("BuildDofPyramid("))
        self.assertIn("s.draw_framebuffer_bound = false", body)

    def test_parameters_are_not_shader_register_readback(self):
        body = self.bridge.split("bool ReadParameters(", 1)[1].split("void Verify(", 1)[0]
        self.assertIn("GetNativeRenderTransforms()", body)
        self.assertIn("MakeDofParameters(", body)
        for name in ("psFloatConstants", "__imp__", "kEngine"):
            self.assertNotIn(name, body)
        for name in ("PPCContext", "bd::mem::", "plume::", "REX_"):
            self.assertNotIn(name, self.parameters)

    def test_native_consume_has_no_quad_or_resolve(self):
        body = self.bridge.split("REX_HOOK_RAW(bdShadowStencilDrawIndexed)", 1)[1].split("} else {", 1)[0]
        self.assertIn("preparation.Consume(ctx.r3.u32, ctx.r4.u32, FrameStatFrameCount())", body)
        self.assertIn("ctx.r3.u64 = 1", body)
        for name in ("__imp__", "D3DDevice_", "sub_8221CD08", "sub_8221CE78"):
            self.assertNotIn(name, body)


if __name__ == "__main__":
    unittest.main()
