// MIT License
//
// Copyright (c) 2026 Kevin Thomas
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
// Author:  Kevin Thomas
// Email:   kevin@mytechnotalent.com
// GitHub:  https://github.com/mytechnotalent/picokit-34-remote-actuator
// File:    monitor.c
// Desc:    Implements the remote actuator state machine that drives the servo
//          from a gateway command and acknowledges it.
// Created: 2026

#include "picokit_34_remote_actuator.h"
#include "monitor.h"
#include "radio.h"
#include "status_led.h"
#include "servo.h"
#include "ccm.h"
#include "envelope.h"
#include "field_secrets.h"
#include "hardware/gpio.h"
#include "pico/time.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/**
 * @brief Module-ready flag.
 *
 * Set to true by monitor_init() once the peripherals are configured.
 * monitor_step() returns false while this flag is clear.
 */
static bool g_ready;

/**
 * @brief Most recent commanded servo angle in degrees.
 */
static uint8_t g_angle;

/**
 * @brief Monotonic transmit sequence number.
 */
static uint16_t g_seq;

/**
 * @brief Absolute time in microseconds of the next authenticated transmit.
 */
static uint64_t g_next_tx_us;

/**
 * @brief Inbound radio line accumulator.
 */
static char g_rx_line[RADIO_LINE_BUF_LEN];

/**
 * @brief Number of bytes currently held in the inbound line accumulator.
 */
static size_t g_rx_len;

/**
 * @brief AES-128 session key for telemetry.
 */
static uint8_t g_key[CCM_KEY_LEN];

/**
 * @brief True once the telemetry session key has been loaded.
 */
static bool g_key_ready;

/**
 * @brief Configure the onboard heartbeat LED as a dark output.
 *
 * @param void No parameters.
 * @return void
 */
static void monitor_state_init_io(void) {
    gpio_init(PICOKIT_34_REMOTE_ACTUATOR_LED_PIN);
    gpio_set_dir(PICOKIT_34_REMOTE_ACTUATOR_LED_PIN, GPIO_OUT);
    gpio_put(PICOKIT_34_REMOTE_ACTUATOR_LED_PIN, 0);
}

/**
 * @brief Reset the commanded angle, sequence, and transmit timing.
 *
 * @param void No parameters.
 * @return void
 */
static void monitor_state_init(void) {
    uint64_t now_us = time_us_64();
    g_angle = 0u;
    g_seq = 0u;
    g_next_tx_us = now_us + (uint64_t)PICOKIT_34_REMOTE_ACTUATOR_TX_INTERVAL_MS * 1000u;
    g_ready = true;
}

/**
 * @brief Load the telemetry session key from the field secret.
 *
 * LAB-ONLY: production must provision the session key through OTP rather
 * than embedding a committed key.
 *
 * @param void No parameters.
 * @return void
 */
static void monitor_load_key(void) {
    static const uint8_t key[CCM_KEY_LEN] = FIELD_SECRET_KEY;
    memcpy(g_key, key, CCM_KEY_LEN);
    g_key_ready = true;
}

/**
 * @brief Print the boot banner for the remote actuator lesson.
 *
 * @param void No parameters.
 * @return void
 */
static void monitor_banner(void) {
    printf("=== PICOKIT-34 REMOTE ACTUATOR // GATEWAY COMMAND + AUTHENTICATED ACK ===\n");
}

/**
 * @brief Derive the field key and announce a ready monitor.
 *
 * @param void No parameters.
 * @return bool true when the field key was derived and installed.
 */
static bool monitor_finish(void) {
    monitor_load_key();
    monitor_banner();
    return true;
}

/**
 * @brief Blink the onboard heartbeat LED exactly once.
 *
 * @param void No parameters.
 * @return void
 */
static void monitor_heartbeat(void) {
    gpio_put(PICOKIT_34_REMOTE_ACTUATOR_LED_PIN, 1);
    sleep_us(MONITOR_HEARTBEAT_BLINK_US);
    gpio_put(PICOKIT_34_REMOTE_ACTUATOR_LED_PIN, 0);
    sleep_us(MONITOR_HEARTBEAT_BLINK_US);
}

/**
 * @brief Clamp an angle to the commandable range.
 *
 * @param value Requested angle in degrees.
 * @return int Clamped angle in degrees.
 */
static int monitor_clamp_angle(int value) {
    if (value > MONITOR_ANGLE_MAX_DEGREES) {
        return MONITOR_ANGLE_MAX_DEGREES;
    }
    if (value < MONITOR_ANGLE_MIN_DEGREES) {
        return MONITOR_ANGLE_MIN_DEGREES;
    }
    return value;
}

/**
 * @brief Parse the decimal angle digits that follow the command prefix.
 *
 * @param p Pointer to the first character after the command prefix.
 * @param value Pointer to the mutable parsed angle output.
 * @return bool true when at least one digit was parsed.
 */
static bool monitor_scan_digits(const char *p, int *value) {
    const char *start = p;
    int acc = 0;
    while (*p >= '0' && *p <= '9') {
        acc = acc * 10 + (*p - '0');
        p++;
    }
    *value = monitor_clamp_angle(acc);
    return p != start;
}

