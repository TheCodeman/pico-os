# Writes version_build.h: the git commit and today's date (UTC), for
# src/version.c. Run at every build; the file is only rewritten when it
# changes, so a build with nothing new doesn't recompile version.c.
#   cmake -DSRC=<source folder> -DOUT=<header> -P version_build.cmake
execute_process(COMMAND git -C "${SRC}" rev-parse --short=7 HEAD
                OUTPUT_VARIABLE git OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE r)
if(NOT r EQUAL 0 OR git STREQUAL "")
    set(git "unknown")
else()
    # "+" when the tree has changes git hasn't got (not counting new files).
    execute_process(COMMAND git -C "${SRC}" diff --quiet HEAD -- RESULT_VARIABLE dirty ERROR_QUIET)
    if(NOT dirty EQUAL 0)
        set(git "${git}+")
    endif()
endif()
string(TIMESTAMP date "%Y-%m-%d" UTC)
set(text "// Written by tools/version_build.cmake: don't edit.\n#define VERSION_GIT \"${git}\"\n#define VERSION_DATE \"${date}\"\n")
if(EXISTS "${OUT}")
    file(READ "${OUT}" old)
endif()
if(NOT old STREQUAL text)
    file(WRITE "${OUT}" "${text}")
endif()
