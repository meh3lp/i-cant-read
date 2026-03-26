# Called by add_custom_command with -DSPV= -DSYMBOL= -DEMBED_C= -DEMBED_H=
file(READ "${SPV}" raw HEX)

# Split hex string into individual bytes, then group into 32-bit words.
# SPIR-V is little-endian by spec.
string(LENGTH "${raw}" hex_len)
math(EXPR word_count "${hex_len} / 8")  # 4 bytes per word, 2 hex chars per byte

set(words "")
set(i 0)
while(i LESS word_count)
    math(EXPR off "${i} * 8")
    string(SUBSTRING "${raw}" ${off} 8 word_hex)
    # Reverse byte order for little-endian → native uint32_t literal
    string(SUBSTRING "${word_hex}" 0 2 b0)
    string(SUBSTRING "${word_hex}" 2 2 b1)
    string(SUBSTRING "${word_hex}" 4 2 b2)
    string(SUBSTRING "${word_hex}" 6 2 b3)
    list(APPEND words "0x${b3}${b2}${b1}${b0}u")
    math(EXPR i "${i} + 1")
endwhile()

list(JOIN words ", " word_list)

file(WRITE "${EMBED_C}"
"/* Auto-generated — do not edit. */
#include \"${SYMBOL}_spv.h\"
const uint32_t ${SYMBOL}_data[] = { ${word_list} };
const unsigned int ${SYMBOL}_size = ${word_count};
")

file(WRITE "${EMBED_H}"
"/* Auto-generated — do not edit. */
#pragma once
#include <stdint.h>
extern const uint32_t     ${SYMBOL}_data[];
extern const unsigned int ${SYMBOL}_size; /* word count, not byte count */
")