# A headless OpenXR runtime for testing the VR path with no headset.
# Built on its own, not as part of reblue: it is a test fixture, and linking it
# into the game would be a way to ship it by accident.
cmake_minimum_required(VERSION 3.25)
project(reblue_xrsim CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Headers only - the runtime implements the API, it does not call it, so it
# must not link the loader.
set(XR_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../../out/xr-headers/openxr/include"
    CACHE PATH "OpenXR headers, including loader_interfaces.h")
set(VK_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../../thirdparty/plume/contrib/Vulkan-Headers/include"
    CACHE PATH "Vulkan headers")

add_library(reblue_xrsim SHARED xrsim.cpp)
target_include_directories(reblue_xrsim PRIVATE ${XR_INCLUDE} ${VK_INCLUDE})
set_target_properties(reblue_xrsim PROPERTIES PREFIX "")

# The manifest the loader reads. XR_RUNTIME_JSON points at this.
#
# An absolute library_path, because the loader resolves a relative one against
# its own working directory rather than the manifest's, and the failure is
# opaque: "Failed to open dynamic library reblue_xrsim.dll with error 2".
# Forward slashes so the JSON needs no escaping.
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/reblue_xrsim.json" CONTENT
"{\n\
    \"file_format_version\": \"1.0.0\",\n\
    \"runtime\": {\n\
        \"api_version\": \"1.0\",\n\
        \"name\": \"reblue xrsim\",\n\
        \"library_path\": \"$<TARGET_FILE:reblue_xrsim>\"\n\
    }\n\
}\n")
