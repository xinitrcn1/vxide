# vxide - CMake build.
#   cmake -B build && cmake --build build -j
#   ctest --test-dir build --output-on-failure
cmake_minimum_required(VERSION 3.20)
project(vxide LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()
set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O2 -g")
add_compile_options(-pedantic -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)

option(VXIDE_SANITIZE "Build with AddressSanitizer and UBSan" OFF)
option(VXIDE_HEAPTRACE "Tag heap allocations with their call site (script command: heap)" OFF)
if(VXIDE_HEAPTRACE)
	add_compile_definitions(VXIDE_HEAPTRACE)
endif()
if(VXIDE_SANITIZE)
	add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
	add_link_options(-fsanitize=address,undefined)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
pkg_check_modules(LIBGIT2 REQUIRED IMPORTED_TARGET libgit2)
pkg_check_modules(VULKAN REQUIRED IMPORTED_TARGET vulkan)
pkg_check_modules(CAPSTONE REQUIRED IMPORTED_TARGET capstone)
pkg_check_modules(PNG REQUIRED IMPORTED_TARGET libpng16)
pkg_check_modules(JPEG REQUIRED IMPORTED_TARGET libjpeg)
find_package(Threads REQUIRED)
find_program(GLSLANG glslangValidator REQUIRED)

# Shaders become C arrays at build time.
set(SHADER_HDRS)
foreach(stage vert frag)
	set(out ${CMAKE_CURRENT_BINARY_DIR}/ui_${stage}.h)
	add_custom_command(OUTPUT ${out}
		COMMAND ${GLSLANG} -V --quiet --vn ui_${stage}_spv -o ${out} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/ui.${stage}
		DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/ui.${stage}
		COMMENT "glslang ui.${stage}")
	list(APPEND SHADER_HDRS ${out})
endforeach()
add_custom_target(shaders DEPENDS ${SHADER_HDRS})

# Core modules: no window or GPU, unit-tested on their own.
set(CORE base heaptrace text syntax theme fstree fswatch json lsp git gitops gitgraph githunk lgblame blame regex search prof
	png binfile)
# Front end: window, Vulkan, fonts, UI.
set(APP main gpu font icons draw ui editor panes menu actions gitui bgwork diffview binview vim projtools man dialog session
	imgload profview mdview)

list(TRANSFORM CORE PREPEND src/ OUTPUT_VARIABLE CORE_SRC)
list(TRANSFORM CORE_SRC APPEND .c)
list(TRANSFORM APP PREPEND src/ OUTPUT_VARIABLE APP_SRC)
list(TRANSFORM APP_SRC APPEND .c)

add_library(vxcore STATIC ${CORE_SRC})
target_include_directories(vxcore PUBLIC src)
target_link_libraries(vxcore PUBLIC PkgConfig::LIBGIT2 PkgConfig::CAPSTONE Threads::Threads m)

add_executable(vxide ${APP_SRC})
add_dependencies(vxide shaders)
target_include_directories(vxide PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(vxide PRIVATE vxcore PkgConfig::SDL3 PkgConfig::FREETYPE PkgConfig::VULKAN PkgConfig::PNG
	PkgConfig::JPEG)

# ---- tests (ctest) --------------------------------------------------------------
enable_testing()
set(TESTS t_text t_syntax t_json t_lsp t_git t_gitops t_gitgraph t_githunk t_blame t_fstree t_regex t_search
	t_binfile t_prof)
foreach(t ${TESTS})
	add_executable(${t} tests/${t}.c)
	target_include_directories(${t} PRIVATE tests)
	target_link_libraries(${t} PRIVATE vxcore)
	add_test(NAME ${t} COMMAND ${t} WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()
# The headless UI scripts need the app and a Vulkan device (GPU or llvmpipe).
add_test(NAME t_ui COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/t_ui.sh)
set_tests_properties(t_ui PROPERTIES ENVIRONMENT "VXIDE=$<TARGET_FILE:vxide>" TIMEOUT 600)
