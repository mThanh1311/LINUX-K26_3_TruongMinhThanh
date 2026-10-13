#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "protocols.h"
#include "sensor_types.h"


static void test_crc(void)
{
    const uint8_t vector[] =
        "123456789";

    uint16_t crc =
        sgp_crc16_ccitt_false(
            vector,
            9
        );

    assert(
        crc ==
        0x29B1
    );

    printf(
        "[PASS] CRC-16 vector = 0x%04X\n",
        crc
    );
}


static void test_tbp(void)
{
    const uint8_t frame[] = {
        0x0A, 0x00,

        0x00, 0x00, 0x00, 0x00,
        0x00, 0x80, 0x35, 0x40,

        0x00, 0xB9, 0x55, 0x69,
        0x00, 0x00, 0x00, 0x00
    };


    sensor_data_t out;

    size_t consumed = 0;


    assert(
        TBP_ADAPTER.parse(
            frame,
            5,
            &out,
            &consumed
        ) ==
        PARSE_NEED_MORE
    );


    consumed = 0;


    assert(
        TBP_ADAPTER.parse(
            frame,
            sizeof(frame),
            &out,
            &consumed
        ) ==
        PARSE_OK
    );


    assert(consumed == 18);
    assert(out.id == 10);
    assert(out.type == SENSOR_TEMPERATURE);

    assert(
        out.value > 21.49 &&
        out.value < 21.51
    );

    assert(
        (long)out.ts ==
        1767225600L
    );


    printf(
        "[PASS] TBP parse\n"
    );
}


static void test_hlp(void)
{
    const uint8_t frame[] =
        "HUM 21 54.3 1767225600\n";


    sensor_data_t out;

    size_t consumed = 0;


    assert(
        HLP_ADAPTER.parse(
            frame,
            8,
            &out,
            &consumed
        ) ==
        PARSE_NEED_MORE
    );


    consumed = 0;


    assert(
        HLP_ADAPTER.parse(
            frame,
            strlen(
                (const char *)frame
            ),
            &out,
            &consumed
        ) ==
        PARSE_OK
    );


    assert(out.id == 21);
    assert(out.type == SENSOR_HUMIDITY);

    assert(
        out.value > 54.29 &&
        out.value < 54.31
    );

    assert(
        (long)out.ts ==
        1767225600L
    );


    printf(
        "[PASS] HLP parse\n"
    );
}


static void test_evp(void)
{
    const uint8_t motion[] = {
        0x01,
        0x06,
        0x00,
        0x1E,
        0x69,
        0x55,
        0xB9,
        0x00
    };


    const uint8_t door[] = {
        0x02,
        0x07,
        0x00,
        0x28,
        0x01,
        0x69,
        0x55,
        0xB9,
        0x00
    };


    sensor_data_t out;

    size_t consumed = 0;


    assert(
        EVP_ADAPTER.parse(
            motion,
            sizeof(motion),
            &out,
            &consumed
        ) ==
        PARSE_OK
    );


    assert(out.id == 30);
    assert(out.type == SENSOR_MOTION);
    assert(out.value == 1.0);


    consumed = 0;


    assert(
        EVP_ADAPTER.parse(
            door,
            sizeof(door),
            &out,
            &consumed
        ) ==
        PARSE_OK
    );


    assert(out.id == 40);
    assert(out.type == SENSOR_DOOR);
    assert(out.value == 1.0);


    printf(
        "[PASS] EVP Motion/Door parse\n"
    );
}


static void test_sgp(void)
{
    uint8_t frame[] = {
        0x7E,
        0x00,
        0x32,
        0x00,
        0x78,
        0x69,
        0x55,
        0xB9,
        0x00,
        0xAC,
        0xF5
    };


    sensor_data_t out;

    size_t consumed = 0;


    assert(
        SGP_ADAPTER.parse(
            frame,
            sizeof(frame),
            &out,
            &consumed
        ) ==
        PARSE_OK
    );


    assert(out.id == 50);
    assert(out.type == SENSOR_SMOKE_GAS);
    assert(out.value == 120.0);


    uint8_t reply = 0;


    assert(
        SGP_ADAPTER.build_reply(
            PARSE_OK,
            &reply,
            1
        ) == 1
    );


    assert(
        reply ==
        0x06
    );


    /*
     * Break CRC.
     */
    frame[10] =
        0x00;


    consumed = 0;


    assert(
        SGP_ADAPTER.parse(
            frame,
            sizeof(frame),
            &out,
            &consumed
        ) ==
        PARSE_ERROR
    );


    assert(
        SGP_ADAPTER.build_reply(
            PARSE_ERROR,
            &reply,
            1
        ) == 1
    );


    assert(
        reply ==
        0x15
    );


    printf(
        "[PASS] SGP parse + ACK/NAK\n"
    );
}


int main(void)
{
    test_crc();

    test_tbp();

    test_hlp();

    test_evp();

    test_sgp();


    printf(
        "\nAll protocol tests passed.\n"
    );


    return 0;
}