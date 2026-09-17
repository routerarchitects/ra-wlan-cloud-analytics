"""
Integration tests for GET /api/v1/devices/{routerId}/radio-temperature-summary.

These tests run against a live OWANALYTICS process with fake OWSEC/OWPROV
services, matching the memory-summary integration test setup.

Required environment:
  OWANALYTICS_TEST_URL
  OWANALYTICS_TEST_DB_DSN

Optional environment:
  OWANALYTICS_TEST_VALID_TOKEN
  OWANALYTICS_TEST_ROUTER_ID
  OWANALYTICS_TEST_BOARD_ID
  OWANALYTICS_TEST_VENUE_ID
"""

from __future__ import annotations

import json
import os
import ssl
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from contextlib import contextmanager
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from typing import Any

import pytest


pytestmark = pytest.mark.integration

DEFAULT_ROUTER_ID = "60cf84f22290"
UNKNOWN_ROUTER_ID = "60cf84f22291"
UNAUTHORIZED_ROUTER_ID = "60cf84f22292"
INVALID_RESPONSE_ROUTER_ID = "60cf84f22293"
DEFAULT_BOARD_ID = "board-test-01"
DEFAULT_VENUE_ID = "venue-test-01"
OTHER_BOARD_ID = "temperature-other-board"
REASSIGNED_BOARD_ID = "old-board-reassigned"
DEFAULT_VALID_TOKEN = "root-token"


def env_or_skip(name: str) -> str:
    value = os.environ.get(name)
    if not value:
        pytest.skip(f"{name} is required for radio-temperature-summary integration tests")
    return value


def router_id() -> str:
    return os.environ.get("OWANALYTICS_TEST_ROUTER_ID", DEFAULT_ROUTER_ID)


def board_id() -> str:
    return os.environ.get("OWANALYTICS_TEST_BOARD_ID", DEFAULT_BOARD_ID)


def venue_id() -> str:
    return os.environ.get("OWANALYTICS_TEST_VENUE_ID", DEFAULT_VENUE_ID)


def valid_token() -> str:
    return os.environ.get("OWANALYTICS_TEST_VALID_TOKEN", DEFAULT_VALID_TOKEN)


def utc_now() -> datetime:
    return datetime.now(timezone.utc)


def format_utc(dt: datetime) -> str:
    return dt.strftime("%Y-%m-%dT%H:%M:%SZ")


def utc_epoch(value: str) -> int:
    return int(datetime.fromisoformat(value.replace("Z", "+00:00")).timestamp())


def json_bytes(payload: dict[str, Any]) -> bytes:
    return json.dumps(payload, separators=(",", ":")).encode("utf-8")


@dataclass
class HttpResult:
    status: int
    body: dict[str, Any]


def analytics_url(path: str) -> str:
    return env_or_skip("OWANALYTICS_TEST_URL").rstrip("/") + path


def http_json(path: str, token: str | None = None) -> HttpResult:
    headers = {"Accept": "application/json"}
    if token is not None:
        headers["Authorization"] = "Bearer " + token
    request = urllib.request.Request(analytics_url(path), headers=headers, method="GET")
    context = None
    if request.full_url.startswith("https://") and os.environ.get("OWANALYTICS_TEST_VERIFY_TLS") != "1":
        context = ssl._create_unverified_context()
    try:
        with urllib.request.urlopen(request, timeout=10, context=context) as response:
            return HttpResult(response.status, json.loads(response.read().decode("utf-8") or "{}"))
    except urllib.error.HTTPError as exc:
        return HttpResult(exc.code, json.loads(exc.read().decode("utf-8") or "{}"))


def temperature_summary_path(
    timestamp_till: str,
    lookback_hours: str = "1",
    *,
    selected_router_id: str | None = None,
    extra_query: str | None = None,
) -> str:
    selected_router = selected_router_id or router_id()
    encoded = urllib.parse.urlencode(
        {"timestampTill": timestamp_till, "lookbackHours": lookback_hours}
    )
    if extra_query:
        encoded += "&" + extra_query
    return f"/api/v1/devices/{selected_router}/radio-temperature-summary?{encoded}"


