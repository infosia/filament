cmake_minimum_required(VERSION 3.19)
project(toony C ASM)

if (NOT FILAMENT_ENABLE_TOONY)
    return()
endif()

set(ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(GENERATION_ROOT ${CMAKE_CURRENT_BINARY_DIR})
set(RESOURCE_DIR  "${GENERATION_ROOT}/generated/resources")
set(MATERIAL_DIR  "${GENERATION_ROOT}/generated/material")
set(TEXTURE_DIR "${GENERATION_ROOT}/generated/texture")
set(RESOURCE_BINS)

# ==================================================================================================
# Build materials
# ==================================================================================================

set(MATERIAL_SRCS
        materials/aiDefaultMat.mat
        materials/bakedColor.mat
        materials/bakedTexture.mat
        materials/pointSprites.mat
        materials/aoPreview.mat
        materials/groundShadow.mat
        materials/heightfield.mat
        materials/mirror.mat
        materials/sandboxCloth.mat
        materials/sandboxLit.mat
        materials/sandboxLitFade.mat
        materials/sandboxLitTransparent.mat
        materials/sandboxLitThinRefraction.mat
        materials/sandboxLitThinRefractionSsr.mat
        materials/sandboxLitSolidRefraction.mat
        materials/sandboxLitSolidRefractionSsr.mat
        materials/sandboxSpecGloss.mat
        materials/sandboxSubsurface.mat
        materials/sandboxUnlit.mat
        materials/texturedLit.mat)

if (CMAKE_CROSSCOMPILING)
    include(${IMPORT_EXECUTABLES})
endif()

file(MAKE_DIRECTORY ${MATERIAL_DIR})

foreach (mat_src ${MATERIAL_SRCS})
    get_filename_component(localname "${mat_src}" NAME_WE)
    get_filename_component(fullname "${mat_src}" ABSOLUTE)
    set(output_path "${MATERIAL_DIR}/${localname}.filamat")
    add_custom_command(
            OUTPUT ${output_path}
            COMMAND matc ${MATC_BASE_FLAGS} -o ${output_path} ${fullname}
            MAIN_DEPENDENCY ${mat_src}
            DEPENDS matc
            COMMENT "Compiling material ${mat_src} to ${output_path}"
    )
    list(APPEND RESOURCE_BINS ${output_path})
endforeach()

# ==================================================================================================
# Build sample resources
# ==================================================================================================

file(MAKE_DIRECTORY ${RESOURCE_DIR})

get_resgen_vars(${RESOURCE_DIR} resources)

add_custom_command(
        OUTPUT ${RESGEN_OUTPUTS}
        COMMAND resgen ${RESGEN_FLAGS} ${RESOURCE_BINS}
        DEPENDS resgen ${RESOURCE_BINS}
        COMMENT "Aggregating resources"
)

if (DEFINED RESGEN_SOURCE_FLAGS)
    set_source_files_properties(${RESGEN_SOURCE} PROPERTIES COMPILE_FLAGS ${RESGEN_SOURCE_FLAGS})
endif()

# CMake fails to invoke ar on Windows unless there is at least one C/C++ file in the library.
set(DUMMY_SRC "${RESOURCE_DIR}/dummy.c")
add_custom_command(OUTPUT ${DUMMY_SRC} COMMAND echo "//" > ${DUMMY_SRC})

add_library(sample-resources ${DUMMY_SRC} ${RESGEN_SOURCE})

# ==================================================================================================
# Invoke cmgen to build KTX files for the default IBL and skybox
# ==================================================================================================

set(CMGEN_ARGS --quiet --format=ktx --size=256 --extract-blur=0.1)

function(add_envmap SOURCE TARGET)
    set(source_envmap "${ROOT_DIR}/${SOURCE}")

    set(target_skybox   "${PROJECT_BINARY_DIR}/${TARGET}/${TARGET}_skybox.ktx")
    set(target_envmap   "${PROJECT_BINARY_DIR}/${TARGET}/${TARGET}_ibl.ktx")

    set(target_envmaps ${target_envmaps} ${target_skybox} PARENT_SCOPE)
    set(target_envmaps ${target_envmaps} ${target_envmap} PARENT_SCOPE)

    add_custom_command(OUTPUT ${target_skybox} ${target_envmap}
        COMMAND cmgen -x ${TARGET} ${CMGEN_ARGS} ${source_envmap}
        MAIN_DEPENDENCY ${source_envmap}
        DEPENDS cmgen
        COMMENT "Generating environment map ${target_envmap}")
endfunction()

add_envmap("third_party/environments/lightroom_14b.hdr" "default_env")

# ==================================================================================================
# Resources for toony_viewer
# ==================================================================================================

add_library(toony-resources ${DUMMY_SRC})

set(TOONY_VIEWER_RESOURCE_FILES
    ${ROOT_DIR}/third_party/models/DamagedHelmet/DamagedHelmet.glb
    ${MATERIAL_DIR}/groundShadow.filamat
)

get_resgen_vars(${RESOURCE_DIR} toony_viewer)

add_custom_command(
        OUTPUT ${RESGEN_OUTPUTS}
        COMMAND resgen ${RESGEN_FLAGS} ${TOONY_VIEWER_RESOURCE_FILES}
        DEPENDS resgen ${TOONY_VIEWER_RESOURCE_FILES}
)

if (DEFINED RESGEN_SOURCE_FLAGS)
    set_source_files_properties(${RESGEN_SOURCE} PROPERTIES COMPILE_FLAGS ${RESGEN_SOURCE_FLAGS})
endif()

target_sources(toony-resources PRIVATE ${RESGEN_SOURCE})

# ==================================================================================================
# Compiler flags
# ==================================================================================================

if (MSVC)
    set(COMPILER_FLAGS $<$<CONFIG:Release>:/fp:fast>)
else()
    set(COMPILER_FLAGS
            -Wno-extern-c-compat
            $<$<NOT:$<PLATFORM_ID:Linux>>:-Wno-address-of-packed-member>
            $<$<CONFIG:Release>:-ffast-math>)
endif()

# ==================================================================================================
# Test apps
# ==================================================================================================

if (NOT ANDROID)
    include_directories(${GENERATION_ROOT} include/)
    add_executable(toony_viewer src/Settings.cpp src/SimpleViewer.cpp src/toony_viewer.cpp)
    target_link_libraries(toony_viewer PRIVATE gltfio filamentapp imgui filament gltfio_core filagui jsmn toony-resources)
    target_compile_options(toony_viewer PRIVATE ${COMPILER_FLAGS})
endif()

# ==================================================================================================
# Copy the MoltenVK dylibs and JSON on MacOS
# ==================================================================================================

if (APPLE AND NOT Vulkan_LIBRARY AND NOT FILAMENT_USE_SWIFTSHADER)
    message(STATUS "No Vulkan SDK was found, using prebuilt MoltenVK.")
    set(MOLTENVK_DIR "../third_party/moltenvk")
    configure_file(
            ${MOLTENVK_DIR}/libvulkan.1.dylib
            ${PROJECT_BINARY_DIR}/libvulkan.1.dylib COPYONLY)
    configure_file(
            ${MOLTENVK_DIR}/MoltenVK_icd.json
            ${PROJECT_BINARY_DIR}/MoltenVK_icd.json COPYONLY)
    configure_file(
            ${MOLTENVK_DIR}/libMoltenVK.dylib
            ${PROJECT_BINARY_DIR}/libMoltenVK.dylib COPYONLY)
    list(APPEND APP_SRCS
            ${PROJECT_BINARY_DIR}/libvulkan.1.dylib
            ${PROJECT_BINARY_DIR}/MoltenVK_icd.json
            ${PROJECT_BINARY_DIR}/libMoltenVK.dylib)
endif()

# ==================================================================================================
# Copy Assets
#
# This creates a structure like:
#
#    /lightbulb (etc)  sample app executable
#    /assets/fonts     copied from REPO/assets
#    /assets/models    copied from REPO/assets
#    /textures         copied from REPO/third_party
#
# ==================================================================================================

file(COPY ../third_party/textures DESTINATION ${PROJECT_BINARY_DIR})
add_custom_target(textures ALL DEPENDS textures)
add_dependencies(filament textures)

file(COPY ../assets DESTINATION ${PROJECT_BINARY_DIR}
        PATTERN "reference" EXCLUDE
        PATTERN "environments" EXCLUDE)
add_custom_target(assets ALL DEPENDS assets)
add_dependencies(filament assets)

add_custom_target(envs DEPENDS ${target_envmaps})
add_dependencies(filament envs)
