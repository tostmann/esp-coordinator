#include <stdint.h>

extern "C" uint8_t* secur_nwk_key_by_seq(uint8_t seq);

void get_the_key() {
    uint8_t* key = secur_nwk_key_by_seq(0);
}