def connect_db(dsn: str):
    try:
        import psycopg

        return psycopg.connect(dsn)
    except ImportError:
        try:
            import psycopg2

            return psycopg2.connect(dsn)
        except ImportError:
            pytest.skip("Install psycopg or psycopg2 to run DB-backed integration tests")


@contextmanager
def db_connection():
    connection = connect_db(env_or_skip("OWANALYTICS_TEST_DB_DSN"))
    try:
        yield connection
        connection.commit()
    finally:
        connection.close()


def cleanup_test_rows(cursor) -> None:
    cursor.execute(
        "delete from timepoints where serialnumber in (%s, %s)",
        (router_id(), "temp-other-router"),
    )
    cursor.execute(
        "delete from timepoints where boardid in (%s, %s, %s)",
        (board_id(), OTHER_BOARD_ID, REASSIGNED_BOARD_ID),
    )
    cursor.execute(
        "delete from boards where id in (%s, %s, %s)",
        (board_id(), OTHER_BOARD_ID, REASSIGNED_BOARD_ID),
    )


def seed_board(cursor, *, board: str | None = None, venue: str | None = None, retention: int = 7200) -> None:
    board = board or board_id()
    venue = venue or venue_id()
    now = int(time.time())
    cursor.execute(
        """
        insert into boards (id, name, description, notes, created, modified, venueId, venueName, venueDescription, retention, interval, monitorSubVenues)
        values (%s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s)
        """,
        (
            board,
            "Radio Temperature Test Board",
            "Integration test board",
            "[]",
            now,
            now,
            venue,
            "Radio Temperature Test Venue",
            "Integration test venue",
            retention,
            60,
            False,
        ),
    )


def insert_timepoint(
    cursor,
    timestamp: str,
    radio_data: list[dict[str, Any]] | str,
    *,
    board: str | None = None,
    venue: str | None = None,
    serial: str | None = None,
    suffix: str = "",
) -> None:
    board = board or board_id()
    venue = venue or venue_id()
    serial = serial or router_id()
    row_id = f"temp-{uuid.uuid4().hex}"
    radio_json = radio_data if isinstance(radio_data, str) else json.dumps(radio_data)
    cursor.execute(
        """
        insert into timepoints (
            id, boardid, timestamp, ap_data, ssid_data, radio_data,
            device_info, serialnumber, resource_data, venueid
        )
        values (%s, %s, %s, %s, %s, %s, %s, %s, %s, %s)
        """,
        (
            row_id,
            board,
            utc_epoch(timestamp),
            "{}",
            "[]",
            radio_json,
            "{}",
            serial,
            "{}",
            venue,
        ),
    )


@pytest.fixture
def seeded_board():
    with db_connection() as connection:
        with connection.cursor() as cursor:
            cleanup_test_rows(cursor)
            seed_board(cursor)
    yield
    with db_connection() as connection:
        with connection.cursor() as cursor:
            cleanup_test_rows(cursor)


def assert_empty_temperature_summary(body: dict[str, Any], expected_start: str, expected_end: str) -> None:
    assert body["meta"]["requestedWindow"] == {
        "startTime": expected_start,
        "endTime": expected_end,
    }
    assert body["meta"]["observedWindow"] == {"startTime": None, "endTime": None}
    assert body["data"]["min_wifi_temp_2.4G"] is None
    assert body["data"]["max_wifi_temp_2.4G"] is None
    assert body["data"]["avg_wifi_temp_2.4G"] is None
    assert body["data"]["latest_wifi_temp_2.4G"] is None
    assert body["data"]["min_wifi_temp_5G"] is None
    assert body["data"]["max_wifi_temp_5G"] is None
    assert body["data"]["avg_wifi_temp_5G"] is None
    assert body["data"]["latest_wifi_temp_5G"] is None


