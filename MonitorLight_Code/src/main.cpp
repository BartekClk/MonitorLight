#define F_CPU 8000000UL

#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <avr/eeprom.h>
#include <avr/power.h>
#include <stdbool.h>

// --- DEFINICJE PINÓW ---
#define PIN_PWM     PB0  // Pin 5: Wyjście PWM (OC0A)
#define PIN_ENC_SW  PB1  // Pin 6: Przycisk enkodera (SW)
#define PIN_ENC_A   PB3  // Pin 2: Enkoder A (CLK)
#define PIN_ENC_B   PB4  // Pin 3: Enkoder B (DT)

// --- PARAMETRY REGULACJI ---
#define BRIGHTNESS_STEPS 32
#define FADE_INTERVAL_MS 2     // Czas między krokami płynnego przejścia (2 ms)
#define EEPROM_DELAY_MS  2000  // Zapis do EEPROM po 2 s bezczynności

// Tablica korekcji gamma CIE1931 (32 kroki jasności)
static const uint8_t PROGMEM gamma_lut[BRIGHTNESS_STEPS] = {
      1,   2,   3,   4,   6,   8,  10,  13,
     16,  20,  25,  30,  36,  43,  51,  60,
     70,  81,  93, 106, 120, 136, 153, 171,
    190, 210, 232, 255, 255, 255, 255, 255
};

// Tablica przejść kwadraturowych enkodera (eliminuje drgania styków)
static const int8_t PROGMEM enc_table[16] = {
     0, -1,  1,  0,
     1,  0,  0, -1,
    -1,  0,  0,  1,
     0,  1, -1,  0
};

// Adresy EEPROM
#define EEPROM_MAGIC_ADDR 0x00
#define EEPROM_LEVEL_ADDR 0x01
#define EEPROM_MAGIC_KEY  0xA5

// --- ZMIENNE WSPÓŁDZIELONE Z PRZERWANIEM ---
volatile int8_t  g_enc_delta   = 0;
volatile bool    g_btn_pressed = false;
volatile bool    g_tick_1ms    = false;

// --- OBSŁUGA PWM (Timer0 - 31.25 kHz) ---
static void pwm_init(void) {
    DDRB |= (1 << PIN_PWM);
    PORTB &= ~(1 << PIN_PWM);

    // Tryb Fast PWM (WGM01 | WGM00), preskaler = 1 (CS00)
    TCCR0A = (1 << WGM01) | (1 << WGM00);
    TCCR0B = (1 << CS00);
    OCR0A  = 0;
}

static void pwm_set(uint8_t duty) {
    if (duty == 0) {
        // Odłącz wyjście OC0A, aby uniknąć szpilki przy OCR0A = 0
        TCCR0A &= ~(1 << COM0A1);
        PORTB  &= ~(1 << PIN_PWM);
    } else {
        OCR0A   = duty;
        TCCR0A |= (1 << COM0A1);
    }
}

// --- BAZA CZASU 1 ms (Timer1) ---
static void timer1_init(void) {
    // Tryb CTC, preskaler = 64 -> 8 MHz / 64 = 125 kHz
    // 125 impulsów (OCR1C = 124) daje przerwanie dokładnie co 1 ms (1 kHz)
    TCCR1  = (1 << CTC1) | (1 << CS12) | (1 << CS11) | (1 << CS10);
    OCR1C  = 124;
    OCR1A  = 124;
    TIMSK |= (1 << OCIE1A);
}

// --- PRZERWANIE CO 1 ms: ODCZYT ENKODERA I PRZYCISKU ---
ISR(TIMER1_COMPA_vect) {
    g_tick_1ms = true;

    // 1. Dekodowanie kwadraturowe enkodera
    static uint8_t prev_ab    = 0x03;
    static int8_t  substep    = 0;

    uint8_t pin_state = PINB;
    uint8_t curr_ab   = 0;
    if (pin_state & (1 << PIN_ENC_A)) curr_ab |= 0x02;
    if (pin_state & (1 << PIN_ENC_B)) curr_ab |= 0x01;

    if (curr_ab != prev_ab) {
        uint8_t idx = (prev_ab << 2) | curr_ab;
        substep += (int8_t)pgm_read_byte(&enc_table[idx]);
        prev_ab = curr_ab;

        // Zarejestruj krok w pełnej pozycji spoczynkowej (oba wejścia w stanie wysokim)
        if (curr_ab == 0x03) {
            if (substep >= 2) {
                g_enc_delta++;
            } else if (substep <= -2) {
                g_enc_delta--;
            }
            substep = 0;
        }
    }

    // 2. Debouncing przycisku SW (filtr 25 ms)
    static uint8_t btn_cnt     = 0;
    static bool    btn_latched = false;

    if (!(pin_state & (1 << PIN_ENC_SW))) {
        if (btn_cnt < 25) {
            btn_cnt++;
            if (btn_cnt == 25 && !btn_latched) {
                g_btn_pressed = true;
                btn_latched   = true;
            }
        }
    } else {
        btn_cnt     = 0;
        btn_latched = false;
    }
}

