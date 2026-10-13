#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protocol_adapter.h"
#include "protocols.h"


#define TBP_FRAME_SIZE 18U
#define HLP_MAX_LINE_SIZE 64U

#define EVP_TYPE_MOTION 0x01U
#define EVP_TYPE_DOOR   0x02U

#define SGP_FRAME_SIZE 11U
#define SGP_MAGIC      0x7EU
#define SGP_ACK        0x06U
#define SGP_NAK        0x15U


_Static_assert(
    sizeof(double) == 8,
    "Protocols require an 8-byte IEEE-754 double"
);


/* ============================================================
 * Common helpers
 * ============================================================ */

static uint16_t read_u16_le(
    const uint8_t *p
)
{
    return
        ((uint16_t)p[0]) |
        ((uint16_t)p[1] << 8);
}


static uint64_t read_u64_le(
    const uint8_t *p
)
{
    return
        ((uint64_t)p[0])       |
        ((uint64_t)p[1] << 8)  |
        ((uint64_t)p[2] << 16) |
        ((uint64_t)p[3] << 24) |
        ((uint64_t)p[4] << 32) |
        ((uint64_t)p[5] << 40) |
        ((uint64_t)p[6] << 48) |
        ((uint64_t)p[7] << 56);
}


static uint16_t read_u16_be(
    const uint8_t *p
)
{
    return
        ((uint16_t)p[0] << 8) |
        ((uint16_t)p[1]);
}


static uint32_t read_u32_be(
    const uint8_t *p
)
{
    return
        ((uint32_t)p[0] << 24) |
        ((uint32_t)p[1] << 16) |
        ((uint32_t)p[2] << 8)  |
        ((uint32_t)p[3]);
}


static size_t no_reply(
    parse_result_t result,
    uint8_t *reply,
    size_t cap
)
{
    (void)result;
    (void)reply;
    (void)cap;

    return 0;
}


/* ============================================================
 * TBP
 * Temperature Binary Protocol
 *
 * Port: P
 *
 * 2-byte id
 * 8-byte double
 * 8-byte int64 timestamp
 * little-endian
 * ============================================================ */

static parse_result_t tbp_parse(
    const uint8_t *buf,
    size_t len,
    sensor_data_t *out,
    size_t *consumed
)
{
    if (
        buf == NULL ||
        out == NULL ||
        consumed == NULL
    ) {
        return PARSE_ERROR;
    }


    *consumed = 0;


    if (len < TBP_FRAME_SIZE) {
        return PARSE_NEED_MORE;
    }


    uint16_t id =
        read_u16_le(
            buf
        );


    uint64_t value_bits =
        read_u64_le(
            buf + 2
        );


    double value;

    memcpy(
        &value,
        &value_bits,
        sizeof(value)
    );


    int64_t timestamp =
        (int64_t)read_u64_le(
            buf + 10
        );


    /*
     * Temperature valid range from spec:
     * -40 .. 85 °C
     */
    if (
        !isfinite(value) ||
        value < -40.0 ||
        value > 85.0
    ) {

        *consumed =
            TBP_FRAME_SIZE;

        return PARSE_ERROR;
    }


    out->id =
        id;

    out->type =
        SENSOR_TEMPERATURE;

    out->value =
        value;

    out->ts =
        (time_t)timestamp;


    *consumed =
        TBP_FRAME_SIZE;


    return PARSE_OK;
}


/* ============================================================
 * HLP
 * Humidity Line Protocol
 *
 * HUM <id> <value> <ts>\n
 * Port: P+1
 * ============================================================ */

