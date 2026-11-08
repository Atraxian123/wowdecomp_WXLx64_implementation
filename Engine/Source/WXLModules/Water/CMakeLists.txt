# WarcraftXL modern water module: wxl_water.dll, loaded from <client folder>\WXL\Modules\ by the 64-bit client.
# The standalone wxl-modern-water module (itself built on CoAVolFog's renderer), built against Engine/Source/WXL/Sdk
# (wxl_api.h, wxl_water_api.h, wxl_module.hpp) and its own folder only: the client's state reaches it through the
# "wxl.water.v1" service the core registers (Engine/Source/WXLApi/WaterHost.cpp).
#
# Opt-in at runtime: the DLL refuses to load unless WXLSetup.ini has [Water] Enabled=1. Its settings,
# waterdata.bin and log live in WXL\Modules\wxl_water\ (the build copies the first two there).
#
# The pixel shaders are compiled with fxc at build time into headers the sources include, as the standalone module
# did; the Windows SDK provides fxc.exe.
if (NOT WOWDECOMP_ARCH_64)
    message(STATUS "WXL modules are 64-bit only: wxl_water is not built")
    return()
endif ()

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


add_custom_target(wxl_water_shaders DEPENDS ${VOLFOG_SHADER_HEADERS})

file(GLOB MODULE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_library(wxl_water SHARED ${MODULE_SOURCES})
add_dependencies(wxl_water wxl_water_shaders)

target_include_directories(wxl_water
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/Engine/Source/WXL/Sdk
        "${VOLFOG_SHADER_OUT}"
)

target_compile_definitions(wxl_water PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)

target_link_libraries(wxl_water PRIVATE d3d9.lib)

set(WXL_WATER_MODULE_DIR "${CMAKE_SOURCE_DIR}/bin/Win/WXL/Modules")
set_target_properties(wxl_water PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    RUNTIME_OUTPUT_DIRECTORY "$<1:${WXL_WATER_MODULE_DIR}>"
)

# The settings file and the water data go beside the DLL, in wxl_water\.
if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/data/waterdata.bin")
    message(WARNING "wxl_water: data/waterdata.bin is missing. Copy it from the standalone wxl-modern-water module's "
                    "data folder; the module refuses to load without it.")
endif ()
add_custom_command(TARGET wxl_water POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${WXL_WATER_MODULE_DIR}/wxl_water"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/wxl-modern-water.ini" "${WXL_WATER_MODULE_DIR}/wxl_water/wxl-modern-water.ini"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/data/waterdata.bin" "${WXL_WATER_MODULE_DIR}/wxl_water/waterdata.bin"
    COMMENT "Deploy wxl_water settings and data -> ${WXL_WATER_MODULE_DIR}/wxl_water")
