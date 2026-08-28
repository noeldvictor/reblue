# Patches

Changes this fork needs in a submodule it does not own. Each one is a plain `git diff` against the
commit pinned in `.gitmodules`, applied from inside the submodule.

Patches live here rather than in a forked submodule because forking a submodule means owning its
history forever. When one of these grows past "small and obviously correct", fork the submodule
properly and delete the patch — the patch is the cheap option, not the right one.

## `plume-openxr-seam.patch`

**What it does.** Lets an OpenXR runtime dictate the parts of Vulkan setup it owns. This is the
top item on the risk register in `docs/VR_PORT_PLAN.md` and it gates every other piece of VR work.

OpenXR does not let an application pick its own Vulkan instance extensions, device extensions, or
physical device. The runtime names all three, and an application that chooses its own gets
`XR_ERROR_GRAPHICS_DEVICE_INVALID` at `xrCreateSession`. plume creates the instance and device
itself and selects the adapter by *name*, so there is nowhere to thread the runtime's answers
through.

The patch adds `plume::VulkanInterfaceOptions`:

```cpp
struct VulkanInterfaceOptions {
    std::vector<std::string> extraInstanceExtensions;  // xrGetVulkanInstanceExtensionsKHR
    std::vector<std::string> extraDeviceExtensions;    // xrGetVulkanDeviceExtensionsKHR
    VkPhysicalDevice forcedPhysicalDevice;             // xrGetVulkanGraphicsDeviceKHR
    uint32_t minApiVersion;                            // xrGetVulkanGraphicsRequirementsKHR
};

std::unique_ptr<RenderInterface> CreateVulkanInterface(const VulkanInterfaceOptions *options);
```

Five small changes, 69 insertions and 5 deletions across two files:

1. `VulkanInterface` gains an `options` member and a defaulted constructor parameter.
2. `appInfo.apiVersion` takes `minApiVersion` as a floor. It only ever raises, never lowers.
3. `extraInstanceExtensions` is inserted into `requiredExtensions` before the availability check —
   deliberately *required* rather than optional, so a driver that cannot provide what the runtime
   asked for is named by plume's existing missing-extension error instead of failing opaquely
   inside `xrCreateSession`.
4. The physical-device loop skips anything that is not `forcedPhysicalDevice` when one is set, so
   no scoring heuristic can override the runtime. This matters on a laptop whose headset is wired
   to the integrated GPU while the discrete one scores higher.
5. `extraDeviceExtensions` is appended to the device extension list, de-duplicated against what is
   already enabled, because Vulkan rejects a repeated extension name.

**Compatibility.** The options pointer defaults to null and the no-argument `CreateVulkanInterface()`
is kept as an overload, so the D3D12 path and every existing caller are untouched. Nothing outside
`plume_vulkan.*` changes, and the cross-backend `RenderInterface` API is not touched at all.

**Status: written, not compiled.** This tree has no Vulkan SDK, no volk, and no ReXGlue SDK, so
nothing here has been through a compiler. Treat it as a reviewed proposal. The first thing that
happens when a real toolchain exists is building it.

### Applying

```sh
git submodule update --init thirdparty/plume
git -C thirdparty/plume apply ../../patches/plume-openxr-seam.patch
```

Check it still applies after a submodule bump with `git -C thirdparty/plume apply --check`. If it
stops applying, that is the signal to fork plume properly rather than to rebase the patch a third
time.

**Do not add `*.patch -text` to `.gitattributes`.** It looks like the right hardening and it is not.
plume has no `.gitattributes`, so its sources are checked out CRLF on Windows and LF on Linux; the
patch, left as normal text, gets the same treatment and stays consistent with them on both. Pinning
the patch to fixed bytes would make it CRLF on a Linux checkout where plume's files are LF, and it
would stop applying there.
