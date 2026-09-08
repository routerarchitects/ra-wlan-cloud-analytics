"""
Integration tests for GET /api/v1/devices/{routerId}/memory-summary.

These tests run against a live OWANALYTICS process. CI starts standalone fake
OWSEC and OWPROV services before OWANALYTICS starts, then injects those service
endpoints through CI_FAKE_EXTERNAL_SERVICES.

Required environment:
  OWANALYTICS_TEST_URL
      Base URL of the Analytics service, for example https://127.0.0.1:16009.

  OWANALYTICS_TEST_DB_DSN
      PostgreSQL DSN for tests that seed board/timepoint data.

  OWANALYTICS_TEST_VALID_TOKEN
      Optional valid token accepted by the fake OWSEC/OWPROV services.

  OWANALYTICS_TEST_ROUTER_ID, OWANALYTICS_TEST_BOARD_ID, OWANALYTICS_TEST_VENUE_ID
      Optional IDs. Defaults match the Markdown integration spec.

The storage-failure test is opt-in because it temporarily changes DB grants:
  OWANALYTICS_TEST_ENABLE_STORAGE_FAILURE=1
  OWANALYTICS_TEST_DB_ADMIN_DSN
  OWANALYTICS_TEST_APP_DB_USER
"""

from __future__ import annotations

import concurrent.futures
import json
import os
import ssl
import time
import urllib.error
import urllib.parse
import urllib.request
from contextlib import contextmanager
from dataclasses import dataclass
from datetime import datetime, timezone, timedelta
from typing import Any

import pytest


pytestmark = pytest.mark.integration

DEFAULT_ROUTER_ID = "60cf84f22290"
UNKNOWN_ROUTER_ID = "60cf84f22291"
UNAUTHORIZED_ROUTER_ID = "60cf84f22292"
INVALID_RESPONSE_ROUTER_ID = "60cf84f22293"
DEFAULT_BOARD_ID = "board-test-01"
DEFAULT_VENUE_ID = "venue-test-01"
OLD_BOARD_ID = "old-board"
OTHER_BOARD_ID = "other-board"
CREATE_BOARD_ID = "create-board"
NO_VENUE_BOARD_ID = "no-venue-board"
MULTI_VENUE_BOARD_ID = "multi-venue-board"
UPDATE_BOARD_ID = "update-board"
CONFLICT_BOARD_ID = "conflict-board"
DUPLICATE_MAPPING_BOARD_A = "duplicate-mapping-board-a"
DUPLICATE_MAPPING_BOARD_B = "duplicate-mapping-board-b"
DEFAULT_VALID_TOKEN = "root-token"


def env_or_skip(name: str) -> str:
    value = os.environ.get(name)
    if not value:
        pytest.skip(f"{name} is required for memory-summary integration tests")
    return value


def default_router_id() -> str:
    return os.environ.get("OWANALYTICS_TEST_ROUTER_ID", DEFAULT_ROUTER_ID)


def router_id() -> str:
    return default_router_id()


def utc_now() -> datetime:
    return datetime.now(timezone.utc)


def format_utc(dt: datetime) -> str:
    return dt.strftime("%Y-%m-%dT%H:%M:%SZ")


def recent_timestamp(minutes_ago: int = 1) -> str:
    return format_utc(utc_now() - timedelta(minutes=minutes_ago))


def board_id() -> str:
    return os.environ.get("OWANALYTICS_TEST_BOARD_ID", DEFAULT_BOARD_ID)


def venue_id() -> str:
    return os.environ.get("OWANALYTICS_TEST_VENUE_ID", DEFAULT_VENUE_ID)


def utc_epoch(value: str) -> int:
    return int(datetime.fromisoformat(value.replace("Z", "+00:00")).timestamp())


def json_bytes(payload: dict[str, Any]) -> bytes:
    return json.dumps(payload, separators=(",", ":")).encode("utf-8")


def valid_token() -> str:
    return os.environ.get("OWANALYTICS_TEST_VALID_TOKEN", DEFAULT_VALID_TOKEN)


