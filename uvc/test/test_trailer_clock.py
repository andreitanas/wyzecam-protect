#!/usr/bin/env python3
"""Regression checks for AAC clocks in Protect's extended FLV trailers."""
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest

PARSER = pathlib.Path(__file__).resolve().parents[1] / "tools" / "flvdump.py"
HEADER = b"FLV\x01\x07\x00\x00\x00\x09\x00\x00\x00\x00"
SCRIPT = b"\x02\x00\x06onMpma\x03\x00\x00\x09"


def tag(kind, timestamp, body, rate, ticks=None):
    header = (bytes([kind]) + len(body).to_bytes(3, "big")
              + (timestamp & 0xffffff).to_bytes(3, "big")
              + bytes([timestamp >> 24]) + bytes(3))
    trailer = (b"\x00" + rate.to_bytes(3, "big") + bytes(4)
               + struct.pack(">Q", ticks if ticks is not None else timestamp * rate // 1000))
    return header + body + struct.pack(">I", len(body) + 11) + trailer


class TrailerClockTest(unittest.TestCase):
    def validate(self, stream):
        with tempfile.TemporaryDirectory() as directory:
            capture = pathlib.Path(directory) / "test.flv"
            capture.write_bytes(HEADER + stream)
            return subprocess.run(
                [sys.executable, "-I", str(PARSER), "--unifi", "-q", str(capture)],
                capture_output=True, text=True)

    def test_aac_uses_its_configured_rate(self):
        for config, rate in [(b"\x14\x08", 16000), (b"\x12\x88", 32000)]:
            with self.subTest(rate=rate):
                result = self.validate(tag(8, 0, b"\xaf\x00" + config, rate)
                                       + tag(8, 64, b"\xaf\x01\x01\x18\x20\x07", rate))
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_old_generic_audio_clock_is_rejected(self):
        result = self.validate(tag(8, 0, b"\xaf\x00\x12\x88", 11025))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("clock 11025, expected 32000 for audio", result.stderr)

    def test_explicit_aac_frequency(self):
        # audioObjectType=2, sample_frequency_index=15, frequency=32000,
        # channelConfiguration=1, then zero GASpecificConfig flags.
        bits = "00010" + "1111" + f"{32000:024b}" + "0001" + "000"
        config = int(bits, 2).to_bytes(5, "big")
        result = self.validate(tag(8, 0, b"\xaf\x00" + config, 32000))
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_script_and_audio_clocks_are_independent(self):
        # A script clock may be ahead of audio. That must not hide a real
        # regression within either track, including at equal clock rates.
        for config, rate in [(b"\x12\x88", 32000), (b"\x15\x08", 11025)]:
            with self.subTest(rate=rate):
                stream = tag(8, 0, b"\xaf\x00" + config, rate)
                stream += tag(18, 0, SCRIPT, 90000, ticks=90000)
                stream += tag(8, 100, b"\xaf\x01\x01\x18\x20\x07", rate)
                result = self.validate(stream)
                self.assertEqual(result.returncode, 0, result.stderr)
                result = self.validate(stream + tag(8, 200, b"\xaf\x01\x01\x18\x20\x07",
                                                    rate, ticks=0))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("audio wall clock went backwards", result.stderr)

    def test_zero_trailer_clock_is_reported(self):
        result = self.validate(tag(8, 0, b"\xaf\x00\x12\x88", 0))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("zero clock rate", result.stderr)

    def test_ticks_follow_the_clock_sync_base(self):
        # Protect dates frames by their 64-bit ticks, which UniFi's cameras
        # keep at (streamClockBase + tag timestamp) on each track's clock.
        # Only the low 32 bits of them is a camera up for 13 hours dated
        # whole wraps in the past, differently on each track.
        def sync(ts, base):
            num = lambda k, v: len(k).to_bytes(2, "big") + k + b"\x00" + struct.pack(">d", v)
            return (b"\x02\x00\x0bonClockSync\x03" + num(b"streamClock", ts)
                    + num(b"streamClockBase", base) + num(b"wallClock", 1.7e12) + b"\x00\x00\x09")
        base = 50_000_000  # 13.9 h: past the first wrap of 90 kHz ticks
        for mask in (2**64 - 1, 2**32 - 1):
            with self.subTest(bits=mask.bit_length()):
                ticks = lambda ts, rate: (base + ts) * rate // 1000 & mask
                stream = (tag(8, 0, b"\xaf\x00\x14\x08", 16000, ticks(0, 16000))
                          + tag(18, 0, sync(0, base), 90000, ticks(0, 90000))
                          + tag(9, 40, b"\x27\x01\x00\x00\x00", 90000, ticks(40, 90000))
                          + tag(8, 64, b"\xaf\x01\x01\x18\x20\x07", 16000, ticks(64, 16000))
                          + tag(10, 80, b"\xb8\xff\xfe", 48000, ticks(80, 48000)))
                result = self.validate(stream)
                if mask > 2**32:
                    self.assertEqual(result.returncode, 0, result.stderr)
                else:
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("video ticks are -4294967296 from", result.stderr)


if __name__ == "__main__":
    unittest.main()
