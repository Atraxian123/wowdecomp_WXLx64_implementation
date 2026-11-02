# WarcraftXL modern water, in-tree: the standalone wxl-modern-water module (itself built on CoAVolFog's
# renderer) running inside the client. Opt-in at runtime: nothing happens unless WXLExtendedEquipment.ini has
# [Water] Enabled=1. Kept apart from WXLCore because it needs the engine (object manager, liquid system, day/night
# state) and the engine already links WXLCore.
#
# The pixel shaders are compiled with fxc at build time into headers the sources include, as the standalone module
# did; the Windows SDK provides fxc.exe.
file(GLOB VOLFOG_FXC_CANDIDATES "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/10.*/x86/fxc.exe")
if(VOLFOG_FXC_CANDIDATES)
    list(SORT VOLFOG_FXC_CANDIDATES COMPARE NATURAL ORDER DESCENDING)
    list(GET VOLFOG_FXC_CANDIDATES 0 VOLFOG_FXC_DEFAULT)
else()
    set(VOLFOG_FXC_DEFAULT "")
endif()
set(VOLFOG_FXC "${VOLFOG_FXC_DEFAULT}" CACHE FILEPATH "HLSL compiler (fxc.exe) for WXLWater")
if(NOT EXISTS "${VOLFOG_FXC}")
    message(FATAL_ERROR "WXLWater needs fxc.exe; install the Windows SDK or set -DVOLFOG_FXC=<path>")
endif()

