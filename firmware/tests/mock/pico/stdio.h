/**
 * FILE: stdio.h
 *
 * DESCRIPTION:
 * Host mock header for the Pico SDK character input with a settable
 * console input queue.
 *
 * BRIEF:
 * Pico SDK stdio mock for native unit testing.
 *
 * AUTHOR: Kevin Thomas
 * DATE: September 2026
 */

#ifndef MOCK_PICO_STDIO_H
#define MOCK_PICO_STDIO_H

#include "pico/stdlib.h"
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Capacity of the mock console input queue.
 */
#define MOCK_STDIO_BUF_SIZE 256u

/**
 * @brief Mock console input bytes.
 */
static char s_mock_stdio_buf[MOCK_STDIO_BUF_SIZE];

/**
 * @brief Total number of bytes in the mock console input queue.
 */
static size_t s_mock_stdio_len;

/**
 * @brief Current read index in the mock console input queue.
 */
static size_t s_mock_stdio_pos;

/**
 * @brief Clear the mock console input queue.
 *
 * @param void No parameters.
 * @return void
 */
static inline void mock_stdio_reset(void) {
    s_mock_stdio_len = 0u;
    s_mock_stdio_pos = 0u;
}

/**
 * @brief Prime the mock console input queue with test characters.
 *
 * @param input Input string of characters to supply to getchar_timeout_us.
 * @param len Number of characters in input.
 * @return void
 */
static inline void mock_stdio_set_input(const char *input, size_t len) {
    size_t i;
    s_mock_stdio_len = 0u;
    s_mock_stdio_pos = 0u;
    if (input == NULL || len == 0u) {
        return;
    }
    for (i = 0u; i < len && i < MOCK_STDIO_BUF_SIZE; ++i) {
        s_mock_stdio_buf[i] = input[i];
    }
    s_mock_stdio_len = i;
}

/**
 * @brief Mock implementation of getchar_timeout_us.
 *
 * @param timeout_us Unused timeout in microseconds, ignored by the mock.
 * @return int Next queued character, or PICO_ERROR_TIMEOUT when empty.
 */
static inline int getchar_timeout_us(uint32_t timeout_us) {
    (void)timeout_us;
    if (s_mock_stdio_pos < s_mock_stdio_len) {
        return (unsigned char)s_mock_stdio_buf[s_mock_stdio_pos++];
    }
    return PICO_ERROR_TIMEOUT;
}

#endif // MOCK_PICO_STDIO_H