@dataclass
class HttpResult:
    status: int
    body: dict[str, Any]


def analytics_url(path: str) -> str:
    return env_or_skip("OWANALYTICS_TEST_URL").rstrip("/") + path


def http_json(
    path: str,
    token: str | None = None,
    *,
    method: str = "GET",
    body: dict[str, Any] | None = None,
) -> HttpResult:
    headers = {"Accept": "application/json"}
    data = None
    if body is not None:
        data = json_bytes(body)
        headers["Content-Type"] = "application/json"
    if token is not None:
        headers["Authorization"] = "Bearer " + token
    request = urllib.request.Request(analytics_url(path), data=data, headers=headers, method=method)
    context = None
    if request.full_url.startswith("https://") and os.environ.get("OWANALYTICS_TEST_VERIFY_TLS") != "1":
        context = ssl._create_unverified_context()
    try:
        with urllib.request.urlopen(request, timeout=10, context=context) as response:
            return HttpResult(response.status, json.loads(response.read().decode("utf-8") or "{}"))
    except urllib.error.HTTPError as exc:
        return HttpResult(exc.code, json.loads(exc.read().decode("utf-8") or "{}"))


def memory_summary_path(
    timestamp_till: str | None = None,
    lookback_hours: str = "1",
    extra_query: str | None = None,
    router_id: str | None = None,
) -> str:
    selected_router = router_id if router_id is not None else default_router_id()
    if timestamp_till is None:
        timestamp_till = recent_timestamp(minutes_ago=1)
    query = {
        "timestampTill": timestamp_till,
        "lookbackHours": lookback_hours,
    }
    encoded = urllib.parse.urlencode(query)
    if extra_query:
        encoded += "&" + extra_query
    return f"/api/v1/devices/{selected_router}/memory-summary?{encoded}"


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


def sql_identifier(value: str) -> str:
    if not value or not all(c.isalnum() or c == "_" for c in value):
        pytest.fail(f"Unsafe SQL identifier in OWANALYTICS_TEST_APP_DB_USER: {value!r}")
    return value


@contextmanager
def db_connection():
    dsn = env_or_skip("OWANALYTICS_TEST_DB_DSN")
    connection = connect_db(dsn)
    try:
        yield connection
        connection.commit()
    finally:
        connection.close()


def cleanup_test_rows(cursor) -> None:
    boards = (
        board_id(),
        board_id() + "-b",
        OLD_BOARD_ID,
        OTHER_BOARD_ID,
        CREATE_BOARD_ID,
        NO_VENUE_BOARD_ID,
        MULTI_VENUE_BOARD_ID,
        UPDATE_BOARD_ID,
        CONFLICT_BOARD_ID,
        DUPLICATE_MAPPING_BOARD_A,
        DUPLICATE_MAPPING_BOARD_B,
    )
    cursor.execute("delete from timepoints where serialnumber in (%s, %s, %s)", (router_id(), router_id() + "9", "other-router"))
    cursor.execute(
        "delete from timepoints where boardid in (%s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s)",
        boards,
    )
    cursor.execute("delete from boards where id in (%s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s)", boards)


def seed_board(cursor, retention: int = 7200, board: str | None = None, venue: str | None = None) -> None:
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
            "Memory Summary Test Board",
            "Integration test board",
            "[]",
            now,
            now,
            venue,
            "Memory Summary Test Venue",
            "Integration test venue",
            retention,
            60,
            False,
        ),
    )


def venue_payload(venue: str | None = None) -> dict[str, Any]:
    return {
        "id": venue or venue_id(),
        "name": "Memory Summary Test Venue",
        "description": "Integration test venue",
        "retention": 7200,
        "interval": 60,
        "monitorSubVenues": False,
    }


def board_payload(
    name: str,
    *,
    description: str = "Integration test board",
    venues: list[dict[str, Any]] | None = None,
) -> dict[str, Any]:
    return {
        "name": name,
        "description": description,
        "venueList": venues if venues is not None else [venue_payload()],
    }


