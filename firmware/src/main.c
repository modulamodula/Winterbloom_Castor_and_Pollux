#include "fix16.h"
#include "gem.h"
#include "printf.h"
#include "sam.h"

static struct gem_settings settings;
static uint32_t adc_results[GEM_IN_COUNT];
static struct gem_adc_errors knob_errors;
static struct gem_button hard_sync_button = {.port = GEM_HARD_SYNC_BUTTON_PORT, .pin = GEM_HARD_SYNC_BUTTON_PIN};
static bool hard_sync = false;

static struct lfo_state {
    fix16_t phase;
    uint32_t last_update;
} lfo = {.phase = F16(0)};

struct oscillator_state {
    struct gem_voice_params params;
    struct gem_smoothie_state smooth;
    fix16_t knob_range;
};

static struct oscillator_state castor = {
    .params = {},
    .smooth =
        {
            ._lowpass1 = F16(0),
            ._lowpass2 = F16(0),
        },
};

static struct oscillator_state pollux = {
    .params = {},
    .smooth =
        {
            ._lowpass1 = F16(0),
            ._lowpass2 = F16(0),
        },
};

static void init() {
    /* Configure clocks. */
    gem_clocks_init();

    /* Configure systick */
    gem_systick_init();

    /* Initialize NVM */
    gem_nvm_init();

    /* Initialize Random Number Generators */
    gem_random_init(gem_serial_number_low());

    /* Load settings */
    gem_settings_load(&settings);
    gem_settings_print(&settings);

    knob_errors = (struct gem_adc_errors){.offset = settings.knob_offset_corr, .gain = settings.knob_gain_corr};

    castor.knob_range = fix16_sub(settings.pollux_knob_max, settings.pollux_knob_min);
    castor.smooth.initial_gain = settings.smooth_initial_gain;
    castor.smooth.sensitivity = settings.smooth_sensitivity;

    pollux.knob_range = fix16_sub(settings.pollux_knob_max, settings.pollux_knob_min);
    pollux.smooth.initial_gain = settings.smooth_initial_gain;
    pollux.smooth.sensitivity = settings.smooth_sensitivity;

    /* Load the LUT table for DAC codes. */
    gem_load_dac_codes_table();

    /* Initialize USB. */
    gem_usb_init();

    /* Initialize MIDI interface. */
    gem_register_sysex_commands();

    /* Enable i2c bus for communicating with the DAC. */
    gem_i2c_init();

    /* Enable spi bus, Dotstars, and LED animations. */
    gem_spi_init();
    gem_dotstar_init(settings.led_brightness);
    gem_led_animation_init();

    /* Configure the ADC and channel scanning. */
    gem_adc_init(settings.adc_offset_corr, settings.adc_gain_corr);

    for (size_t i = 0; i < GEM_IN_COUNT; i++) { gem_adc_init_input(&gem_adc_inputs[i]); }
    gem_adc_start_scanning(gem_adc_inputs, GEM_IN_COUNT, adc_results);

    /* Configure the timers/PWM generators. */
    gem_pulseout_init();

    /* Configure input for the hard sync button. */
    gem_button_init(&hard_sync_button);
}