def test_radio_temperature_summary_aggregates_persisted_temperature_samples(seeded_board) -> None:
    end_dt = utc_now() - timedelta(seconds=30)
    start_dt = end_dt - timedelta(hours=1)
    t_a = end_dt - timedelta(minutes=50)
    t_b = end_dt - timedelta(minutes=30)
    t_c = end_dt - timedelta(minutes=10)

    with db_connection() as connection:
        with connection.cursor() as cursor:
            insert_timepoint(cursor, format_utc(t_a), [{"band": 2, "temperature": 62}, {"band": 5, "temperature": 56}], suffix="a")
            insert_timepoint(cursor, format_utc(t_b), [{"band": 2, "temperature": 70}, {"band": 5, "temperature": 65}], suffix="b")
            insert_timepoint(cursor, format_utc(t_c), [{"band": 2, "temperature": 68}, {"band": 5, "temperature": 60}], suffix="c")

    result = http_json(temperature_summary_path(format_utc(end_dt)), valid_token())

    assert result.status == 200
    assert set(result.body) == {"data", "meta"}
    assert set(result.body["meta"]) == {"requestedWindow", "observedWindow"}
    assert set(result.body["data"]) == {
        "min_wifi_temp_2.4G",
        "max_wifi_temp_2.4G",
        "avg_wifi_temp_2.4G",
        "latest_wifi_temp_2.4G",
        "min_wifi_temp_5G",
        "max_wifi_temp_5G",
        "avg_wifi_temp_5G",
        "latest_wifi_temp_5G",
    }
    assert result.body["meta"]["requestedWindow"] == {
        "startTime": format_utc(start_dt),
        "endTime": format_utc(end_dt),
    }
    assert result.body["meta"]["observedWindow"] == {
        "startTime": format_utc(t_a),
        "endTime": format_utc(t_c),
    }
    assert result.body["data"]["min_wifi_temp_2.4G"] == 62
    assert result.body["data"]["max_wifi_temp_2.4G"] == 70
    assert result.body["data"]["avg_wifi_temp_2.4G"] == pytest.approx((62 + 70 + 68) / 3)
    assert result.body["data"]["latest_wifi_temp_2.4G"] == 68
    assert result.body["data"]["min_wifi_temp_5G"] == 56
    assert result.body["data"]["max_wifi_temp_5G"] == 65
    assert result.body["data"]["avg_wifi_temp_5G"] == pytest.approx((56 + 65 + 60) / 3)
    assert result.body["data"]["latest_wifi_temp_5G"] == 60


def test_radio_temperature_summary_filters_window_board_serial_and_band(seeded_board) -> None:
    end_dt = utc_now() - timedelta(seconds=30)
    start_dt = end_dt - timedelta(hours=1)
    before_dt = start_dt - timedelta(seconds=1)
    start_sample_dt = start_dt
    inside_dt = end_dt - timedelta(seconds=1)
    end_sample_dt = end_dt
    mid_dt = end_dt - timedelta(minutes=30)

    with db_connection() as connection:
        with connection.cursor() as cursor:
            insert_timepoint(cursor, format_utc(before_dt), [{"band": 2, "temperature": 1}], suffix="before")
            insert_timepoint(cursor, format_utc(start_sample_dt), [{"band": 2, "temperature": 20}], suffix="start")
            insert_timepoint(cursor, format_utc(inside_dt), [{"band": 2, "temperature": 30}, {"band": 6, "temperature": 99}], suffix="inside")
            insert_timepoint(cursor, format_utc(end_sample_dt), [{"band": 2, "temperature": 100}], suffix="end")
            insert_timepoint(cursor, format_utc(mid_dt), [{"band": 2, "temperature": 2}], serial="temp-other-router", suffix="oth")
            insert_timepoint(cursor, format_utc(mid_dt), [{"band": 2, "temperature": 3}], board=OTHER_BOARD_ID, suffix="other-board")

    result = http_json(temperature_summary_path(format_utc(end_dt)), valid_token())

    assert result.status == 200
    assert result.body["data"]["min_wifi_temp_2.4G"] == 20
    assert result.body["data"]["max_wifi_temp_2.4G"] == 30
    assert result.body["data"]["avg_wifi_temp_2.4G"] == 25
    assert result.body["data"]["latest_wifi_temp_2.4G"] == 30
    assert result.body["data"]["min_wifi_temp_5G"] is None
    assert result.body["data"]["latest_wifi_temp_5G"] is None
    assert result.body["meta"]["observedWindow"] == {
        "startTime": format_utc(start_sample_dt),
        "endTime": format_utc(inside_dt),
    }