def delete_board_rows(cursor, created_board_id: str) -> None:
    cursor.execute("delete from timepoints where boardid = %s", (created_board_id,))
    cursor.execute("delete from boards where id = %s", (created_board_id,))


def fetch_board_storage(cursor, board: str) -> tuple[str, str, list[dict[str, Any]]]:
    cursor.execute("select name, description, venueId, venueName, venueDescription, retention, interval, monitorSubVenues from boards where id = %s", (board,))
    row = cursor.fetchone()
    assert row is not None
    stored_venues = [{
        "id": row[2],
        "name": row[3],
        "description": row[4],
        "retention": row[5],
        "interval": row[6],
        "monitorSubVenues": bool(row[7]),
    }] if row[2] else []
    return row[0], row[1], stored_venues


def insert_timepoint(
    cursor,
    timestamp: str,
    resource_data: dict[str, Any],
    *,
    board: str | None = None,
    venue: str | None = None,
    serial: str | None = None,
    suffix: str = "",
) -> None:
    board = board or board_id()
    venue = venue or venue_id()
    serial = serial or router_id()
    row_id = f"mem-int-{board}-{serial}-{utc_epoch(timestamp)}-{suffix}".replace(":", "-")
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
            "[]",
            "{}",
            serial,
            json.dumps(resource_data),
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


def assert_empty_summary(body: dict[str, Any], expected_start: str, expected_end: str) -> None:
    assert body["meta"]["requestedWindow"] == {
        "startTime": expected_start,
        "endTime": expected_end,
    }
    assert body["meta"]["observedWindow"] == {"startTime": None, "endTime": None}
    assert body["data"] == {
        "min_memfree": None,
        "max_memfree": None,
        "avg_memfree": None,
        "latest_memfree": None,
    }


def test_memory_summary_aggregates_persisted_samples(seeded_board) -> None:
    now = utc_now()
    end_dt = now - timedelta(seconds=30)
    start_dt = end_dt - timedelta(hours=1)
    t_a = end_dt - timedelta(minutes=55)
    t_b = end_dt - timedelta(minutes=30)
    t_c = end_dt - timedelta(minutes=5)

    with db_connection() as connection:
        with connection.cursor() as cursor:
            insert_timepoint(cursor, format_utc(t_a), {"memory_free": 200000, "memory_total": 512000}, suffix="a")
            insert_timepoint(cursor, format_utc(t_b), {"memory_free": 250000, "memory_total": 512000}, suffix="b")
            insert_timepoint(cursor, format_utc(t_c), {"memory_free": 300000, "memory_total": 512000}, suffix="c")

    token = valid_token()
    result = http_json(memory_summary_path(timestamp_till=format_utc(end_dt), lookback_hours="1"), token)

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
    assert result.body["data"] == {
        "min_memfree": 200000,
        "max_memfree": 300000,
        "avg_memfree": 250000,
        "latest_memfree": 300000,
    }


def test_memory_summary_applies_half_open_window_and_filters_board_and_serial(seeded_board) -> None:
    now = utc_now()
    end_dt = now - timedelta(seconds=30)
    start_dt = end_dt - timedelta(hours=1)
    before_dt = start_dt - timedelta(seconds=1)
    start_sample_dt = start_dt
    inside_dt = end_dt - timedelta(seconds=1)
    end_sample_dt = end_dt
    mid_dt = end_dt - timedelta(minutes=30)

    with db_connection() as connection:
        with connection.cursor() as cursor:
            insert_timepoint(cursor, format_utc(before_dt), {"memory_free": 111111, "memory_total": 512000}, suffix="before")
            insert_timepoint(cursor, format_utc(start_sample_dt), {"memory_free": 200000, "memory_total": 512000}, suffix="start")
            insert_timepoint(cursor, format_utc(inside_dt), {"memory_free": 300000, "memory_total": 512000}, suffix="inside")
            insert_timepoint(cursor, format_utc(end_sample_dt), {"memory_free": 999999, "memory_total": 999999}, suffix="end")
            insert_timepoint(
                cursor,
                format_utc(mid_dt),
                {"memory_free": 1, "memory_total": 512000},
                serial="other-router",
                suffix="other-router",
            )
            insert_timepoint(
                cursor,
                format_utc(mid_dt),
                {"memory_free": 2, "memory_total": 512000},
                board=OTHER_BOARD_ID,
                suffix="other-board",
            )

    result = http_json(memory_summary_path(timestamp_till=format_utc(end_dt), lookback_hours="1"), valid_token())

    assert result.status == 200
    assert result.body["data"] == {
        "min_memfree": 200000,
        "max_memfree": 300000,
        "avg_memfree": 250000,
        "latest_memfree": 300000,
    }
    assert result.body["meta"]["observedWindow"] == {
        "startTime": format_utc(start_sample_dt),
        "endTime": format_utc(inside_dt),
    }


