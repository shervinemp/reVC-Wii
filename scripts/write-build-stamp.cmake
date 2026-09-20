# Rewrites wii_build_stamp.h with the current wall time on every invocation.
# Because the file's content timestamp changes every build, anything including
# it (wii_game.cpp's boot banner) recompiles every build, which is the only way
# the on-screen "built ..." banner can be trusted on real hardware.
string(TIMESTAMP WII_STAMP "%b %d %Y %H:%M:%S UTC")
if(NOT DEFINED STAMP_H)
	message(FATAL_ERROR "write-build-stamp: pass -DSTAMP_H=<path>")
endif()
if(EXISTS "${STAMP_H}")
	file(READ "${STAMP_H}" _prev)
else()
	set(_prev "")
endif()
set(_content
	"#ifndef WII_BUILD_STAMP_H\n"
	"#define WII_BUILD_STAMP_H\n"
	"// Generated per build by scripts/write-build-stamp.cmake.\n"
	"#define WII_BUILD_STAMP \"${WII_STAMP}\"\n"
	"#endif\n")
string(REPLACE ";" "" _content "${_content}")
string(REPLACE "\n" "" _prev_flat "${_prev}")
if(NOT _prev STREQUAL _content)
	file(WRITE "${STAMP_H}" ${_content})
endif()
