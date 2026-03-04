/**
 * Copyright (c) 2024 Vesa-Pekka Palmu, Pihlaja Voipio
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Notes from Pihlaja:
 *
 *  This version is a trimmed down and butchered version of depili's original,
 * merged back into a single source file and turned all static with nothing but
 * roe_init() and roe_draw_image() exposed to the global namespace.
 *
 * If you want to play with how the panels work, you should probably get the
 * original code with some additional neat tricks; this version removes anything
 * not required for straight framebuffer dumps in order to minimize RAM usage.
 *
 */

#include <stdio.h>
#include <math.h>

#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "hardware/pio.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

#include "gclock.pio.h"

// Pins used
#define PIN_R2  0
#define PIN_R1  1
#define PIN_G2  2
#define PIN_G1  3
#define PIN_B2  4
#define PIN_B1  5
#define PIN_DCLK 6
#define PIN_LE 7

#define PIN_OE 8

#define PIN_A 10
#define PIN_B 11
#define PIN_C 12
#define PIN_D 13
#define DATA_MASK   0x7F

// GCLOCK divider down from Pico's 125MHz clock
#define GCLOCK_DIV 6

// this is how many times we pulse the pin before IRQ
#define GCLOCK_COUNT 513

// Panel size
#define SCANLINES 11
#define CHAIN 22
#define PIXELS 88
#define CHAIN_BITS 16 * CHAIN

// Driver command pulse lengths
#define CMD_VSYNC 2
#define CMD_WRITE_CMD1 4
#define CMD_ERROR_DETECT 7
#define CMD_WRITE_CMD2 8
#define CMD_RESET 10
#define CMD_PRE 14

#define CURRENT_GAIN_DEFAULT 0b0000000000101011
#define CURRENT_GAIN_LOW     0b0000000000000000
#define GCLK_MULT            0b0000000001000000
#define PWM_13BIT            0b0000000010000000
#define MUX_11               0b0000101000000000
#define GHOST_CANCEL         0b1000000000000000
#define CONF2_DEFAULT        0b0001000000010000
#define DOUBLE_FRAMERATE     0b0000010000000000
#define DIM_COMP_35          0b0000000000001110
#define DIM_COMP_5           0b0000000000000010

#define MAX_SRGB 255
#define MAX_LINEAR 65535

// This holds a lookup table from sRGB "gamma-space" to PWM values
static uint16_t gamma_lookup[256];

static const uint8_t allPins[] = {
    PIN_R1, PIN_R2, PIN_G1, PIN_G2, PIN_B1, PIN_B2,
    PIN_A, PIN_B, PIN_C, PIN_D, PIN_LE, PIN_DCLK
};
static const uint8_t addrPins[] = { PIN_A, PIN_B, PIN_C, PIN_D };
static const uint8_t dataPins[] = { PIN_R1, PIN_G1, PIN_B1, PIN_R2, PIN_G2, PIN_B2 };

static const uint16_t configRegister1 = CURRENT_GAIN_LOW | MUX_11;
static const uint16_t configRegister2 = CONF2_DEFAULT | DIM_COMP_5;

static inline void dataDelay() {
#ifdef PICO_RP2350
#define DELAY_ITER 4
#else
#define DELAY_ITER 2
#endif
    // this seems like a more reliable delay(?)
    for(volatile int i = 0; i < DELAY_ITER; ++i) {}
}

// program offset and state machine ID
static int gclock_offset = -1;
static int gclock_sm = -1;

static volatile uint8_t gclock_addr= 0;
static volatile bool vsyncRequest = false;

static inline void incrementAddress() {
	gclock_addr= (gclock_addr+ 1) % SCANLINES;
}

// Assume all address pins start from PIN_A
static inline void writeAddress() {
	const uint32_t mask = 0x0F << PIN_A;
	uint32_t set = gclock_addr<< PIN_A;

	gpio_put_masked(mask, set);
}

static void sendCommand(uint8_t length) {
	gpio_put(PIN_LE, 0);
	gpio_put(PIN_DCLK, 0);
	dataDelay();
	gpio_put(PIN_LE, 1);
	dataDelay();

	for (int i = 0; i < length; i++) {
		gpio_put(PIN_DCLK, 1);
		dataDelay();
		gpio_put(PIN_DCLK, 0);
		dataDelay();
	}
	gpio_put(PIN_LE, 0);
}

