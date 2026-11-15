# cmake -DPATCHES="a.patch|b.patch" -P ucas_patch.cmake, in ucas's checkout
#
# The patches on the pinned commit, in order, once: a stamp of their hashes
# says the tree has exactly them, and a re-run with the same patches does
# nothing.  With other patches (or none applied yet) the checkout goes back
# to the commit first, so a patch is never applied twice (BSD patch takes
# that for a reversed patch and undoes it) and a removed one does not stay.
string(REPLACE "|" ";" PATCHES "${PATCHES}")
set(stamp "")
foreach(p IN LISTS PATCHES)
	file(SHA256 "${p}" h)
	string(APPEND stamp "${h}\n")
endforeach()
set(mark .vxide-patches)
if(EXISTS ${mark})
	file(READ ${mark} old)
	if(old STREQUAL stamp)
		return()
	endif()
endif()
find_package(Git REQUIRED)
execute_process(COMMAND ${GIT_EXECUTABLE} checkout -q -- . COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND ${GIT_EXECUTABLE} clean -fdq COMMAND_ERROR_IS_FATAL ANY)
foreach(p IN LISTS PATCHES)
	execute_process(COMMAND ${GIT_EXECUTABLE} apply --whitespace=nowarn "${p}" RESULT_VARIABLE rc)
	if(NOT rc EQUAL 0)
		message(FATAL_ERROR "ucas: ${p} does not apply to the pinned commit")
	endif()
	message(STATUS "ucas: applied ${p}")
endforeach()
file(WRITE ${mark} "${stamp}")