set(VOLFOG_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(VOLFOG_SHADER_OUT "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY ${VOLFOG_SHADER_OUT})
set(VOLFOG_SHADER_HEADERS "")

set(VOLFOG_VF_INCLUDES ${VOLFOG_SHADER_DIR}/vf_common.hlsli ${VOLFOG_SHADER_DIR}/vf_integrate.hlsli
    ${VOLFOG_SHADER_DIR}/vf_local_lights.hlsli ${VOLFOG_SHADER_DIR}/vf_density_variation.hlsli
    ${VOLFOG_SHADER_DIR}/vf_authored_noise.hlsli ${VOLFOG_SHADER_DIR}/vf_composite.hlsli
    ${VOLFOG_SHADER_DIR}/vf_sample_split.hlsli)
set(VOLFOG_VW_FFT_INCLUDES ${VOLFOG_SHADER_DIR}/vw_fft_common.hlsli)
set(VOLFOG_VW_INCLUDES ${VOLFOG_SHADER_DIR}/vf_common.hlsli ${VOLFOG_SHADER_DIR}/vw_constants.hlsli
    ${VOLFOG_SHADER_DIR}/vw_surface.hlsli ${VOLFOG_SHADER_DIR}/vw_optics.hlsli
    ${VOLFOG_SHADER_DIR}/vw_reflection.hlsli)

function(volfog_shader name source profile)
    if(source MATCHES "^vw_fft_")
        set(includes ${VOLFOG_VW_FFT_INCLUDES})
    elseif(source MATCHES "^vw_")
        set(includes ${VOLFOG_VW_INCLUDES})
    else()
        set(includes ${VOLFOG_VF_INCLUDES})
    endif()
    set(out ${VOLFOG_SHADER_OUT}/${name}.h)
    add_custom_command(
        OUTPUT ${out}
        COMMAND "${VOLFOG_FXC}" /nologo /O3 /T ${profile} /E main /Vn g_${name} ${ARGN}
                /Fh ${out} /Fc ${VOLFOG_SHADER_OUT}/${name}.asm ${VOLFOG_SHADER_DIR}/${source}
        DEPENDS ${VOLFOG_SHADER_DIR}/${source} ${includes}
        COMMENT "fxc ${name}"
        VERBATIM)
    set(VOLFOG_SHADER_HEADERS ${VOLFOG_SHADER_HEADERS} ${out} PARENT_SCOPE)
endfunction()

volfog_shader(vs_fullscreen vf_fullscreen_vs.hlsl vs_3_0)
volfog_shader(ps_march_low vf_march.hlsl ps_3_0 /DSTEPS=16 /DLOCAL_LIGHTS=0)
volfog_shader(ps_march_mid vf_march.hlsl ps_3_0 /DSTEPS=24 /DLOCAL_LIGHTS=0)
volfog_shader(ps_march_high vf_march.hlsl ps_3_0 /DSTEPS=32 /DLOCAL_LIGHTS=0)
volfog_shader(ps_lit_march_low vf_march.hlsl ps_3_0 /DSTEPS=16 /DLOCAL_LIGHTS=1)
volfog_shader(ps_lit_march_mid vf_march.hlsl ps_3_0 /DSTEPS=24 /DLOCAL_LIGHTS=1)
volfog_shader(ps_lit_march_high vf_march.hlsl ps_3_0 /DSTEPS=32 /DLOCAL_LIGHTS=1)
volfog_shader(ps_noisy_march_low vf_march.hlsl ps_3_0 /DSTEPS=16 /DLOCAL_LIGHTS=0 /DSAMPLES_AUTHORED_NOISE=1)
volfog_shader(ps_noisy_march_mid vf_march.hlsl ps_3_0 /DSTEPS=24 /DLOCAL_LIGHTS=0 /DSAMPLES_AUTHORED_NOISE=1)
volfog_shader(ps_noisy_march_high vf_march.hlsl ps_3_0 /DSTEPS=32 /DLOCAL_LIGHTS=0 /DSAMPLES_AUTHORED_NOISE=1)
volfog_shader(ps_lit_noisy_march_low vf_march.hlsl ps_3_0 /DSTEPS=16 /DLOCAL_LIGHTS=1 /DSAMPLES_AUTHORED_NOISE=1)
volfog_shader(ps_lit_noisy_march_mid vf_march.hlsl ps_3_0 /DSTEPS=24 /DLOCAL_LIGHTS=1 /DSAMPLES_AUTHORED_NOISE=1)
volfog_shader(ps_lit_noisy_march_high vf_march.hlsl ps_3_0 /DSTEPS=32 /DLOCAL_LIGHTS=1 /DSAMPLES_AUTHORED_NOISE=1)
volfog_shader(ps_density_probe vf_density_probe.hlsl ps_3_0)
volfog_shader(ps_temporal vf_temporal.hlsl ps_3_0)
volfog_shader(ps_history_depth vf_history_depth.hlsl ps_3_0)
volfog_shader(ps_composite_low vf_composite.hlsl ps_3_0 /DSTEPS=16 /DLOCAL_LIGHTS=0)
volfog_shader(ps_composite_mid vf_composite.hlsl ps_3_0 /DSTEPS=24 /DLOCAL_LIGHTS=0)
volfog_shader(ps_composite_high vf_composite.hlsl ps_3_0 /DSTEPS=32 /DLOCAL_LIGHTS=0)
volfog_shader(ps_noisy_composite_low vf_composite.hlsl ps_3_0 /DSTEPS=16 /DLOCAL_LIGHTS=0 /DSAMPLES_AUTHORED_NOISE=1)
volfog_shader(ps_noisy_composite_mid vf_composite.hlsl ps_3_0 /DSTEPS=24 /DLOCAL_LIGHTS=0 /DSAMPLES_AUTHORED_NOISE=1)
volfog_shader(ps_noisy_composite_high vf_composite.hlsl ps_3_0 /DSTEPS=32 /DLOCAL_LIGHTS=0 /DSAMPLES_AUTHORED_NOISE=1)
volfog_shader(ps_lit_composite_low vf_composite.hlsl ps_3_0 /DSTEPS=16 /DLOCAL_LIGHTS=1)
volfog_shader(ps_lit_composite_mid vf_composite.hlsl ps_3_0 /DSTEPS=24 /DLOCAL_LIGHTS=1)
volfog_shader(ps_lit_composite_high vf_composite.hlsl ps_3_0 /DSTEPS=32 /DLOCAL_LIGHTS=1)
volfog_shader(ps_split_composite_low vf_composite_split.hlsl ps_3_0 /DSTEPS=16 /DLOCAL_LIGHTS=0)
volfog_shader(ps_split_composite_mid vf_composite_split.hlsl ps_3_0 /DSTEPS=24 /DLOCAL_LIGHTS=0)
volfog_shader(ps_split_composite_high vf_composite_split.hlsl ps_3_0 /DSTEPS=32 /DLOCAL_LIGHTS=0)
volfog_shader(ps_noisy_split_composite_low vf_composite_split.hlsl ps_3_0 /DSTEPS=16 /DLOCAL_LIGHTS=0
              /DSAMPLES_AUTHORED_NOISE=1)
volfog_shader(ps_noisy_split_composite_mid vf_composite_split.hlsl ps_3_0 /DSTEPS=24 /DLOCAL_LIGHTS=0
              /DSAMPLES_AUTHORED_NOISE=1)
volfog_shader(ps_noisy_split_composite_high vf_composite_split.hlsl ps_3_0 /DSTEPS=32 /DLOCAL_LIGHTS=0
              /DSAMPLES_AUTHORED_NOISE=1)
volfog_shader(ps_lit_split_composite_low vf_composite_split.hlsl ps_3_0 /DSTEPS=16 /DLOCAL_LIGHTS=1)
volfog_shader(ps_lit_split_composite_mid vf_composite_split.hlsl ps_3_0 /DSTEPS=24 /DLOCAL_LIGHTS=1)
volfog_shader(ps_lit_split_composite_high vf_composite_split.hlsl ps_3_0 /DSTEPS=32 /DLOCAL_LIGHTS=1)
volfog_shader(ps_silhouette_mask vf_silhouette_mask.hlsl ps_3_0 /DLOCAL_LIGHTS=0)
volfog_shader(ps_ray_mask vf_ray_mask.hlsl ps_3_0)
volfog_shader(ps_ray_blur vf_ray_blur.hlsl ps_3_0)
volfog_shader(ps_ray_composite vf_ray_composite.hlsl ps_3_0 /DSTEPS=16 /DLOCAL_LIGHTS=0)
volfog_shader(ps_probe vf_probe.hlsl ps_3_0)
volfog_shader(ps_depth_check vf_depth_check.hlsl ps_3_0)
volfog_shader(ps_grade vp_grade.hlsl ps_3_0)

volfog_shader(ps_vw_fft_evolve vw_fft_evolve.hlsl ps_3_0)
volfog_shader(ps_vw_fft_rows vw_fft_butterfly.hlsl ps_3_0 /DCOLUMNS=0 /DFUSED_STAGES=0 /DASSEMBLE=0)
volfog_shader(ps_vw_fft_rows_fused vw_fft_butterfly.hlsl ps_3_0 /DCOLUMNS=0 /DFUSED_STAGES=1 /DASSEMBLE=0)
volfog_shader(ps_vw_fft_columns vw_fft_butterfly.hlsl ps_3_0 /DCOLUMNS=1 /DFUSED_STAGES=0 /DASSEMBLE=0)
volfog_shader(ps_vw_fft_columns_fused vw_fft_butterfly.hlsl ps_3_0 /DCOLUMNS=1 /DFUSED_STAGES=1 /DASSEMBLE=0)
volfog_shader(ps_vw_fft_assemble_fused vw_fft_butterfly.hlsl ps_3_0 /DCOLUMNS=1 /DFUSED_STAGES=1 /DASSEMBLE=1)
volfog_shader(ps_vw_fft_surface_foam vw_fft_surface.hlsl ps_3_0 /DSURFACE=1 /DFOAM=1)
volfog_shader(ps_vw_fft_surface vw_fft_surface.hlsl ps_3_0 /DSURFACE=1 /DFOAM=0)
volfog_shader(ps_vw_fft_foam vw_fft_surface.hlsl ps_3_0 /DSURFACE=0 /DFOAM=1)
volfog_shader(ps_vw_fft_mip vw_fft_mip.hlsl ps_3_0 /DPAIRED=0)
volfog_shader(ps_vw_fft_mip_pair vw_fft_mip.hlsl ps_3_0 /DPAIRED=1)

volfog_shader(ps_vw_depth vw_linear_depth.hlsl ps_3_0 /DPACKED_DEPTH=0)
volfog_shader(ps_vw_depth_packed vw_linear_depth.hlsl ps_3_0 /DPACKED_DEPTH=1)
volfog_shader(ps_vw_shade_low vw_water.hlsl ps_3_0 /DSSR_STEPS=0)
volfog_shader(ps_vw_shade_mid vw_water.hlsl ps_3_0 /DSSR_STEPS=8)
volfog_shader(ps_vw_shade_high vw_water.hlsl ps_3_0 /DSSR_STEPS=16)
volfog_shader(ps_vw_ripple_step vw_ripple_step.hlsl ps_3_0)


add_custom_target(WXLWater_shaders DEPENDS ${VOLFOG_SHADER_HEADERS})

file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_library(WXLWater STATIC
    ${PRIVATE_SOURCES}
)
add_dependencies(WXLWater WXLWater_shaders)

target_include_directories(WXLWater
    PUBLIC
        ${CMAKE_SOURCE_DIR}
    PRIVATE
        "${VOLFOG_SHADER_OUT}"
        "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_compile_definitions(WXLWater PRIVATE _CRT_SECURE_NO_WARNINGS)

target_link_libraries(WXLWater
    PUBLIC
        WXLCore
    PRIVATE
        d3d9.lib
)

# The settings file and the water data sit in a WXLWater folder beside the client executable. Set
# -DWXLWATER_DEPLOY_DIR=<that folder> to have the build copy them there.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/data/waterdata.bin")
    message(WARNING "WXLWater: data/waterdata.bin is missing. Copy it from the wxl-modern-water module's data folder; "
                    "the water stays off without it.")
endif()
set(WXLWATER_DEPLOY_DIR "" CACHE PATH "Folder (WXLWater, beside the client executable) to copy the water settings and data to")
if(WXLWATER_DEPLOY_DIR)
    add_custom_command(TARGET WXLWater POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${WXLWATER_DEPLOY_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/wxl-modern-water.ini" "${WXLWATER_DEPLOY_DIR}/wxl-modern-water.ini"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/data/waterdata.bin" "${WXLWATER_DEPLOY_DIR}/waterdata.bin"
        COMMENT "Deploy WXLWater settings and data -> ${WXLWATER_DEPLOY_DIR}")
endif()
