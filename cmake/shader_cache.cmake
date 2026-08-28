# Runs the Xenos shader recompiler over INPUT_DIR at build time into OUTPUT_CPP.
# Defines reblue_shader_cache_gen, and reblue_shader_hlsl_dump for inspecting
# the intermediate HLSL.
function(reblue_shader_cache)
    cmake_parse_arguments(ARG "" "RECOMP_TARGET;INPUT_DIR;INCLUDE_FILE;OUTPUT_CPP" "" ${ARGN})
    foreach(arg RECOMP_TARGET INPUT_DIR INCLUDE_FILE OUTPUT_CPP)
        if(NOT ARG_${arg})
            message(FATAL_ERROR "reblue_shader_cache: missing ${arg}")
        endif()
    endforeach()

    cmake_path(GET ARG_OUTPUT_CPP PARENT_PATH output_dir)
    file(MAKE_DIRECTORY "${output_dir}")

    file(GLOB_RECURSE shader_inputs CONFIGURE_DEPENDS
        "${ARG_INPUT_DIR}/*.vso" "${ARG_INPUT_DIR}/*.pso" "${ARG_INPUT_DIR}/*.xex")

    # XenosRecomp runs at build time, so on a cross build it has to be a host
    # binary - a cross-compiled one cannot execute. Point REBLUE_XENOSRECOMP at
    # one built for the host and the in-tree target is only used for its headers
    # and smol-v sources. See the devloop skill for the two-configure recipe.
    if(REBLUE_XENOSRECOMP)
        set(recomp_command "${REBLUE_XENOSRECOMP}")
        set(recomp_depends "")
    else()
        set(recomp_command "$<TARGET_FILE:${ARG_RECOMP_TARGET}>")
        set(recomp_depends ${ARG_RECOMP_TARGET})
    endif()

    add_custom_command(
        OUTPUT "${ARG_OUTPUT_CPP}"
        COMMAND ${recomp_command}
                "${ARG_INPUT_DIR}" "${ARG_OUTPUT_CPP}" "${ARG_INCLUDE_FILE}"
        DEPENDS ${recomp_depends} "${ARG_INCLUDE_FILE}" ${shader_inputs}
        COMMENT "Recompiling Xenos shaders from ${ARG_INPUT_DIR}"
        USES_TERMINAL VERBATIM)
    add_custom_target(reblue_shader_cache_gen DEPENDS "${ARG_OUTPUT_CPP}")

    set(REBLUE_HLSL_DUMP_DIR "${CMAKE_BINARY_DIR}/hlsl_dump" CACHE PATH
        "Directory reblue_shader_hlsl_dump writes recompiled HLSL into")
    add_custom_target(reblue_shader_hlsl_dump
        COMMAND ${CMAKE_COMMAND} -E make_directory "${REBLUE_HLSL_DUMP_DIR}"
        COMMAND ${recomp_command}
                "${ARG_INPUT_DIR}" "${output_dir}/shader_cache.hlsldump.cpp"
                "${ARG_INCLUDE_FILE}" "${REBLUE_HLSL_DUMP_DIR}"
        COMMENT "Dumping recompiled HLSL to ${REBLUE_HLSL_DUMP_DIR}"
        USES_TERMINAL VERBATIM)
    if(recomp_depends)
        add_dependencies(reblue_shader_hlsl_dump ${recomp_depends})
    endif()
endfunction()
