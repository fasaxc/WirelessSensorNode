/*
 * Copyright (c) 2012, Shaun Crampton
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without 
 * modification, are permitted provided that the following conditions are met:
 * 
 * Redistributions of source code must retain the above copyright notice, this 
 * list of conditions and the following disclaimer.
 * 
 * Redistributions in binary form must reproduce the above copyright notice, 
 * this list of conditions and the following disclaimer in the documentation 
 * and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" 
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE 
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE 
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE 
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR 
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF 
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN 
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) 
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE 
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <avr/common.h>
#include <avr/io.h>
#include <util/delay.h>
#include "sensor_node.h"
#include <avr/sleep.h>
#include <avr/interrupt.h>
#include <avr/wdt.h>

#define NODE_ID 3

#define TX_PORT PORTB
#define TX_TOGGLE_REG PINB
#define TX_PIN PB3

#define TEMP_SENSE_POWER_PORT PORTB
#define TEMP_SENSE_POWER_PIN PB0

#define RADIO_POWER_PORT PORTB
#define RADIO_POWER_PIN PB1

#define TEMP_SENSE_INPUT_DIG_PIN PB4
#define TEMP_SENSE_INPUT_AN_PIN 2

void transmit(void);

#define HALF_BIT_DELAY_TIME_MILLIS 0.5

static volatile char adc_done;

ISR(ADC_vect)
{
    adc_done = 1;
}

EMPTY_INTERRUPT(WDT_vect);

static uint8_t rand_byte();
static void mix_rand(uint8_t x);

inline static void do_adc_conversion() {
    adc_done = 0;
    set_sleep_mode(SLEEP_MODE_ADC);
    sleep_mode();
    while (!adc_done) {
        // Just in case we were woken by a different interrupt.
    }
    mix_rand((uint8_t)ADCW);
}

inline static uint16_t read_temperature() {
    // Power up the temperature sensor.
    TEMP_SENSE_POWER_PORT |= _BV(TEMP_SENSE_POWER_PIN);

    // Configure the ADC.
    ADMUX = _BV(REFS0) | /* 1.1V reference */
            TEMP_SENSE_INPUT_AN_PIN /* Pin selection */;
    ADCSRA = _BV(ADEN) | /* Enable ADC */
             _BV(ADIE) | /* Enable completion interrupt. */
             6           /* Select CLK/32 prescaler. */;

    // Trigger a conversion but discard the result because the first conversion
    // can be garbage.
    do_adc_conversion();

    // Use the second conversion.  Kick it off by entering sleep mode.
    do_adc_conversion();
    uint16_t result = ADCW;

    // Disable ADC.
    ADCSRA &= ~_BV(ADEN);

    // Power down the temp sensor.
    TEMP_SENSE_POWER_PORT &= ~_BV(TEMP_SENSE_POWER_PIN);

    return result;
}

#define WDT_DURATION 2 /* Seconds */
#define SLEEP_TIME 234 /* Seconds */

static void deep_sleep()
{
    // Select the deepest sleep mode
    set_sleep_mode(SLEEP_MODE_PWR_DOWN);

    // Reset the watchdog timer to avoid a race.
    wdt_reset();
    // Enable the watchdog interrupt, or we won't wake up again!
    WDTCR |= _BV(WDTIE);

    // The maximum WDT timeout is too short so we must loop to get an adequate
    // delay.  Add a bit of randomness to the length of the loop to avoid
    // collisions.
    uint8_t num_sleeps = ((SLEEP_TIME / WDT_DURATION) + (rand_byte() & 7));
    for (uint8_t i = 0; i < num_sleeps; ++i)
    {
        // Go to sleep until the watchdog timer pops.
        sleep_mode();
    }

    // Turn off the watchdog interrupt.
    WDTCR &= ~_BV(WDTIE);
}

