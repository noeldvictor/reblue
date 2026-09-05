include("${CMAKE_CURRENT_LIST_DIR}/build_info.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/embed.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/shader_cache.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/shaders.cmake")

# A build stamp in target_compile_definitions puts the commit on every TU's
# command line, so every commit would rebuild the whole target.
reblue_write_build_info("${REBLUE_GEN_DIR}/core/build_info.h")

reblue_shader_cache(
    RECOMP_TARGET XenosRecomp
    INPUT_DIR     "${CMAKE_CURRENT_SOURCE_DIR}/assets"
    INCLUDE_FILE  "${REBLUE_SHADER_COMMON_H}"
    OUTPUT_CPP    "${REBLUE_GEN_DIR}/shader_cache.cpp")
set(REBLUE_GENERATED_SOURCES "${REBLUE_GEN_DIR}/shader_cache.cpp")

foreach(shader IN ITEMS copy_vs bd_2d_blit_vs imgui_vs)
    reblue_host_shader(${shader} vs_6_0)
endforeach()
reblue_host_shader(lens_flare_vs vs_6_0)
reblue_host_shader(lens_flare_ps ps_6_0)
foreach(shader IN ITEMS
        cel_ps pfx_occlusion_count_ps
        bd_2d_blit_ps imgui_ps)
    reblue_host_shader(${shader} ps_6_0)
endforeach()
# 6.1: the present pass reads SV_ViewID for the layered XR swapchain, where
# each output array layer takes its own eye's layer of the source instead of
# the pair being flattened side by side (bd_xr_layered_swapchain).
reblue_host_shader(gamma_correction_ps ps_6_1)
# 6.1: the MSAA resolves read SV_ViewID to pick the right eye's slice view of
# a two-layer multisampled scene - without it they flattened the stereo pair.
foreach(shader IN ITEMS
        resolve_msaa_color_2x resolve_msaa_color_4x resolve_msaa_color_8x
        resolve_msaa_depth_2x resolve_msaa_depth_4x resolve_msaa_depth_8x)
    reblue_host_shader(${shader} ps_6_1)
endforeach()
# 6.1: copy_color_ps reads SV_ViewID so the guest's EDRAM resolve copies each
# eye's own layer instead of flattening the pair to layer 0.
reblue_host_shader(copy_color_ps ps_6_1)
# Same reason: copy_depth_ps reads SV_ViewID so a depth resolve keeps both eyes.
reblue_host_shader(copy_depth_ps ps_6_1)
foreach(shader IN ITEMS bd_pe_ps_brightpass_clamp bd_pe_ps_ms_bright_clamp)
    reblue_host_shader(${shader} ps_6_0 -D REBLUE_RECOMP)
endforeach()
# The host lit material (gpu/shaders/hlsl/bd_normal_lit.hlsl) replaces the
# guest's bd_normal_ps by hash at link time; 6.1 for SV_ViewID.
reblue_host_shader(bd_normal_lit ps_6_1 -D REBLUE_RECOMP)
# The occlusion proxy (gpu/occlusion_cull.cpp): a cube per node under a query.
reblue_host_shader(occ_proxy_vs vs_6_1 -D REBLUE_RECOMP)
reblue_host_shader(occ_proxy_ps ps_6_0)
reblue_host_shader(bd_normal_wind_lit ps_6_1 -D REBLUE_RECOMP)
# The host-owned post chain (gpu/post_chain.cpp): downsample, separable blur
# and bright mask, producing the guest's pyramid textures without the tile.
foreach(shader IN ITEMS post_down_ps post_blur_ps post_bright_ps
                        post_dual_down_ps post_composite_ps post_pyramid_ps post_adjust_ps
                        post_scanline_ps post_grade_ps post_bloom_direction_ps)
    reblue_host_shader(${shader} ps_6_1)
endforeach()

if(REBLUE_BUILD_INSTALLER)
    set(embed_skip "")
else()
    set(embed_skip installer)
endif()
reblue_embed_directory(
    ROOT        "${CMAKE_CURRENT_SOURCE_DIR}/res/embed"
    HEADER      "${REBLUE_GEN_DIR}/embedded.h"
    SOURCES_VAR REBLUE_EMBEDDED_SOURCES
    SKIP_DIRS   ${embed_skip})
