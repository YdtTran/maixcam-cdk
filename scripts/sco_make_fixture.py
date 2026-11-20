#!/usr/bin/env python3
"""Generate the exact fixed SCO test fixture using the installed Flite voice."""

import argparse
import ctypes
import hashlib
from pathlib import Path
import tempfile
import wave

from sco_experiment import FIXTURE_HASH


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    if args.output.exists():
        raise ValueError('Refusing to overwrite the fixture')
    flite = ctypes.CDLL('libflite.so.1', mode=ctypes.RTLD_GLOBAL)
    voice = ctypes.CDLL('libflite_cmu_us_kal.so.1')
    flite.flite_init()
    voice.register_cmu_us_kal.restype = ctypes.c_void_p
    flite.flite_text_to_wave.argtypes = [ctypes.c_char_p, ctypes.c_void_p]
    flite.flite_text_to_wave.restype = ctypes.c_void_p
    flite.cst_wave_save_riff.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    spoken = flite.flite_text_to_wave(
        b'One, two, three, four, five. Testing Bluetooth audio. Six, seven, eight, nine, ten.',
        voice.register_cmu_us_kal())
    if not spoken:
        raise RuntimeError('Flite synthesis failed')
    with tempfile.TemporaryDirectory() as directory:
        source = Path(directory) / 'source.wav'
        if flite.cst_wave_save_riff(spoken, bytes(source)) != 0:
            raise RuntimeError('Flite could not write audio')
        with wave.open(str(source), 'rb') as wav:
            if (wav.getnchannels(), wav.getsampwidth(), wav.getframerate()) != (1, 2, 8000):
                raise RuntimeError('Flite voice is not 8 kHz mono S16')
            pcm = wav.readframes(wav.getnframes())
        if len(pcm) > 240000:
            raise RuntimeError('Spoken sentence exceeds 15 seconds')
        result = Path(directory) / 'fixture.wav'
        with wave.open(str(result), 'wb') as wav:
            wav.setnchannels(1)
            wav.setsampwidth(2)
            wav.setframerate(8000)
            wav.writeframes(pcm + bytes(240000 - len(pcm)))
        if hashlib.sha256(result.read_bytes()).hexdigest() != FIXTURE_HASH:
            raise RuntimeError('Flite output differs from the established fixture')
        with args.output.open('xb') as output:
            output.write(result.read_bytes())
    print(FIXTURE_HASH)


if __name__ == '__main__':
    main()
