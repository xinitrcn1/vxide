# ucas (https://github.com/scarletnx/ucas), a computer algebra system for
# programmers, built into the vxide binary.
#
#   - fetched by CPM at a pinned commit; its BSD Makefile is read for the
#     source list, so a new pin brings its new files along
#   - its generated sources (nlp_words.h, thy_absalg.inc) are made here
#   - every object is linked into one relocatable object (ld -r) whose
#     symbols are all made local except ucas_main: ucas has its own
#     json_parse, xmalloc, lex, fail ... and none of them can meet vxide's
#   - the vxide binary is a multi-call binary: run as "ucas" (argv[0]'s base
#     name, or `vxide --ucas ...`) it is the whole ucas command line, which
#     is also how the ucas tab runs it (src/ucasview.c)
#
# Gives the static library `ucas` (link it, and call ucas_main) and
# UCAS_SOURCE_DIR.

set(UCAS_GIT_TAG c69384774dbad31f557f45e97207c93ee73c1de1 CACHE STRING "ucas commit built into vxide")

CPMAddPackage(
	NAME ucas
	GIT_REPOSITORY https://github.com/scarletnx/ucas.git
	GIT_TAG ${UCAS_GIT_TAG}
	DOWNLOAD_ONLY YES
	# the patches, in order (applied once, by ucas_patch.cmake): the library
	# entry and Cancel; then adamfit (shapes fitted by Adam, ranked by cost
	# and proved error; C and GLSL code sampled natively, integers as
	# fixed-point forms): ucas's commits e2becc3 and dff78cb on its branch
	# adamfit, as one diff
	PATCH_COMMAND ${CMAKE_COMMAND}
		"-DPATCHES=${CMAKE_CURRENT_LIST_DIR}/ucas-library.patch|${CMAKE_CURRENT_LIST_DIR}/ucas-adamfit.patch"
		-P ${CMAKE_CURRENT_LIST_DIR}/ucas_patch.cmake)
set(UCAS_SOURCE_DIR ${ucas_SOURCE_DIR})
set(us ${ucas_SOURCE_DIR}/src)

# ---- the sources: SRCS= / SRCS+= lines of the Makefile ------------------------------
file(READ ${ucas_SOURCE_DIR}/Makefile mk)
string(REGEX REPLACE "\\\\\n" " " mk "${mk}") # continuation lines
string(REGEX MATCHALL "(^|\n)SRCS\\+?=[^\n]*" lines "${mk}")
set(UCAS_SRC)
foreach(l IN LISTS lines)
	string(REGEX REPLACE "^\n?SRCS\\+?=" "" l "${l}")
	string(REGEX REPLACE "[ \t]+" ";" l "${l}")
	foreach(f IN LISTS l)
		if(f AND EXISTS ${us}/${f})
			list(APPEND UCAS_SRC ${us}/${f})
		endif()
	endforeach()
endforeach()
list(REMOVE_DUPLICATES UCAS_SRC)
list(LENGTH UCAS_SRC n)
if(n LESS 100)
	message(FATAL_ERROR "ucas: only ${n} sources found in its Makefile")
endif()
# ScarletNX's GLSL ES compiler, a library of its own there (its glslc.c is
# not ucas's glslc.c; the object names differ by folder here)
foreach(f lexer parser emit builtins glslc)
	list(APPEND UCAS_SRC ${us}/snxglslc/${f}.c)
endforeach()

# ---- generated sources ----------------------------------------------------------------
set(gen ${CMAKE_CURRENT_BINARY_DIR}/ucas_gen)
file(MAKE_DIRECTORY ${gen})
find_program(AWK NAMES awk gawk mawk nawk REQUIRED)
set(nlp_in ${us}/nlp.c ${us}/nlp_calc2.inc ${us}/nlp_hyper.inc ${us}/nlp_alg.inc ${us}/nlp_xform.inc
	${us}/nlp_img.inc)