int main(void)
{
    // Make the watchdog timer cause an interrupt rather than system reset and
    // use C/256K prescaler.
    MCUSR &= ~_BV(WDRF);
    wdt_disable();
    WDTCR |= _BV(WDTIF) | _BV(WDP2) | _BV(WDP1) | _BV(WDP0);

    // Configure all pins as outputs except the temperature input pin and the
    // TX pin (which is tri-stated until we transmit).
    DDRB = 0xFF ^ _BV(TEMP_SENSE_INPUT_DIG_PIN) ^ _BV(TX_PIN);
    // Disable digital input buffer on the analog input pin.
    DIDR0 |= _BV(TEMP_SENSE_INPUT_DIG_PIN);

    // Enable interrupts.
    sei();

    while (1)
    {
        manchester_union.manchester_packet.node_id = NODE_ID;
        manchester_union.manchester_packet.seq_no += 1;
        manchester_union.manchester_packet.reading_type = READING_TYPE_TEMP;
        manchester_union.manchester_packet.reading = read_temperature();
        transmit();
        deep_sleep();
    }

    return (1); // should never happen
}

// Transmit one byte using manchester encoding.  Sends LSB first.
void transmit_byte(uint8_t b)
{
    for (char j = 0; j < 8; j++)
    {
        if (b & 1)
        {
            // Transmit a 1 bit.  High then low.
            TX_PORT |= (char)_BV(TX_PIN);
        }
        else
        {
            // Transmit a 0 bit.  Low then high.
            TX_PORT &= (char)~_BV(TX_PIN);
        }
        _delay_ms(HALF_BIT_DELAY_TIME_MILLIS);
        // Transmit the other half of the bit.
        TX_PORT ^= (char)_BV(TX_PIN);
        _delay_ms(HALF_BIT_DELAY_TIME_MILLIS);

        b >>= 1;
    }
}

inline void transmit(void)
{
    // Power up radio.
    RADIO_POWER_PORT |= _BV(RADIO_POWER_PIN);
    DDRB |= _BV(TX_PIN);
    _delay_ms(0.1);

    manchester_union.manchester_packet.checksum = calculate_checksum(&manchester_union.manchester_packet);

    // Transmit preamble
    for (int i = 4; i > 0; i--) {
        transmit_byte(0xFF);
    }
    transmit_byte(0x7F);

    // Transmit data
    int len = sizeof(manchester_union);
    for (int i = 0; i < len; i++)
    {
        char b = manchester_union.manchester_data[i];
        transmit_byte(b);
    }
    // Generate one final transition
    TX_PORT ^= (char)_BV(TX_PIN);
    _delay_ms(HALF_BIT_DELAY_TIME_MILLIS);

    // Then disable the transmitter
    RADIO_POWER_PORT &= ~_BV(RADIO_POWER_PIN);

    // And tri-state the TX pin
    DDRB &= (char)~_BV(TX_PIN);
    TX_PORT &= (char)~_BV(TX_PIN);
}

static uint16_t lfsr_state;
static uint8_t rand_byte()
{
    if (!lfsr_state)
    {
        // Can't allow LFSR to be all 0, it won't recover.
        lfsr_state = 0xDEAD;
    }
    for (uint8_t i = 8; i>0; i--)
    {
        uint8_t new_bit = 0;
        if (lfsr_state & (1 << 15)) {
            new_bit = !new_bit;
        }
        if (lfsr_state & (1 << 13)) {
            new_bit = !new_bit;
        }
        if (lfsr_state & (1 << 12)) {
            new_bit = !new_bit;
        }
        if (lfsr_state & (1 << 10)) {
            new_bit = !new_bit;
        }
        lfsr_state <<= 1;
        lfsr_state |= new_bit;
    }
    return (uint8_t)lfsr_state;
}

static void mix_rand(uint8_t x)
{
    if (x & 1) {
        uint8_t tmp = (lfsr_state & (1 << 15)) >> 15;
        uint8_t tmp2 = (lfsr_state & (1 << 3)) >> 3;
        if (tmp != tmp2)
        {
            lfsr_state ^= 1 << 3;
            lfsr_state ^= 1 << 15;
        }
    }
}
