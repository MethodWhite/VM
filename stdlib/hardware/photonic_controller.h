#ifndef _PHOTONIC_CONTROLLER_H
#define _PHOTONIC_CONTROLLER_H

#include <stdint.h>
#include <stdbool.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * Pin assignments
 * ================================================================ */
#define PIN_LASER_R         2
#define PIN_LASER_G         3
#define PIN_LASER_B         4
#define PIN_LASER_IR        5
#define PIN_LASER_UV        6

#define PIN_UART_RX         7
#define PIN_UART_TX         8

#define PIN_ADC_FEEDBACK    26   // GPIO 26 = ADC0
#define PIN_ADC_TEMP        27   // GPIO 27 = ADC1

/* ================================================================
 * Laser channel indices
 * ================================================================ */
#define LASER_R             0
#define LASER_G             1
#define LASER_B             2
#define LASER_IR            3
#define LASER_UV            4
#define NUM_LASERS          5

/* ================================================================
 * Default PWM period (µs)
 * ================================================================ */
#define LASER_PWM_PERIOD_US 100u   // 100 µs = 10 kHz

/* ================================================================
 * Protocol command codes
 * ================================================================ */
#define CMD_EMIT            0x01   // Fire laser
#define CMD_READ_FEEDBACK   0x02   // Read ADC feedback (mV)
#define CMD_RESONANCE       0x03   // Resonance scan
#define CMD_LASER_OFF       0x04   // Turn off laser
#define CMD_SET_TEMP        0x05   // Set target temperature

/* ================================================================
 * Response status codes
 * ================================================================ */
#define RESP_OK             0x00000000
#define RESP_ERR_UNKNOWN    0xFFFFFFFF
#define RESP_ERR_INVALID    0xFFFFFFFE

/* ================================================================
 * UART baud rate
 * ================================================================ */
#define UART_BAUD           115200

/* ================================================================
 * Ring buffer size (must be power of 2)
 * ================================================================ */
#define RING_BUF_BITS       8
#define RING_BUF_SIZE       (1u << RING_BUF_BITS)   // 256
#define RING_BUF_MASK       (RING_BUF_SIZE - 1)

/* ================================================================
 * Command frame (5 bytes on wire)
 * ================================================================ */
typedef struct __attribute__((packed)) {
    uint8_t  command;
    uint8_t  payload[4];
} command_frame_t;

/* ================================================================
 * Laser channel descriptor (from photonic_laser.pio.h)
 * ================================================================ */
typedef struct {
    PIO    pio;
    uint   sm;
    uint   offset;
    uint   gpio;
    uint   pio_idx;
} laser_channel_t;

/* ================================================================
 * UART RX descriptor (from photonic_uart.pio.h)
 * ================================================================ */
typedef struct {
    PIO    pio;
    uint   sm;
    uint   offset;
    uint   rx_pin;
    uint   baud;
} photonic_uart_rx_t;

/* ================================================================
 * Global state
 * ================================================================ */
extern laser_channel_t      g_lasers[NUM_LASERS];
extern photonic_uart_rx_t   g_uart;
extern int32_t              g_target_temp_mdeg;

/* ================================================================
 * ADC helpers
 * ================================================================ */
void     adc_init_all(void);
uint32_t adc_read_mv(uint channel);
uint32_t adc_read_temp_mdeg(void);

/* ================================================================
 * Protocol handlers
 * ================================================================ */
void     handle_command(const command_frame_t *frame);
void     handle_emit(uint32_t payload);
void     handle_read_feedback(void);
void     handle_resonance(uint32_t payload);
void     handle_laser_off(uint32_t payload);
void     handle_set_temp(uint32_t payload);
void     send_response(uint32_t value);

/* ================================================================
 * Ring buffer / DMA
 * ================================================================ */
void     ring_init(void);
bool     ring_read_bytes(uint8_t *dst, uint count);

/* ================================================================
 * Laser frequency table
 * ================================================================ */
int      laser_select_by_freq(uint16_t freq_01nm);

#ifdef __cplusplus
}
#endif

#endif /* _PHOTONIC_CONTROLLER_H */