static parse_result_t hlp_parse(
    const uint8_t *buf,
    size_t len,
    sensor_data_t *out,
    size_t *consumed
)
{
    if (
        buf == NULL ||
        out == NULL ||
        consumed == NULL
    ) {
        return PARSE_ERROR;
    }


    *consumed = 0;


    const uint8_t *lf =
        memchr(
            buf,
            '\n',
            len
        );


    if (lf == NULL) {

        /*
         * Maximum includes LF.
         *
         * At 64 bytes without LF,
         * even the next byte would make
         * the line invalid.
         */
        if (len >= HLP_MAX_LINE_SIZE) {

            *consumed =
                len;

            return PARSE_ERROR;
        }


        return PARSE_NEED_MORE;
    }


    size_t line_len =
        (size_t)(
            lf - buf
        ) + 1;


    if (
        line_len >
        HLP_MAX_LINE_SIZE
    ) {

        *consumed =
            line_len;

        return PARSE_ERROR;
    }


    char line[
        HLP_MAX_LINE_SIZE + 1
    ];


    size_t text_len =
        line_len - 1;


    memcpy(
        line,
        buf,
        text_len
    );

    line[text_len] =
        '\0';


    /*
     * HLP permits ASCII line format;
     * reject CR and TAB here.
     */
    if (
        strchr(line, '\r') != NULL ||
        strchr(line, '\t') != NULL
    ) {

        *consumed =
            line_len;

        return PARSE_ERROR;
    }


    if (
        strncmp(
            line,
            "HUM ",
            4
        ) != 0
    ) {

        *consumed =
            line_len;

        return PARSE_ERROR;
    }


    char *p =
        line + 4;

    char *end = NULL;


    /*
     * ID
     */
    errno = 0;

    unsigned long id =
        strtoul(
            p,
            &end,
            10
        );


    if (
        errno != 0 ||
        end == p ||
        *end != ' ' ||
        id > UINT16_MAX
    ) {

        *consumed =
            line_len;

        return PARSE_ERROR;
    }


    p =
        end + 1;


    /*
     * Reject multiple spaces.
     */
    if (
        *p == '\0' ||
        *p == ' '
    ) {

        *consumed =
            line_len;

        return PARSE_ERROR;
    }


    /*
     * Humidity value.
     */
    errno = 0;

    double value =
        strtod(
            p,
            &end
        );


    if (
        errno != 0 ||
        end == p ||
        *end != ' ' ||
        !isfinite(value) ||
        value < 0.0 ||
        value > 100.0
    ) {

        *consumed =
            line_len;

        return PARSE_ERROR;
    }


    p =
        end + 1;


    if (
        *p == '\0' ||
        *p == ' '
    ) {

        *consumed =
            line_len;

        return PARSE_ERROR;
    }


    /*
     * Timestamp.
     */
    errno = 0;

    long long timestamp =
        strtoll(
            p,
            &end,
            10
        );


    if (
        errno != 0 ||
        end == p ||
        *end != '\0'
    ) {

        *consumed =
            line_len;

        return PARSE_ERROR;
    }


    out->id =
        (sensor_id_t)id;

    out->type =
        SENSOR_HUMIDITY;

    out->value =
        value;

    out->ts =
        (time_t)timestamp;


    *consumed =
        line_len;


    return PARSE_OK;
}


/* ============================================================
 * EVP
 * Event TLV Protocol
 *
 * Port P+2
 *
 * Motion:
 * 01 06 | id:u16 | ts:u32
 *
 * Door:
 * 02 07 | id:u16 | state:u8 | ts:u32
 *
 * All multibyte fields big-endian.
 * ============================================================ */

static parse_result_t evp_parse(
    const uint8_t *buf,
    size_t len,
    sensor_data_t *out,
    size_t *consumed
)
{
    if (
        buf == NULL ||
        out == NULL ||
        consumed == NULL
    ) {
        return PARSE_ERROR;
    }


    *consumed = 0;


    if (len < 2) {
        return PARSE_NEED_MORE;
    }


    uint8_t type =
        buf[0];

    uint8_t payload_len =
        buf[1];


    size_t expected_payload;


    if (type == EVP_TYPE_MOTION) {

        expected_payload = 6;

    } else if (
        type == EVP_TYPE_DOOR
    ) {

        expected_payload = 7;

    } else {

        /*
         * Drop one byte to search for
         * next possible TLV type.
         */
        *consumed = 1;

        return PARSE_ERROR;
    }


    if (
        payload_len !=
        expected_payload
    ) {

        *consumed = 1;

        return PARSE_ERROR;
    }


    size_t frame_len =
        2U +
        expected_payload;


    if (len < frame_len) {
        return PARSE_NEED_MORE;
    }


    sensor_id_t id =
        read_u16_be(
            buf + 2
        );


    if (
        type ==
        EVP_TYPE_MOTION
    ) {

        uint32_t timestamp =
            read_u32_be(
                buf + 4
            );


        out->id =
            id;

        out->type =
            SENSOR_MOTION;

        out->value =
            1.0;

        out->ts =
            (time_t)timestamp;

    } else {

        uint8_t state =
            buf[4];


        if (
            state != 0 &&
            state != 1
        ) {

            *consumed =
                frame_len;

            return PARSE_ERROR;
        }


        uint32_t timestamp =
            read_u32_be(
                buf + 5
            );


        out->id =
            id;

        out->type =
            SENSOR_DOOR;

        out->value =
            (double)state;

        out->ts =
            (time_t)timestamp;
    }


    *consumed =
        frame_len;


    return PARSE_OK;
}