def test_radio_temperature_summary_no_samples_returns_empty_success(seeded_board) -> None:
    end_dt = utc_now() - timedelta(seconds=30)
    start_dt = end_dt - timedelta(hours=1)

    result = http_json(temperature_summary_path(format_utc(end_dt)), valid_token())

    assert result.status == 200
    assert_empty_temperature_summary(result.body, format_utc(start_dt), format_utc(end_dt))


def test_radio_temperature_summary_historical_null_temperatures_return_empty_success(seeded_board) -> None:
    end_dt = utc_now() - timedelta(seconds=30)
    start_dt = end_dt - timedelta(hours=1)
    t_sample = end_dt - timedelta(minutes=30)

    with db_connection() as connection:
        with connection.cursor() as cursor:
            insert_timepoint(
                cursor,
                format_utc(t_sample),
                [{"band": 2, "temperature": None}, {"band": 5, "temperature": None}],
                suffix="null-only",
            )

    result = http_json(temperature_summary_path(format_utc(end_dt)), valid_token())

    assert result.status == 200
    assert_empty_temperature_summary(result.body, format_utc(start_dt), format_utc(end_dt))


def test_radio_temperature_summary_ignores_invalid_missing_and_null_temperature(seeded_board) -> None:
    end_dt = utc_now() - timedelta(seconds=30)
    t_invalid_a = end_dt - timedelta(minutes=50)
    t_invalid_b = end_dt - timedelta(minutes=45)
    t_invalid_c = end_dt - timedelta(minutes=40)
    t_zero_a = end_dt - timedelta(minutes=35)
    t_malformed = end_dt - timedelta(minutes=30)
    t_valid_a = end_dt - timedelta(minutes=25)
    t_valid_b = end_dt - timedelta(minutes=20)

    with db_connection() as connection:
        with connection.cursor() as cursor:
            insert_timepoint(cursor, format_utc(t_invalid_a), [{"band": 2}], suffix="missing")
            insert_timepoint(cursor, format_utc(t_invalid_b), [{"band": 2, "temperature": None}], suffix="null")
            insert_timepoint(cursor, format_utc(t_invalid_c), [{"band": 2, "temperature": -41}, {"band": 5, "temperature": 126}], suffix="range")
            insert_timepoint(cursor, format_utc(t_zero_a), [{"band": 2, "temperature": 255}, {"band": 5, "temperature": 0}], suffix="zero-a")
            insert_timepoint(cursor, format_utc(t_malformed), "[", suffix="malformed")
            insert_timepoint(cursor, format_utc(t_valid_a), [{"band": 2, "temperature": 20}, {"band": 5, "temperature": 0}], suffix="valid-a")
            insert_timepoint(cursor, format_utc(t_valid_b), [{"band": 2, "temperature": 30}, {"band": 5, "temperature": 10}], suffix="valid-b")

    result = http_json(temperature_summary_path(format_utc(end_dt)), valid_token())

    assert result.status == 200
    assert result.body["data"]["min_wifi_temp_2.4G"] == 20
    assert result.body["data"]["max_wifi_temp_2.4G"] == 30
    assert result.body["data"]["avg_wifi_temp_2.4G"] == 25
    assert result.body["data"]["latest_wifi_temp_2.4G"] == 30
    assert result.body["data"]["min_wifi_temp_5G"] == 0
    assert result.body["data"]["max_wifi_temp_5G"] == 10
    assert result.body["data"]["avg_wifi_temp_5G"] == pytest.approx(10 / 3)
    assert result.body["data"]["latest_wifi_temp_5G"] == 10
    assert result.body["meta"]["observedWindow"] == {
        "startTime": format_utc(t_zero_a),
        "endTime": format_utc(t_valid_b),
    }


@pytest.mark.parametrize("selected_router_id", [UNKNOWN_ROUTER_ID, UNAUTHORIZED_ROUTER_ID])
def test_radio_temperature_summary_resolves_router_before_retention_validation(
    selected_router_id: str,
) -> None:
    result = http_json(
        temperature_summary_path(
            "2026-07-01T00:30:00Z",
            lookback_hours="1",
            selected_router_id=selected_router_id,
        ),
        valid_token(),
    )

    assert result.status == 404
    assert result.body["error"] == "not_found"