/**
 * @brief Parse an ACT angle command from a plaintext gateway payload.
 *
 * @param payload Pointer to the NUL-terminated payload text.
 * @param out Pointer to the mutable decoded angle output.
 * @return bool true when the payload carried a valid command.
 */
static bool monitor_parse_angle(const char *payload, uint8_t *out) {
    int value = 0;
    if ((payload == NULL) || (out == NULL) || (strncmp(payload, MONITOR_COMMAND_PREFIX, 4u) != 0)) {
        return false;
    }
    if (!monitor_scan_digits(payload + 4, &value)) {
        return false;
    }
    *out = (uint8_t)value;
    return true;
}

/**
 * @brief Format the heartbeat JSON body for the commanded angle.
 *
 * @param frame Pointer to the mutable frame output buffer.
 * @param frame_len Capacity of the frame output buffer in bytes.
 * @return size_t Number of JSON bytes written, or zero on overflow.
 */
static size_t monitor_build_frame(char *frame, size_t frame_len) {
    int written = snprintf(frame, frame_len, "{\"n\":%u,\"s\":%u,\"a\":%u}", (unsigned)PACKET_NODE_ID, (unsigned)g_seq, (unsigned)g_angle);
    return (written > 0 && (size_t)written < frame_len) ? (size_t)written : 0u;
}

/**
 * @brief Seal the current heartbeat body into a hex envelope.
 *
 * @param hex Pointer to the NUL-terminated hex output buffer.
 * @param hex_len Capacity of the hex output buffer in bytes.
 * @return bool true when the heartbeat was sealed and encoded.
 */
static bool monitor_seal_frame(char *hex, size_t hex_len) {
    char frame[PICOKIT_34_REMOTE_ACTUATOR_FRAME_SIZE];
    uint8_t nonce[ENVELOPE_NONCE_LEN];
    uint8_t ad = (uint8_t)PACKET_NODE_ID;
    size_t frame_len = monitor_build_frame(frame, sizeof(frame));
    envelope_fill_nonce(nonce);
    return envelope_seal_hex(g_key, nonce, &ad, 1u, (const uint8_t *)frame, frame_len, hex, hex_len);
}

/**
 * @brief Build and transmit the authenticated heartbeat frame.
 *
 * @param void No parameters.
 * @return void
 */
static void monitor_transmit(void) {
    char hex[ENVELOPE_MAX_HEX_LEN];
    if (!g_key_ready) {
        return;
    }
    if (monitor_seal_frame(hex, sizeof(hex))) {
        radio_send_frame(PICOKIT_34_REMOTE_ACTUATOR_UART, (const uint8_t *)hex, strlen(hex));
        g_seq += 1u;
    }
}

/**
 * @brief Transmit one heartbeat and schedule the next transmit.
 *
 * @param now_us Current monotonic time in microseconds.
 * @return void
 */
static void monitor_tx_tick(uint64_t now_us) {
    monitor_heartbeat();
    monitor_transmit();
    g_next_tx_us = now_us + (uint64_t)PICOKIT_34_REMOTE_ACTUATOR_TX_INTERVAL_MS * 1000u;
}

/**
 * @brief Apply one gateway command, drive the servo, and acknowledge it.
 *
 * @param payload Pointer to the NUL-terminated payload text.
 * @return void
 */
static void monitor_apply_command(const char *payload) {
    uint8_t angle;
    if (!monitor_parse_angle(payload, &angle)) {
        return;
    }
    g_angle = angle;
    servo_set_angle(g_angle);
    printf("ACT %u\n", (unsigned)g_angle);
    monitor_transmit();
}

/**
 * @brief Drain inbound radio lines and apply every valid +RCV command.
 *
 * @param void No parameters.
 * @return void
 */
static void monitor_rx_tick(void) {
    radio_rcv_t rcv;
    while (radio_line_pump(PICOKIT_34_REMOTE_ACTUATOR_UART, g_rx_line, &g_rx_len)) {
        if (radio_parse_rcv(g_rx_line, &rcv) == RADIO_RESULT_OK) {
            printf("RX from 0x%04X, %u bytes\n", (unsigned)rcv.sender, (unsigned)rcv.len);
            monitor_apply_command(rcv.payload);
        }
    }
}

/**
 * @brief Service the heartbeat transmit timer.
 *
 * @param now_us Current monotonic time in microseconds.
 * @return void
 */
static void monitor_service_timers(uint64_t now_us) {
    if (now_us >= g_next_tx_us) {
        monitor_tx_tick(now_us);
    }
}

bool monitor_init(void) {
    bool ok;
    ok = status_led_init() && radio_init(PICOKIT_34_REMOTE_ACTUATOR_UART);
    ok = ok && servo_init();
    monitor_state_init_io();
    monitor_state_init();
    return ok && monitor_finish();
}

void monitor_deinit(void) {
    g_ready = false;
}

bool monitor_step(void) {
    uint64_t now_us;
    if (!g_ready) {
        return false;
    }
    now_us = time_us_64();
    monitor_service_timers(now_us);
    monitor_rx_tick();
    return true;
}
