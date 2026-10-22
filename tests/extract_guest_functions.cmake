# Copies generated guest functions out of the diagnostic's sources, for a
# test that runs them on their own (kinect_depth_view_guest_test):
#
#   cmake -DDIR=<out/recomp/diagnostic> -DOUT=<file.inc> -DFUNCTIONS=82439998,82439D80 -P extract_guest_functions.cmake
#
# Each function is its PPC_FUNC_IMPL(__imp__sub_<address>) body, from that
# line to the first line that is only "}". The generated code stays in out/
# (it is the game's), so this runs at build time and writes into the build
# directory only.
if(NOT DIR OR NOT OUT OR NOT FUNCTIONS)
    message(FATAL_ERROR "usage: cmake -DDIR=... -DOUT=... -DFUNCTIONS=a,b -P extract_guest_functions.cmake")
endif()
string(REPLACE "," ";" wanted "${FUNCTIONS}")
file(GLOB sources "${DIR}/ppc_recomp.*.cpp")
set(result "// Made by tests/extract_guest_functions.cmake from ${DIR}: generated code, not committed.\n")
foreach(function IN LISTS wanted)
    set(found FALSE)
    foreach(source IN LISTS sources)
        file(READ "${source}" text)
        string(FIND "${text}" "PPC_FUNC_IMPL(__imp__sub_${function}) {" start)
        if(start GREATER -1)
            string(SUBSTRING "${text}" ${start} -1 rest)
            string(FIND "${rest}" "\n}\n" end)
            if(end LESS 0)
                message(FATAL_ERROR "sub_${function} in ${source} has no end")
            endif()
            math(EXPR end "${end} + 3")
            string(SUBSTRING "${rest}" 0 ${end} body)
            string(APPEND result "\n// ${source}\n${body}")
            set(found TRUE)
            break()
        endif()
    endforeach()
    if(NOT found)
        message(FATAL_ERROR "sub_${function} is not in ${DIR}")
    endif()
endforeach()
file(WRITE "${OUT}" "${result}")
