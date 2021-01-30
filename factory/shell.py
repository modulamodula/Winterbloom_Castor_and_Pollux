import readline

from libgemini import gemini, reference_calibration

readline.parse_and_bind("tab: complete")

gem = gemini.Gemini()
gem.enter_calibration_mode()

settings = None


def _frequency_to_timer_period(frequency):
    return round(((8_000_000 / 1) / frequency) - 1)


def _midi_note_to_frequency(note):
    return pow(2, (note - 69) / 12) * 440


def _estimate_charge_code(frequency):
    return min(round(10 * frequency / 5_000 / 3.3 * 4095), 4095)


midi_note = 11


while True:
    cmd, *vals = input("> ").strip().split()
    vals = [int(val) for val in vals]
    val = vals[0] if vals else None

    if cmd == "note":
        freq = vals[0]
        charge_code = vals[1]
        period = _frequency_to_timer_period(freq)
        print(f"Period: {period}")
        gem.set_period(0, period)
        gem.set_dac(0, charge_code, 0)

    if cmd == "next":
        midi_note += 2
        freq = _midi_note_to_frequency(midi_note)
        period = _frequency_to_timer_period(freq)
        charge_code = _estimate_charge_code(freq)
        print(f"Note: {midi_note}, Freq: {freq}, Charge code: {charge_code}")
        gem.set_period(0, period)
        gem.set_dac(0, charge_code, 0)
        gem.set_period(1, period)
        gem.set_dac(2, reference_calibration.pollux[period], 1)

    elif cmd == "read_adc":
        print(gem.read_adc(val))

    elif cmd == "set_dac":
        if len(vals) < 2:
            print("requires channel, value, and vref")
        gem.set_dac(vals[0], vals[1], vref=vals[2])

    elif cmd == "set_freq":
        if len(vals) < 2:
            print("requires channel and period")
        gem.set_period(vals[0], vals[1])

    elif cmd == "set_adc_gain":
        gem.set_adc_gain_error_int(val)

    elif cmd == "set_adc_error":
        gem.set_adc_offset_error(val)

    elif cmd == "load_settings":
        settings = gem.read_settings()
        gem_settings = settings
        print(settings)

    elif cmd == "reset_settings":
        gem.reset_settings()
        print(settings)

    elif cmd == "save_settings":
        gem.save_settings(settings)

    elif cmd.startswith("$"):
        eval(cmd[1:])

    else:
        print("unknown command")