def test_radio_temperature_summary_missing_auth_rejects_before_query_validation() -> None:
    result = http_json(temperature_summary_path("not-a-timestamp", lookback_hours="bad"))

    assert result.status == 401
    assert result.body["error"] == "unauthorized"


def test_radio_temperature_summary_invalid_query_rejects_after_auth() -> None:
    result = http_json(
        temperature_summary_path(format_utc(utc_now() - timedelta(seconds=30)), extra_query="unexpected=true"),
        valid_token(),
    )

    assert result.status == 400
    assert result.body["error"] == "invalid_query_parameter"


def test_radio_temperature_summary_invalid_token_rejects_via_fake_owsec() -> None:
    result = http_json(
        temperature_summary_path(format_utc(utc_now() - timedelta(seconds=30))),
        "bad-token",
    )

    assert result.status == 401
    assert result.body["error"] == "unauthorized"


def test_radio_temperature_summary_unknown_router_from_owprov_returns_not_found(seeded_board) -> None:
    result = http_json(
        temperature_summary_path(
            format_utc(utc_now() - timedelta(seconds=30)),
            selected_router_id=UNKNOWN_ROUTER_ID,
        ),
        valid_token(),
    )

    assert result.status == 404
    assert result.body["error"] == "not_found"


def test_radio_temperature_summary_forbidden_router_from_owprov_returns_not_found(seeded_board) -> None:
    result = http_json(
        temperature_summary_path(
            format_utc(utc_now() - timedelta(seconds=30)),
            selected_router_id=UNAUTHORIZED_ROUTER_ID,
        ),
        valid_token(),
    )

    assert result.status == 404
    assert result.body["error"] == "not_found"


def test_radio_temperature_summary_invalid_owprov_response_returns_bad_gateway() -> None:
    result = http_json(
        temperature_summary_path(
            format_utc(utc_now() - timedelta(seconds=30)),
            selected_router_id=INVALID_RESPONSE_ROUTER_ID,
        ),
        valid_token(),
    )

    assert result.status == 502
    assert result.body["error"] == "owprov_invalid_response"


def test_radio_temperature_summary_local_venue_cache_does_not_bypass_authorization(seeded_board) -> None:
    end_dt = utc_now() - timedelta(seconds=30)
    with db_connection() as connection:
        with connection.cursor() as cursor:
            insert_timepoint(
                cursor,
                format_utc(end_dt - timedelta(minutes=10)),
                [{"band": 2, "temperature": 65}],
                serial=UNAUTHORIZED_ROUTER_ID,
                suffix="unauth-cache-test",
            )

    result = http_json(
        temperature_summary_path(format_utc(end_dt), selected_router_id=UNAUTHORIZED_ROUTER_ID),
        valid_token(),
    )

    assert result.status == 404
    assert result.body["error"] == "not_found"