def test_memory_summary_no_samples_returns_empty_success(seeded_board) -> None:
    now = utc_now()
    end_dt = now - timedelta(seconds=30)
    start_dt = end_dt - timedelta(hours=1)

    result = http_json(memory_summary_path(timestamp_till=format_utc(end_dt), lookback_hours="1"), valid_token())

    assert result.status == 200
    assert_empty_summary(result.body, format_utc(start_dt), format_utc(end_dt))


def test_memory_summary_ignores_invalid_resource_rows(seeded_board) -> None:
    now = utc_now()
    end_dt = now - timedelta(seconds=30)
    t_empty = end_dt - timedelta(minutes=55)
    t_missing = end_dt - timedelta(minutes=50)
    t_corrupt = end_dt - timedelta(minutes=45)
    t_valid_a = end_dt - timedelta(minutes=40)
    t_valid_b = end_dt - timedelta(minutes=35)

    with db_connection() as connection:
        with connection.cursor() as cursor:
            insert_timepoint(cursor, format_utc(t_empty), {}, suffix="empty")
            insert_timepoint(cursor, format_utc(t_missing), {"memory_total": 512000}, suffix="missing-free")
            insert_timepoint(cursor, format_utc(t_corrupt), {"memory_free": 700000, "memory_total": 512000}, suffix="corrupt")
            insert_timepoint(cursor, format_utc(t_valid_a), {"memory_free": 200000, "memory_total": 512000}, suffix="valid-a")
            insert_timepoint(cursor, format_utc(t_valid_b), {"memory_free": 300000}, suffix="valid-b")

    result = http_json(memory_summary_path(timestamp_till=format_utc(end_dt), lookback_hours="1"), valid_token())

    assert result.status == 200
    assert result.body["data"] == {
        "min_memfree": 200000,
        "max_memfree": 300000,
        "avg_memfree": 250000,
        "latest_memfree": 300000,
    }
    assert result.body["meta"]["observedWindow"] == {
        "startTime": format_utc(t_valid_a),
        "endTime": format_utc(t_valid_b),
    }


def test_memory_summary_missing_auth_rejects_before_query_validation() -> None:
    result = http_json(memory_summary_path(timestamp_till="not-a-timestamp", lookback_hours="bad"))

    assert result.status == 401
    assert result.body["error"] == "unauthorized"


def test_memory_summary_invalid_token_rejects_via_fake_owsec() -> None:
    result = http_json(memory_summary_path(timestamp_till=recent_timestamp(), lookback_hours="1"), "bad-token")

    assert result.status == 401
    assert result.body["error"] == "unauthorized"


def test_memory_summary_invalid_query_rejects_after_auth() -> None:
    token = valid_token()
    result = http_json(memory_summary_path(timestamp_till=recent_timestamp(), lookback_hours="1", extra_query="unexpected=true"), token)

    assert result.status == 400
    assert result.body["error"] == "invalid_query_parameter"


