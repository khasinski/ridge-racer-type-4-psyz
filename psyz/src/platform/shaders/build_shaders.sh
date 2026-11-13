#!/bin/sh

# Compiles the SDL_GPU shaders to SPIR-V (Vulkan), MSL (Metal) and DXIL (D3D12)
# Requirements: glslangValidator, xxd, spirv-cross, dxc or SDL shadercross.
# SKIP_DXIL=1 regenerates only SPIR-V and MSL (the Windows SDK dxc is needed
# for DXIL: the psyz-shaders workflow of Ridge Racer Type 4 PC runs this on a
# Windows runner). Every header records the SHA-256 of the GLSL it was built
# from (CR stripped); check_shaders.cmake fails the configure on every
# platform while any header (any format) no longer matches its GLSL, so
# SKIP_DXIL leaves the build broken until the DXIL headers are rebuilt. The
# CI shader jobs (pc.yml: shaders, shaders-dxil) rebuild all of them and
# fail when the bytes differ from the committed ones.

# DXIL/Windows requires signing. The best way to pre-compile the shader is
# C:\Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/dxc.exe
# running via Wine.

set -eu
cd "$(dirname "$0")"

if ! command -v spirv-cross > /dev/null 2>&1; then
    echo "build_shaders.sh: spirv-cross not found; needed for MSL/HLSL headers" >&2
    exit 1
fi

if [ -z "${SKIP_DXIL:-}" ] &&
   ! command -v dxc > /dev/null 2>&1 &&
   ! command -v shadercross > /dev/null 2>&1; then
    echo "build_shaders.sh: dxc or SDL shadercross required for DXIL headers" >&2
    exit 1
fi

sha256() {
    if command -v sha256sum > /dev/null 2>&1; then
        tr -d '\r' < "$1" | sha256sum | cut -d' ' -f1
    else
        tr -d '\r' < "$1" | shasum -a 256 | cut -d' ' -f1
    fi
}

emit_header() {
    src=$1        # intermediate file, e.g. psx.vert.spv
    # (POSIX sh has no locals: header, not name, which the loop below keeps
    # for the DXIL header after psx.frag's framebuffer-fetch MSL)
    header=$2     # canonical base, e.g. psx_vert
    suffix=$3     # spv, msl or dxil
    {
        echo "// clang-format off"
        xxd -i "$src" | sed \
            -e "s/unsigned char [A-Za-z0-9_]*\[\]/unsigned char ${header}_${suffix}[]/" \
            -e "s/unsigned int [A-Za-z0-9_]*_len/unsigned int ${header}_${suffix}_len/"
        # the interface revision the C side checks (PSX_SHADER_REVISION)
        if [ -n "$revision" ]; then
            echo "#define ${header}_${suffix}_revision $revision"
        fi
        # the source it was built from (check_shaders.cmake)
        echo "#define ${header}_${suffix}_source_sha256 \"$source_sha256\""
    } > "${header}_${suffix}.h"
}

# Metal on Apple GPUs: the fragment shader reads the destination pixel from
# the colour attachment ([[color(0)]], framebuffer fetch) instead of the
# texBlend copy, so semi-transparent primitives need no copy or new render
# pass (sdl3_gpu.c, destination_fetch).
emit_fetch_msl() {
    read_dst='texBlend.read(uint2(int2(gl_FragCoord.xy)), 0)'
    position='float4 gl_FragCoord \[\[position\]\])'
    grep -qF "$read_dst" "$1" && grep -q "$position" "$1" || {
        echo "build_shaders.sh: $1 has no texBlend read or position input" >&2
        exit 1
    }
    sed -e "s/texBlend\.read(uint2(int2(gl_FragCoord\.xy)), 0)/psyz_destination/g" \
        -e "s/$position/float4 gl_FragCoord [[position]], float4 psyz_destination [[color(0)]])/" \
        "$1" > "$2"
    if grep -q "texBlend\.read" "$2"; then
        echo "build_shaders.sh: $2 still reads texBlend" >&2
        exit 1
    fi
}

if [ "$#" -eq 0 ]; then
    set -- psx.vert psx.frag clear.vert clear.frag
fi
for shader in "$@"; do
    name=$(echo "$shader" | tr . _)
    stage=${shader##*.}
    revision=$(sed -n 's,^// PSYZ_SHADER_REVISION ,,p' "$shader.glsl")
    source_sha256=$(sha256 "$shader.glsl")
    glslangValidator -V --target-env vulkan1.0 -S "$stage" \
        "$shader.glsl" -o "$shader.spv"
    emit_header "$shader.spv" "$name" spv
    spirv-cross --msl --msl-decoration-binding "$shader.spv" --output "$shader.metal"
    emit_header "$shader.metal" "$name" msl
    if [ "$shader" = psx.frag ]; then
        emit_fetch_msl "$shader.metal" "$shader.fetch.metal"
        emit_header "$shader.fetch.metal" psx_frag_fetch msl
        rm -f "$shader.fetch.metal"
    fi
    if [ -n "${SKIP_DXIL:-}" ]; then
        rm -f "$shader.spv" "$shader.metal"
        continue
    fi
    case "$stage" in
        vert) profile=vs_6_0; cross_stage=vertex ;;
        frag) profile=ps_6_0; cross_stage=fragment ;;
    esac
    spirv-cross --hlsl --shader-model 60 "$shader.spv" --output "$shader.hlsl"
    if command -v dxc > /dev/null 2>&1; then
        dxc -T "$profile" -E main "$shader.hlsl" -Fo "$shader.dxil"
    else
        shadercross "$shader.hlsl" -s HLSL -d DXIL -t "$cross_stage" \
            -o "$shader.dxil"
    fi
    emit_header "$shader.dxil" "$name" dxil
    rm -f "$shader.spv" "$shader.metal" "$shader.hlsl" "$shader.dxil"
done
