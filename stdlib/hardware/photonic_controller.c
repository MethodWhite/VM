/*
 * photonic_controller.c
 * RP2350 (Pico 2) firmware for the photonic AGI laser system.
 *
 * Communicates with VestaVM over UART and directly controls:
 *   – 5 laser diodes (R, G, B, IR, UV) via PIO
 *   – Self-mixing interferometry feedback reading via ADC
 *   – Quartz crystal temperature monitoring
 *   – UTF-32 protocol decoding from serial
 */

#include "photonic_controller.h"
#include "photonic_laser.pio.h"
#include "photonic_uart.pio.h"

#include <string.h>
#include <stdlib.h>

/* =================================================================
 * Global state
 * ================================================================ */
laser_channel_t    g_lasers[NUM_LASERS];
photonic_uart_rx_t g_uart;
int32_t            g_target_temp_mdeg = 25000;   /* 25.0 °C default */

/* Ring buffer for UART DMA */
static uint8_t     g_ring[RING_BUF_SIZE];
static volatile uint32_t g_wr_idx = 0;
static volatile uint32_t g_rd_idx = 0;

/* DMA channel */
static int         g_dma_chan = -1;

/* =================================================================
 * Frequency-to-laser mapping table (wavelength in 0.1 nm)
 *
 * Each entry defines the inclusive wavelength range and the centre
 * used for tie-breaking when a frequency falls into an overlap.
 * ================================================================ */
typedef struct {
    uint8_t  index;
    uint16_t freq_min;      /* 0.1 nm */
    uint16_t freq_max;
    uint16_t freq_centre;
} laser_map_t;

static const laser_map_t k_laser_map[NUM_LASERS] = {
    { LASER_R,  6200, 7500, 6800 },   /* ~680 nm */
    { LASER_G,  4950, 5700, 5320 },   /* ~532 nm */
    { LASER_B,  4500, 4950, 4730 },   /* ~473 nm */
    { LASER_IR, 7000, 10000, 8080 },  /* ~808 nm */
    { LASER_UV, 1000, 4000, 3550 },   /* ~355 nm */
};

/* =================================================================
 * Laser selection by frequency
 *
 * Returns the laser index with the smallest distance from its centre
 * to the requested frequency, or -1 if no laser covers that range.
 * ================================================================ */
int laser_select_by_freq(uint16_t freq_01nm)
{
    int  best    = -1;
    int  best_d  = INT32_MAX;

    for (int i = 0; i < NUM_LASERS; i++) {
        if (freq_01nm >= k_laser_map[i].freq_min &&
            freq_01nm <= k_laser_map[i].freq_max) {
            int d = abs((int)freq_01nm - (int)k_laser_map[i].freq_centre);
            if (d < best_d) {
                best_d = d;
                best   = i;
            }
        }
    }
    return best;
}

/* =================================================================
 * ADC helpers
 * ================================================================ */
void adc_init_all(void)
{
    adc_init();
    adc_gpio_init(PIN_ADC_FEEDBACK);
    adc_gpio_init(PIN_ADC_TEMP);
}

/* Read ADC channel and return value in millivolts (3.3 V ref). */
uint32_t adc_read_mv(uint channel)
{
    adc_select_input(channel);
    uint16_t raw = adc_read();                /* 12-bit */
    return ((uint32_t)raw * 3300u + 2047u) / 4095u;
}

/* Read internal temperature sensor (channel 4 on RP2350).
 * Returns millidegrees Celsius.  Uses the RP2040/Pico formula:
 *   T = 27 - (ADC_voltage - 0.706) / 0.001721
 * which gives degrees C; we multiply by 1000.
 */
uint32_t adc_read_temp_mdeg(void)
{
    adc_select_input(4);                      /* temp sensor */
    uint16_t raw = adc_read();
    /* Convert to voltage in mV */
    uint32_t v_mv = ((uint32_t)raw * 3300u + 2047u) / 4095u;

    /* V_ref = 706 mV at 27 °C, slope = 1.721 mV/°C */
    int32_t t = 27000 - ((int32_t)v_mv - 706) * 1000 / 1721;
    return (uint32_t)(t < 0 ? 0 : t);
}

/* =================================================================
 * UART / DMA ring buffer
 * ================================================================ */
