get_filename_component(_dxc_root "${CMAKE_CURRENT_LIST_DIR}" PATH)
get_filename_component(_dxc_root "${_dxc_root}" PATH)

set(DIRECTX_DXC_TOOL "${_dxc_root}/tools/directx-dxc/dxc.exe" CACHE PATH "Location of the dxc tool")
mark_as_advanced(DIRECTX_DXC_TOOL)

add_library(Microsoft::DirectXShaderCompiler SHARED IMPORTED)
set_target_properties(Microsoft::DirectXShaderCompiler PROPERTIES
   IMPORTED_LOCATION                    "${_dxc_root}/bin/dxcompiler.dll"
   IMPORTED_IMPLIB                      "${_dxc_root}/lib/dxcompiler.lib"
   IMPORTED_SONAME                      "dxcompiler.lib"
   INTERFACE_INCLUDE_DIRECTORIES        "${_dxc_root}/include/directx-dxc"
   INTERFACE_LINK_LIBRARIES             "Microsoft::DXIL"
   IMPORTED_LINK_INTERFACE_LANGUAGES    "C")

add_library(Microsoft::DXIL SHARED IMPORTED)
set_target_properties(Microsoft::DXIL PROPERTIES
   IMPORTED_LOCATION                    "${_dxc_root}/bin/dxil.dll"
   IMPORTED_IMPLIB                      "${_dxc_root}/lib/dxcompiler.lib"
   IMPORTED_NO_SONAME                   TRUE
   INTERFACE_INCLUDE_DIRECTORIES        "${_dxc_root}/include/directx-dxc"
   IMPORTED_LINK_INTERFACE_LANGUAGES    "C")

unset(_dxc_root)