def test_board_create_with_exactly_one_venue_succeeds() -> None:
    with db_connection() as connection:
        with connection.cursor() as cursor:
            cleanup_test_rows(cursor)

    token = valid_token()
    result = http_json(
        "/api/v1/board/0",
        token,
        method="POST",
        body=board_payload("Create Board"),
    )

    created_board_id = result.body.get("id")
    try:
        assert result.status == 200
        assert isinstance(created_board_id, str)
        assert created_board_id
        with db_connection() as connection:
            with connection.cursor() as cursor:
                _, _, stored_venues = fetch_board_storage(cursor, created_board_id)
                assert [venue["id"] for venue in stored_venues] == [venue_id()]
    finally:
        if created_board_id:
            with db_connection() as connection:
                with connection.cursor() as cursor:
                    delete_board_rows(cursor, created_board_id)
        with db_connection() as connection:
            with connection.cursor() as cursor:
                cleanup_test_rows(cursor)


def test_board_create_with_no_venue_fails() -> None:
    with db_connection() as connection:
        with connection.cursor() as cursor:
            cleanup_test_rows(cursor)

    result = http_json(
        "/api/v1/board/0",
        valid_token(),
        method="POST",
        body=board_payload("No Venue Board", venues=[]),
    )

    created_board_id = result.body.get("id")
    try:
        assert result.status == 400
    finally:
        if created_board_id:
            with db_connection() as connection:
                with connection.cursor() as cursor:
                    delete_board_rows(cursor, created_board_id)
        with db_connection() as connection:
            with connection.cursor() as cursor:
                cleanup_test_rows(cursor)


def test_board_create_with_multiple_venues_fails() -> None:
    with db_connection() as connection:
        with connection.cursor() as cursor:
            cleanup_test_rows(cursor)

    result = http_json(
        "/api/v1/board/0",
        valid_token(),
        method="POST",
        body=board_payload(
            "Multi Venue Board",
            venues=[venue_payload(), venue_payload(venue_id() + "-second")],
        ),
    )

    created_board_id = result.body.get("id")
    try:
        assert result.status == 400
    finally:
        if created_board_id:
            with db_connection() as connection:
                with connection.cursor() as cursor:
                    delete_board_rows(cursor, created_board_id)
        with db_connection() as connection:
            with connection.cursor() as cursor:
                cleanup_test_rows(cursor)


def test_board_create_rejects_venue_already_assigned_to_another_board() -> None:
    with db_connection() as connection:
        with connection.cursor() as cursor:
            cleanup_test_rows(cursor)
            seed_board(cursor)

    token = valid_token()
    result = http_json(
        "/api/v1/board/0",
        token,
        method="POST",
        body={
            "id": CONFLICT_BOARD_ID,
            "name": "Conflict Board",
            "venueList": [
                {
                    "id": venue_id(),
                    "name": "Venue A",
                    "description": "",
                    "retention": 7200,
                    "interval": 60,
                    "monitorSubVenues": False,
                },
            ],
        },
    )

    created_board_id = result.body.get("id")
    try:
        assert result.status == 400
    finally:
        if created_board_id:
            with db_connection() as connection:
                with connection.cursor() as cursor:
                    delete_board_rows(cursor, created_board_id)
        with db_connection() as connection:
            with connection.cursor() as cursor:
                cleanup_test_rows(cursor)


def test_board_create_concurrent_same_venue_rejects_second() -> None:
    with db_connection() as connection:
        with connection.cursor() as cursor:
            cleanup_test_rows(cursor)

    target_venue = venue_id() + "-concurrent"

    def create_request(board_name: str):
        return http_json(
            "/api/v1/board/0",
            valid_token(),
            method="POST",
            body=board_payload(board_name, venues=[venue_payload(target_venue)]),
        )

    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
        f1 = executor.submit(create_request, "Concurrent Board A")
        f2 = executor.submit(create_request, "Concurrent Board B")
        r1 = f1.result()
        r2 = f2.result()

    statuses = [r1.status, r2.status]
    created_board_ids = [r.body.get("id") for r in (r1, r2) if isinstance(r.body.get("id"), str)]

    try:
        assert sorted(statuses) == [200, 400]
        failed_res = r1 if r1.status == 400 else r2
        assert "Venue is already assigned to another board" in failed_res.body.get("ErrorDescription", "")
        with db_connection() as connection:
            with connection.cursor() as cursor:
                cursor.execute(
                    "select count(*) from boards where venueId = %s",
                    (target_venue,),
                )
                cnt = cursor.fetchone()[0]
                assert cnt == 1
    finally:
        for bid in created_board_ids:
            if bid:
                with db_connection() as connection:
                    with connection.cursor() as cursor:
                        delete_board_rows(cursor, bid)
        with db_connection() as connection:
            with connection.cursor() as cursor:
                cleanup_test_rows(cursor)