static void sendReset() {
	uint32_t irq_save = save_and_disable_interrupts();
	restore_interrupts(irq_save);

	sendCommand(CMD_RESET);

	irq_save = save_and_disable_interrupts();
	restore_interrupts(irq_save);
}

static void writeCommand1(uint16_t data) {
	// Write the pre-active
	for (uint i = 0; i < sizeof(dataPins); i++) {
		gpio_put(dataPins[i], 0);
	}

	sendCommand(CMD_PRE);

	// Write the command register
	for (int i = 0; i < CHAIN_BITS; i++) {
		if (i == (CHAIN_BITS - CMD_WRITE_CMD1)) {
			// Need to put LE high CMD_WRITE_CMD1 cycles before the end...
			gpio_put(PIN_LE, 1);
		}

		uint8_t bit = 1&(configRegister1 >> (15 - (i % 16)));

		for (uint j = 0; j < sizeof(dataPins); j++) {
			gpio_put(dataPins[j], bit);
		}

		gpio_put(PIN_DCLK, 1);
		dataDelay();
		gpio_put(PIN_DCLK, 0);
		dataDelay();
	}
	gpio_put(PIN_LE, 0);
}


static void writeCommand2(uint16_t data) {
	// Write the pre-active
	for (uint i = 0; i < sizeof(dataPins); i++) {
		gpio_put(dataPins[i], 0);
	}

	sendCommand(CMD_PRE);

	// Write the command register
	for (int i = 0; i < CHAIN_BITS; i++) {
		if (i == (CHAIN_BITS - CMD_WRITE_CMD2)) {
			// Need to put LE high CMD_WRITE_CMD1 cycles before the end...
			gpio_put(PIN_LE, 1);
		}

		uint8_t bit = 1&(configRegister1>>(15 - (i % 16)));

		for (uint j = 0; j < sizeof(dataPins); j++) {
			gpio_put(dataPins[j], bit);
		}

		gpio_put(PIN_DCLK, 1);
		dataDelay();
		gpio_put(PIN_DCLK, 0);
		dataDelay();
	}
	gpio_put(PIN_LE, 0);
}

static inline void vsync() {
	vsyncRequest = true;
	while(vsyncRequest) {
	}
}

// interrupt handler
static void gclock_handler(void) {
	pio_interrupt_clear(pio0, 0);

	if (vsyncRequest) {
		sendCommand(CMD_VSYNC);
		vsyncRequest = false;
		gclock_addr= SCANLINES - 1;
	}

	incrementAddress();
	writeAddress();

	dataDelay();
	dataDelay();
	dataDelay();

	// tell PIO to start another cycle
	pio_sm_put_blocking(pio0, gclock_sm, GCLOCK_COUNT-1);
}

static void gclock_init() {
	// add program from gclock.pio.h to PIO0
	gclock_offset = pio_add_program(pio0, &gclock_program);

	// claim a state-machine
	gclock_sm = pio_claim_unused_sm(pio0, true);

	// set the pin as PIO pin
	pio_gpio_init(pio0, PIN_OE);
	// direction is output
	pio_sm_set_consecutive_pindirs(pio0, gclock_sm, PIN_OE, 1, true);

	// get default config (in gclock.pio.h by PioAsm)
	pio_sm_config c = gclock_program_get_default_config(gclock_offset);

	// see GCLOCK_DIV above
	sm_config_set_clkdiv_int_frac(&c, GCLOCK_DIV, 0);

	// the clock is output from side-set pin
	sm_config_set_sideset_pins(&c, PIN_OE);

	// set interrupt handler
	irq_set_exclusive_handler(PIO0_IRQ_0, &gclock_handler);
	irq_set_enabled(PIO0_IRQ_0, true);

	// enable in PIO
	pio_set_irq0_source_enabled(pio0, pis_interrupt0 + gclock_sm, true);

	// Load config and jump to start
	pio_sm_init(pio0, gclock_sm, gclock_offset, &c);

	// start .. our PIO starts by stalling on FIFO
	pio_sm_set_enabled(pio0, gclock_sm, true);

	// fire off GCLOCK_COUNT pulses
	pio_sm_put_blocking(pio0, gclock_sm, GCLOCK_COUNT-1);
}

static inline void zeroDataPins() {
	const uint32_t mask = 0x7F;
	gpio_put_masked(mask, 0);
}