/* ============================================================
 * SGP CRC-16/CCITT-FALSE
 *
 * poly    = 0x1021
 * init    = 0xFFFF
 * refin   = false
 * refout  = false
 * xorout  = 0
 *
 * "123456789" => 0x29B1
 * ============================================================ */

uint16_t sgp_crc16_ccitt_false(
    const uint8_t *data,
    size_t len
)
{
    uint16_t crc =
        0xFFFFU;


    if (data == NULL) {
        return crc;
    }


    for (
        size_t i = 0;
        i < len;
        i++
    ) {

        crc ^=
            (uint16_t)data[i]
            << 8;


        for (
            unsigned int bit = 0;
            bit < 8;
            bit++
        ) {

            if (
                crc & 0x8000U
            ) {

                crc =
                    (uint16_t)(
                        (crc << 1) ^
                        0x1021U
                    );

            } else {

                crc =
                    (uint16_t)(
                        crc << 1
                    );
            }
        }
    }


    return crc;
}


/* ============================================================
 * SGP
 * Smoke/Gas Protocol
 *
 * Port P+3
 *
 * 0     : 0x7E
 * 1..2  : id big-endian
 * 3..4  : ppm big-endian
 * 5..8  : ts big-endian
 * 9..10 : CRC big-endian
 * ============================================================ */

static parse_result_t sgp_parse(
    const uint8_t *buf,
    size_t len,
    sensor_data_t *out,
    size_t *consumed
)
{
    if (
        buf == NULL ||
        out == NULL ||
        consumed == NULL
    ) {
        return PARSE_ERROR;
    }


    *consumed = 0;


    if (len == 0) {
        return PARSE_NEED_MORE;
    }


    if (buf[0] != SGP_MAGIC) {

        /*
         * Resynchronize by searching
         * byte-by-byte for 0x7E.
         */
        *consumed = 1;

        return PARSE_ERROR;
    }


    if (len < SGP_FRAME_SIZE) {
        return PARSE_NEED_MORE;
    }


    uint16_t received_crc =
        read_u16_be(
            buf + 9
        );


    uint16_t calculated_crc =
        sgp_crc16_ccitt_false(
            buf + 1,
            8
        );


    if (
        received_crc !=
        calculated_crc
    ) {

        *consumed =
            SGP_FRAME_SIZE;

        return PARSE_ERROR;
    }


    uint16_t ppm =
        read_u16_be(
            buf + 3
        );


    if (ppm > 10000U) {

        *consumed =
            SGP_FRAME_SIZE;

        return PARSE_ERROR;
    }


    out->id =
        read_u16_be(
            buf + 1
        );

    out->type =
        SENSOR_SMOKE_GAS;

    out->value =
        (double)ppm;

    out->ts =
        (time_t)read_u32_be(
            buf + 5
        );


    *consumed =
        SGP_FRAME_SIZE;


    return PARSE_OK;
}


static size_t sgp_build_reply(
    parse_result_t result,
    uint8_t *reply,
    size_t cap
)
{
    if (
        reply == NULL ||
        cap < 1
    ) {

        return 0;
    }


    if (result == PARSE_OK) {

        reply[0] =
            SGP_ACK;

        return 1;
    }


    if (result == PARSE_ERROR) {

        reply[0] =
            SGP_NAK;

        return 1;
    }


    return 0;
}


/* ============================================================
 * Adapter objects
 * ============================================================ */

const protocol_adapter_t
TBP_ADAPTER = {
    .name = "TBP",
    .port_offset = 0,
    .parse = tbp_parse,
    .build_reply = no_reply
};


const protocol_adapter_t
HLP_ADAPTER = {
    .name = "HLP",
    .port_offset = 1,
    .parse = hlp_parse,
    .build_reply = no_reply
};


const protocol_adapter_t
EVP_ADAPTER = {
    .name = "EVP",
    .port_offset = 2,
    .parse = evp_parse,
    .build_reply = no_reply
};


const protocol_adapter_t
SGP_ADAPTER = {
    .name = "SGP",
    .port_offset = 3,
    .parse = sgp_parse,
    .build_reply = sgp_build_reply
};


const protocol_adapter_t *const
ADAPTERS[PROTOCOL_COUNT] = {
    &TBP_ADAPTER,
    &HLP_ADAPTER,
    &EVP_ADAPTER,
    &SGP_ADAPTER
};