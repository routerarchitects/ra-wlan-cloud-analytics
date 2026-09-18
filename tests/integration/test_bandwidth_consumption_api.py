"""
Integration tests for GET /api/v1/devices/{routerId}/wifi-clients/usage-summary.

These tests run against a live OWANALYTICS process with fake OWSEC/OWPROV
services, matching the memory-summary and radio-temperature-summary integration
test setup.

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
OTHER_BOARD_ID = "usage-other-board"
OTHER_VENUE_ID = "usage-other-venue"
DEFAULT_VALID_TOKEN = "root-token"
MISSING = object()


def env_or_skip(name: str) -> str:
    value = os.environ.get(name)
    if not value:
        pytest.skip(f"{name} is required for bandwidth consumption integration tests")
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


def recent_timestamp(minutes_ago: int = 1) -> str:
    return format_utc(utc_now() - timedelta(minutes=minutes_ago))


def utc_epoch(value: str) -> int:
    return int(datetime.fromisoformat(value.replace("Z", "+00:00")).timestamp())


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


def usage_summary_path(
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
    return f"/api/v1/devices/{selected_router}/wifi-clients/usage-summary?{encoded}"


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
        (router_id(), "usage-other-router"),
    )
    cursor.execute(
        "delete from timepoints where boardid in (%s, %s)",
        (board_id(), OTHER_BOARD_ID),
    )
    cursor.execute("delete from timepoints where venueid in (%s, %s)", (venue_id(), OTHER_VENUE_ID))
    cursor.execute("delete from boards where id in (%s, %s)", (board_id(), OTHER_BOARD_ID))


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
            "Bandwidth Consumption Test Board",
            "Integration test board",
            "[]",
            now,
            now,
            venue,
            "Bandwidth Consumption Test Venue",
            "Integration test venue",
            retention,
            60,
            False,
        ),
    )


def assoc(
    station: str,
    rx_bytes: int | None | object = MISSING,
    tx_bytes: int | None | object = MISSING,
) -> dict[str, Any]:
    item: dict[str, Any] = {"station": station}
    if rx_bytes is not MISSING:
        item["rx_bytes"] = rx_bytes
    if tx_bytes is not MISSING:
        item["tx_bytes"] = tx_bytes
    return item


def ssid(
    associations: list[dict[str, Any]],
    *,
    bssid: str = "aa:bb:cc:dd:ee:ff",
    ssid_name: str = "main",
    band: int = 5,
) -> dict[str, Any]:
    return {
        "bssid": bssid,
        "ssid": ssid_name,
        "band": band,
        "associations": associations,
    }


def insert_timepoint(
    cursor,
    timestamp: str,
    ssid_data: list[dict[str, Any]] | str,
    *,
    board: str | None = None,
    venue: str | None = None,
    serial: str | None = None,
) -> None:
    board = board or board_id()
    venue = venue or venue_id()
    serial = serial or router_id()
    ssid_json = ssid_data if isinstance(ssid_data, str) else json.dumps(ssid_data)
    cursor.execute(
        """
        insert into timepoints (
            id, boardid, timestamp, ap_data, ssid_data, radio_data,
            device_info, serialnumber, resource_data, venueid
        )
        values (%s, %s, %s, %s, %s, %s, %s, %s, %s, %s)
        """,
        (
            f"usage-{uuid.uuid4().hex}",
            board,
            utc_epoch(timestamp),
            "{}",
            ssid_json,
            "[]",
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


def assert_empty_usage_summary(body: dict[str, Any], expected_start: str, expected_end: str) -> None:
    assert body["meta"]["requestedWindow"] == {
        "startTime": expected_start,
        "endTime": expected_end,
    }
    assert body["meta"]["observedWindow"] == {"startTime": None, "endTime": None}
    assert body["data"] == {"items": [], "totalClients": 0, "truncated": False}


def test_usage_summary_calculates_cumulative_counter_deltas(seeded_board) -> None:
    end_dt = utc_now() - timedelta(seconds=30)
    start_dt = end_dt - timedelta(hours=1)
    t_a = start_dt + timedelta(minutes=10)
    t_b = start_dt + timedelta(minutes=20)
    t_c = start_dt + timedelta(minutes=30)

    with db_connection() as connection:
        with connection.cursor() as cursor:
            insert_timepoint(cursor, format_utc(t_a), [ssid([assoc("E25195ED0F28", 1000, 500)])])
            insert_timepoint(cursor, format_utc(t_b), [ssid([assoc("e2:51:95:ed:0f:28", 3000, 1500)])])
            insert_timepoint(cursor, format_utc(t_c), [ssid([assoc("e2-51-95-ed-0f-28", 6000, 2500)])])

    result = http_json(usage_summary_path(format_utc(end_dt)), valid_token())

    assert result.status == 200
    assert set(result.body) == {"data", "meta"}
    assert result.body["meta"]["requestedWindow"] == {
        "startTime": format_utc(start_dt),
        "endTime": format_utc(end_dt),
    }
    assert result.body["meta"]["observedWindow"] == {
        "startTime": format_utc(t_a),
        "endTime": format_utc(t_c),
    }
    assert result.body["data"]["totalClients"] == 1
    assert result.body["data"]["truncated"] is False
    assert len(result.body["data"]["items"]) == 1
    item = result.body["data"]["items"][0]
    assert item == {
        "mac": "e2:51:95:ed:0f:28",
        "rx_bytes": 5000,
        "tx_bytes": 2000,
        "total_bytes": 7000,
        "data_consume_rx": "0.01 MB",
        "data_consume_tx": "0.00 MB",
        "total_data_usage": "0.01 MB",
    }


def test_usage_summary_uses_boundary_samples_and_filters_gateway_scope(seeded_board) -> None:
    end_dt = utc_now() - timedelta(seconds=30)
    start_dt = end_dt - timedelta(hours=1)
    baseline_dt = start_dt - timedelta(minutes=9)
    mid_dt = start_dt + timedelta(minutes=30)
    end_boundary_dt = end_dt

    with db_connection() as connection:
        with connection.cursor() as cursor:
            seed_board(cursor, board=OTHER_BOARD_ID, venue=OTHER_VENUE_ID, retention=7200)
            insert_timepoint(cursor, format_utc(baseline_dt), [ssid([assoc("28:39:26:a1:7c:a5", 1_000_000, 1_000_000)])])
            insert_timepoint(cursor, format_utc(mid_dt), [ssid([assoc("28:39:26:a1:7c:a5", 2_000_000, 2_000_000)])])
            insert_timepoint(cursor, format_utc(end_boundary_dt), [ssid([assoc("28:39:26:a1:7c:a5", 3_000_000, 3_000_000)])])
            insert_timepoint(
                cursor,
                format_utc(mid_dt),
                [ssid([assoc("28:39:26:a1:7c:a5", 999_000_000, 999_000_000)])],
                serial="usage-other-router",
            )
            insert_timepoint(
                cursor,
                format_utc(mid_dt),
                [ssid([assoc("28:39:26:a1:7c:a5", 888_000_000, 888_000_000)])],
                board=OTHER_BOARD_ID,
                venue=OTHER_VENUE_ID,
            )

    result = http_json(usage_summary_path(format_utc(end_dt)), valid_token())

    assert result.status == 200
    assert result.body["meta"]["observedWindow"] == {
        "startTime": format_utc(baseline_dt),
        "endTime": format_utc(end_boundary_dt),
    }
    assert result.body["data"]["totalClients"] == 1
    item = result.body["data"]["items"][0]
    assert item["mac"] == "28:39:26:a1:7c:a5"
    assert item["rx_bytes"] == 2_000_000
    assert item["tx_bytes"] == 2_000_000
    assert item["total_bytes"] == 4_000_000
    assert item["data_consume_rx"] == "2.00 MB"
    assert item["data_consume_tx"] == "2.00 MB"
    assert item["total_data_usage"] == "4.00 MB"


def test_usage_summary_preserves_missing_and_null_counter_semantics(seeded_board) -> None:
    end_dt = utc_now() - timedelta(seconds=30)
    start_dt = end_dt - timedelta(hours=1)
    t1 = start_dt + timedelta(minutes=10)
    t2 = start_dt + timedelta(minutes=20)
    t3 = start_dt + timedelta(minutes=30)

    with db_connection() as connection:
        with connection.cursor() as cursor:
            insert_timepoint(
                cursor,
                format_utc(t1),
                [
                    ssid(
                        [
                            assoc("aa:bb:cc:dd:ee:20", 100),
                            assoc("aa:bb:cc:dd:ee:21", None, 100),
                            assoc("aa:bb:cc:dd:ee:22", 100, 100),
                            assoc("aa:bb:cc:dd:ee:23", 0, 0),
                        ]
                    )
                ],
            )
            insert_timepoint(
                cursor,
                format_utc(t2),
                [
                    ssid(
                        [
                            assoc("aa:bb:cc:dd:ee:20", 300, 500),
                            assoc("aa:bb:cc:dd:ee:21", 500, 300),
                            assoc("aa:bb:cc:dd:ee:22", tx_bytes=200),
                            assoc("aa:bb:cc:dd:ee:23", 500, 250),
                        ]
                    )
                ],
            )
            insert_timepoint(
                cursor,
                format_utc(t3),
                [
                    ssid(
                        [
                            assoc("aa:bb:cc:dd:ee:22", 400, 300),
                        ]
                    )
                ],
            )

    result = http_json(usage_summary_path(format_utc(end_dt)), valid_token())

    assert result.status == 200
    items = {item["mac"]: item for item in result.body["data"]["items"]}
    assert items["aa:bb:cc:dd:ee:20"]["rx_bytes"] == 200
    assert items["aa:bb:cc:dd:ee:20"]["tx_bytes"] == 0
    assert items["aa:bb:cc:dd:ee:21"]["rx_bytes"] == 0
    assert items["aa:bb:cc:dd:ee:21"]["tx_bytes"] == 200
    assert items["aa:bb:cc:dd:ee:22"]["rx_bytes"] == 300
    assert items["aa:bb:cc:dd:ee:22"]["tx_bytes"] == 200
    assert items["aa:bb:cc:dd:ee:23"]["rx_bytes"] == 500
    assert items["aa:bb:cc:dd:ee:23"]["tx_bytes"] == 250


def test_usage_summary_handles_resets_and_stream_changes_without_overcounting(seeded_board) -> None:
    end_dt = utc_now() - timedelta(seconds=30)
    start_dt = end_dt - timedelta(hours=1)
    t1 = start_dt + timedelta(minutes=10)
    t2 = start_dt + timedelta(minutes=20)
    t3 = start_dt + timedelta(minutes=30)
    t4 = start_dt + timedelta(minutes=40)

    with db_connection() as connection:
        with connection.cursor() as cursor:
            insert_timepoint(cursor, format_utc(t1), [ssid([assoc("54:6c:0e:44:11:09", 1000, 1000)], bssid="bssid-a")])
            insert_timepoint(cursor, format_utc(t2), [ssid([assoc("54:6c:0e:44:11:09", 1500, 1200)], bssid="bssid-a")])
            insert_timepoint(cursor, format_utc(t3), [ssid([assoc("54:6c:0e:44:11:09", 100, 1300)], bssid="bssid-a")])
            insert_timepoint(cursor, format_utc(t4), [ssid([assoc("54:6c:0e:44:11:09", 300, 1500)], bssid="bssid-a")])
            insert_timepoint(cursor, format_utc(t1), [ssid([assoc("54:6c:0e:44:11:09", 50, 20)], bssid="bssid-b")])
            insert_timepoint(cursor, format_utc(t2), [ssid([assoc("54:6c:0e:44:11:09", 90, 40)], bssid="bssid-b")])

    result = http_json(usage_summary_path(format_utc(end_dt)), valid_token())

    assert result.status == 200
    assert result.body["data"]["totalClients"] == 1
    item = result.body["data"]["items"][0]
    assert item["mac"] == "54:6c:0e:44:11:09"
    assert item["rx_bytes"] == 740
    assert item["tx_bytes"] == 520
    assert item["total_bytes"] == 1260


def test_usage_summary_empty_and_outside_window_only_clients_return_empty_success(seeded_board) -> None:
    end_dt = utc_now() - timedelta(seconds=30)
    start_dt = end_dt - timedelta(hours=1)
    before_dt = start_dt - timedelta(seconds=30)
    after_dt = end_dt + timedelta(seconds=30)

    with db_connection() as connection:
        with connection.cursor() as cursor:
            insert_timepoint(cursor, format_utc(before_dt), [ssid([assoc("aa:bb:cc:dd:ee:01", 100, 100)])])
            insert_timepoint(cursor, format_utc(after_dt), [ssid([assoc("aa:bb:cc:dd:ee:01", 200, 200)])])

    result = http_json(usage_summary_path(format_utc(end_dt)), valid_token())

    assert result.status == 200
    assert_empty_usage_summary(result.body, format_utc(start_dt), format_utc(end_dt))


def test_usage_summary_response_ordering_and_item_invariants(seeded_board) -> None:
    end_dt = utc_now() - timedelta(seconds=30)
    start_dt = end_dt - timedelta(hours=1)
    t1 = start_dt + timedelta(minutes=10)
    t2 = start_dt + timedelta(minutes=20)

    with db_connection() as connection:
        with connection.cursor() as cursor:
            insert_timepoint(
                cursor,
                format_utc(t1),
                [
                    ssid(
                        [
                            assoc("aa:bb:cc:dd:ee:02", 100, 100),
                            assoc("aa:bb:cc:dd:ee:03", 100, 100),
                        ]
                    )
                ],
            )
            insert_timepoint(
                cursor,
                format_utc(t2),
                [
                    ssid(
                        [
                            assoc("aa:bb:cc:dd:ee:02", 150, 150),
                            assoc("aa:bb:cc:dd:ee:03", 300, 100),
                        ]
                    )
                ],
            )

    result = http_json(usage_summary_path(format_utc(end_dt)), valid_token())

    assert result.status == 200
    assert result.body["data"]["totalClients"] == 2
    assert result.body["data"]["truncated"] is False
    items = result.body["data"]["items"]
    assert [item["mac"] for item in items] == ["aa:bb:cc:dd:ee:03", "aa:bb:cc:dd:ee:02"]
    for item in items:
        assert set(item) == {
            "mac",
            "rx_bytes",
            "tx_bytes",
            "total_bytes",
            "data_consume_rx",
            "data_consume_tx",
            "total_data_usage",
        }
        assert item["total_bytes"] == item["rx_bytes"] + item["tx_bytes"]
        assert item["data_consume_rx"].endswith(" MB")
        assert item["data_consume_tx"].endswith(" MB")
        assert item["total_data_usage"].endswith(" MB")


def test_usage_summary_missing_auth_rejects_before_query_validation() -> None:
    result = http_json(usage_summary_path("not-a-timestamp", lookback_hours="bad"))

    assert result.status == 401
    assert result.body["error"] == "unauthorized"


def test_usage_summary_invalid_query_rejects_after_auth() -> None:
    result = http_json(
        usage_summary_path(format_utc(utc_now() - timedelta(seconds=30)), extra_query="unexpected=true"),
        valid_token(),
    )

    assert result.status == 400
    assert result.body["error"] == "invalid_query_parameter"


def test_usage_summary_invalid_token_rejects_via_fake_owsec() -> None:
    result = http_json(
        usage_summary_path(format_utc(utc_now() - timedelta(seconds=30))),
        "bad-token",
    )

    assert result.status == 401
    assert result.body["error"] == "unauthorized"


def test_usage_summary_without_analytics_board_mapping_returns_not_found() -> None:
    with db_connection() as connection:
        with connection.cursor() as cursor:
            cleanup_test_rows(cursor)

    result = http_json(usage_summary_path(recent_timestamp(), lookback_hours="1"), valid_token())

    assert result.status == 404
    assert result.body["error"] == "not_found"


def test_usage_summary_unknown_router_from_fake_owprov_returns_not_found(seeded_board) -> None:
    result = http_json(
        usage_summary_path(
            format_utc(utc_now() - timedelta(seconds=30)),
            selected_router_id=UNKNOWN_ROUTER_ID,
        ),
        valid_token(),
    )

    assert result.status == 404
    assert result.body["error"] == "not_found"


def test_usage_summary_forbidden_router_from_fake_owprov_returns_not_found(seeded_board) -> None:
    result = http_json(
        usage_summary_path(
            format_utc(utc_now() - timedelta(seconds=30)),
            selected_router_id=UNAUTHORIZED_ROUTER_ID,
        ),
        valid_token(),
    )

    assert result.status == 404
    assert result.body["error"] == "not_found"


def test_usage_summary_invalid_fake_owprov_response_returns_bad_gateway() -> None:
    result = http_json(
        usage_summary_path(
            format_utc(utc_now() - timedelta(seconds=30)),
            selected_router_id=INVALID_RESPONSE_ROUTER_ID,
        ),
        valid_token(),
    )

    assert result.status == 502
    assert result.body["error"] == "owprov_invalid_response"


def test_usage_summary_local_venue_cache_does_not_bypass_fake_owprov_authorization(
    seeded_board,
) -> None:
    end_dt = utc_now() - timedelta(seconds=30)
    with db_connection() as connection:
        with connection.cursor() as cursor:
            insert_timepoint(
                cursor,
                format_utc(end_dt - timedelta(minutes=10)),
                [ssid([assoc("aa:bb:cc:dd:ee:10", 100, 100)])],
                serial=UNAUTHORIZED_ROUTER_ID,
            )

    result = http_json(
        usage_summary_path(format_utc(end_dt), selected_router_id=UNAUTHORIZED_ROUTER_ID),
        valid_token(),
    )

    assert result.status == 404
    assert result.body["error"] == "not_found"


def test_usage_summary_retention_is_enforced_from_resolved_fake_owprov_venue() -> None:
    with db_connection() as connection:
        with connection.cursor() as cursor:
            cleanup_test_rows(cursor)
            seed_board(cursor, retention=3600)

    try:
        result = http_json(usage_summary_path(recent_timestamp(), lookback_hours="2"), valid_token())
        assert result.status == 400
        assert result.body["error"] == "invalid_lookback_hours"
    finally:
        with db_connection() as connection:
            with connection.cursor() as cursor:
                cleanup_test_rows(cursor)