def test_board_update_name_and_description_without_venue_change_succeeds() -> None:
    with db_connection() as connection:
        with connection.cursor() as cursor:
            cleanup_test_rows(cursor)
            seed_board(cursor, board=UPDATE_BOARD_ID)

    try:
        result = http_json(
            f"/api/v1/board/{UPDATE_BOARD_ID}",
            valid_token(),
            method="PUT",
            body={
                "name": "Updated Board Name",
                "description": "Updated board description",
            },
        )

        assert result.status == 200
        with db_connection() as connection:
            with connection.cursor() as cursor:
                name, description, stored_venues = fetch_board_storage(cursor, UPDATE_BOARD_ID)
                assert name == "Updated Board Name"
                assert description == "Updated board description"
                assert [venue["id"] for venue in stored_venues] == [venue_id()]
    finally:
        with db_connection() as connection:
            with connection.cursor() as cursor:
                cleanup_test_rows(cursor)


def test_board_put_with_same_existing_venue_succeeds() -> None:
    with db_connection() as connection:
        with connection.cursor() as cursor:
            cleanup_test_rows(cursor)
            seed_board(cursor, board=UPDATE_BOARD_ID)

    try:
        result = http_json(
            f"/api/v1/board/{UPDATE_BOARD_ID}",
            valid_token(),
            method="PUT",
            body=board_payload("Same Venue Update"),
        )

        assert result.status == 200
        with db_connection() as connection:
            with connection.cursor() as cursor:
                name, _, stored_venues = fetch_board_storage(cursor, UPDATE_BOARD_ID)
                assert name == "Same Venue Update"
                assert [venue["id"] for venue in stored_venues] == [venue_id()]
    finally:
        with db_connection() as connection:
            with connection.cursor() as cursor:
                cleanup_test_rows(cursor)


def test_board_put_attempting_venue_reassignment_fails_without_changing_storage() -> None:
    with db_connection() as connection:
        with connection.cursor() as cursor:
            cleanup_test_rows(cursor)
            seed_board(cursor, board=UPDATE_BOARD_ID)

    try:
        result = http_json(
            f"/api/v1/board/{UPDATE_BOARD_ID}",
            valid_token(),
            method="PUT",
            body=board_payload(
                "Rejected Venue Update",
                description="Rejected description",
                venues=[venue_payload(venue_id() + "-second")],
            ),
        )

        assert result.status == 400
        assert "Venue reassignment is not supported" in result.body["ErrorDescription"]
        with db_connection() as connection:
            with connection.cursor() as cursor:
                name, description, stored_venues = fetch_board_storage(cursor, UPDATE_BOARD_ID)
                assert name == "Memory Summary Test Board"
                assert description == "Integration test board"
                assert [venue["id"] for venue in stored_venues] == [venue_id()]
    finally:
        with db_connection() as connection:
            with connection.cursor() as cursor:
                cleanup_test_rows(cursor)


def test_board_delete_succeeds() -> None:
    with db_connection() as connection:
        with connection.cursor() as cursor:
            cleanup_test_rows(cursor)
            seed_board(cursor, board=UPDATE_BOARD_ID)

    try:
        result = http_json(
            f"/api/v1/board/{UPDATE_BOARD_ID}",
            valid_token(),
            method="DELETE",
        )

        assert result.status == 200
        with db_connection() as connection:
            with connection.cursor() as cursor:
                cursor.execute("select id from boards where id = %s", (UPDATE_BOARD_ID,))
                assert cursor.fetchone() is None
    finally:
        with db_connection() as connection:
            with connection.cursor() as cursor:
                cleanup_test_rows(cursor)