void ring_init(void)
{
    g_wr_idx = 0;
    g_rd_idx = 0;
    memset((void *)g_ring, 0, RING_BUF_SIZE);
}

/* Read up to `count` bytes from the ring buffer.
 * Returns true if enough bytes were available. */
bool ring_read_bytes(uint8_t *dst, uint count)
{
    uint32_t avail = (g_wr_idx - g_rd_idx) & RING_BUF_MASK;
    if (avail < count) return false;

    for (uint i = 0; i < count; i++) {
        dst[i] = g_ring[g_rd_idx & RING_BUF_MASK];
        g_rd_idx++;
    }
    return true;
}

/* DMA IRQ handler – called on DMA completion (buffer full / wrap). */
static void dma_irq_handler(void)
{
    if (dma_irqn_get_channel_status(0, g_dma_chan)) {
        dma_irqn_acknowledge_channel(0, g_dma_chan);
        /* The DMA wraps automatically (ring mode); nothing to do.
         * We just acknowledge and continue. */
    }
}

static void dma_uart_init(void)
{
    g_dma_chan = dma_claim_unused_channel(true);
    if (g_dma_chan < 0) return;

    dma_channel_config c = dma_channel_get_default_config(g_dma_chan);
    channel_config_set_read_addr(&c, &pio1_hw->rxf[g_uart.sm]);
    channel_config_set_write_addr(&c, g_ring);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_dreq(&c, photonic_uart_get_dreq(&g_uart));
    /* Ring wrapping: write address wraps at RING_BUF_SIZE boundary */
    channel_config_set_ring(&c, false, RING_BUF_BITS);

    dma_channel_configure(g_dma_chan, &c,
                          g_ring,                    /* write addr */
                          &pio1_hw->rxf[g_uart.sm], /* read addr  */
                          0xFFFFFFFF,                /* count: infinite */
                          true);                     /* start */

    /* Enable DMA IRQ */
    dma_irqn_set_channel_enabled(0, g_dma_chan, true);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq_handler);
    irq_set_enabled(DMA_IRQ_0, true);
}

/* =================================================================
 * UART transmit (via hardware UART)
 * ================================================================ */
void send_response(uint32_t value)
{
    uart_putc_raw(uart0,  value        & 0xFF);
    uart_putc_raw(uart0, (value >>  8) & 0xFF);
    uart_putc_raw(uart0, (value >> 16) & 0xFF);
    uart_putc_raw(uart0, (value >> 24) & 0xFF);
}

/* =================================================================
 * Command handlers
 * ================================================================ */

/* CMD_EMIT (0x01)
 * Payload: UTF-32 code
 *   bits 0-15  : frequency in 0.1 nm
 *   bits 16-31 : duty cycle in ns
 *
 * The firmware selects the best-matching laser and fires it.
 * The duty cycle determines the on-time; the period is fixed
 * (LASER_PWM_PERIOD_US).
 */
void handle_emit(uint32_t payload)
{
    uint16_t freq_01nm = (uint16_t)(payload & 0xFFFF);
    uint16_t duty_ns   = (uint16_t)((payload >> 16) & 0xFFFF);

    int idx = laser_select_by_freq(freq_01nm);
    if (idx < 0) {
        send_response(RESP_ERR_INVALID);
        return;
    }

    /* Convert duty cycle from ns to sysclk cycles */
    uint32_t clk_hz = clock_get_hz(clk_sys);
    uint32_t on_cyc = ns_to_cycles(duty_ns, clk_hz);
    if (on_cyc > 0 && on_cyc < 2) on_cyc = 1;  /* clamp to minimum */

    uint32_t period_cyc = (uint32_t)((uint64_t)clk_hz *
                           LASER_PWM_PERIOD_US / 1000000ull);
    uint32_t off_cyc = (on_cyc >= period_cyc) ? 0 : (period_cyc - on_cyc);
    if (off_cyc > 65535) off_cyc = 65535;

    laser_fire(&g_lasers[idx], (uint16_t)on_cyc, (uint16_t)off_cyc);
    send_response(RESP_OK);
}

/* CMD_READ_FEEDBACK (0x02)
 * Returns the self-mixing interferometry feedback voltage in mV.
 */