// --- PĘTLA GŁÓWNA ---
int main(void) {
    // Wymuś taktowanie 8 MHz (wyłącza podział zegara CKDIV8)
    clock_prescale_set(clock_div_1);

    // Piny enkodera i przycisku jako wejścia z wewnętrznym podciąganiem Pull-Up
    DDRB  &= ~((1 << PIN_ENC_A) | (1 << PIN_ENC_B) | (1 << PIN_ENC_SW));
    PORTB |=  ((1 << PIN_ENC_A) | (1 << PIN_ENC_B) | (1 << PIN_ENC_SW));

    pwm_init();
    timer1_init();
    sei();

    // Odczyt ostatniej jasności z pamięci EEPROM
    uint8_t level = 15; // Domyślnie ~50%
    if (eeprom_read_byte((const uint8_t*)EEPROM_MAGIC_ADDR) == EEPROM_MAGIC_KEY) {
        uint8_t saved = eeprom_read_byte((const uint8_t*)EEPROM_LEVEL_ADDR);
        if (saved < BRIGHTNESS_STEPS) {
            level = saved;
        }
    }

    bool     lamp_on      = false;
    uint8_t  current_pwm  = 0;
    uint8_t  target_pwm   = pgm_read_byte(&gamma_lut[level]);
    uint8_t  fade_timer   = 0;
    uint16_t eeprom_timer = 0;
    bool     eeprom_dirty = false;

    while (1) {
        // 1. Kliknięcie przycisku (włącz / wyłącz)
        if (g_btn_pressed) {
            g_btn_pressed = false;
            lamp_on = !lamp_on;
        }

        // 2. Obrót enkodera (zmiana jasności)
        int8_t delta = 0;
        cli();
        delta = g_enc_delta;
        g_enc_delta = 0;
        sei();

        if (delta != 0) {
            if (!lamp_on && delta > 0) {
                // Obrót w prawo przy wyłączonej lampce włącza ją od minimum
                lamp_on = true;
                level   = 0;
            } else if (lamp_on) {
                int16_t next_level = (int16_t)level + delta;
                if (next_level < 0) next_level = 0;
                if (next_level >= BRIGHTNESS_STEPS) next_level = BRIGHTNESS_STEPS - 1;

                if ((uint8_t)next_level != level) {
                    level        = (uint8_t)next_level;
                    eeprom_dirty = true;
                    eeprom_timer = EEPROM_DELAY_MS;
                }
            }
        }

        // 3. Zadania czasowe wykonywane co 1 ms
        if (g_tick_1ms) {
            g_tick_1ms = false;

            // Wyznaczenie docelowego wypełnienia PWM
            target_pwm = lamp_on ? pgm_read_byte(&gamma_lut[level]) : 0;

            // Płynna zmiana jasności (Soft-Fade) co 2 ms
            fade_timer++;
            if (fade_timer >= FADE_INTERVAL_MS) {
                fade_timer = 0;
                if (current_pwm < target_pwm) {
                    current_pwm++;
                    pwm_set(current_pwm);
                } else if (current_pwm > target_pwm) {
                    current_pwm--;
                    pwm_set(current_pwm);
                }
            }

            // Opóźniony zapis do EEPROM (ochrona przed zużyciem komórek)
            if (eeprom_dirty) {
                if (eeprom_timer > 0) {
                    eeprom_timer--;
                } else {
                    eeprom_update_byte((uint8_t*)EEPROM_MAGIC_ADDR, EEPROM_MAGIC_KEY);
                    eeprom_update_byte((uint8_t*)EEPROM_LEVEL_ADDR, level);
                    eeprom_dirty = false;
                }
            }
        }
    }
}