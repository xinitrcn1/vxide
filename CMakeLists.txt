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

# ucas, the computer algebra system behind the ucas tab, fetched by CPM and
# built into the vxide binary (cmake/ucas.cmake). CPM_SOURCE_CACHE keeps one
# download for every build folder.
option(VXIDE_UCAS "Build ucas into vxide (the ucas tab)" ON)
if(VXIDE_UCAS)
	include(cmake/CPM.cmake)
	include(cmake/ucas.cmake)
endif()

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
	png binfile dap cmake ucasproto)
# Front end: window, Vulkan, fonts, UI.
set(APP main gpu font icons draw ui editor panes menu actions gitui bgwork diffview binview vim projtools man dialog session
	imgload profview mdview pmcview workflow ucasview ucasimg)

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
if(VXIDE_UCAS)
	target_link_libraries(vxide PRIVATE ucas)
	target_compile_definitions(vxide PRIVATE VXIDE_UCAS)
	# stb_image from ucas's tree decodes its GIFs for the tab (src/ucasimg.c)
	target_include_directories(vxide PRIVATE ${UCAS_SOURCE_DIR}/src/third_party)
	set_source_files_properties(src/ucasimg.c PROPERTIES COMPILE_OPTIONS "-w") # stb_image's warnings
	# the snippet databases `find F snippets` reads, grown by ucas itself
	# (its Makefile does the same at every build)
	add_custom_command(OUTPUT ${UCAS_SNIPDIR}/.grown
		COMMAND ${CMAKE_COMMAND} -E make_directory ${UCAS_SNIPDIR}
		COMMAND ${CMAKE_COMMAND} -E copy_if_different ${UCAS_SOURCE_DIR}/snippets/grow.ucas ${UCAS_SNIPDIR}/grow.ucas
		COMMAND $<TARGET_FILE:vxide> --ucas -q grow.ucas > ${UCAS_SNIPDIR}/grow.log
		COMMAND ${CMAKE_COMMAND} -E touch ${UCAS_SNIPDIR}/.grown
		WORKING_DIRECTORY ${UCAS_SNIPDIR}
		DEPENDS vxide ${UCAS_SOURCE_DIR}/snippets/grow.ucas
		COMMENT "ucas: growing the snippet databases")
	add_custom_target(ucas_snippets ALL DEPENDS ${UCAS_SNIPDIR}/.grown)
endif()

# ---- tests (ctest) --------------------------------------------------------------
enable_testing()
set(TESTS t_text t_syntax t_json t_lsp t_git t_gitops t_gitgraph t_githunk t_blame t_fstree t_regex t_search
	t_binfile t_prof t_dap t_cmake t_ucasproto)
foreach(t ${TESTS})
	add_executable(${t} tests/${t}.c)
	target_include_directories(${t} PRIVATE tests)
	target_link_libraries(${t} PRIVATE vxcore)
	add_test(NAME ${t} COMMAND ${t} WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()
# The headless UI scripts need the app and a Vulkan device (GPU or llvmpipe).
add_test(NAME t_ui COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/t_ui.sh)
set_tests_properties(t_ui PROPERTIES ENVIRONMENT "VXIDE=$<TARGET_FILE:vxide>" TIMEOUT 600)
# ucas inside vxide: the multi-call entry, the JSON frontend the tab talks to, Cancel
find_package(Python3 COMPONENTS Interpreter)
if(VXIDE_UCAS AND Python3_FOUND)
	add_test(NAME t_ucas COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/t_ucas.py)
	set_tests_properties(t_ucas PROPERTIES ENVIRONMENT "VXIDE=$<TARGET_FILE:vxide>" TIMEOUT 120)
endif()
