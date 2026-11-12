# Fails the configure step when the shader headers this platform compiles
# (sdl3_gpu.c: DXIL on Windows, MSL on Apple, SPIR-V elsewhere) were not
# built from the GLSL next to them. build_shaders.sh writes the SHA-256 of
# the GLSL (CR stripped) into each header; a GLSL change without the
# matching headers would otherwise run the old shaders, or fall back to the
# old shader interface, on that platform only.

set(_psyz_shader_dir ${CMAKE_CURRENT_LIST_DIR})
if(WIN32)
    set(_psyz_shader_format dxil)
elseif(APPLE)
    set(_psyz_shader_format msl)
else()
    set(_psyz_shader_format spv)
endif()

set(_psyz_shader_headers
    "psx_vert:psx.vert" "psx_frag:psx.frag" "clear_vert:clear.vert"
    "clear_frag:clear.frag")
if(APPLE)
    list(APPEND _psyz_shader_headers "psx_frag_fetch:psx.frag")
endif()

set(_psyz_stale_shaders)
foreach(_entry IN LISTS _psyz_shader_headers)
    string(REPLACE ":" ";" _pair "${_entry}")
    list(GET _pair 0 _name)
    list(GET _pair 1 _glsl)
    set(_header ${_psyz_shader_dir}/${_name}_${_psyz_shader_format}.h)
    set(_source ${_psyz_shader_dir}/${_glsl}.glsl)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        ${_header} ${_source})
    file(READ ${_source} _text)
    string(REPLACE "\r" "" _text "${_text}")
    string(SHA256 _expected "${_text}")
    file(STRINGS ${_header} _recorded
        REGEX "^#define ${_name}_${_psyz_shader_format}_source_sha256 ")
    string(REGEX MATCH "\"([0-9a-f]+)\"" _match "${_recorded}")
    if(NOT _match OR NOT CMAKE_MATCH_1 STREQUAL _expected)
        list(APPEND _psyz_stale_shaders "${_name}_${_psyz_shader_format}.h")
    endif()
endforeach()

if(_psyz_stale_shaders)
    list(JOIN _psyz_stale_shaders ", " _stale)
    message(FATAL_ERROR
        "PsyZ shader headers not built from the current GLSL: ${_stale}. "
        "Run src/platform/shaders/build_shaders.sh (DXIL needs the Windows "
        "SDK dxc, e.g. Ridge Racer Type 4 PC's psyz-shaders workflow).")
endif()
