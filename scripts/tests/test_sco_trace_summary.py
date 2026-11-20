import importlib.util
import io
import pathlib
import unittest

path = pathlib.Path(__file__).resolve().parents[1] / "sco_trace_summary.py"
spec = importlib.util.spec_from_file_location("sco_trace_summary", path)
summary = importlib.util.module_from_spec(spec)
spec.loader.exec_module(summary)


class ScoTraceSummaryTest(unittest.TestCase):
    def test_counts_timing_and_does_not_include_payload(self):
        trace = io.StringIO(
            "> SCO Data RX: Handle 385 flags 0x00 dlen 120 #1 [hci0] 1.000000\n"
            "  secret microphone payload\n"
            "> SCO Data RX: Handle 385 flags 0x01 dlen 120 #2 [hci0] 1.007500\n"
            "> SCO Data RX: Handle 385 flags 0x02 dlen 120 #3 [hci0] 1.015000\n"
            "> SCO Data RX: Handle 385 flags 0x03 dlen 120 #4 [hci0] 1.022500\n"
            "< SCO Data TX: Handle 385 flags 0x00 dlen 254 #5 [hci0] 1.023000\n"
            "< SCO Data TX: Handle 385 flags 0x00 dlen 254 #6 [hci0] 1.038875\n"
        )
        result = summary.summarize(trace)
        self.assertEqual(result["rx"]["status"], {"valid": 1, "possibly_invalid": 1, "no_data": 1, "partially_lost": 1, "other": 0})
        self.assertEqual(result["tx"]["lengths"], {254: 2})
        self.assertAlmostEqual(result["tx"]["gap_ms"]["median"], 15.875)
        self.assertNotIn("secret", repr(result))

    def test_empty_and_out_of_order_timestamps(self):
        result = summary.summarize(io.StringIO(
            "< SCO Data TX: Handle 1 flags 0x00 dlen 120 #1 [hci0] 2.000\n"
            "< SCO Data TX: Handle 1 flags 0x00 dlen 120 #2 [hci0] 1.000\n"
        ))
        self.assertIsNone(result["tx"]["gap_ms"])
        self.assertEqual(result["rx"]["count"], 0)


if __name__ == "__main__":
    unittest.main()
