#ifndef PROTOCOLS_H
#define PROTOCOLS_H

#include <stddef.h>
#include <stdint.h>

#include "protocol_adapter.h"


#define PROTOCOL_COUNT 4


extern const protocol_adapter_t TBP_ADAPTER;
extern const protocol_adapter_t HLP_ADAPTER;
extern const protocol_adapter_t EVP_ADAPTER;
extern const protocol_adapter_t SGP_ADAPTER;


extern const protocol_adapter_t *const
    ADAPTERS[PROTOCOL_COUNT];


/*
 * Exposed mainly for unit testing.
 *
 * Test vector:
 * "123456789" -> 0x29B1
 */
uint16_t sgp_crc16_ccitt_false(
    const uint8_t *data,
    size_t len
);


#endif