static void loop() {
    /* Castor's basic pitch determination algorithm is

        1.0v + (CV in * 6.0v) + ((CV knob * 2.0) - 1.0)

    */
    uint16_t castor_pitch_cv_code = (4095 - adc_results[GEM_IN_CV_A]);
    fix16_t castor_pitch_cv_value = fix16_div(fix16_from_int(castor_pitch_cv_code), F16(4095.0));

    fix16_t castor_pitch_cv = fix16_add(GEM_CV_BASE_OFFSET, fix16_mul(GEM_CV_INPUT_RANGE, castor_pitch_cv_value));

    fix16_t castor_pitch_knob_code =
        gem_adc_correct_errors(fix16_from_int(4095 - adc_results[GEM_IN_CV_A_POT]), knob_errors);
    fix16_t castor_pitch_knob_value = fix16_div(castor_pitch_knob_code, F16(4095.0));
    fix16_t castor_pitch_knob =
        fix16_add(settings.castor_knob_min, fix16_mul(castor.knob_range, castor_pitch_knob_value));

    castor_pitch_cv = fix16_add(castor_pitch_cv, castor_pitch_knob);

    /* Pollux is the "follower", so its pitch determination is based on whether or not
        it has input CV.

        If CV in == ~0, then it follows Castor:

            1.0v + (Castor CV * 6.0v) + ((CV knob * 2.0) - 1.0)

        Else it uses the input CV:

            1.0v + (CV in * 6.0v) + ((CV knob * 2.0) - 1.0)

        This means that if there's no pitch input, then Pollux is the same pitch as
        Castor but fine-tuned up or down using the CV knob. If there is a pitch CV
        applied, the the knob just acts as a normal fine-tune.
    */
    fix16_t pollux_pitch_cv = castor_pitch_cv;

    uint16_t pollux_pitch_cv_code = (4095 - adc_results[GEM_IN_CV_B]);

    if (pollux_pitch_cv_code > settings.pollux_follower_threshold) {
        fix16_t pollux_pitch_cv_value = fix16_div(fix16_from_int(pollux_pitch_cv_code), F16(4095.0));
        pollux_pitch_cv = fix16_add(GEM_CV_BASE_OFFSET, fix16_mul(GEM_CV_INPUT_RANGE, pollux_pitch_cv_value));
    }

    fix16_t pollux_pitch_knob_code =
        gem_adc_correct_errors(fix16_from_int(4095 - adc_results[GEM_IN_CV_B_POT]), knob_errors);
    fix16_t pollux_pitch_knob_value = fix16_div(pollux_pitch_knob_code, F16(4095.0));
    fix16_t pollux_pitch_knob =
        fix16_add(settings.pollux_knob_min, fix16_mul(pollux.knob_range, pollux_pitch_knob_value));

    pollux_pitch_cv = fix16_add(pollux_pitch_cv, pollux_pitch_knob);

    /* Apply smoothing to input CVs. */
    castor_pitch_cv = gem_smoothie_step(&castor.smooth, castor_pitch_cv);
    pollux_pitch_cv = gem_smoothie_step(&pollux.smooth, pollux_pitch_cv);

    /*
        Calculate the chorus LFO and account for LFO in Pollux's pitch.
    */
    uint32_t now = gem_get_ticks();
    uint32_t lfo_delta = now - lfo.last_update;

    if (lfo_delta > 0) {
        lfo.phase += fix16_mul(fix16_div(settings.chorus_frequency, F16(1000.0)), fix16_from_int(lfo_delta));

        if (lfo.phase > F16(1.0))
            lfo.phase = fix16_sub(lfo.phase, F16(1.0));

        lfo.last_update = now;
    }

    uint16_t lfo_amount_code = (4095 - adc_results[GEM_IN_CHORUS_POT]);
    fix16_t lfo_amount = fix16_div(fix16_from_int(lfo_amount_code), F16(4095.0));

    fix16_t chorus_lfo_mod = fix16_mul(settings.chorus_max_intensity, fix16_mul(lfo_amount, gem_triangle(lfo.phase)));
    pollux_pitch_cv = fix16_add(pollux_pitch_cv, chorus_lfo_mod);

    /*
        Limit pitch CVs to fit within the parameter table's max value.
    */
    if (castor_pitch_cv < F16(0.0))
        castor_pitch_cv = F16(0.0);
    if (pollux_pitch_cv < F16(0.0))
        pollux_pitch_cv = F16(0.0);
    if (castor_pitch_cv > F16(7.0))
        castor_pitch_cv = F16(7.0);
    if (pollux_pitch_cv > F16(7.0))
        pollux_pitch_cv = F16(7.0);

    /*
        PWM inputs.
    */

    uint16_t castor_duty = 4095 - adc_results[GEM_IN_DUTY_A_POT];
    castor_duty += 4095 - adc_results[GEM_IN_DUTY_A];
    if (castor_duty > 4095)
        castor_duty = 4095;
    uint16_t pollux_duty = 4095 - adc_results[GEM_IN_DUTY_B_POT];
    pollux_duty += 4095 - adc_results[GEM_IN_DUTY_B];
    if (pollux_duty > 4095)
        pollux_duty = 4095;

    /*
        Check for hard sync.
    */
    gem_button_update(&hard_sync_button);

    if (gem_button_tapped(&hard_sync_button)) {
        hard_sync = !hard_sync;
        if (hard_sync) {
            gem_pulseout_hard_sync(true);
            gem_led_animation_set_mode(GEM_LED_MODE_HARD_SYNC);
        } else {
            gem_pulseout_hard_sync(false);
            gem_led_animation_set_mode(GEM_LED_MODE_NORMAL);
        }
    }

    /*
        Calculate the final voice parameters given the input CVs.
    */
    gem_voice_params_from_cv(
        gem_voice_voltage_and_period_table,
        gem_voice_dac_codes_table,
        gem_voice_param_table_len,
        castor_pitch_cv,
        &castor.params);
    gem_voice_params_from_cv(
        gem_voice_voltage_and_period_table,
        gem_voice_dac_codes_table,
        gem_voice_param_table_len,
        pollux_pitch_cv,
        &pollux.params);

    /*
        Update timers.
    */

    /* Disable interrupts while changing timers, as any interrupt here could mess them up. */
    __disable_irq();
    gem_pulseout_set_period(0, castor.params.voltage_and_period.period);
    gem_pulseout_set_period(1, pollux.params.voltage_and_period.period);
    __enable_irq();

    /*
        Update DACs.
    */
    gem_mcp_4728_write_channels(
        (struct gem_mcp4728_channel){.value = castor.params.dac_codes.castor, .vref = 1},
        (struct gem_mcp4728_channel){.value = castor_duty},
        (struct gem_mcp4728_channel){.value = pollux.params.dac_codes.pollux, .vref = 1},
        (struct gem_mcp4728_channel){.value = pollux_duty});
}

int main(void) {
    init();

    while (1) {
        gem_usb_task();
        gem_midi_task();
        gem_led_animation_step();

        if (gem_adc_results_ready()) {
            loop();
        }
    }

    return 0;
}