add_custom_command(OUTPUT ${gen}/nlp_words.h
	COMMAND ${AWK} -f ${us}/nlp_words.awk ${nlp_in} > ${gen}/nlp_words.h
	DEPENDS ${nlp_in} ${us}/nlp_words.awk
	COMMENT "ucas: nlp_words.h")
add_custom_command(OUTPUT ${gen}/thy_absalg.inc
	COMMAND ${CMAKE_COMMAND} -DIN=${us}/thy_absalg.thy -DOUT=${gen}/thy_absalg.inc
		-P ${CMAKE_CURRENT_LIST_DIR}/ucas_cstring.cmake
	DEPENDS ${us}/thy_absalg.thy ${CMAKE_CURRENT_LIST_DIR}/ucas_cstring.cmake
	COMMENT "ucas: thy_absalg.inc")
set_source_files_properties(${us}/nlp.c PROPERTIES OBJECT_DEPENDS ${gen}/nlp_words.h)
set_source_files_properties(${us}/kscript.c PROPERTIES OBJECT_DEPENDS ${gen}/thy_absalg.inc)

# ---- the objects ----------------------------------------------------------------------
# `find F snippets` reads the snippet databases ucas grows from
# snippets/grow.ucas (target ucas_snippets below).
set(UCAS_SNIPDIR ${CMAKE_CURRENT_BINARY_DIR}/ucas_snippets)
file(MAKE_DIRECTORY ${UCAS_SNIPDIR})
add_library(ucas_objs OBJECT ${UCAS_SRC})
set_target_properties(ucas_objs PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS ON
	POSITION_INDEPENDENT_CODE ON)
target_include_directories(ucas_objs PRIVATE ${us} ${gen})
target_compile_definitions(ucas_objs PRIVATE UCAS_LIBRARY UCAS_SNIPDIR="${UCAS_SNIPDIR}")
if(EXISTS ${us}/cfx86.c)
	target_compile_definitions(ucas_objs PRIVATE CF_HAVE_X86)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	target_compile_definitions(ucas_objs PRIVATE _GNU_SOURCE) # as its libc-test builds it
endif()
# Third-party code: its warnings are its own (its Makefile builds with -Wall
# -Wextra and a few off; vxide's -pedantic is not for it).
target_compile_options(ucas_objs PRIVATE -w)
# Not instrumented under VXIDE_SANITIZE: the sanitizers' module constructors
# and globals do not survive the one-object link below (their symbols are
# made local), and ucas is not the code under test there.
if(VXIDE_SANITIZE)
	target_compile_options(ucas_objs PRIVATE -fno-sanitize=all)
endif()

# ---- one object, ucas_main its only global ---------------------------------------------
if(NOT CMAKE_OBJCOPY)
	find_program(CMAKE_OBJCOPY NAMES objcopy llvm-objcopy REQUIRED)
endif()
find_program(UCAS_LD NAMES ld ld.lld REQUIRED)
set(ucas_o ${CMAKE_CURRENT_BINARY_DIR}/ucas_lib.o)
add_custom_command(OUTPUT ${ucas_o}
	COMMAND ${UCAS_LD} -r -o ${ucas_o}.all $<TARGET_OBJECTS:ucas_objs>
	COMMAND ${CMAKE_OBJCOPY} --keep-global-symbol=ucas_main ${ucas_o}.all ${ucas_o}
	COMMAND ${CMAKE_COMMAND} -E rm -f ${ucas_o}.all
	DEPENDS ucas_objs $<TARGET_OBJECTS:ucas_objs>
	COMMAND_EXPAND_LISTS
	COMMENT "ucas: one object, symbols made local but ucas_main")
set_source_files_properties(${ucas_o} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
add_library(ucas STATIC ${ucas_o})
set_target_properties(ucas PROPERTIES LINKER_LANGUAGE C)
target_link_libraries(ucas INTERFACE m Threads::Threads)