def test_memory_summary_duplicate_legacy_boards_returns_conflict_409() -> None:
    with db_connection() as connection:
        with connection.cursor() as cursor:
            cleanup_test_rows(cursor)
            seed_board(cursor, board=DUPLICATE_MAPPING_BOARD_A)
            seed_board(cursor, board=DUPLICATE_MAPPING_BOARD_B)

    try:
        result = http_json(memory_summary_path(timestamp_till=recent_timestamp(), lookback_hours="1"), valid_token())
        assert result.status == 409
        assert result.body["error"] == "multiple_boards"
    finally:
        with db_connection() as connection:
            with connection.cursor() as cursor:
                cleanup_test_rows(cursor)


def test_memory_summary_current_router_ownership_controls_board_data(seeded_board) -> None:
    now = utc_now()
    end_dt = now - timedelta(seconds=30)
    t_old = end_dt - timedelta(minutes=45)
    t_cur_a = end_dt - timedelta(minutes=40)
    t_cur_b = end_dt - timedelta(minutes=35)

    with db_connection() as connection:
        with connection.cursor() as cursor:
            insert_timepoint(
                cursor,
                format_utc(t_old),
                {"memory_free": 999999, "memory_total": 999999},
                board=OLD_BOARD_ID,
                suffix="old",
            )
            insert_timepoint(cursor, format_utc(t_cur_a), {"memory_free": 200000, "memory_total": 512000}, suffix="current-a")
            insert_timepoint(cursor, format_utc(t_cur_b), {"memory_free": 300000, "memory_total": 512000}, suffix="current-b")

    result = http_json(memory_summary_path(timestamp_till=format_utc(end_dt), lookback_hours="1"), valid_token())

    assert result.status == 200
    assert result.body["data"] == {
        "min_memfree": 200000,
        "max_memfree": 300000,
        "avg_memfree": 250000,
        "latest_memfree": 300000,
    }


def test_memory_summary_retention_is_enforced_from_resolved_venue() -> None:
    with db_connection() as connection:
        with connection.cursor() as cursor:
            cleanup_test_rows(cursor)
            seed_board(cursor, retention=3600)

    try:
        result = http_json(memory_summary_path(timestamp_till=recent_timestamp(), lookback_hours="2"), valid_token())
        assert result.status == 400
        assert result.body["error"] == "invalid_lookback_hours"
    finally:
        with db_connection() as connection:
            with connection.cursor() as cursor:
                cleanup_test_rows(cursor)


def test_memory_summary_without_analytics_board_mapping_returns_not_found() -> None:
    with db_connection() as connection:
        with connection.cursor() as cursor:
            cleanup_test_rows(cursor)

    result = http_json(memory_summary_path(timestamp_till=recent_timestamp(), lookback_hours="1"), valid_token())

    assert result.status == 404
    assert result.body["error"] == "not_found"


def test_memory_summary_unknown_router_from_owprov_returns_not_found(seeded_board) -> None:
    result = http_json(
        memory_summary_path(
            timestamp_till=recent_timestamp(),
            lookback_hours="1",
            router_id=UNKNOWN_ROUTER_ID,
        ),
        valid_token(),
    )

    assert result.status == 404
    assert result.body["error"] == "not_found"


def test_memory_summary_forbidden_router_from_owprov_returns_not_found(seeded_board) -> None:
    result = http_json(
        memory_summary_path(
            timestamp_till=recent_timestamp(),
            lookback_hours="1",
            router_id=UNAUTHORIZED_ROUTER_ID,
        ),
        valid_token(),
    )

    assert result.status == 404
    assert result.body["error"] == "not_found"


def test_memory_summary_invalid_owprov_response_returns_bad_gateway() -> None:
    result = http_json(
        memory_summary_path(
            timestamp_till=recent_timestamp(),
            lookback_hours="1",
            router_id=INVALID_RESPONSE_ROUTER_ID,
        ),
        valid_token(),
    )

    assert result.status == 502
    assert result.body["error"] == "owprov_invalid_response"


