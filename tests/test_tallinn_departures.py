from pathlib import Path
import unittest

from tools.tallinn_departures import DepartureFormatError, parse_departures


FIXTURE = Path(__file__).parent / "fixtures" / "tallinn_departures_2026-08-25.csv"
SINGLE_FIXTURE = (
    Path(__file__).parent / "fixtures" / "tallinn_departures_single_2026-08-26.csv"
)
MULTI_FIXTURE = (
    Path(__file__).parent / "fixtures" / "tallinn_departures_multi_2026-08-26.csv"
)
UNKNOWN_CODE_FIXTURE = (
    Path(__file__).parent
    / "fixtures"
    / "tallinn_departures_unknown_internal_code_2026-08-26.csv"
)
FETCHED_AT = 1_787_650_000


class TallinnDeparturesParserTests(unittest.TestCase):
    def test_redacted_real_sample(self) -> None:
        result = parse_departures(FIXTURE.read_bytes(), "1234", FETCHED_AT)
        self.assertEqual(
            [item.route for item in result.departures], ["T2", "T1", "T5"]
        )
        self.assertEqual(result.departures[0].departure_epoch, FETCHED_AT + 414)
        self.assertEqual(result.departures[0].scheduled_epoch, FETCHED_AT + 384)
        self.assertTrue(result.departures[0].realtime)
        self.assertEqual(result.accepted_rows, 3)

    def test_current_real_single_and_multi_stop_captures(self) -> None:
        single = parse_departures(SINGLE_FIXTURE.read_bytes(), "1234", FETCHED_AT)
        self.assertEqual([item.route for item in single.departures], ["T5", "T2"])
        self.assertEqual(single.departures[0].departure_epoch, FETCHED_AT + 295)

        first = parse_departures(MULTI_FIXTURE.read_bytes(), "1234", FETCHED_AT)
        second = parse_departures(MULTI_FIXTURE.read_bytes(), "1235", FETCHED_AT)
        self.assertEqual([item.route for item in first.departures], ["T5", "T2"])
        self.assertEqual([item.route for item in second.departures], ["T1", "T2"])
        self.assertEqual(second.departures[0].departure_epoch, FETCHED_AT + 128)

    def test_internal_stop_code_header_only_response_is_unknown(self) -> None:
        with self.assertRaisesRegex(DepartureFormatError, "requested stop block"):
            parse_departures(UNKNOWN_CODE_FIXTURE.read_bytes(), "10801-1", FETCHED_AT)

    def test_exact_line_filter_and_scheduled_value(self) -> None:
        result = parse_departures(FIXTURE.read_bytes(), "1234", FETCHED_AT, "t5")
        self.assertEqual(len(result.departures), 1)
        self.assertEqual(result.departures[0].departure_epoch, FETCHED_AT + 978)
        self.assertFalse(result.departures[0].realtime)

    def test_midnight_wrap(self) -> None:
        body = (
            "Transport,RouteNum,ExpectedTimeInSeconds,ScheduleTimeInSeconds,86370,version20201024\n"
            "stop,TEST\n"
            "bus,1,30,30,Destination,60,\n"
        )
        result = parse_departures(body, "TEST", FETCHED_AT)
        self.assertEqual(result.departures[0].departure_epoch, FETCHED_AT + 60)

    def test_scheduled_time_normalizes_around_midnight(self) -> None:
        body = (
            "Transport,RouteNum,ExpectedTimeInSeconds,ScheduleTimeInSeconds,86370,version20201024\n"
            "stop,TEST\n"
            "bus,1,30,86390,Destination,60,\n"
        )
        result = parse_departures(body, "TEST", FETCHED_AT)
        self.assertEqual(result.departures[0].scheduled_epoch, FETCHED_AT + 20)

    def test_wait_discrepancy_over_tolerance_rejects_publish(self) -> None:
        body = (
            "Transport,RouteNum,ExpectedTimeInSeconds,ScheduleTimeInSeconds,86370,version20201024\n"
            "stop,TEST\n"
            "bus,1,30,30,Destination,181,\n"
        )
        with self.assertRaises(DepartureFormatError):
            parse_departures(body, "TEST", FETCHED_AT)

    def test_wrong_stop_and_html_are_rejected(self) -> None:
        with self.assertRaises(DepartureFormatError):
            parse_departures(FIXTURE.read_bytes(), "9999", FETCHED_AT)
        with self.assertRaises(DepartureFormatError):
            parse_departures("<html>temporary error</html>", "1234", FETCHED_AT)

    def test_bounds_rows_lines_and_line_length(self) -> None:
        header = (
            "Transport,RouteNum,ExpectedTimeInSeconds,ScheduleTimeInSeconds,100,version20201024"
        )
        rows = [f"bus,R{i:02d},{110+i},{110+i},Destination,{10+i}," for i in range(17)]
        result = parse_departures("\n".join([header, "stop,TEST", *rows]), "TEST", FETCHED_AT)
        self.assertEqual(result.accepted_rows, 16)
        self.assertEqual(len(result.departures), 4)
        with self.assertRaises(DepartureFormatError):
            parse_departures("\n".join([header, "stop,TEST", *([rows[0]] * 63)]), "TEST", FETCHED_AT)
        with self.assertRaises(DepartureFormatError):
            parse_departures("\n".join([header, "stop,TEST", "bus,1,110,110," + "X" * 150 + ",10,"]), "TEST", FETCHED_AT)

    def test_sorts_and_deduplicates(self) -> None:
        body = (
            "Transport,RouteNum,ExpectedTimeInSeconds,ScheduleTimeInSeconds,100,version20201024\n"
            "stop,TEST\n"
            "bus,B,130,130,Later,30,\n"
            "bus,A,110,110,Sooner,10,\n"
            "bus,A,110,110,Sooner,10,\n"
        )
        result = parse_departures(body, "TEST", FETCHED_AT)
        self.assertEqual([item.route for item in result.departures], ["A", "B"])


if __name__ == "__main__":
    unittest.main()