// Do the chain data-shuffling in sRGB format with separate RGB arrays
static void getChainDataSRGB(uint32_t * bitmap, uint scanline, uint led,
    uint8_t dataR[44], uint8_t dataG[44], uint8_t dataB[44])
{
	// Row delta from current led
	uint8_t ledRow = (led / 8) ? 0 : 11;
	// Led delta in columns
	uint8_t ledColumn = led % 8;

	int row = scanline + ledRow;

	if (ledRow == 11)
    {
		ledColumn = (7 - ledColumn);
	}

	// Get every 8th pixel
	int start = 33;

	for (int j = 0; j < 4; j++)
    {
        uint32_t * bitmap_row = bitmap + 88*row;
        
		for (int i = 0; i < 11; i++)
        {
            uint8_t * pixel = (uint8_t*) (bitmap_row + (8*i + ledColumn));
			dataR[start+i] = pixel[0];
			dataG[start+i] = pixel[1];
			dataB[start+i] = pixel[2];
		}

		row += 22;
		start -= 11;
	}
}

// This writes chain-data with LUT-based SRGB conversion on the fly
static void writeChainSRGB(
    uint8_t dataR[44], uint8_t dataG[44], uint8_t dataB[44])
{
    int data_len = CHAIN_BITS;
	for (int ic = 0; ic < CHAIN; ic++)
    {
        uint16_t r1 = gamma_lookup[dataR[ic]];
        uint16_t g1 = gamma_lookup[dataG[ic]];
        uint16_t b1 = gamma_lookup[dataB[ic]];
        
        uint16_t r2 = gamma_lookup[dataR[ic+22]];
        uint16_t g2 = gamma_lookup[dataG[ic+22]];
        uint16_t b2 = gamma_lookup[dataB[ic+22]];
        
		for (int bit = 15; bit >= 0; bit--)
        {
			if (data_len == 1)
            {
				gpio_put(PIN_LE, 1);
			}
			gpio_put(PIN_R1, 0x1&(r1>>bit));
			gpio_put(PIN_G1, 0x1&(g1>>bit));
			gpio_put(PIN_B1, 0x1&(b1>>bit));

			gpio_put(PIN_R2, 0x1&(r2>>bit));
			gpio_put(PIN_G2, 0x1&(g2>>bit));
			gpio_put(PIN_B2, 0x1&(b2>>bit));

			gpio_put(PIN_DCLK, 0);
			dataDelay();
			gpio_put(PIN_DCLK, 1);

			data_len--;
		}
	}
	gpio_put(PIN_LE, 0);
	dataDelay();
}

// Function to convert a single 8-bit sRGB value to linear
// extra gamma should be 1.0 for straight sRGB, but additional factor
// but our panels are a little bright-heavy so it can be bumped a bit
static uint16_t sRGB_to_linear(uint8_t srgb, float extraGamma) {
	float normalized = srgb / 255.0f;  // Normalize to [0, 1]

	// Convert from gamma corrected sRGB to linear RGB
	if (normalized <= 0.04045f) {
		normalized /= 12.92f;
	} else {
		normalized = powf((normalized + 0.055f) / 1.055f, 2.4f);
	}

    normalized = powf(normalized, extraGamma);

	// Scale to 16-bit range [0, 65535]
	return (uint16_t)(normalized * MAX_LINEAR);
}

void roe_init() {

    // build a lookup so we can do this faster
    for (uint i = 0; i < 0x100; ++i)
    {
        // straight sRGB is a bit bright heavy
        // so add an extra gamma factor.. ~1.15 seems reasonable
        gamma_lookup[i] = sRGB_to_linear(i, 1.1f);
    }

	for (uint i = 0; i < sizeof(allPins); i++)
    {
		gpio_init(allPins[i]);
		gpio_set_dir(allPins[i], GPIO_OUT);
	}

	gpio_put(PIN_OE, 1);

	printf("panel: Sending reset\n");
	sendReset();

    // this works before gclock is running, right?
	printf("panel: Writing command register\n");
	writeCommand1(configRegister1);
	sleep_ms(10);

	gclock_init();
}


// this is perhaps not the cleanest but .. whatever -mystran
void roe_draw_image(uint32_t * image)
{

	uint8_t dataR[44];
	uint8_t dataG[44];
	uint8_t dataB[44];

	zeroDataPins();
	dataDelay();

	for (int scanline = 0; scanline < SCANLINES; scanline++)
    {
		// Send all 16 leds per IC per scanline
		for (int led = 0; led < 16; led++)
        {
			getChainDataSRGB(image, scanline, led, dataR, dataG, dataB);
			writeChainSRGB(dataR, dataG, dataB);
		}
	}
    
    vsync();
}
