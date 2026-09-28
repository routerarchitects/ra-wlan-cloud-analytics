"""
Integration tests for GET /api/v1/devices/{routerId}/availability-summary.

These tests cover the read-path API contract from analytics_mcp_api_test_cases.md:
persisted offline transition rows are counted by current Analytics board and
durable serial number after the caller is authenticated and the router is
resolved through fake OWPROV.
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
OLD_BOARD_ID = "availability-old-board"
OTHER_BOARD_ID = "availability-other-board"
DEFAULT_VALID_TOKEN = "root-token"


def env_or_skip(name: str) -> str:
    value = os.environ.get(name)
    if not value:
        pytest.skip(f"{name} is required for availability-summary integration tests")
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


def availability_summary_path(
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
    return f"/api/v1/devices/{selected_router}/availability-summary?{encoded}"


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


@contextmanager
def temporary_availability_valid_from(timestamp: str):
    connection = connect_db(env_or_skip("OWANALYTICS_TEST_DB_DSN"))
    original_value = None
    try:
        with connection.cursor() as cursor:
            cursor.execute(
                "select property_value from system_properties where property_key = %s",
                ("availability_valid_from",),
            )
            row = cursor.fetchone()
            original_value = row[0] if row else None
            cursor.execute(
                """
                insert into system_properties (property_key, property_value, created, modified)
                values (%s, %s, %s, %s)
                on conflict (property_key) do update
                set property_value = excluded.property_value,
                    modified = excluded.modified
                """,
                ("availability_valid_from", timestamp, int(time.time()), int(time.time())),
            )
        connection.commit()
        yield
    finally:
        with connection.cursor() as cursor:
            if original_value is None:
                cursor.execute(
                    "delete from system_properties where property_key = %s",
                    ("availability_valid_from",),
                )
            else:
                cursor.execute(
                    """
                    update system_properties
                    set property_value = %s, modified = %s
                    where property_key = %s
                    """,
                    (original_value, int(time.time()), "availability_valid_from"),
                )
        connection.commit()
        connection.close()


def cleanup_test_rows(cursor) -> None:
    cursor.execute(
        """
        delete from device_availability_events
        where id like %s
           or serialnumber in (%s, %s, %s, %s)
           or board_id in (%s, %s, %s)
        """,
        (
            "avail-test-%",
            router_id(),
            router_id() + "9",
            "availability-other-router",
            UNAUTHORIZED_ROUTER_ID,
            board_id(),
            OLD_BOARD_ID,
            OTHER_BOARD_ID,
        ),
    )
    cursor.execute(
        "delete from boards where id in (%s, %s, %s)",
        (board_id(), OLD_BOARD_ID, OTHER_BOARD_ID),
    )


def seed_board(cursor, *, board: str | None = None, venue: str | None = None, retention: int = 86400) -> None:
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
            "Availability Test Board",
            "Integration test board",
            "[]",
            now,
            now,
            venue,
            "Availability Test Venue",
            "Integration test venue",
            retention,
            60,
            False,
        ),
    )


def insert_availability_event(
    cursor,
    timestamp: str,
    *,
    event_type: str = "offline",
    serial: str | None = None,
    board: str | None = None,
    reason: str = "",
    connection_ip: str = "",
    session_id: str = "",
) -> None:
    serial = serial or router_id()
    board = board or board_id()
    row_id = f"avail-test-{uuid.uuid4().hex}"
    cursor.execute(
        """
        insert into device_availability_events (
            id, board_id, serialnumber, event_type, event_time, reason,
            connection_ip, session_id, event_id, idempotency_key
        )
        values (%s, %s, %s, %s, %s, %s, %s, %s, %s, %s)
        """,
        (
            row_id,
            board,
            serial,
            event_type,
            utc_epoch(timestamp),
            reason,
            connection_ip,
            session_id,
            row_id,
            f"{row_id}-idempotency",
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


def assert_zero_availability(body: dict[str, Any], expected_start: str, expected_end: str) -> None:
    assert body["meta"] == {
        "requestedWindow": {
            "startTime": expected_start,
            "endTime": expected_end,
        },
        "observedWindow": {"startTime": None, "endTime": None},
    }
    assert body["data"] == {
        "gw_uuid": router_id(),
        "fetch_status": "success",
        "offlineEventCount": 0,
    }


def test_availability_event_schema_exists_with_required_indexes() -> None:
    with db_connection() as connection:
        with connection.cursor() as cursor:
            cursor.execute(
                """
                select column_name
                from information_schema.columns
                where table_name = 'device_availability_events'
                """
            )
            columns = {row[0] for row in cursor.fetchall()}
            assert {
                "id",
                "board_id",
                "serialnumber",
                "event_type",
                "event_time",
                "reason",
                "connection_ip",
                "session_id",
                "event_id",
                "idempotency_key",
            }.issubset(columns)

            cursor.execute(
                """
                select indexname, indexdef
                from pg_indexes
                where tablename = 'device_availability_events'
                """
            )
            indexes = {row[0]: row[1] for row in cursor.fetchall()}
            assert "availability_serial_time_index" in indexes
            assert "serialnumber" in indexes["availability_serial_time_index"].lower()
            assert "event_time" in indexes["availability_serial_time_index"].lower()
            assert "availability_board_serial_time_index" in indexes
            board_serial_index = indexes["availability_board_serial_time_index"].lower()
            assert "board_id" in board_serial_index
            assert "serialnumber" in board_serial_index
            assert "event_time" in board_serial_index
            assert "availability_idempotency_key_unique" in indexes
            assert "unique" in indexes["availability_idempotency_key_unique"].lower()


def test_availability_summary_no_events_returns_empty_success(seeded_board) -> None:
    end_dt = utc_now() - timedelta(seconds=30)
    start_dt = end_dt - timedelta(hours=1)

    result = http_json(
        availability_summary_path(format_utc(end_dt), lookback_hours="1"),
        valid_token(),
    )

    assert result.status == 200
    assert set(result.body) == {"meta", "data"}
    assert_zero_availability(result.body, format_utc(start_dt), format_utc(end_dt))


def test_availability_summary_counts_current_board_half_open_offline_events_only(seeded_board) -> None:
    end_dt = utc_now() - timedelta(seconds=30)
    start_dt = end_dt - timedelta(hours=4)
    before_dt = start_dt - timedelta(seconds=1)
    included_dt = start_dt + timedelta(minutes=30)
    online_dt = start_dt + timedelta(hours=1)
    other_router_dt = start_dt + timedelta(hours=2)
    other_board_dt = start_dt + timedelta(hours=3)
    end_event_dt = end_dt

    with db_connection() as connection:
        with connection.cursor() as cursor:
            insert_availability_event(cursor, format_utc(before_dt), reason="before")
            insert_availability_event(cursor, format_utc(included_dt), reason="included")
            insert_availability_event(cursor, format_utc(online_dt), event_type="online")
            insert_availability_event(
                cursor,
                format_utc(other_router_dt),
                serial="availability-other-router",
                reason="other-router",
            )
            insert_availability_event(
                cursor,
                format_utc(other_board_dt),
                board=OTHER_BOARD_ID,
                reason="other-board",
            )
            insert_availability_event(cursor, format_utc(end_event_dt), reason="end")

    result = http_json(
        availability_summary_path(format_utc(end_dt), lookback_hours="4"),
        valid_token(),
    )

    assert result.status == 200
    assert result.body["meta"]["requestedWindow"] == {
        "startTime": format_utc(start_dt),
        "endTime": format_utc(end_dt),
    }
    assert result.body["meta"]["observedWindow"] == {
        "startTime": format_utc(included_dt),
        "endTime": format_utc(included_dt),
    }
    assert result.body["data"] == {
        "gw_uuid": router_id(),
        "fetch_status": "success",
        "offlineEventCount": 1,
    }


def test_availability_summary_clamps_query_start_to_availability_valid_from(seeded_board) -> None:
    end_dt = utc_now() - timedelta(seconds=30)
    start_dt = end_dt - timedelta(hours=4)
    valid_from_dt = end_dt - timedelta(hours=2)
    pre_cutover_event_dt = valid_from_dt - timedelta(minutes=15)
    post_cutover_event_dt = valid_from_dt + timedelta(minutes=15)

    with db_connection() as connection:
        with connection.cursor() as cursor:
            insert_availability_event(
                cursor, format_utc(pre_cutover_event_dt), reason="pre-cutover"
            )
            insert_availability_event(
                cursor, format_utc(post_cutover_event_dt), reason="post-cutover"
            )

    with temporary_availability_valid_from(format_utc(valid_from_dt)):
        result = http_json(
            availability_summary_path(format_utc(end_dt), lookback_hours="4"),
            valid_token(),
        )

    assert result.status == 200
    assert result.body["meta"] == {
        "requestedWindow": {
            "startTime": format_utc(start_dt),
            "endTime": format_utc(end_dt),
        },
        "observedWindow": {
            "startTime": format_utc(post_cutover_event_dt),
            "endTime": format_utc(post_cutover_event_dt),
        },
    }
    assert result.body["data"] == {
        "gw_uuid": router_id(),
        "fetch_status": "success",
        "offlineEventCount": 1,
    }


def test_availability_summary_entirely_before_availability_valid_from_returns_zero(seeded_board) -> None:
    end_dt = utc_now() - timedelta(hours=3)
    start_dt = end_dt - timedelta(hours=1)
    valid_from_dt = end_dt + timedelta(hours=1)
    event_dt = start_dt + timedelta(minutes=15)

    with db_connection() as connection:
        with connection.cursor() as cursor:
            insert_availability_event(
                cursor, format_utc(event_dt), reason="unsupported-window"
            )

    with temporary_availability_valid_from(format_utc(valid_from_dt)):
        result = http_json(
            availability_summary_path(format_utc(end_dt), lookback_hours="1"),
            valid_token(),
        )

    assert result.status == 200
    assert result.body["meta"] == {
        "requestedWindow": {
            "startTime": format_utc(start_dt),
            "endTime": format_utc(end_dt),
        },
        "observedWindow": {"startTime": None, "endTime": None},
    }
    assert result.body["data"] == {
        "gw_uuid": router_id(),
        "fetch_status": "success",
        "offlineEventCount": 0,
    }


def test_availability_summary_excludes_events_from_previous_board(seeded_board) -> None:
    end_dt = utc_now() - timedelta(seconds=30)
    old_board_event_dt = end_dt - timedelta(minutes=45)
    current_board_event_dt = end_dt - timedelta(minutes=15)

    with db_connection() as connection:
        with connection.cursor() as cursor:
            insert_availability_event(
                cursor,
                format_utc(old_board_event_dt),
                board=OLD_BOARD_ID,
                reason="old-board",
            )
            insert_availability_event(
                cursor,
                format_utc(current_board_event_dt),
                reason="current-board",
            )

    result = http_json(
        availability_summary_path(format_utc(end_dt), lookback_hours="1"),
        valid_token(),
    )

    assert result.status == 200
    assert result.body["data"]["offlineEventCount"] == 1
    assert result.body["meta"]["observedWindow"] == {
        "startTime": format_utc(current_board_event_dt),
        "endTime": format_utc(current_board_event_dt),
    }


def test_availability_summary_missing_auth_rejects_before_query_validation() -> None:
    result = http_json(availability_summary_path("not-a-timestamp", lookback_hours="bad"))

    assert result.status == 401
    assert result.body["error"] == "unauthorized"


def test_availability_summary_invalid_token_rejects_via_fake_owsec() -> None:
    result = http_json(
        availability_summary_path(recent_timestamp(), lookback_hours="1"),
        "bad-token",
    )

    assert result.status == 401
    assert result.body["error"] == "unauthorized"


@pytest.mark.parametrize(
    ("timestamp_till", "lookback_hours", "expected_error"),
    [
        ("invalid-time", "1", "invalid_timestamp"),
        (recent_timestamp(), "0", "invalid_lookback_hours"),
        (recent_timestamp(), "-1", "invalid_lookback_hours"),
        (recent_timestamp(), "999999", "invalid_lookback_hours"),
    ],
)
def test_availability_summary_invalid_query_rejects_after_auth(
    timestamp_till: str,
    lookback_hours: str,
    expected_error: str,
) -> None:
    result = http_json(
        availability_summary_path(timestamp_till, lookback_hours=lookback_hours),
        valid_token(),
    )

    assert result.status == 400
    assert result.body["error"] == expected_error


def test_availability_summary_missing_timestamp_rejects_after_auth() -> None:
    path = f"/api/v1/devices/{router_id()}/availability-summary?lookbackHours=1"
    result = http_json(path, valid_token())

    assert result.status == 400
    assert result.body["error"] == "invalid_timestamp"


@pytest.mark.parametrize("selected_router_id", [":::", "router%20id"])
def test_availability_summary_invalid_router_id_syntax_rejects(selected_router_id: str) -> None:
    result = http_json(
        availability_summary_path(
            recent_timestamp(),
            lookback_hours="1",
            selected_router_id=selected_router_id,
        ),
        valid_token(),
    )

    assert result.status == 400
    assert result.body["error"] == "invalid_router_id"


@pytest.mark.parametrize("selected_router_id", [UNKNOWN_ROUTER_ID, UNAUTHORIZED_ROUTER_ID])
def test_availability_summary_router_not_visible_returns_not_found(
    selected_router_id: str,
    seeded_board,
) -> None:
    result = http_json(
        availability_summary_path(
            recent_timestamp(),
            lookback_hours="1",
            selected_router_id=selected_router_id,
        ),
        valid_token(),
    )

    assert result.status == 404
    assert result.body["error"] == "not_found"


def test_availability_summary_invalid_owprov_response_returns_bad_gateway() -> None:
    result = http_json(
        availability_summary_path(
            recent_timestamp(),
            lookback_hours="1",
            selected_router_id=INVALID_RESPONSE_ROUTER_ID,
        ),
        valid_token(),
    )

    assert result.status == 502
    assert result.body["error"] == "owprov_invalid_response"


def test_availability_summary_retention_is_enforced_from_resolved_venue() -> None:
    with db_connection() as connection:
        with connection.cursor() as cursor:
            cleanup_test_rows(cursor)
            seed_board(cursor, retention=3600)

    try:
        result = http_json(
            availability_summary_path(recent_timestamp(), lookback_hours="2"),
            valid_token(),
        )
        assert result.status == 400
        assert result.body["error"] == "invalid_lookback_hours"
    finally:
        with db_connection() as connection:
            with connection.cursor() as cursor:
                cleanup_test_rows(cursor)


def test_availability_summary_without_analytics_board_mapping_returns_not_found() -> None:
    with db_connection() as connection:
        with connection.cursor() as cursor:
            cleanup_test_rows(cursor)

    result = http_json(
        availability_summary_path(recent_timestamp(), lookback_hours="1"),
        valid_token(),
    )

    assert result.status == 404
    assert result.body["error"] == "not_found"


def test_availability_summary_local_event_does_not_bypass_owprov_authorization(
    seeded_board,
) -> None:
    end_dt = utc_now() - timedelta(seconds=30)
    with db_connection() as connection:
        with connection.cursor() as cursor:
            insert_availability_event(
                cursor,
                format_utc(end_dt - timedelta(minutes=10)),
                serial=UNAUTHORIZED_ROUTER_ID,
                reason="unauthorized-router",
            )

    result = http_json(
        availability_summary_path(
            format_utc(end_dt),
            lookback_hours="1",
            selected_router_id=UNAUTHORIZED_ROUTER_ID,
        ),
        valid_token(),
    )

    assert result.status == 404
    assert result.body["error"] == "not_found"
