"""Source-boundary regression guard; not a runtime concurrency proof.

The draw hook owns VideoState::mutex. A registry lookup there can deadlock
with an IO mirror upload (registry -> video). Keep lookup in node commit,
before the template-store lock, and snapshot the selector at draw time.
"""
from pathlib import Path
import unittest


class ReflectionLockOrderTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = (Path(__file__).resolve().parents[1]
                  / "src/gpu/scene/host_draw.cpp").read_text(encoding="utf-8")
        capture = source.index("void HostDrawCapture(")
        commit = source.index("void HostDrawCommit(")
        end = source.index("u32 RenderListCount()", commit)
        cls.capture = source[capture:commit]
        cls.commit = source[commit:end]

    def test_capture_snapshots_without_registry_lookup(self):
        self.assertIn("p.reflection_checks.push_back", self.capture)
        self.assertIn("SelectReflectionTextureImport", self.capture)
        for name in ("ResolveGuestTexture(", "ResolveReflectionBinding(",
                     "ResolveReflectionAddress(", "PrepareNativeSceneTextures("):
            self.assertNotIn(name, self.capture)

    def test_commit_resolves_before_store_lock_and_publication(self):
        lookup = self.commit.index("ResolveReflectionAddress(check.address)")
        self.assertLess(lookup, self.commit.index("std::lock_guard lock(st.mutex)"))
        self.assertLess(lookup, self.commit.index("!p.replayable"))
        self.assertNotIn("SelectReflectionTextureImport(", self.commit)
        self.assertIn("p.reflection_checks.clear()", self.commit)

    def test_scene_capture_uses_prepared_input_and_discards_retained_slots(self):
        self.assertIn("p.scene_texture_inputs[i]", self.capture)
        self.assertIn("expected.native == CaptureNativeTexture(actual)", self.capture)
        self.assertIn("d.scene_textures.SlotMask()", self.commit)
        self.assertIn("d.native_textures[k] = {}", self.commit)
        self.assertIn("d.textures[k] = nullptr", self.commit)
        self.assertIn("d.tex_va[k] = 0", self.commit)


if __name__ == "__main__":
    unittest.main()