void handle_read_feedback(void)
{
    uint32_t mv = adc_read_mv(0);   /* ADC0 = feedback */
    send_response(mv);
}

/* CMD_RESONANCE (0x03)
 * Payload: resonance scan parameter (reserved for future use).
 * Returns the current feedback reading as a placeholder.
 */
void handle_resonance(uint32_t payload __unused)
{
    /* In a full implementation this would sweep the laser frequency
     * and look for a peak in the self-mixing feedback. */
    uint32_t mv = adc_read_mv(0);
    send_response(mv);
}

/* CMD_LASER_OFF (0x04)
 * Payload: laser index (byte 0).
 */
void handle_laser_off(uint32_t payload)
{
    uint idx = payload & 0xFF;
    if (idx >= NUM_LASERS) {
        send_response(RESP_ERR_INVALID);
        return;
    }
    laser_off(&g_lasers[idx]);
    send_response(RESP_OK);
}

/* CMD_SET_TEMP (0x05)
 * Payload: target temperature in millidegrees Celsius.
 * Returns the current temperature reading.
 */
void handle_set_temp(uint32_t payload)
{
    g_target_temp_mdeg = (int32_t)payload;
    /* In a real system this would drive a heater/cooler PID loop.
     * Here we just return the current temperature. */
    uint32_t cur = adc_read_temp_mdeg();
    send_response(cur);
}

/* =================================================================
 * Main dispatch
 * ================================================================ */
void handle_command(const command_frame_t *frame)
{
    uint32_t payload;
    memcpy(&payload, frame->payload, 4);   /* little-endian */

    switch (frame->command) {
    case CMD_EMIT:          handle_emit(payload);        break;
    case CMD_READ_FEEDBACK: handle_read_feedback();      break;
    case CMD_RESONANCE:     handle_resonance(payload);   break;
    case CMD_LASER_OFF:     handle_laser_off(payload);   break;
    case CMD_SET_TEMP:      handle_set_temp(payload);    break;
    default:                send_response(RESP_ERR_UNKNOWN); break;
    }
}

/* =================================================================
 * Initialise all laser channels
 * ================================================================ */
static void lasers_init(void)
{
    /* PIO0: lasers 0..3 (R, G, B, IR) */
    static const uint gpio0[] = { PIN_LASER_R, PIN_LASER_G,
                                  PIN_LASER_B, PIN_LASER_IR };
    for (int i = 0; i < 4; i++) {
        laser_channel_init(&g_lasers[i], pio0, i, gpio0[i]);
    }

    /* PIO1: laser 4 (UV) */
    laser_channel_init(&g_lasers[LASER_UV], pio1, 0, PIN_LASER_UV);
}

/* =================================================================
 * Core 1 – background monitoring loop
 * ================================================================ */
void core1_main(void)
{
    uint32_t tick = 0;

    while (1) {
        tight_loop_contents();

        /* Every ~100 ms read the crystal temperature and log it
         * (in a real system this drives a PID to control a heater). */
        if (tick++ % 10000 == 0) {
            uint32_t t = adc_read_temp_mdeg();
            /* The temperature is available – a real PID loop would
             * use g_target_temp_mdeg + t to adjust heater PWM. */
            (void)t;
        }
    }
}

/* =================================================================
 * Main
 * ================================================================ */
int main(void)
{
    /* System init */
    stdio_init_all();
    clocks_init();

    /* Hardware init */
    uart_init(uart0, UART_BAUD);
    gpio_set_function(PIN_UART_TX, GPIO_FUNC_UART);
    gpio_set_function(PIN_UART_RX, GPIO_FUNC_UART);
    adc_init_all();

    /* PIO init */
    lasers_init();
    photonic_uart_rx_init(&g_uart, pio1, 1, PIN_UART_RX, UART_BAUD);

    /* DMA ring buffer for UART RX */
    ring_init();
    dma_uart_init();

    /* Launch core 1 for background monitoring */
    multicore_launch_core1(core1_main);

    /* ---------- Main command loop ---------- */
    command_frame_t frame;

    while (1) {
        /* Try to read a 5-byte command frame from the ring buffer */
        if (ring_read_bytes((uint8_t *)&frame, sizeof(frame))) {
            handle_command(&frame);
        }
    }

    return 0;
}
