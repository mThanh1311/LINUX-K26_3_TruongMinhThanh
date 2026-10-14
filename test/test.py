#!/usr/bin/env python3

import socket
import struct
import time


HOST = "127.0.0.1"
BASE_PORT = 1234


def pause():
    time.sleep(0.08)


def send_temperature(sensor_id, values):
    with socket.create_connection((HOST, BASE_PORT)) as sock:
        for value in values:
            frame = struct.pack(
                "<Hdq",
                sensor_id,
                float(value),
                int(time.time()),
            )

            sock.sendall(frame)
            pause()


def send_humidity(sensor_id, values):
    with socket.create_connection((HOST, BASE_PORT + 1)) as sock:
        for value in values:
            frame = (
                f"HUM {sensor_id} {value:.1f} "
                f"{int(time.time())}\n"
            ).encode()

            sock.sendall(frame)
            pause()


def send_motion(sensor_id, count):
    with socket.create_connection((HOST, BASE_PORT + 2)) as sock:
        for _ in range(count):
            frame = struct.pack(
                ">BBHI",
                0x01,
                0x06,
                sensor_id,
                int(time.time()),
            )

            sock.sendall(frame)
            pause()


def send_door(sensor_id, states):
    with socket.create_connection((HOST, BASE_PORT + 2)) as sock:
        for state in states:
            frame = struct.pack(
                ">BBHBI",
                0x02,
                0x07,
                sensor_id,
                state,
                int(time.time()),
            )

            sock.sendall(frame)
            pause()


def crc16_ccitt_false(data):
    crc = 0xFFFF

    for byte in data:
        crc ^= byte << 8

        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF

    return crc


def build_sgp_frame(sensor_id, ppm):
    timestamp = int(time.time())

    body = struct.pack(
        ">HHI",
        sensor_id,
        ppm,
        timestamp,
    )

    crc = crc16_ccitt_false(body)

    return (
        bytes([0x7E])
        + body
        + struct.pack(">H", crc)
    )


def send_smoke_gas(sensor_id, values):
    with socket.create_connection((HOST, BASE_PORT + 3)) as sock:
        sock.settimeout(2)

        for ppm in values:
            frame = build_sgp_frame(
                sensor_id,
                ppm,
            )

            sock.sendall(frame)

            reply = sock.recv(1)

            if reply != b"\x06":
                raise RuntimeError(
                    f"SGP expected ACK, got {reply.hex()}"
                )

            pause()


def send_invalid_id():
    with socket.create_connection((HOST, BASE_PORT + 1)) as sock:
        sock.sendall(
            b"HUM 999 50.0 1767225600\n"
        )

def send_type_mismatch():
    # id=21 is configured as humidity,
    # but TBP decodes it as temperature.
    with socket.create_connection((HOST, BASE_PORT)) as sock:
        frame = struct.pack(
            "<Hdq",
            21,
            18.0,
            int(time.time()),
        )

        sock.sendall(frame)


def main():
    print("[M5] Temperature")
    send_temperature(
        10,
        [10.0] * 5
        + [18.0] * 5
        + [25.0] * 5,
    )

    print("[M5] Humidity")
    send_humidity(
        21,
        [20.0] * 5
        + [50.0] * 5
        + [80.0] * 5,
    )

    print("[M5] Motion")
    send_motion(
        30,
        3,
    )

    print("[M5] Door")
    send_door(
        40,
        [1, 1, 0, 0, 1],
    )

    print("[M5] Smoke/Gas")
    send_smoke_gas(
        50,
        [100, 350, 400, 100, 350],
    )

    print("[M5] Unknown sensor")
    send_invalid_id()

    print("[M5] Type mismatch")
    send_type_mismatch()

    print("[M5] Test traffic completed")


if __name__ == "__main__":
    main()