def test_radio_temperature_summary_reassigned_router_ignores_old_board_samples(seeded_board) -> None:
    end_dt = utc_now() - timedelta(seconds=30)
    old_board_id = REASSIGNED_BOARD_ID
    old_venue_id = "old-venue-reassigned"
    
    with db_connection() as connection:
        with connection.cursor() as cursor:
            # Increase current board retention to support 6-hour lookback query
            cursor.execute("update boards set retention = 86400 where id = %s", (board_id(),))
            # Seed old board with old venue and 24h retention
            seed_board(cursor, board=old_board_id, venue=old_venue_id, retention=86400)
            
            # Historical samples on old Board A and old venue
            insert_timepoint(
                cursor,
                format_utc(end_dt - timedelta(hours=5)),
                [{"band": 2, "temperature": 40.0}],
                board=old_board_id,
                venue=old_venue_id,
                suffix="old-board-sample-1",
            )
            insert_timepoint(
                cursor,
                format_utc(end_dt - timedelta(hours=4)),
                [{"band": 2, "temperature": 45.0}],
                board=old_board_id,
                venue=old_venue_id,
                suffix="old-board-sample-2",
            )

            # Current samples on current Board B
            t1 = end_dt - timedelta(hours=2)
            t2 = end_dt - timedelta(hours=1)
            insert_timepoint(
                cursor,
                format_utc(t1),
                [{"band": 2, "temperature": 72.0}],
                suffix="current-board-sample-1",
            )
            insert_timepoint(
                cursor,
                format_utc(t2),
                [{"band": 2, "temperature": 78.0}],
                suffix="current-board-sample-2",
            )

    req_start = end_dt - timedelta(hours=6)
    result = http_json(
        temperature_summary_path(format_utc(end_dt), lookback_hours=6),
        valid_token(),
    )

    assert result.status == 200
    assert result.body["meta"]["requestedWindow"]["startTime"] == format_utc(req_start)
    assert result.body["meta"]["requestedWindow"]["endTime"] == format_utc(end_dt)
    # Observed window & statistics should be derived strictly from current board B samples
    assert result.body["meta"]["observedWindow"]["startTime"] == format_utc(t1)
    assert result.body["meta"]["observedWindow"]["endTime"] == format_utc(t2)
    assert result.body["data"]["min_wifi_temp_2.4G"] == 72.0
    assert result.body["data"]["max_wifi_temp_2.4G"] == 78.0
    assert result.body["data"]["avg_wifi_temp_2.4G"] == 75.0
    assert result.body["data"]["latest_wifi_temp_2.4G"] == 78.0


def test_radio_temperature_summary_raw_telemetry_json_payload(seeded_board) -> None:
    end_dt = utc_now() - timedelta(seconds=30)
    t_sample = end_dt - timedelta(minutes=20)

    # Test raw radio telemetry payload structure as ingested from device state messages
    raw_radios = [
        {"band": 2, "channel": 6, "temperature": 52.5},
        {"band": 5, "channel": 36, "temperature": 0.0},
    ]

    with db_connection() as connection:
        with connection.cursor() as cursor:
            insert_timepoint(cursor, format_utc(t_sample), raw_radios, suffix="raw-telemetry")

    result = http_json(temperature_summary_path(format_utc(end_dt)), valid_token())

    assert result.status == 200
    assert result.body["data"]["min_wifi_temp_2.4G"] == 52.5
    assert result.body["data"]["max_wifi_temp_2.4G"] == 52.5
    assert result.body["data"]["avg_wifi_temp_2.4G"] == 52.5
    assert result.body["data"]["latest_wifi_temp_2.4G"] == 52.5
    assert result.body["data"]["min_wifi_temp_5G"] == 0
    assert result.body["data"]["max_wifi_temp_5G"] == 0
    assert result.body["data"]["avg_wifi_temp_5G"] == 0
    assert result.body["data"]["latest_wifi_temp_5G"] == 0


def test_radio_temperature_summary_exceeds_max_samples_with_malformed_records(seeded_board) -> None:
    end_dt = utc_now() - timedelta(seconds=30)
    t_base = end_dt - timedelta(minutes=50)
    base_ts = utc_epoch(format_utc(t_base))

    # Bulk insert 10,005 raw timepoint rows with malformed JSON payloads interspersed.
    # Estimated sample count for a 1h half-open query window @ 60s interval is 60 samples.
    # But actual DB row count (10,005) genuinely exceeds the default 10,000 max_samples limit.
    with db_connection() as connection:
        with connection.cursor() as cursor:
            cursor.execute(
                """
                insert into timepoints (
                    id, boardid, timestamp, ap_data, ssid_data, radio_data,
                    device_info, serialnumber, resource_data, venueid
                )
                select
                    'temp-exceed-' || s,
                    %s,
                    %s + mod(s, 3000),
                    '{}',
                    '[]',
                    case when mod(s, 3) = 0 then '{' else '[{"band": 2, "temperature": 50.0}]' end,
                    '{}',
                    %s,
                    '{}',
                    %s
                from generate_series(1, 10005) as s
                """,
                (board_id(), base_ts, router_id(), venue_id()),
            )

    result = http_json(temperature_summary_path(format_utc(end_dt)), valid_token())
    assert result.status == 400
    assert result.body["error"] == "exceeds_max_samples"
    assert result.body["message"] == "Requested query window exceeds maximum allowed telemetry sample count"