list(APPEND REBLUE_GENERATED_SOURCES ${REBLUE_EMBEDDED_SOURCES})

# Neither the recompiled guest code nor these data blobs read REBLUE_D3D12, so
# both exes link one set of objects instead of paying for 54 recomp TUs and a
# multi-megabyte shader cache per backend. OBJECT and never STATIC: an archive
# drops the REX_HOOK registration symbols.
#
# The two halves are separate libraries so the recomp TUs can take the SDK's
# reblue_pch.h, which a shared library would force onto the shader cache and the
# embedded blobs as well.
function(reblue_generated_object_library name)
    add_library(${name} OBJECT ${ARGN})
    target_include_directories(${name} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        # XenosRecomp emits a bare #include "shader_cache.h" at the top of the
        # cache it generates, and the header lives beside the shader sources
        # rather than in src/ root, so a quote-include from the build directory
        # cannot reach it.
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/shaders"
        "${REBLUE_GEN_DIR}"
        "${REXGLUE_ENTRYPOINT_INCLUDE_DIR}")
    target_compile_definitions(${name} PRIVATE
        $<${REBLUE_PROFILING_ON}:REXGLUE_ENABLE_PROFILING>)
    target_link_libraries(${name} PRIVATE
        rex::runtime
        # reblue_init.h reaches for tracy/Tracy.hpp when zones are on, and these
        # objects have to see the same headers the exes compile against.
        $<${REBLUE_PROFILING_ON}:rex::TracyClient>)
    rexglue_apply_target_settings(${name})
endfunction()

# Empty until codegen has run once, and a tree in that state still has to
# configure so the build can generate it.
set(REBLUE_RECOMP_LIB "")
if(REXGLUE_ENTRYPOINT_GENERATED_SOURCES)
    reblue_generated_object_library(reblue_recomp ${REXGLUE_ENTRYPOINT_GENERATED_SOURCES})
    rexglue_apply_recomp_settings(reblue_recomp
        "${REXGLUE_ENTRYPOINT_INCLUDE_DIR}/reblue_pch.h")
    add_dependencies(reblue_recomp reblue_codegen)
    set(REBLUE_RECOMP_LIB reblue_recomp)
endif()

reblue_generated_object_library(reblue_generated ${REBLUE_GENERATED_SOURCES})
add_dependencies(reblue_generated reblue_shader_cache_gen)

# rexglue_setup_target adds the entrypoint sources to whatever target it is
# called on, which would compile them again into each exe.
set(REXGLUE_ENTRYPOINT_GENERATED_SOURCES "")

# reblue_prelink DXC-links every specConstantsMask-subset variant of the shader
# cache at build time, so the runtime never pays a DXC link for a shipped shader.
if(REBLUE_D3D12_TARGETS)
    add_executable(reblue_prelink
        tools/prelink_shader_cache.cpp
        src/gpu/shaders/dxc_link.cpp
        "${REBLUE_GEN_DIR}/shader_cache.cpp"
        thirdparty/miniz/miniz.cpp
        thirdparty/zstd/zstddeclib.c)
    add_dependencies(reblue_prelink reblue_shader_cache_gen)
    target_include_directories(reblue_prelink PRIVATE src thirdparty/miniz thirdparty/zstd)
    target_link_libraries(reblue_prelink PRIVATE Microsoft::DirectXShaderCompiler)
    reblue_stage_dxc(reblue_prelink)

    add_custom_command(
        OUTPUT "${REBLUE_GEN_DIR}/linked_shader_cache.cpp"
        COMMAND reblue_prelink "${REBLUE_GEN_DIR}/linked_shader_cache.cpp"
        DEPENDS reblue_prelink
        COMMENT "Pre-linking spec-constant shader variants"
        VERBATIM)
    set_source_files_properties("${REBLUE_GEN_DIR}/linked_shader_cache.cpp"
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    foreach(target IN LISTS REBLUE_D3D12_TARGETS)
        target_sources(${target} PRIVATE "${REBLUE_GEN_DIR}/linked_shader_cache.cpp")
    endforeach()
endif()
