# Rewrites the generated GitSHA1.cpp with the current HEAD, on every build.
#
# This exists because configure_file() only runs when CMake reconfigures, and CMake
# only reconfigures when a CMakeLists.txt changes.  Committing to the working branch
# does not reconfigure anything, so the SHA baked into the dol went stale and the
# console banner confidently reported a revision that was several commits behind
# what was actually running.  That is worse than having no SHA at all: an absent
# revision is obviously absent, whereas a stale one is trusted.
#
# The neighbouring write-build-stamp.cmake already solves exactly this problem for
# the build timestamp, using an always-fires custom target plus OBJECT_DEPENDS.  The
# SHA now uses the same mechanism, so both halves of the banner are per-build facts.
#
# Rewrites only when the content actually changes, so a rebuild at the same revision
# does not recompile the object for nothing -- which is the whole point of the
# timestamp script guarding its write the same way.
if(NOT DEFINED SHA_CPP)
	message(FATAL_ERROR "write-git-sha: pass -DSHA_CPP=<path>")
endif()
if(NOT DEFINED SRC_DIR)
	set(SRC_DIR ".")
endif()

find_package(Git QUIET)
set(_sha "unknown")
if(GIT_FOUND)
	execute_process(
		COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
		WORKING_DIRECTORY "${SRC_DIR}"
		OUTPUT_VARIABLE _sha_out
		OUTPUT_STRIP_TRAILING_WHITESPACE
		ERROR_QUIET)
	if(_sha_out)
		string(SUBSTRING "${_sha_out}" 0 40 _sha)
	endif()
	# Recorded alongside the SHA.  A build from a dirty tree is a build that does not
	# correspond to any revision at all, and reporting only the SHA would claim
	# otherwise -- the same class of lie as a stale one, and much harder to spot,
	# because the SHA would look right.
	execute_process(
		COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
		WORKING_DIRECTORY "${SRC_DIR}"
		OUTPUT_VARIABLE _dirty
		ERROR_QUIET)
	string(STRIP "${_dirty}" _dirty)
	if(_dirty)
		set(_sha "${_sha}-dirty")
	endif()
endif()

set(_content [==[
// Generated per build by scripts/write-git-sha.cmake.  Do not edit.
const char* g_GIT_SHA1 = "@SHA@";
]==])
string(REPLACE "@SHA@" "${_sha}" _content "${_content}")

set(_prev "")
if(EXISTS "${SHA_CPP}")
	file(READ "${SHA_CPP}" _prev)
endif()
# Compared whole, exactly as write-build-stamp.cmake does.  Bracket syntax above
# and a quoted expansion below are both load-bearing: a semicolon inside a CMake
# quoted argument is a list separator, so the naive spelling of this line silently
# compiled to a declaration with no terminating semicolon and no diagnostic pointing
# at the generator.
if(NOT _prev STREQUAL _content)
	file(WRITE "${SHA_CPP}" "${_content}")
	message(STATUS "git sha for the banner: ${_sha}")
endif()