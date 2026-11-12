# cmake -DSPV=<in.spv> -DOUT=<out.h> -DNAME=<symbol> -P embed_spv.cmake
file(READ "${SPV}" hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," arr "${hex}")
file(WRITE "${OUT}" "// generated from ${SPV}\n#pragma once\nstatic const unsigned char ${NAME}[] = {${arr}};\n")
