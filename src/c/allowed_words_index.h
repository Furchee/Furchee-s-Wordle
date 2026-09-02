#pragma once
#include <stdint.h>

#define ALLOWED_WORD_COUNT 14855u
#define PACKED_WORD_SIZE 4u

// A..Z start offsets, followed by the end sentinel.
static const uint16_t ALLOWED_WORD_LETTER_START[27] = {0, 868, 1871, 2841, 3576, 3906, 4552, 5237, 5769, 5949, 6174, 6603, 7228, 8179, 8647, 8999, 10129, 10232, 11027, 12693, 13575, 13792, 14076, 14510, 14528, 14733, 14855};
