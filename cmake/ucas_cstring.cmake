# cmake -DIN=file -DOUT=file.inc -P ucas_cstring.cmake
# Each line of IN as a C string literal with its newline, as ucas's Makefile
# makes thy_absalg.inc with sed: backslashes and quotes escaped.
file(READ ${IN} s)
string(REPLACE "\\" "\\\\" s "${s}")
string(REPLACE "\"" "\\\"" s "${s}")
string(REGEX REPLACE "\n$" "" s "${s}")
string(REPLACE "\n" "\\n\"\n\"" s "${s}")
file(WRITE ${OUT}.tmp "\"${s}\\n\"\n")
file(RENAME ${OUT}.tmp ${OUT})
