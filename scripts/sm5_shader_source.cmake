# A copy of a backend shader for FXC (shader model 5.0, Direct3D 11, Issue
# #359): the same source without register spaces, which 5.0 does not have.
#   cmake -DSOURCE=<file.hlsl> -DOUTPUT=<copy.hlsl> -P sm5_shader_source.cmake
file(READ "${SOURCE}" text)
string(REGEX REPLACE ",[ \t]*space[0-9]+[ \t]*\\)" ")" text "${text}")
file(WRITE "${OUTPUT}" "${text}")