def test_memory_summary_timepoint_storage_failure_returns_memory_specific_error() -> None:
    if os.environ.get("OWANALYTICS_TEST_ENABLE_STORAGE_FAILURE") != "1":
        pytest.skip("Set OWANALYTICS_TEST_ENABLE_STORAGE_FAILURE=1 to run this DB grant test")

    admin_dsn = env_or_skip("OWANALYTICS_TEST_DB_ADMIN_DSN")
    app_user = sql_identifier(env_or_skip("OWANALYTICS_TEST_APP_DB_USER"))

    with db_connection() as connection:
        with connection.cursor() as cursor:
            cleanup_test_rows(cursor)
            seed_board(cursor)

    admin = connect_db(admin_dsn)
    try:
        with admin.cursor() as cursor:
            cursor.execute(f"revoke select on table timepoints from {app_user}")
        admin.commit()

        result = http_json(memory_summary_path(timestamp_till=recent_timestamp(), lookback_hours="1"), valid_token())
        assert result.status == 500
        assert result.body["error"] == "memory_summary_query_failed"
        assert result.body["message"] == "Unable to retrieve gateway memory history"
    finally:
        with admin.cursor() as cursor:
            cursor.execute(f"grant select on table timepoints to {app_user}")
        admin.commit()
        admin.close()
        with db_connection() as connection:
            with connection.cursor() as cursor:
                cleanup_test_rows(cursor)


def test_memory_summary_filters_by_venue_and_serial_number() -> None:
    venue_a = venue_id()
    venue_b = venue_id() + "-b"
    board_a = board_id()
    board_b = board_id() + "-b"
    router_1 = router_id()
    router_2 = router_id() + "9"

    now = utc_now()
    end_dt = now - timedelta(seconds=30)
    t1 = end_dt - timedelta(minutes=45)
    t2 = end_dt - timedelta(minutes=40)
    t3 = end_dt - timedelta(minutes=35)

    with db_connection() as connection:
        with connection.cursor() as cursor:
            cleanup_test_rows(cursor)
            seed_board(cursor, board=board_a, venue=venue_a)
            seed_board(cursor, board=board_b, venue=venue_b)
            # Sample for venue_a + router_1 -> memory_free 100000
            insert_timepoint(cursor, format_utc(t1), {"memory_free": 100000, "memory_total": 512000}, board=board_a, venue=venue_a, serial=router_1, suffix="vA-r1")
            # Sample for venue_b + router_1 -> memory_free 200000 (different venue, same serial)
            insert_timepoint(cursor, format_utc(t2), {"memory_free": 200000, "memory_total": 512000}, board=board_b, venue=venue_b, serial=router_1, suffix="vB-r1")
            # Sample for venue_a + router_2 -> memory_free 300000 (same venue, different serial)
            insert_timepoint(cursor, format_utc(t3), {"memory_free": 300000, "memory_total": 512000}, board=board_a, venue=venue_a, serial=router_2, suffix="vA-r2")

    try:
        # Querying venue_a + router_1 must return ONLY the 100000 sample
        result = http_json(memory_summary_path(timestamp_till=format_utc(end_dt), lookback_hours="1", router_id=router_1), valid_token())
        assert result.status == 200
        assert result.body["data"]["min_memfree"] == 100000
        assert result.body["data"]["max_memfree"] == 100000
        assert result.body["data"]["avg_memfree"] == 100000
        assert result.body["data"]["latest_memfree"] == 100000
    finally:
        with db_connection() as connection:
            with connection.cursor() as cursor:
                cleanup_test_rows(cursor)


def test_memory_summary_path_router_id_override() -> None:
    path1 = memory_summary_path(router_id="router-1")
    path2 = memory_summary_path(router_id="router-2")
    assert "/api/v1/devices/router-1/memory-summary" in path1
    assert "/api/v1/devices/router-2/memory-summary" in path2
