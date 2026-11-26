cmake_minimum_required(VERSION 3.25)
project(FearEffectRe CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

# The game's code is recompiled from your own disc at build time. Nothing
# derived from the game is stored in this repository.
set(FE_DISC_IMAGE "" CACHE FILEPATH "Fear Effect (USA) disc image (.cue/.bin) used to generate the recompiled code")
set(FE_RECOMP_SHARDS 16 CACHE STRING "Number of generated translation units")

add_library(fe_core STATIC
    src/core/disc/DiscImage.cpp
    src/core/psx/Machine.cpp
    src/core/psx/Bios.cpp
    src/core/psx/Cdrom.cpp
    src/core/psx/Gpu.cpp
    src/core/psx/Gte.cpp
    src/core/psx/Mdec.cpp
    src/core/psx/Spu.cpp
    src/core/psx/Hardware.cpp
)
target_include_directories(fe_core PUBLIC src/core)
if(MSVC)
    target_compile_options(fe_core PRIVATE /W4)
else()
    target_compile_options(fe_core PRIVATE -Wall -Wextra)
endif()

if(FE_DISC_IMAGE)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(FE_GEN_DIR ${CMAKE_BINARY_DIR}/gen)
    set(FE_GEN_SOURCES ${FE_GEN_DIR}/fe_exe_table.cpp)
    math(EXPR last_shard "${FE_RECOMP_SHARDS} - 1")
    foreach(i RANGE ${last_shard})
        if(i LESS 10)
            set(i "0${i}")
        endif()
        list(APPEND FE_GEN_SOURCES ${FE_GEN_DIR}/fe_exe_${i}.cpp)
    endforeach()
    file(GLOB FE_RECOMP_SCRIPTS CONFIGURE_DEPENDS tools/recomp/*.py tools/psxdisc.py)
    add_custom_command(
        OUTPUT ${FE_GEN_DIR}/recomp.stamp
        BYPRODUCTS ${FE_GEN_SOURCES} ${FE_GEN_DIR}/fe_exe_decl.h
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/recomp/recomp.py
                --disc "${FE_DISC_IMAGE}" --out ${FE_GEN_DIR} --shards ${FE_RECOMP_SHARDS}
        COMMAND ${CMAKE_COMMAND} -E touch ${FE_GEN_DIR}/recomp.stamp
        DEPENDS ${FE_RECOMP_SCRIPTS} "${FE_DISC_IMAGE}"
        COMMENT "Recompiling the game executable to C++"
        VERBATIM
    )
    add_custom_target(fe_recomp_gen DEPENDS ${FE_GEN_DIR}/recomp.stamp)
    add_library(fe_recomp STATIC ${FE_GEN_SOURCES})
    add_dependencies(fe_recomp fe_recomp_gen)
    target_link_libraries(fe_recomp PUBLIC fe_core)
    target_include_directories(fe_recomp PRIVATE ${FE_GEN_DIR})
    if(MSVC)
        target_compile_options(fe_recomp PRIVATE /bigobj /w)
    else()
        target_compile_options(fe_recomp PRIVATE -Wno-unused-label -Wno-unused-variable)
    endif()
    set(FE_RECOMP_LIB fe_recomp)
    set(FE_HAVE_RECOMP 1)
else()
    message(WARNING "FE_DISC_IMAGE is not set: building the interpreter only.")
    set(FE_RECOMP_LIB "")
    set(FE_HAVE_RECOMP 0)
endif()

add_executable(FearEffectRe src/app/main.cpp src/app/ModernControls.cpp)
if(WIN32)
    target_sources(FearEffectRe PRIVATE src/app/Window.cpp src/app/Launcher.cpp src/app/VideoAI.cpp src/app/StrVideo.cpp)
    target_include_directories(FearEffectRe PRIVATE third_party/onnxruntime/include)
    target_link_libraries(FearEffectRe PRIVATE user32 gdi32 winmm comdlg32 opengl32 ole32 windowscodecs)
    # AI upscaling: a folder holding onnxruntime.dll, DirectML.dll, fast_x4.onnx and
    # best_x4.onnx (see tools/ai/README.md) is copied next to the program as `ai`.
    set(FE_AI_DIR "" CACHE PATH "Folder with the ONNX Runtime/DirectML DLLs and the upscaling models")
    if(FE_AI_DIR)
        add_custom_command(TARGET FearEffectRe POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${FE_AI_DIR}" "$<TARGET_FILE_DIR:FearEffectRe>/ai")
    endif()
    # driver for the SeedVR2 cache builder (SeedVR2 itself goes in ai/seedvr2, see tools/ai/README.md)
    add_custom_command(TARGET FearEffectRe POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:FearEffectRe>/ai"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/tools/ai/seedvr2_chunk.py"
                "$<TARGET_FILE_DIR:FearEffectRe>/ai/seedvr2_chunk.py")
    target_compile_definitions(FearEffectRe PRIVATE FE_HAVE_WINDOW=1)
endif()
target_link_libraries(FearEffectRe PRIVATE fe_core ${FE_RECOMP_LIB})
target_compile_definitions(FearEffectRe PRIVATE FE_HAVE_RECOMP=${FE_HAVE_RECOMP})

if(FE_HAVE_RECOMP)
    add_executable(fe_difftest tests/difftest.cpp)
    target_link_libraries(fe_difftest PRIVATE fe_core fe_recomp)
    enable_testing()
    add_test(NAME recomp_matches_interpreter COMMAND fe_difftest "${FE_DISC_IMAGE}")
endif()

if(MINGW)
    # so the program starts by double-click, without the compiler on PATH
    get_filename_component(FE_COMPILER_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    if(EXISTS "${FE_COMPILER_DIR}/libwinpthread-1.dll")
        add_custom_command(TARGET FearEffectRe POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
                           "${FE_COMPILER_DIR}/libwinpthread-1.dll" $<TARGET_FILE_DIR:FearEffectRe>)
    endif()
    foreach(t FearEffectRe fe_difftest)
        if(TARGET ${t})
            target_link_options(${t} PRIVATE -static-libgcc -static-libstdc++)
        endif()
    endforeach()
endif()
