set(rr "${OUTPUT}.rr")
set(rrc "${OUTPUT}.rrc")

if(MSVC)
    set(preprocess /nologo /EP /TC)
    set(include_headers "/I${HEADERS}")
    set(resource_define /DMSWindows)
else()
    set(preprocess -E -P -x c)
    set(include_headers "-I${HEADERS}")
    set(resource_define -DMSWindows)
endif()

execute_process(COMMAND "${COMPILER}" ${preprocess} "${include_headers}" "${SOURCE}"
    OUTPUT_FILE "${rr}" COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${PIPL_TOOL}" "${rr}" "${rrc}"
    COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${COMPILER}" ${preprocess} "${resource_define}" "${rrc}"
    OUTPUT_FILE "${OUTPUT}.tmp" COMMAND_ERROR_IS_FATAL ANY)
file(READ "${OUTPUT}.tmp" resource_text)
# SDK 27's PiPLTool omits the comma after some DWORD property lengths.
string(REGEX REPLACE "([0-9]+L)[ \t]*\r?\n([ \t]+[0-9]+L,)"
    "\\1,\n\\2" resource_text "${resource_text}")
if(NOT MSVC)
    # windres reads the B in Bevel as part of the preceding hex escape.
    string(REPLACE "\\x0BBevel Alpha" "\\013Bevel Alpha" resource_text "${resource_text}")
endif()
file(WRITE "${OUTPUT}" "${resource_text}")
