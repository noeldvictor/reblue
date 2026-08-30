file(GLOB_RECURSE reblue_all RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
     CONFIGURE_DEPENDS "*.cpp" "*.h")

set(reblue_backend_only
    gpu/bindless.cpp
    gpu/device.cpp
    gpu/device_lost.cpp
    gpu/device_pipelines.cpp
    gpu/draw.cpp
    gpu/draw_framebuffer.cpp
    gpu/dred.cpp
    gpu/frame_ring.cpp
    gpu/gpu_profiling.cpp
    gpu/gpu_timing.cpp
    gpu/hooks/shader.cpp
    gpu/multiview_resolve.cpp
    gpu/imgui_overlay_drawer.cpp
    gpu/occlusion.cpp
    gpu/pipeline/pipeline_cache.cpp
    gpu/present.cpp
    gpu/resolve.cpp
    gpu/sampler_cache.cpp
    gpu/shaders/guest_shaders.cpp
    gpu/texture_upload.cpp
    gpu/vertex_declaration.cpp
    platform/crash_handler.cpp
    reblue_app.cpp
    ui/report_issue_dialog.cpp)

# The runtime DXC shader linker has no Vulkan counterpart.
set(reblue_d3d12_only
    gpu/shaders/dxc_link.cpp
    gpu/shaders/dxc_link.h
    gpu/shaders/shader_linker.cpp
    gpu/shaders/shader_linker.h)

# The parts of src/xr that reach an OpenXR header, and so only compile when the
# loader is present. xr_camera and xr_settings are deliberately not here: the
# camera composition is plain maths and the settings are plain cvars, so both
# build in every configuration and stay testable without a headset attached.
set(reblue_openxr_only
    xr/xr_session.cpp
    xr/xr_session.h)

set(reblue_installer_only
    installer/disc_install.cpp
    installer/disc_install.h
    installer/installer_wizard.cpp
    installer/installer_wizard.h
    installer/self_install.cpp
    installer/self_install.h
    installer/upgrade_prompt.cpp
    installer/upgrade_prompt.h)

list(REMOVE_ITEM reblue_all
     ${reblue_backend_only} ${reblue_d3d12_only} ${reblue_installer_only}
     ${reblue_openxr_only})

reblue_sources(${reblue_all})
reblue_backend_sources(${reblue_backend_only})
reblue_d3d12_sources(${reblue_d3d12_only})
if(REBLUE_BUILD_INSTALLER)
    reblue_sources(${reblue_installer_only})
endif()
# Backend sources, not common ones: the session hands plume the physical device
# and extension list the runtime demands, so it is Vulkan-specific by nature.
if(REBLUE_OPENXR AND reblue_openxr_only)
    reblue_backend_sources(${reblue_openxr_only})
endif()
