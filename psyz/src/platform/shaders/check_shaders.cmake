# Fails the configure step when a shader header was not built from the GLSL
# next to it. build_shaders.sh writes the SHA-256 of the GLSL (CR stripped)
# into each header; a GLSL change without the matching headers would
# otherwise run the old shaders, or fall back to the old shader interface,
# on the platforms whose headers were left behind. Every platform checks
# every format (SPIR-V for Vulkan, MSL for Metal, DXIL for D3D12), so a
# change made on one platform cannot leave another's headers stale. These
# are all the shaders sdl3_gpu.c compiles in; the CI shader jobs also
# rebuild them and fail when the bytes differ.

set(_psyz_shader_dir ${CMAKE_CURRENT_LIST_DIR})

# header base name : GLSL source : formats
set(_psyz_shader_headers
    "psx_vert:psx.vert:spv,msl,dxil"
    "psx_frag:psx.frag:spv,msl,dxil"
    "psx_frag_fetch:psx.frag:msl"
    "clear_vert:clear.vert:spv,msl,dxil"
    "clear_frag:clear.frag:spv,msl,dxil")

set(_psyz_stale_shaders)
foreach(_entry IN LISTS _psyz_shader_headers)
    string(REPLACE ":" ";" _fields "${_entry}")
    list(GET _fields 0 _name)
    list(GET _fields 1 _glsl)
    list(GET _fields 2 _formats)
    string(REPLACE "," ";" _formats "${_formats}")
    set(_source ${_psyz_shader_dir}/${_glsl}.glsl)
    file(READ ${_source} _text)
    string(REPLACE "\r" "" _text "${_text}")
    string(SHA256 _expected "${_text}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_source})
    foreach(_format IN LISTS _formats)
        set(_header ${_psyz_shader_dir}/${_name}_${_format}.h)
        if(NOT EXISTS ${_header})
            list(APPEND _psyz_stale_shaders "${_name}_${_format}.h (missing)")
            continue()
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            ${_header})
        file(STRINGS ${_header} _recorded
            REGEX "^#define ${_name}_${_format}_source_sha256 ")
        string(REGEX MATCH "\"([0-9a-f]+)\"" _match "${_recorded}")
        if(NOT _match OR NOT CMAKE_MATCH_1 STREQUAL _expected)
            list(APPEND _psyz_stale_shaders "${_name}_${_format}.h")
        endif()
    endforeach()
endforeach()

if(_psyz_stale_shaders)
    list(JOIN _psyz_stale_shaders ", " _stale)
    message(FATAL_ERROR
        "PsyZ shader headers not built from the current GLSL: ${_stale}. "
        "Run src/platform/shaders/build_shaders.sh (SKIP_DXIL=1 for SPIR-V "
        "and MSL; DXIL needs the Windows SDK dxc, e.g. Ridge Racer Type 4 "
        "PC's psyz-shaders workflow).")
endif()
