# Analytics MCP APIs & Gateway Availability Service — Comprehensive Test Specification

## 1. Scope

This document defines the comprehensive test suite for the `ra-wlan-cloud-analytics` service, covering all five MCP tool endpoints and the background Gateway Availability tracking pipeline:

```http
GET /api/v1/devices/{routerId}/memory-summary
GET /api/v1/devices/{routerId}/radio-temperature-summary
GET /api/v1/devices/{routerId}/wifi-clients/usage-summary
GET /api/v1/devices/{routerId}/wifi-clients/rssi-summary
GET /api/v1/devices/{routerId}/availability-summary
```

The APIs correspond to these MCP tools:

```text
get_gateway_free_memory
get_gateway_wifi_temp
get_device_bandwidth_consumption
get_device_rssi_quality
get_gateway_offline_count
```

The specification and test suite are modularized into three distinct architectural components:
1. **Public API Contract Specification**: OpenAPI 3.0 schemas (`openapi/owanalytics.yaml` v2.7.0) defining external REST endpoints, query parameters, HTTP status codes, and response envelopes.
2. **Persistence & Pipeline Architecture Design**: Backend storage structures (`device_availability_events`, `device_availability_state`, `device_availability_ingestion_checkpoint`, `device_availability_ingestion_gaps`), Kafka event consumption/ordering semantics, and cutover/migration behavior.
3. **Test Specification & Verification Matrix**: Independent verification matrix (this document) defining assertion criteria for API contracts, integration workflows, white-box database rules, and failure modes.

> [!IMPORTANT]
> This PR reframes and defines the complete target contract and test specification suite. The specified production architecture changes—including four new availability persistence tables (`device_availability_events`, `device_availability_state`, `device_availability_ingestion_checkpoint`, `device_availability_ingestion_gaps`), Kafka event ordering/checkpointing semantics, cutover migration rules, and OpenAPI v2.7.0 endpoint schemas—constitute a production architecture specification. Approving or merging this test specification PR does NOT bypass separate explicit architecture design sign-off for backend schema additions and production Kafka pipeline changes prior to production deployment.

The test cases cover:
* API contract tests for request shapes, HTTP status codes, response schemas, parameter validation, filtering, and half-open time-range window semantics `[startTime, endTime)`.
* Service integration tests for Kafka topic consumption, OWPROV fallback resolution, `VenueCoordinator` maintained ownership map, process-level router resolution cache, and PostgreSQL storage queries.
* Database/white-box tests for `device_availability_events`, `device_availability_state`, `timepoints`, and `wificlienthistory` table schemas, constraints, and event counting behavior.
* End-to-end physical device scenarios covering gateway shutdown, power restoration, cable removal, and reconnection flows.

---

# 2. Common Preconditions and Database Setup

Before executing the test cases:

1. Analytics service is running and connected to PostgreSQL.
2. Analytics is consuming the Kafka `connection` topic.
3. The test gateway is registered in OWPROV with a known `serialNumber`.
4. The test gateway is associated with a venue and mapped to an Analytics board where applicable.
5. `VenueCoordinator` contains the current `routerId → boardId` mapping.
6. The test gateway regularly sends `ping` or `capabilities` messages (approximate interval: 2 minutes).
7. The following storage tables are available:

```text
device_availability_events
device_availability_state
device_availability_ingestion_checkpoint
device_availability_ingestion_gaps
timepoints
wificlienthistory
```

8. Gateway availability uses `device_availability_state` as the authoritative restart-safe state table and `device_availability_events` as the transition log. Kafka `disconnection` messages are treated as `offline`; Kafka `ping` and `capabilities` messages are treated as `online`. Exact transition counting requires serialNumber-scoped source-event ordering before state-machine processing. Every accepted source-newer message emitted by that ordering stage updates `device_availability_state.last_event_time`. `DeviceInfo.lastContact` is contact/processing metadata and is not used for source-event ordering.
9. Availability exact-zero responses require durable per-serial coverage proof in `device_availability_ingestion_checkpoint` and no overlapping persisted gap in `device_availability_ingestion_gaps`.
10. Database records for the test gateway are cleared or isolated before each independent test.
11. A valid authorization token is available for testing endpoint access.

Example test parameters:

```text
routerId: 60cf84f22290
boardId: board-test-01
venueId: venue-test-01
timestampTill: 2026-07-29T12:00:00Z
lookbackHours: 24
```

---

# 3. Common Service Integration and API Contract Test Cases

## TC-COMMON-001: Valid router resolves from local ownership map

### Steps

1. Add the gateway to a monitored Analytics board.
2. Confirm that `VenueCoordinator` contains:

```text
60cf84f22290 → board-test-01
```

3. Call the memory-summary, radio-temperature-summary, usage-summary, and rssi-summary APIs.
4. Call the availability-summary API.

### Expected result

* The router is resolved using the maintained local map.
* A full OWPROV inventory lookup is not required.
* For memory, temperature, usage, and RSSI, Analytics queries metric data using:

```text
boardId = board-test-01
serialNumber = 60cf84f22290
```

* For availability-summary:
  * Current board and venue ownership are used for request authorization only.
  * Historical availability events are queried by durable `serialNumber`.
  * Historical `board_id` is nullable context and is not required to match the current board.
* The request succeeds.

---

## TC-COMMON-002: Router resolves after local-map cache miss

### Steps

1. Remove or expire the gateway mapping from the local map.
2. Keep the gateway registered in OWPROV.
3. Call an API for the gateway.
4. Observe the resolution process.

### Expected result

* Analytics performs a status-aware OWPROV inventory lookup.
* The venue is read from `inventoryTag.venue`.
* Board ownership is determined from the monitored venue device lists.
* The local `routerId → boardId` map is refreshed.
* The request succeeds.

---

## TC-COMMON-003: Gateway belongs to a child venue

### Preconditions

* Board monitors a parent venue with `monitorSubVenues = true`.
* Gateway belongs to a child venue.

### Steps

1. Call each metric API using the gateway serial number.
2. Observe board resolution.

### Expected result

* Gateway resolves to the parent venue's Analytics board.
* The implementation does not require the gateway venue to be directly listed as the board venue.
* Each API returns the gateway's data.

---

## TC-COMMON-004: Router is not found in OWPROV

### Steps

Call an API with a syntactically valid but nonexistent router ID:

```http
GET /api/v1/devices/deadbeef1234/memory-summary
    ?timestampTill=2026-07-29T12:00:00Z
    &lookbackHours=24
```

### Expected result

* HTTP `404 Not Found`.
* Response indicates that the inventory device was not found.
* Analytics does not query another gateway's data.

---

## TC-COMMON-005: Invalid router ID syntax

### Steps

Call an API with a syntactically invalid router ID (e.g. containing invalid dot-segment punctuation `.`):

```http
GET /api/v1/devices/unknown.router/memory-summary
    ?timestampTill=2026-07-29T12:00:00Z
    &lookbackHours=24
```

### Expected result

* HTTP `400 Bad Request`.
* Error is `invalid_router_id`.
* OWPROV ownership lookup and metric aggregation are not executed.

---

## TC-COMMON-005A: Router ID syntax boundaries

### Objective

Verify the public `routerId` contract: path-safe strings only (1 to 64 alphanumeric characters, hyphens, or underscores matching `^[a-zA-Z0-9_-]+$`). Syntax validation runs before OWPROV ownership resolution.

### Requests and expected results

| routerId path segment | Expected result |
| --- | --- |
| `abcdef123456` | Syntax is accepted. If the gateway does not exist or is outside scope, return HTTP `404 Not Found`, `error: "not_found"`. |
| `gateway-serial-1234` | Syntax is accepted (non-hex alphanumeric string with hyphens). If the gateway does not exist in OWPROV, return HTTP `404 Not Found`, `error: "not_found"`. |
| `ABCDEF123456` | Syntax is accepted. If the gateway does not exist or is outside scope, return HTTP `404 Not Found`, `error: "not_found"`. |
| `.` | Handler-level validation returns HTTP `400 Bad Request`, `error: "invalid_router_id"`. End-to-end route tests may observe framework-level rejection first if the server normalizes dot-segments, but OWPROV lookup must not run. |
| `..` | Handler-level validation returns HTTP `400 Bad Request`, `error: "invalid_router_id"`. End-to-end route tests may observe framework-level rejection first if the server normalizes dot-segments, but OWPROV lookup must not run. |
| `abc%2Fdef123456` | Rejected before OWPROV lookup. Encoded path separators must not be decoded into a router ID that reaches ownership resolution. |
| `:::`, `router space` | HTTP `400 Bad Request`, `error: "invalid_router_id"` because punctuation or spaces violate path-safety validation. |
| (string > 64 chars) | HTTP `400 Bad Request`, `error: "invalid_router_id"` because max length is 64 characters. |

For all rejected syntax cases, OWPROV ownership lookup and metric aggregation are not executed.

---

## TC-COMMON-006: Router inventory has no venue

### Preconditions

* Router exists in OWPROV.
* `inventoryTag.venue` is empty or missing.

### Expected result

* HTTP `404 Not Found`.
* Request does not continue to metric aggregation.

---

## TC-COMMON-007: No Analytics board monitors the venue

### Preconditions

* Router and venue exist.
* No Analytics board is configured for the venue.

### Expected result

* HTTP `404 Not Found`.
* Response indicates that no Analytics board is configured.

---

## TC-COMMON-008: Router matches multiple boards

### Preconditions

* The same router is incorrectly present in two board device lists.
* No deterministic ownership rule resolves the conflict.

### Expected result

* HTTP `409 Conflict`.
* No arbitrary board is selected.
* No metric data is returned.

---



## TC-COMMON-009: Valid timestamp and lookback

### Request

```http
?timestampTill=2026-07-29T12:00:00Z
&lookbackHours=24
```

### Expected result

```text
startTime = 2026-07-28T12:00:00Z
endTime = 2026-07-29T12:00:00Z
```

Only samples inside the half-open range `[startTime, endTime)` are used.

---

## TC-COMMON-010: Invalid timestamp

### Request

```http
?timestampTill=not-a-date
&lookbackHours=24
```

### Expected result

* HTTP `400 Bad Request`.
* Error identifies `timestampTill` as invalid.

---

## TC-COMMON-011: Unsupported timezone format or numeric offset

### Objective

Verify that non-UTC timezone formats, explicit numeric timezone offsets, or missing timezone designators are rejected.

### Requests

1. Request with explicit numeric timezone offset:

```http
GET /api/v1/devices/60cf84f22290/memory-summary
    ?timestampTill=2026-07-29T12:00:00%2B05:30
    &lookbackHours=24
```

2. Request with missing timezone designator:

```http
GET /api/v1/devices/60cf84f22290/memory-summary
    ?timestampTill=2026-07-29T12:00:00
    &lookbackHours=24
```

### Expected result

* HTTP `400 Bad Request`.
* Response resembles:

```json
{
  "error": "invalid_timestamp",
  "message": "timestampTill must be a valid UTC timestamp ending with 'Z'"
}
```

* OWPROV ownership lookup and metric aggregation are not executed.

---

## TC-COMMON-011A: Literal unencoded plus in timestamp is malformed

### Objective

Verify that a literal `+` in a query string is not treated as a valid numeric timezone offset. HTTP frameworks commonly decode `+` as a space in query parameters.

### Request

```http
GET /api/v1/devices/60cf84f22290/memory-summary
    ?timestampTill=2026-07-29T12:00:00+05:30
    &lookbackHours=24
```

### Expected result

* HTTP `400 Bad Request`.
* Error is `invalid_timestamp`.
* The encoded numeric-offset case in `TC-COMMON-011` is still required to prove valid RFC3339 numeric offsets are intentionally unsupported.

---

## TC-COMMON-012: Semantically invalid timestamp

### Request

```http
?timestampTill=2026-99-99T88:77:66Z
&lookbackHours=24
```

### Expected result

* HTTP `400 Bad Request`.
* Error is `invalid_timestamp`.
* The value is rejected even though it matches the timestamp shape.
* No database aggregation is performed.

---

## TC-COMMON-012A: Repeated timestampTill parameter

### Request

```http
?timestampTill=2026-07-29T12:00:00Z
&timestampTill=2026-07-29T13:00:00Z
&lookbackHours=24
```

### Expected result

* HTTP `400 Bad Request`.
* Error is `invalid_timestamp`.
* Repeated `timestampTill` query parameters are ambiguous and must not be accepted by framework parameter binders silently selecting the first or last value. Handlers must inspect the raw query collection to enforce exactly one `timestampTill` parameter.

---

## TC-COMMON-013: Zero lookback

### Request

```http
?timestampTill=2026-07-29T12:00:00Z
&lookbackHours=0
```

### Expected result

* HTTP `400 Bad Request`.
* Error is `invalid_lookback_hours`, not `invalid_timestamp`.

---

## TC-COMMON-014: Negative lookback

### Request

```http
?timestampTill=2026-07-29T12:00:00Z
&lookbackHours=-1
```

### Expected result

* HTTP `400 Bad Request`.
* Error is `invalid_lookback_hours`, not `invalid_timestamp`.

---

## TC-COMMON-015: Lookback exceeds configured maximum

### Request

```http
?timestampTill=2026-07-29T12:00:00Z
&lookbackHours=10000
```

### Expected result

* HTTP `400 Bad Request`.
* Error is `invalid_lookback_hours`, not `invalid_timestamp`.
* Error message may indicate that the maximum supported lookback was exceeded.

---

## TC-COMMON-016: Missing timestamp

### Request

```http
?lookbackHours=24
```

### Expected result

* HTTP `400 Bad Request`.
* Error is `invalid_timestamp`.

---

## TC-COMMON-017: Missing lookback

### Request

```http
?timestampTill=2026-07-29T12:00:00Z
```

### Expected result

* HTTP `400 Bad Request`.
* Error is `invalid_lookback_hours`, not `invalid_timestamp`.

---

## TC-COMMON-017A: Non-numeric lookback

### Request

```http
?timestampTill=2026-07-29T12:00:00Z
&lookbackHours=abc
```

### Expected result

* HTTP `400 Bad Request`.
* Error is `invalid_lookback_hours`, not `invalid_timestamp`.
* The value is not accepted as `0` or as a missing parameter.

---

## TC-COMMON-017B: Fractional lookback

### Request

```http
?timestampTill=2026-07-29T12:00:00Z
&lookbackHours=1.5
```

### Expected result

* HTTP `400 Bad Request`.
* Error is `invalid_lookback_hours`, not `invalid_timestamp`.
* `lookbackHours` must be parsed as a strict whole decimal integer.

---

## TC-COMMON-017C: Partially numeric lookback

### Request

```http
?timestampTill=2026-07-29T12:00:00Z
&lookbackHours=24hours
```

### Expected result

* HTTP `400 Bad Request`.
* Error is `invalid_lookback_hours`, not `invalid_timestamp`.
* Parsers must reject partial conversions such as `atoi("24hours") == 24`.

---

## TC-COMMON-017D: Empty lookback

### Request

```http
?timestampTill=2026-07-29T12:00:00Z
&lookbackHours=
```

### Expected result

* HTTP `400 Bad Request`.
* Error is `invalid_lookback_hours`, not `invalid_timestamp`.

---

## TC-COMMON-017E: Overflowing lookback

### Request

```http
?timestampTill=2026-07-29T12:00:00Z
&lookbackHours=999999999999999999999
```

### Expected result

* HTTP `400 Bad Request`.
* Error is `invalid_lookback_hours`, not `invalid_timestamp`.
* Repeated `lookbackHours` query parameters are ambiguous and must not be accepted by picking the first or last value.

---

## TC-COMMON-017G: Timestamp arithmetic underflow before Unix epoch

### Request

```http
?timestampTill=1970-01-01T00:00:00Z
&lookbackHours=1
```

### Expected result

* HTTP `400 Bad Request`.
* Error is `invalid_timestamp`.
* Message states `"Calculated startTime precedes supported Unix epoch minimum"`.
* Unsigned integer underflow / wraparound to large positive timestamps (e.g. `18446744073709548616`) is strictly prohibited.

---

## TC-COMMON-018: Missing authorization token

### Expected result

* HTTP `401 Unauthorized`.
* No gateway data is returned.

---

## TC-COMMON-019: User is not authorized for the gateway scope

### Preconditions

* Caller has a valid token.
* Caller lacks `analytics.gateway_metrics.read` on the resolved board, venue and parent entity.

### Expected result

* HTTP `404 Not Found`.
* Error is `not_found`.
* The response does not reveal whether the gateway exists.

---

## TC-COMMON-020: Monitoring is disabled

### Preconditions

* Router ownership resolves successfully.
* Monitoring is disabled for the resolved router scope.

### Expected result

* HTTP `409 Conflict`.
* Error is `monitoring_disabled`.
* Metric aggregation is not executed.

---

## TC-COMMON-021: Monitoring is not configured

### Preconditions

* Router ownership resolves successfully.
* No monitoring configuration exists for the resolved router scope.

### Expected result

* HTTP `404 Not Found`.
* Error is `monitoring_not_configured`.
* Metric aggregation is not executed.

---

## TC-COMMON-022: Requested range outside retention

### Request

Use a valid router ID and a lookback window whose calculated `[startTime, endTime)` falls outside the configured monitoring retention window.

### Expected result

* HTTP `400 Bad Request`.
* Error is `lookback_outside_retention`.
* Metric aggregation is not executed.

---

## TC-COMMON-023: Availability range before cutover

### Request

Call `GET /api/v1/devices/{routerId}/availability-summary` with a calculated `startTime` before `availabilityValidFrom`.

### Expected result

* HTTP `400 Bad Request`.
* Error is `availability_range_before_cutover`.
* The API does not return `offline_count: 0` for pre-cutover ranges.

---

## TC-COMMON-024: Router-resolution cache expires

### Preconditions

* A positive router-resolution cache entry exists.
* Its TTL has expired.

### Expected result

* The expired entry is not used for authorization or metric lookup.
* Router ownership is refreshed before aggregation.
* The response reflects the refreshed ownership context.

---

## TC-COMMON-025: Router ownership version changes

### Preconditions

* A positive router-resolution cache entry exists.
* The current ownership version is newer than the cached entry's ownership version.

### Expected result

* The stale cache entry is invalidated.
* Router ownership is refreshed before aggregation.
* Stale ownership cannot grant access to another caller's router data.

---



# 4. Gateway Availability Service & availability-summary Test Cases

## 4.1. Database/White-Box Validation Queries

## TC-AVAIL-SCHEMA-001: Availability state table schema exists

### Objective

Verify that the required restart-safe state table exists with deterministic fields, sequence tracking, and constraints.

### Expected result

* `device_availability_state` exists.
* `serialNumber` is the primary identity for a gateway state row.
* Required fields exist:

```text
serialNumber
board_id
current_state
last_event_time
last_source_sequence
last_idempotency_key
updated_at
metadata
```

* `last_source_sequence` exists, is nullable (`BIGINT NULL`), and is stored as a 64-bit integer to preserve numeric ordering.
* `current_state` accepts only `online`, `offline`, or `unknown`.
* An index exists for `board_id` when board-scoped maintenance queries need it.

---

## TC-AVAIL-SCHEMA-002: Availability event table schema exists

### Objective

Verify that transition history is stored separately from current state with sequence-aware ordering indexes.

### Expected result

* `device_availability_events` exists.
* Required event fields exist:

```text
id
serialNumber
board_id
event_type
event_time
source_sequence
event_id
idempotency_key
reason
connection_ip
session_id
metadata
```

* `id` exists and is the primary key.
* `serialNumber` is non-null.
* `event_type` is non-null.
* `event_time` is non-null.
* `source_sequence` exists, is nullable (`BIGINT NULL`), and is stored as a 64-bit integer.
* `idempotency_key` is non-null and unique.
* `session_id` exists and is nullable.
* Sequence-aware composite indexes exist to support deterministic event lookup and ordering, verified via `pg_get_indexdef()` or schema inspection:
  * `availability_serial_time_index`: `(serialNumber ASC, event_time ASC, source_sequence ASC NULLS FIRST)`
  * `availability_board_serial_time_index`: `(board_id ASC, serialNumber ASC, event_time ASC, source_sequence ASC NULLS FIRST)`
* `event_type` accepts only stored transition values `online` and `offline`.

---

## TC-AVAIL-SCHEMA-003: Availability migrations are idempotent across all availability tables

### Objective

Verify that migration from a database without availability objects, or with a partial prior release schema missing sequence columns or coverage tables, creates all four required tables and sequence columns safely and idempotently.

### Steps

1. Start with a database that has no availability objects, or a partial deployment database missing sequence columns or ingestion coverage tables.
2. Run Analytics migrations.
3. Run Analytics migrations again.
4. Inspect the schema and table definitions.

### Expected result

* All four availability storage objects exist after migration:
  * `device_availability_state`
  * `device_availability_events`
  * `device_availability_ingestion_checkpoint`
  * `device_availability_ingestion_gaps`
* Both `source_sequence` (`device_availability_events`) and `last_source_sequence` (`device_availability_state`) exist as `BIGINT NULL`.
* All sequence-aware indexes, primary keys, uniqueness constraints, and state check constraints exist.
* Re-running migration does not drop, fail, or duplicate tables, columns, indexes, or constraints.
* Existing `timepoints` and `wificlienthistory` data remains intact.

---

## TC-AVAIL-SCHEMA-004: Ingestion checkpoint table schema exists

### Objective

Verify that durable per-serial ingestion coverage watermarks are stored in a dedicated checkpoint table.

### Expected result

* `device_availability_ingestion_checkpoint` exists.
* `serialNumber` is the primary key.
* Required fields exist:

```text
serialNumber
coverage_start
state_known_from
processed_through_event_time
partition
offset
ordering_strategy
reorder_window_ms
ingestion_gap_known
updated_at
metadata
```

* `coverage_start`, `state_known_from`, and `processed_through_event_time` exist and are 64-bit integers (`BIGINT`).
* `ingestion_gap_known` exists and is boolean.

---

## TC-AVAIL-SCHEMA-005: Ingestion gaps table schema exists

### Objective

Verify that persisted availability ingestion gaps are stored in a dedicated gaps table.

### Expected result

* `device_availability_ingestion_gaps` exists.
* Required fields exist:

```text
serialNumber
gap_start_event_time
gap_end_event_time
reason
detected_at
metadata
```

* `gap_start_event_time` and `gap_end_event_time` exist and are 64-bit integers (`BIGINT`).
* An index exists to support range overlapping queries on `(serialNumber ASC, gap_start_event_time ASC, gap_end_event_time ASC)`.

---

### Availability state row query

```sql
SELECT serialNumber,
       board_id,
       current_state,
       last_event_time,
       last_source_sequence,
       last_idempotency_key,
       updated_at,
       metadata
FROM device_availability_state
WHERE serialNumber = '60cf84f22290'
FOR UPDATE;
```

### Latest transition event query

```sql
SELECT *
FROM device_availability_events
WHERE serialNumber = '60cf84f22290'
ORDER BY event_time DESC, source_sequence DESC NULLS LAST
LIMIT 1;
```

### Availability history query

```sql
SELECT *
FROM device_availability_events
WHERE serialNumber = '60cf84f22290'
ORDER BY event_time ASC, source_sequence ASC NULLS FIRST;
```

SQL composite sequence ordering rules:

```text
- Ascending queries use ORDER BY event_time ASC, source_sequence ASC NULLS FIRST so unsequenced events (source_sequence = NULL) at a timestamp sort before sequenced events at the same timestamp.
- Descending queries use ORDER BY event_time DESC, source_sequence DESC NULLS LAST so the event with the highest sequence number at a timestamp sorts first, and unsequenced events sort last.
```

### Offline event count query

```sql
SELECT COUNT(*)
FROM device_availability_events
WHERE serialNumber = ?
  AND event_type = 'offline'
  AND event_time >= ?
  AND event_time < ?;
```

---

## 4.2. Service Integration Functional Test Cases

## TC-AVAIL-001: First ping initializes online state without transition event

### Objective

Verify that the first observed ping initializes `device_availability_state` as online without creating an online transition event.

### Preconditions

* No availability event exists for the gateway.
* No `device_availability_state` row exists for the gateway.


### Steps

1. Start the gateway.
2. Wait for the gateway to publish a `ping` message.
3. Wait for Analytics to consume the message.
4. Query `device_availability_state`.
5. Query `device_availability_events`.

### Expected result

* No `online` event is inserted.
* No `offline` event is inserted.
* One `device_availability_state` row is created:

```text
serialNumber = 60cf84f22290
current_state = online
last_event_time = ping source timestamp
last_idempotency_key = ping idempotency key
updated_at >= processing time
```

* The checkpoint's `state_known_from` timestamp is set to the first ping source timestamp.
* If requested window starts BEFORE the first ping (`startTime < state_known_from`):
  * Prior state boundary is unknown.
  * The response returns `meta.coverage = "partial"`, `meta.accuracy = "lower_bound"`, and `meta.availabilityCoverage.stateKnownFrom` equal to the first ping timestamp.
* If requested window starts AT OR AFTER the first ping (`startTime >= state_known_from`):
  * State is authoritatively known as online throughout the window.
  * The response returns `meta.coverage = "full"`, `meta.accuracy = "exact"`, `data.offline_count = 0`, and `meta.availabilityCoverage.stateKnownFrom = first ping timestamp`.

---

## TC-AVAIL-002: Repeated pings while already online do not create duplicates

### Objective

Verify that regular gateway pings do not create repeated online transition events.

### Preconditions

* `device_availability_state` has:

```text
current_state = online
last_event_time < latest ping source timestamp
```

### Steps

1. Keep the gateway online.
2. Allow it to send at least three ping messages.
3. Wait for Analytics to process all messages.
4. Query `device_availability_state`.
5. Query `device_availability_events`.

Example messages:

```text
12:00 ping
12:02 ping
12:04 ping
```

### Expected result

* No additional `online` event is inserted after the first online state.
* `last_event_time` is updated to the latest source-newer ping timestamp accepted by the per-serial ordering stage.
* `last_idempotency_key` is updated to the latest accepted ping idempotency key.
* `current_state` remains `online`.
* `offline_count` remains `0`.

---

## TC-AVAIL-003: Device is physically powered off

### Objective

Verify that physically powering off the gateway creates one offline transition.

### Preconditions

* Gateway is online.
* `device_availability_state.current_state = online`.

### Steps

1. Confirm that the gateway is sending ping messages.
2. Disconnect the gateway's power supply.
3. Wait for the gateway/controller disconnection detection.
4. Verify that a Kafka `disconnection` message is published.
5. Wait for Analytics to process the message.
6. Query the availability-event table.
7. Call the availability-summary API.

### Expected result

* Exactly one `offline` event is inserted.
* `device_availability_state` is updated:

```text
current_state = offline
last_event_time = disconnection source timestamp
last_idempotency_key = disconnection idempotency key
updated_at >= processing time
```

* The event contains:

```text
serialNumber = 60cf84f22290
event_type = offline
event_time = disconnection message timestamp
```

* The API returns:

```json
{
  "meta": {
    "requestedWindow": {
      "startTime": "<startTime>",
      "endTime": "<endTime>"
    },
    "observedWindow": {
      "firstSampleAt": "<firstOfflineEventAt>",
      "lastSampleAt": "<lastOfflineEventAt>"
    },
    "sourceWindow": {
      "firstSampleAt": "<firstSourceAt>",
      "lastSampleAt": "<lastSourceAt>"
    },
    "contributingWindow": {
      "firstSampleAt": "<firstOfflineEventAt>",
      "lastSampleAt": "<lastOfflineEventAt>"
    },
    "selection": "boundary_assisted",
    "coverage": "full",
    "accuracy": "exact",
    "sampleCount": 1,
    "offlineEventCount": 1,
    "effectiveSamplingIntervalSeconds": 0,
    "allowedGapSeconds": 0,
    "boundarySamplesUsed": {
      "beforeStart": false,
      "atStart": false,
      "atEnd": false,
      "afterEnd": false
    },
    "availabilityCoverage": {
      "coverageStart": "<= startTime",
      "stateKnownFrom": "<= startTime",
      "processedThrough": ">= endTime",
      "allowedIngestionDelaySeconds": "<configured availability ingestion delay>",
      "ingestionGapKnown": false,
      "proofSource": "serial_partition_checkpoint"
    }
  },
  "data": {
    "gw_uuid": "60cf84f22290",
    "fetch_status": "success",
    "offline_count": 1
  }
}
```

---

## TC-AVAIL-004: Device operating system is shut down gracefully

### Objective

Verify that running a proper operating-system shutdown is counted as one offline occurrence.

### Preconditions

* Gateway is online.

### Steps

1. Connect to the gateway through SSH.
2. Run:

```bash
sudo shutdown -h now
```

3. Wait for the gateway to stop communicating.
4. Verify that a disconnection message reaches Kafka.
5. Wait for Analytics processing.
6. Query the event table.
7. Call the availability-summary API.

### Expected result

* One offline transition is stored.
* Repeated controller checks while the gateway remains shut down do not create more offline events.
* `offline_count` increases by exactly `1`.

---

## TC-AVAIL-005: Device is disconnected from Ethernet

### Objective

Verify that removing the gateway's Ethernet connection creates one offline transition.

### Preconditions

* Gateway uses Ethernet for controller connectivity.
* Gateway is online.

### Steps

1. Confirm that the gateway is sending pings.
2. Disconnect the Ethernet cable.
3. Wait for the disconnection timeout.
4. Verify that a Kafka disconnection message is published.
5. Query availability storage.
6. Call the API.

### Expected result

* One offline event is stored.
* `offline_count` increases by `1`.
* Keeping the cable disconnected does not create repeated offline events.

---

## TC-AVAIL-006: Device Wi-Fi uplink is disconnected

### Objective

Verify that losing the gateway's Wi-Fi uplink creates an offline transition.

### Preconditions

* Gateway uses Wi-Fi uplink for cloud connectivity.
* Gateway is online.

### Steps

1. Confirm that the gateway is online.
2. Disable the gateway's uplink Wi-Fi interface or shut down its upstream access point.
3. Wait for connection loss detection.
4. Verify that a Kafka disconnection message is generated.
5. Query `device_availability_events`.
6. Call the API.

### Expected result

* One offline event is stored.
* `offline_count` increases by exactly `1`.

---

## TC-AVAIL-007: Internet is unavailable but gateway remains powered on

### Objective

Verify that a powered-on gateway with no cloud connectivity is considered offline from the Analytics/controller perspective.

### Preconditions

* Gateway is online.
* Gateway remains powered on during the test.

### Steps

1. Block the gateway's internet access using the upstream router or firewall.
2. Keep local power and LAN connectivity available.
3. Wait for the controller to detect that the gateway is no longer connected.
4. Verify the disconnection Kafka message.
5. Call the availability API.

### Expected result

* One offline transition is stored.
* The gateway is considered offline even though it is still powered on.
* The reason or metadata may indicate network or connection loss when available.
* `offline_count` increases by `1`.

---

## TC-AVAIL-008: Gateway reconnects after physical power restoration

### Objective

Verify that restoring power changes the gateway state from offline to online without increasing the offline count.

### Preconditions

* Gateway is physically powered off.
* One offline event already exists.

### Steps

1. Restore power to the gateway.
2. Wait for the gateway to boot.
3. Wait for the first `ping` or `capabilities` message.
4. Query `device_availability_events`.
5. Call the availability API.

### Expected result

* One `online` transition event is inserted.
* No additional `offline` event is inserted.
* `device_availability_state` is updated:

```text
current_state = online
last_event_time = ping or capabilities source timestamp
last_idempotency_key = ping or capabilities idempotency key
updated_at >= processing time
```

* Existing offline count remains unchanged.

Example history:

```text
12:10 offline
12:15 online
```

API result:

```json
{
  "meta": {
    "requestedWindow": {
      "startTime": "<startTime>",
      "endTime": "<endTime>"
    },
    "observedWindow": {
      "firstSampleAt": "<firstOfflineEventAt>",
      "lastSampleAt": "<lastOfflineEventAt>"
    },
    "sourceWindow": {
      "firstSampleAt": "<firstSourceAt>",
      "lastSampleAt": "<lastSourceAt>"
    },
    "contributingWindow": {
      "firstSampleAt": "<firstOfflineEventAt>",
      "lastSampleAt": "<lastOfflineEventAt>"
    },
    "selection": "boundary_assisted",
    "coverage": "full",
    "accuracy": "exact",
    "sampleCount": 1,
    "offlineEventCount": 1,
    "effectiveSamplingIntervalSeconds": 0,
    "allowedGapSeconds": 0,
    "boundarySamplesUsed": {
      "beforeStart": false,
      "atStart": false,
      "atEnd": false,
      "afterEnd": false
    },
    "availabilityCoverage": {
      "coverageStart": "<= startTime",
      "processedThrough": ">= endTime",
      "allowedIngestionDelaySeconds": "<configured availability ingestion delay>",
      "ingestionGapKnown": false,
      "proofSource": "serial_partition_checkpoint"
    }
  },
  "data": {
    "gw_uuid": "60cf84f22290",
    "fetch_status": "success",
    "offline_count": 1
  }
}
```

---

## TC-AVAIL-009: Ethernet connection is restored

### Objective

Verify recovery after an Ethernet disconnection.

### Preconditions

* Ethernet cable is disconnected.
* One offline event already exists for the outage.

### Steps

1. Reconnect the Ethernet cable.
2. Wait for the gateway to reconnect to the controller.
3. Wait for a ping or capabilities message.
4. Query availability storage.
5. Call the API.

### Expected result

* One `online` transition event is inserted.
* No additional offline event is inserted.
* `device_availability_state.current_state` becomes `online`.
* `last_event_time`, `last_idempotency_key`, `updated_at`, and metadata are updated.
* Offline count remains unchanged.

---

## TC-AVAIL-010: Multiple shutdown and recovery cycles

### Objective

Verify that each separate online-to-offline transition is counted once.

### Preconditions

* Gateway begins online.

### Steps

Perform the following sequence:

```text
12:00 initial ping  → initialize online state, no event
12:10 shutdown      → offline event
12:15 boot          → online event
13:00 disconnect    → offline event
13:05 reconnect     → online event
14:00 power off     → offline event
14:10 power on      → online event
```

Call the API for a time range covering the entire sequence.

### Expected result

The event table contains:

```text
12:10 offline
12:15 online
13:00 offline
13:05 online
14:00 offline
14:10 online
```

The API returns:

```json
{
  "meta": {
    "requestedWindow": {
      "startTime": "<startTime>",
      "endTime": "<endTime>"
    },
    "observedWindow": {
      "firstSampleAt": "<firstOfflineEventAt>",
      "lastSampleAt": "<lastOfflineEventAt>"
    },
    "sourceWindow": {
      "firstSampleAt": "<firstSourceAt>",
      "lastSampleAt": "<lastSourceAt>"
    },
    "contributingWindow": {
      "firstSampleAt": "<firstOfflineEventAt>",
      "lastSampleAt": "<lastOfflineEventAt>"
    },
    "selection": "boundary_assisted",
    "coverage": "full",
    "accuracy": "exact",
    "sampleCount": 3,
    "offlineEventCount": 3,
    "effectiveSamplingIntervalSeconds": 0,
    "allowedGapSeconds": 0,
    "boundarySamplesUsed": {
      "beforeStart": false,
      "atStart": false,
      "atEnd": false,
      "afterEnd": false
    },
    "availabilityCoverage": {
      "coverageStart": "<= startTime",
      "processedThrough": ">= endTime",
      "allowedIngestionDelaySeconds": "<configured availability ingestion delay>",
      "ingestionGapKnown": false,
      "proofSource": "serial_partition_checkpoint"
    }
  },
  "data": {
    "gw_uuid": "60cf84f22290",
    "fetch_status": "success",
    "offline_count": 3
  }
}
```

---

## TC-AVAIL-011: Repeated disconnection while already offline is ignored

### Objective

Verify that repeated offline messages do not create duplicate offline transitions.

### Preconditions

* The gateway has already produced one offline transition.
* `device_availability_state.current_state = offline`.

### Steps

1. Keep the gateway offline after the first disconnection.
2. Deliver another newer `disconnection` message for the same gateway while `current_state = offline`.
3. Query the availability-event table.
4. Call the API.

Example:

```text
12:10 disconnection
12:12 disconnection
12:14 disconnection
```

### Expected result

* Only the first offline transition is stored.
* Later same-state disconnection messages are ignored.
* For source-newer repeated disconnection messages accepted by the per-serial ordering stage while `current_state = offline`, `last_event_time` advances.
* `last_idempotency_key`, `updated_at`, and metadata are updated for the latest accepted same-state message.
* `offline_count` is `1`.

---

## TC-AVAIL-012: Duplicate Kafka delivery of the same event is ignored

### Objective

Verify storage-level idempotency when Kafka redelivers the same logical connection event.

### Preconditions

* Gateway is online.

### Steps

1. Shut down or disconnect the gateway.
2. Capture the resulting Kafka disconnection message.
3. Deliver the exact same Kafka message again.
4. Query `device_availability_events`.
5. Query the stored `idempotency_key` or `event_id`.
6. Call the API.

### Expected result

* Both deliveries map to the same deterministic `idempotency_key` or source `event_id`.
* Exactly one transition event row is inserted.
* The duplicate delivery does not move `current_state`, `last_event_time`, `last_idempotency_key`, `updated_at`, or transition history.
* `offline_count` is `1`.

---

## TC-AVAIL-012A: Same logical event at a different Kafka offset is ignored

### Objective

Verify that Kafka offset is not used as the logical idempotency key.

### Steps

1. Publish a disconnection message with a stable source UUID and source timestamp.
2. Publish the same logical disconnection again as a separate Kafka record at a different topic offset.
3. Query `device_availability_events` and `device_availability_state`.

### Expected result

* Both records derive the same logical `idempotency_key`.
* Exactly one offline transition row exists.
* `device_availability_state` is updated once for the logical event.
* Kafka topic, partition, and offset may appear in metadata only.

---

## TC-AVAIL-012B: Same UUID from different source namespaces is not conflated

### Objective

Verify that `system.host` and `system.id` are part of the idempotency namespace.

### Steps

1. Process a ping from source namespace A with `payload.ping.uuid = 44702320`.
2. Process a different ping for the same gateway from source namespace B with the same UUID.
3. Query `device_availability_state`.

### Expected result

* The two messages derive different `idempotency_key` values because the source namespace differs.
* If both messages are newer source events and same-state online, no transition row is inserted.
* `last_event_time` advances only according to source timestamp ordering.

---

## TC-AVAIL-012C: Missing or unstable UUID uses stable logical fields

### Objective

Verify that UUID absence does not force Kafka-offset idempotency.

### Steps

1. Process a valid disconnection message without a stable source UUID.
2. Derive the idempotency key from stable fields such as `serialNumber`, `message_type`, `event_type`, `event_time`, `reason`, and `connection_ip`.
3. Re-deliver the same logical message.

### Expected result

* Both deliveries derive the same idempotency key.
* Exactly one transition event is inserted.
* If stable logical fields are insufficient, the message is rejected as a counted event instead of using Kafka offset as identity.

---

## TC-AVAIL-013: Repeated ping while already online is ignored

### Objective

Verify that repeated online messages after an offline-to-online transition do not create duplicate online rows.

### Steps

1. Reconnect or power on the gateway from an offline state.
2. Capture the first recovery ping that creates the online transition.
3. Deliver another newer ping while `device_availability_state.current_state = online`.
4. Query availability storage.

### Expected result

* Exactly one online transition event is inserted.
* No additional offline event is inserted.
* The source-newer repeated ping accepted by the per-serial ordering stage updates `last_event_time`, `last_idempotency_key`, `updated_at`, and metadata.
* `offline_count` is unchanged.

---

## TC-AVAIL-014: Gateway changes board after an offline event

### Objective

Verify that historical offline events remain queryable after board reassignment.

### Preconditions

* Gateway previously generated an offline event under Board A.
* Gateway is later assigned to Board B.

### Steps

1. Generate an offline event while the gateway belongs to Board A.
2. Reconnect the gateway.
3. Reassign the gateway to Board B.
4. Call the availability-summary API using the gateway serial number.

### Expected result

* The historical offline event remains available.
* The query does not require the old or current board ID.
* Offline count includes the event created under Board A.

---

## 4.3. API Contract Time-Range Test Cases

## TC-AVAIL-015: Count events inside the requested lookback window

### Objective

Verify that only offline events inside the requested range are counted.

### Test data

```text
10:00 offline
12:00 offline
14:00 offline
```

### Request

```http
GET /api/v1/devices/60cf84f22290/availability-summary
    ?timestampTill=2026-07-29T15:00:00Z
    &lookbackHours=4
```

The time window is:

```text
[11:00, 15:00)
```

### Expected result

* The `10:00` event is excluded.
* The `12:00` and `14:00` events are included.
* `offline_count` is `2`.

---

## TC-AVAIL-016: Offline event exactly at start time

### Objective

Verify inclusive start-time handling.

### Test data

```text
startTime = 12:00
offline event = 12:00
```

### Expected result

* The event is included.
* Offline count increases by `1`.

---

## TC-AVAIL-017: Offline event exactly at end time

### Objective

Verify exclusive end-time handling.

### Test data

```text
endTime = 16:00
offline event = 16:00
```

### Expected result

* The event is excluded.
* Offline count does not increase.

---

## TC-AVAIL-018: No offline events in the requested range

### Objective

Verify successful empty results.

### Preconditions

* The requested range starts at or after `availabilityValidFrom`.
* The gateway has a durable `device_availability_ingestion_checkpoint` with `coverage_start <= startTime` and `processed_through_event_time >= endTime`.
* No `device_availability_ingestion_gaps` row overlaps `[startTime, endTime)`.
* The cutover behavior for ranges beginning before `availabilityValidFrom` is covered by `TC-COMMON-023`.

### Steps

1. Select a time range containing no offline events.
2. Call the API.

### Expected result

* The response uses the top-level `meta` and `data` shape.
* `meta.coverage = "full"`.
* `meta.accuracy = "exact"`.
* `meta.availabilityCoverage.coverageStart <= startTime`.
* `meta.availabilityCoverage.processedThrough >= endTime`.
* `meta.availabilityCoverage.ingestionGapKnown = false`.
* `meta.availabilityCoverage.proofSource != "unavailable"`.
* `data.gw_uuid = "60cf84f22290"`.
* `data.fetch_status = "success"`.
* `data.offline_count = 0`.

```json
{
  "meta": {
    "requestedWindow": {
      "startTime": "<startTime>",
      "endTime": "<endTime>"
    },
    "observedWindow": {
      "firstSampleAt": null,
      "lastSampleAt": null
    },
    "sourceWindow": {
      "firstSampleAt": "<firstSourceAt>",
      "lastSampleAt": "<lastSourceAt>"
    },
    "contributingWindow": {
      "firstSampleAt": null,
      "lastSampleAt": null
    },
    "selection": "boundary_assisted",
    "coverage": "full",
    "accuracy": "exact",
    "sampleCount": 0,
    "offlineEventCount": 0,
    "effectiveSamplingIntervalSeconds": 0,
    "allowedGapSeconds": 0,
    "boundarySamplesUsed": {
      "beforeStart": false,
      "atStart": false,
      "atEnd": false,
      "afterEnd": false
    },
    "availabilityCoverage": {
      "coverageStart": "<= startTime",
      "processedThrough": ">= endTime",
      "allowedIngestionDelaySeconds": "<configured availability ingestion delay>",
      "ingestionGapKnown": false,
      "proofSource": "serial_partition_checkpoint"
    }
  },
  "data": {
    "gw_uuid": "60cf84f22290",
    "fetch_status": "success",
    "offline_count": 0
  }
}
```

The API must not treat a full-coverage empty result as an error. Absence of
matching offline rows is not enough by itself to prove an exact zero; if coverage
is partial or unavailable, the response must use partial/lower-bound or no
coverage semantics instead of reporting an exact zero.

---

## TC-AVAIL-018A: No offline events with partial ingestion coverage

### Objective

Verify that zero matching offline rows are not reported as an exact zero when the
availability ingestion stream does not prove full interval coverage.

### Preconditions

* The requested range starts at or after `availabilityValidFrom`.
* No `offline` rows match `[startTime, endTime)`.
* `availabilityCoverage.processedThrough < endTime` or a known ingestion gap overlaps the covered portion of the requested range.

### Steps

1. Select a range with no matching offline rows.
2. Set availability coverage proof to overlap only part of the requested range.
3. Call the availability-summary API.

### Expected result

* HTTP `200 OK`.
* `data.offline_count = 0`.
* `meta.coverage = "partial"`.
* `meta.accuracy = "lower_bound"`.
* The response includes `meta.availabilityCoverage`.
* The response must not describe the zero as exact full coverage.

---

## TC-AVAIL-018B: No offline events with unavailable ingestion coverage

### Objective

Verify that the API does not present a zero matching count as a meaningful
interval result when no durable availability coverage proof exists.

### Preconditions

* No `offline` rows match `[startTime, endTime)`.
* `availabilityCoverage.proofSource = "unavailable"` or no durable processed-through watermark exists.

### Steps

1. Select a range with no matching offline rows.
2. Make availability coverage proof unavailable.
3. Call the availability-summary API.

### Expected result

* HTTP `200 OK` when the storage query itself succeeds.
* `meta.coverage = "none"`.
* `meta.accuracy = "not_applicable"`.
* `meta.sampleCount = null`.
* `meta.offlineEventCount = null`.
* `meta.availabilityCoverage.proofSource = "unavailable"`.
* `data.offline_count = null`.

---

## TC-AVAIL-018C: No offline events with last message inside ingestion allowance

### Objective

Verify that when the latest gateway source message is inside the allowed ingestion delay window (`endTime - 30 seconds`), the query reports partial coverage for `[startTime, endTime)` because ingestion has not yet reached `endTime`.

### Preconditions

* The requested range starts at or after `availabilityValidFrom`.
* No `offline` rows match `[startTime, endTime)`.
* `allowedIngestionDelaySeconds = 60`.
* The per-serial checkpoint has `coverage_start <= startTime`.
* The latest processed source event for the gateway is a ping at `endTime - 30 seconds`.
* No `device_availability_ingestion_gaps` row overlaps `[startTime, endTime - 30 seconds)`.

### Steps

1. Set the availability checkpoint `processed_through_event_time = endTime - 30 seconds`.
2. Call the availability-summary API with `timestampTill = endTime`.

### Expected result

* HTTP `200 OK`.
* `meta.coverage = "partial"`.
* `meta.accuracy = "lower_bound"`.
* `meta.availabilityCoverage.processedThrough < endTime`.
* `meta.availabilityCoverage.ingestionGapKnown = false`.
* `data.offline_count = 0`.

---

## TC-AVAIL-019: Offline event exists outside the requested range

### Objective

Verify that old events do not affect the current range.

### Test data

```text
Offline event: 48 hours ago
Requested lookback: 24 hours
```

### Expected result

* The old event is excluded by the half-open requested window.
* `data.offline_count = 0` only when the response proves full availability coverage.
* `meta.coverage = "full"`.
* `meta.accuracy = "exact"`.
* `meta.availabilityCoverage.coverageStart <= startTime`.
* `meta.availabilityCoverage.processedThrough >= endTime`.
* `meta.availabilityCoverage.ingestionGapKnown = false`.
* `meta.availabilityCoverage.proofSource != "unavailable"`.

If these coverage assertions fail, the zero matching count must be reported using
partial/lower-bound or no-coverage semantics instead of as an exact zero.

---

## 4.4. API Validation Test Cases

## TC-AVAIL-020: Missing router ID

### Request

```http
GET /api/v1/devices//availability-summary
    ?timestampTill=2026-07-29T12:00:00Z
    &lookbackHours=24
```

### Expected result

* HTTP `404 Not Found` at the router/framework level.
* The Analytics handler is not invoked because the `{routerId}` path segment is missing.
* The documented Analytics error body is not required for this route-level failure.
* No database state is changed.

---

## TC-AVAIL-021: Unknown router ID

### Request

```http
GET /api/v1/devices/deadbeef1234/availability-summary
    ?timestampTill=2026-07-29T12:00:00Z
    &lookbackHours=24
```

### Expected result

* HTTP `404 Not Found`.
* Error is `not_found`.
* It must not return another gateway's events.

---

## TC-AVAIL-022: Invalid router ID syntax

### Objective

Verify that syntactically invalid router IDs (such as path dot-segments, path separators, spaces, or invalid punctuation) are rejected with HTTP 400 before OWPROV ownership lookup or availability database queries are executed.

### Requests

1. Handler-level validation requests (invalid path characters or out-of-bounds length):

```http
GET /api/v1/devices/:::/availability-summary
    ?timestampTill=2026-07-29T12:00:00Z
    &lookbackHours=24

GET /api/v1/devices/router%20id/availability-summary
    ?timestampTill=2026-07-29T12:00:00Z
    &lookbackHours=24
```

2. Syntax-accepted nonexistent values (hexadecimal and non-hex serial strings):

```http
GET /api/v1/devices/abcdef123456/availability-summary
    ?timestampTill=2026-07-29T12:00:00Z
    &lookbackHours=24

GET /api/v1/devices/gateway-serial-1234/availability-summary
    ?timestampTill=2026-07-29T12:00:00Z
    &lookbackHours=24

GET /api/v1/devices/ABCDEF123456/availability-summary
    ?timestampTill=2026-07-29T12:00:00Z
    &lookbackHours=24
```

3. Framework / route-level security requests (path dot-segments and encoded slash):

```http
GET /api/v1/devices/./availability-summary
GET /api/v1/devices/../availability-summary
GET /api/v1/devices/abc%2Fdef123456/availability-summary
```

### Expected result

* For handler-level validation requests (`:::`, spaces):
  * HTTP `400 Bad Request`.
  * Response resembles:

```json
{
  "error": "invalid_router_id",
  "message": "routerId must be a valid path-safe OWPROV gateway serial number (1 to 64 alphanumeric characters, hyphens, or underscores)"
}
```

  * OWPROV resolution is not called.
  * No availability query is executed.

* For syntax-accepted nonexistent values (`abcdef123456`, `gateway-serial-1234`, and uppercase hexadecimal):
  * HTTP `404 Not Found`.
  * Error is `not_found`.
  * The response proves the value passed syntax validation before OWPROV ownership resolution determined it was nonexistent or outside scope.

* For dot-segment and encoded slash requests (`.`, `..`, `%2F`):
  * Handler-level validation of `.` and `..` returns HTTP `400 Bad Request` with `error: "invalid_router_id"` when those values reach the handler as router IDs.
  * End-to-end route tests may observe framework-level rejection before reaching the handler if the HTTP server normalizes dot-segments or rejects encoded path separators.
  * These path values are never passed to OWPROV ownership resolution.

---

## TC-AVAIL-023: Invalid timestamp format

### Request

```http
GET /api/v1/devices/60cf84f22290/availability-summary
    ?timestampTill=invalid-time
    &lookbackHours=24
```

### Expected result

* HTTP `400 Bad Request`.
* Error indicates invalid `timestampTill`.

---

## TC-AVAIL-024: Missing timestamp

### Request

```http
GET /api/v1/devices/60cf84f22290/availability-summary
    ?lookbackHours=24
```

### Expected result

* HTTP `400 Bad Request`.
* Error is `invalid_timestamp`.
* No query is executed with an undefined time range.

---

## TC-AVAIL-025: Zero lookback hours

### Request

```http
GET /api/v1/devices/60cf84f22290/availability-summary
    ?timestampTill=2026-07-29T12:00:00Z
    &lookbackHours=0
```

### Expected result

* HTTP `400 Bad Request`.
* Error is `invalid_lookback_hours`, not `invalid_timestamp`.

---

## TC-AVAIL-026: Negative lookback hours

### Request

```http
GET /api/v1/devices/60cf84f22290/availability-summary
    ?timestampTill=2026-07-29T12:00:00Z
    &lookbackHours=-1
```

### Expected result

* HTTP `400 Bad Request`.
* Error is `invalid_lookback_hours`, not `invalid_timestamp`.

---

## TC-AVAIL-027: Lookback exceeds maximum allowed range

### Request

```http
GET /api/v1/devices/60cf84f22290/availability-summary
    ?timestampTill=2026-07-29T12:00:00Z
    &lookbackHours=999999
```

### Expected result

* HTTP `400 Bad Request`.
* Error is `invalid_lookback_hours`, not `invalid_timestamp`.
* Error message may indicate that the supported lookback limit was exceeded.

---



## 4.5. Concurrency and Reliability Test Cases

## TC-AVAIL-028: Two consumers process concurrent disconnection messages

### Objective

Verify that concurrent processing cannot create duplicate offline transitions.

### Preconditions

* Gateway is online.

### Steps

1. Start with `device_availability_state.current_state = online`.
2. Deliver two disconnection messages for the same gateway concurrently.
3. Query event storage.

### Expected result

* State check, idempotency check, event insertion, and state update happen inside one database transaction.
* The `device_availability_state` row is locked by `serialNumber` while processing.
* Exactly one offline event row is inserted.
* The `device_availability_state` row ends with `current_state = offline` and `last_event_time` equal to the accepted disconnection source timestamp.
* Offline count is `1`.

---

## TC-AVAIL-028A: Two first disconnections race when no state row exists

### Objective

Verify that first-row creation is concurrency-safe and does not rely on locking a nonexistent row with only `SELECT ... FOR UPDATE`.

### Preconditions

* No `device_availability_state` row exists for the gateway.
* No availability event exists for the gateway.

### Steps

1. Deliver two disconnection messages for the same gateway concurrently.
2. Force both consumers to attempt state initialization.
3. Query `device_availability_state` and `device_availability_events`.

### Expected result

* The implementation uses an atomic first-row mechanism, such as `INSERT ... ON CONFLICT`, a PostgreSQL advisory transaction lock, or a serializable transaction with retry.
* Exactly one `device_availability_state` row exists.
* The final state is `current_state = offline`.
* No offline transition row is inserted because the prior state was unknown.
* An availability-summary response for a window containing these first observations does not report an exact zero, because the prior state boundary is unknown.
* The response uses `meta.coverage = "partial"` and `meta.accuracy = "lower_bound"` when durable coverage begins at the first observation, or `meta.coverage = "none"` and `meta.accuracy = "not_applicable"` when no coverage proof exists.
* When `meta.coverage = "partial"`, `data.offline_count = 0` is allowed as a lower-bound count of stored offline transitions.
* When `meta.coverage = "none"`, `meta.sampleCount = null`, `meta.offlineEventCount = null`, and `data.offline_count = null`.

---

## TC-AVAIL-028B: Opposite-state events arriving concurrently are serialized

### Objective

Verify that an offline and online source event for the same gateway are applied in source-event order, even when delivery to workers is concurrent.

### Preconditions

* `device_availability_state.current_state = online`.
* Existing `last_event_time = 12:00`.

### Steps

1. Deliver an offline source event at `12:03`.
2. Deliver an online source event at `12:04` concurrently.
3. Query `device_availability_state` and transition history.

### Expected result

* The ingestion path provides one explicit ordering guarantee for each `serialNumber` before transition detection, such as:
  * Kafka partitioning keyed by `serialNumber`, preserving gateway event order; or
  * a bounded event-time reorder window; or
  * an equivalent serialNumber-scoped ordering mechanism that prevents a newer online event from causing an older offline transition to be discarded silently.
* Processing must produce the source-time outcome:

```text
12:03 offline is applied first
offline transition event is inserted
device_availability_state.current_state = offline
last_event_time = 12:03

12:04 online is applied second
online transition event is inserted
device_availability_state.current_state = online
last_event_time = 12:04
```

* Final `device_availability_state.current_state = online`.
* Final `last_event_time = 12:04`.
* State never moves backward.
* No duplicate transition events are inserted.
* State and event writes are atomic.
* It is not valid to process the `12:04` online event first, mark the `12:03` offline event stale, and lose the outage. If the implementation intentionally chooses best-effort counting instead of an ordering guarantee, this test must be replaced by an explicit documented best-effort contract and must assert that the undercount is observable as degraded accuracy, not silently successful exact counting.

---

## TC-AVAIL-028C: Event insert and state update roll back together

### Objective

Verify atomicity when the transition event insert succeeds but the state update fails.

### Steps

1. Start from `device_availability_state.current_state = online`.
2. Inject a failure after inserting the offline event but before updating `device_availability_state`.
3. Roll back the transaction.
4. Query both availability tables.

### Expected result

* No offline transition row remains after rollback.
* `device_availability_state` remains unchanged.
* A retry can process the same logical event exactly once.

---

## TC-AVAIL-028D: State update and event insert roll back together

### Objective

Verify atomicity when state mutation would succeed but event insertion fails.

### Steps

1. Start from `device_availability_state.current_state = online`.
2. Inject an event insert failure for a newer offline transition.
3. Query both availability tables.

### Expected result

* `device_availability_state` remains `online`.
* `last_event_time` does not advance.
* No offline event is counted.

---

## TC-AVAIL-028E: Restart and multiple replicas use persisted state

### Objective

Verify that transition detection does not depend on in-memory `DeviceInfo` state.

### Steps

1. Process a disconnection and commit:

```text
device_availability_state.current_state = offline
last_event_time = 12:10
```

2. Restart Analytics or route the next message to another Analytics replica.
3. Process a recovery ping at `12:15`.
4. Query availability storage.

### Expected result

* The new process or replica loads `device_availability_state`.
* One online transition event is inserted.
* `device_availability_state.current_state = online`.
* No duplicate offline event is created after restart.

---



## TC-AVAIL-029: Stale out-of-order event is ignored

### Objective

Verify that an event older than `device_availability_state.last_event_time`
cannot change availability history after the per-serial ordering strategy has
advanced beyond that source time.

### Preconditions

* The connection topic is serialNumber-keyed and source-event ordered, or the bounded reorder window has already closed for source times up to `12:10`.
* No ordering strategy can still accept a `12:05` transition for this serialNumber.

### Steps

1. Set `device_availability_state` to:

```text
current_state = online
last_event_time = 12:10
last_idempotency_key = ping-1210
```

2. Deliver a delayed `disconnection` message for the same gateway at `12:05`.
3. Query `device_availability_state`.
4. Query `device_availability_events`.

### Expected result

* The stale `12:05` event is ignored.
* No offline event row is inserted.
* `device_availability_state` remains:

```text
current_state = online
last_event_time = 12:10
last_idempotency_key = ping-1210
```

---

## TC-AVAIL-029A: Equal timestamp without source sequence creates ambiguity gap

### Objective

Verify that a distinct opposite-state message with the same source timestamp is
not silently discarded when no reliable source sequence can prove ordering.

### Steps

1. Set `device_availability_state` to:

```text
current_state = online
last_event_time = 12:10
last_idempotency_key = ping-1210
```

2. Deliver a disconnection for the same gateway with `event_time = 12:10`, a different logical `idempotency_key`, and no reliable source sequence.
3. Query `device_availability_state`, `device_availability_events`, and `device_availability_ingestion_gaps`.

### Expected result

* The disconnection is treated as unordered or ambiguous, not as an exact duplicate.
* No offline transition event is inserted.
* `device_availability_state` remains unchanged.
* An ingestion ambiguity gap is persisted for the overlapping source time.
* Availability queries overlapping the ambiguous source time do not return `meta.accuracy = "exact"`.

---

## TC-AVAIL-029B: Equal timestamp with source sequence preserves numeric transitions

### Objective

Verify that distinct opposite-state events sharing a timestamp are processed when
a deterministic 64-bit numeric `source_sequence` (`BIGINT`) orders them, and that
numeric comparison (`10 > 2`) is preserved rather than string lexicographical order.

### Preconditions

* Gateway state is initialized as online before `12:00:00`.
* The source provides a reliable per-serial numeric sequence field (`BIGINT NULL`).

### Steps

1. Deliver a disconnection with:

```text
event_time = 12:00:00
source_sequence = 2
idempotency_key = offline-120000-2
```

2. Deliver a ping with:

```text
event_time = 12:00:00
source_sequence = 10
idempotency_key = online-120000-10
```

3. Query `device_availability_state` and `device_availability_events`.

### Expected result

* The offline transition is inserted with ordering key `(12:00:00, 2)`.
* The online transition is inserted with ordering key `(12:00:00, 10)` because numeric comparison evaluates `10 > 2` (whereas string comparison would incorrectly evaluate `"10" < "2"`).
* `device_availability_state.current_state = online`.
* `device_availability_state.last_event_time = 12:00:00`.
* `device_availability_state.last_source_sequence = 10`.
* A full-coverage query over this interval returns `offline_count = 1`.

---

## TC-AVAIL-029C: Delayed offline before newer same-state ping is not lost

### Objective

Verify that out-of-order Kafka delivery does not silently drop an older offline
transition when a newer same-state online ping arrives first.

### Preconditions

* Gateway state is initialized as online at `12:00`.
* The deployment does not rely on raw Kafka arrival order unless the topic is keyed and source-event ordered by serialNumber.

### Steps

1. Deliver a ping with source `event_time = 12:10`.
2. Deliver a disconnection for the same gateway with source `event_time = 12:05`.
3. Flush the per-serial ordering mechanism for source times through `12:10`.
4. Query `device_availability_state` and `device_availability_events`.

### Expected result

* The `12:05` disconnection is accepted as an offline transition.
* The `12:10` ping is accepted as an online transition after the offline transition.
* `device_availability_state.current_state = online`.
* `device_availability_state.last_event_time = 12:10`.
* `offline_count` for a full-coverage window containing `12:05` is `1`.
* If the implementation cannot provide serial-keyed source-event ordering or a bounded reorder window for this sequence, the API must not mark the count as `meta.accuracy = "exact"`.

---

## TC-AVAIL-030: First event is a disconnection

### Objective

Verify the explicit bootstrap behavior when the first accepted availability
message for a gateway is `disconnection`.

This contract treats an initial unknown-to-offline observation as state
initialization, not as a counted offline transition, because `offline_count`
counts observed online-to-offline transitions.

### Preconditions

* No availability event exists for the gateway.
* No `device_availability_state` row exists for the gateway.

### Steps

1. Deliver a `disconnection` message.
2. Query `device_availability_events`.
3. Call availability-summary for a window containing the event.

### Expected result

* No `offline` transition event row is inserted.
* A `device_availability_state` row is created with `current_state = offline` and `last_event_time` equal to the disconnection source timestamp.
* The checkpoint's `state_known_from` timestamp is set to this first observed event timestamp (or an unproven gap is recorded for `[coverage_start, initial_event_time)`).
* An availability-summary response for a window containing the first disconnection does not report an exact zero, because `state_known_from > startTime` proves the prior state boundary is unknown.
* The response uses `meta.coverage = "partial"` and `meta.accuracy = "lower_bound"` when durable coverage begins at the first observation, or `meta.coverage = "none"` and `meta.accuracy = "not_applicable"` when no coverage proof exists.
* A full-coverage exact-zero response is allowed only for a requested window that begins at or after `state_known_from` (`state_known_from <= startTime`) and is fully covered by the per-serial ingestion checkpoint (`processed_through_event_time >= endTime`).

---

## TC-AVAIL-030A: Delayed but source-newer event is accepted

### Objective

Verify that Kafka delivery delay does not cause a valid source-newer event to be rejected by processing-time `lastContact`.

### Steps

1. Process a ping with:

```text
source event_time = 12:00
processed/contact time = 12:05
```

2. Confirm `device_availability_state` has:

```text
current_state = online
last_event_time = 12:00
updated_at = 12:05 or equivalent processing timestamp
```

3. Process a delayed disconnection with:

```text
source event_time = 12:03
processed/contact time = 12:06
```

4. Query `device_availability_state` and `device_availability_events`.

### Expected result

* The disconnection is accepted because `12:03 > last_event_time 12:00`.
* The implementation does not compare `12:03` against processing-time `DeviceInfo.lastContact = 12:05`.
* One `offline` event row is inserted with `event_time = 12:03`.
* `device_availability_state` ends with:

```text
current_state = offline
last_event_time = 12:03
updated_at = 12:06 or equivalent processing timestamp
last_idempotency_key = disconnection idempotency key
```

---

## TC-AVAIL-031: Separate gateways maintain independent latest states

### Objective

Verify that transition detection is scoped by `serialNumber`.

### Steps

1. Store an `offline` event for gateway A.
2. Store an `online` event for gateway B.
3. Deliver a repeated disconnection for gateway A.
4. Deliver a disconnection for gateway B.
5. Query `device_availability_events` for both serial numbers.

### Expected result

* Gateway A's repeated offline message is ignored.
* Gateway B's online-to-offline transition is inserted.
* Counts and latest states are not mixed across serial numbers.

---

## TC-AVAIL-032: Offline count includes only stored offline transitions

### Objective

Verify that online transition rows do not affect `offline_count`.

### Steps

1. Store this event sequence for one gateway:

```text
12:10 offline
12:15 online
12:30 offline
```

2. Call availability-summary for a window containing the whole sequence.

### Expected result

* The API counts only the two stored `offline` rows.
* Stored `online` rows are ignored by the offline count.
* `meta.sampleCount = 2`.
* `meta.offlineEventCount = 2`.
* `offline_count` is `2`.

---

## 4.6. End-to-End Physical Device Scenario

## TC-AVAIL-033: Complete shutdown, restart, disconnect and reconnect sequence

### Objective

Verify the complete real-device availability flow.

### Steps

1. Start the gateway and confirm it is online.
2. Wait for at least one ping.
3. Shut down the gateway using:

```bash
sudo shutdown -h now
```

4. Wait for one offline event.
5. Keep the gateway shut down for at least five minutes.
6. Confirm no additional offline events are created.
7. Power on the gateway.
8. Wait for recovery pings.
9. Disconnect the Ethernet cable.
10. Wait for another offline event.
11. Keep the Ethernet cable disconnected for at least five minutes.
12. Confirm no repeated offline event is stored.
13. Reconnect Ethernet.
14. Wait for recovery pings.
15. Call the API for a time range covering the complete test.

### Expected event sequence

```text
Initial ping                 → initialize online state, no event
Device shutdown             → offline event 1
Device remains shut down    → no new event
Device powers on            → online event
Ethernet disconnected       → offline event 2
Ethernet remains unplugged  → no new event
Ethernet reconnected        → online event
```

### Expected API response

```json
{
  "meta": {
    "requestedWindow": {
      "startTime": "<startTime>",
      "endTime": "<endTime>"
    },
    "observedWindow": {
      "firstSampleAt": "<firstOfflineEventAt>",
      "lastSampleAt": "<lastOfflineEventAt>"
    },
    "sourceWindow": {
      "firstSampleAt": "<firstSourceAt>",
      "lastSampleAt": "<lastSourceAt>"
    },
    "contributingWindow": {
      "firstSampleAt": "<firstOfflineEventAt>",
      "lastSampleAt": "<lastOfflineEventAt>"
    },
    "selection": "boundary_assisted",
    "coverage": "full",
    "accuracy": "exact",
    "sampleCount": 2,
    "offlineEventCount": 2,
    "effectiveSamplingIntervalSeconds": 0,
    "allowedGapSeconds": 0,
    "boundarySamplesUsed": {
      "beforeStart": false,
      "atStart": false,
      "atEnd": false,
      "afterEnd": false
    },
    "availabilityCoverage": {
      "coverageStart": "<= startTime",
      "processedThrough": ">= endTime",
      "allowedIngestionDelaySeconds": "<configured availability ingestion delay>",
      "ingestionGapKnown": false,
      "proofSource": "serial_partition_checkpoint"
    }
  },
  "data": {
    "gw_uuid": "60cf84f22290",
    "fetch_status": "success",
    "offline_count": 2
  }
}
```

---

## TC-AVAIL-034: Multi-replica and process restart availabilityValidFrom consistency

### Objective

Verify that `availabilityValidFrom` is loaded from a durable configuration key or DB migration metadata table so that multiple service replicas and restarted instances make identical cutover decisions.

### Steps

1. Configure `availability_valid_from = 2026-07-01T00:00:00Z` in system configuration / DB properties.
2. Start Replica A and Replica B.
3. Issue a request with `startTime = 2026-06-30T23:00:00Z` (`startTime < availabilityValidFrom`) to both replicas.
4. Restart Replica A and reissue the same request.

### Expected result

* Both Replica A and Replica B reject the request with HTTP `400 Bad Request` and `error: "availability_range_before_cutover"`.
* After restart, Replica A continues to return identical HTTP 400 rejection for `startTime < availabilityValidFrom`.
* Dynamic process startup timestamps (e.g. `std::chrono::system_clock::now()`) are prohibited.

---

# 5. Gateway Memory Summary Test Cases

## Endpoint

```http
GET /api/v1/devices/{routerId}/memory-summary
```

Expected response:

```json
{
  "min_memfree": 211374,
  "max_memfree": 215050,
  "avg_memfree": 212074.36
}
```

---

## TC-MEM-001: Calculate minimum, maximum and average memory

### Test data

```text
Timestamp    memory_free    memory_total
10:00        200000         512000
10:10        250000         512000
10:20        300000         512000
```

### Expected result

```json
{
  "min_memfree": 200000,
  "max_memfree": 300000,
  "avg_memfree": 250000.0
}
```

---

## TC-MEM-002: Single valid memory sample

### Test data

```text
memory_free = 212074
memory_total = 512000
```

### Expected result

```json
{
  "min_memfree": 212074,
  "max_memfree": 212074,
  "avg_memfree": 212074.0
}
```

---

## TC-MEM-003: Samples outside the requested range

### Test data

```text
09:00 memory_free = 100000
10:00 memory_free = 200000
11:00 memory_free = 300000
12:00 memory_free = 400000
```

Requested range:

```text
[10:00, 11:00)
```

### Expected result

```json
{
  "min_memfree": 200000,
  "max_memfree": 200000,
  "avg_memfree": 200000.0
}
```

The `09:00`, `11:00`, and `12:00` samples are excluded.

---

## TC-MEM-004: Samples exactly at range boundaries

### Test data

```text
startTime sample = 200000
endTime sample = 300000
```

### Expected result

* The `startTime` sample is included.
* The `endTime` sample is excluded.
* The summary is calculated from the `startTime` sample only.

---

## TC-MEM-005: No memory samples

### Expected result

```json
{
  "min_memfree": null,
  "max_memfree": null,
  "avg_memfree": null
}
```

* HTTP `200 OK`.

---

## TC-MEM-006: Historical row has no `resource_data`

### Preconditions

* Record was created before the memory schema migration.
* `resource_data` is absent.

### Expected result

* Record is ignored.
* Missing data is not interpreted as zero free memory.
* If no valid rows remain, all summary fields are `null`.

---

## TC-MEM-007: Missing memory block in a new timepoint

### Test data

```json
{
  "unit": {
    "cpu_load": "0.10"
  }
}
```

### Expected result

* No valid memory sample is created for the summary.
* `memory_free` is not counted as zero.

---

## TC-MEM-008: Legacy row marks resource block as absent

### Test data

```text
resource_data is absent
memory_free is absent
memory_total is absent
```

### Expected result

* Sample is ignored.
* Missing optional fields are not represented as zero.
* It does not make `min_memfree` equal to zero.

---

## TC-MEM-009: Genuine free memory value is zero

### Preconditions

The ingestion metadata proves that the memory block was present and reported:

```text
memory_free = 0
memory_total > 0
```

### Expected result

* The zero value is treated as a valid measured sample.
* It may become `min_memfree`.

---

## TC-MEM-010: Very large unsigned memory values

### Test data

Use memory values greater than the 32-bit integer range.

### Expected result

* Values are stored and aggregated without integer overflow.
* Average is calculated using a sufficiently wide numeric type.

---

## TC-MEM-011: Average contains decimal value

### Test data

```text
memory_free samples:
100
101
```

### Expected result

```text
avg_memfree = 100.5
```

The result is not truncated to `100`.

---

## TC-MEM-012: Memory summary bounds and ordering

### Expected result

* `min_memfree`, `max_memfree`, and `avg_memfree` are never negative when they are not `null`.
* When samples exist, the response satisfies:

```text
min_memfree <= avg_memfree <= max_memfree
```

---

## TC-MEM-012A: Negative memory value is excluded

### Test data

```text
10:00 memory_free = -1      memory_total = 512000
10:10 memory_free = 200000  memory_total = 512000
10:20 memory_free = 300000  memory_total = 512000
```

### Expected result

```json
{
  "min_memfree": 200000,
  "max_memfree": 300000,
  "avg_memfree": 250000.0
}
```

* Sample-level rejection policy: When any present memory field (`free`, `total`, `cached`, `buffered`) is negative (`< 0`), non-numeric, or invalid, the ENTIRE memory sample is marked invalid and excluded from aggregation.
* The 10:00 sample containing `memory_free = -1` (or negative `memory_total = -1`, negative `memory_cached = -1`, or negative `memory_buffered = -1`) is rejected as corrupted telemetry.
* Corrupted memory samples do not affect `min_memfree`, `max_memfree`, or `avg_memfree`.

---

## TC-MEM-012A1: Negative memory_total or cached/buffered rejects entire sample

### Test data

```text
10:00 memory_free = 200000  memory_total = -1
10:10 memory_free = 300000  memory_total = 512000
```

### Expected result

* Sample 10:00 has valid `memory_free` but negative `memory_total`.
* The entire 10:00 memory sample is rejected at ingestion/validation stage; `memory_free = 200000` is NOT retained or included in aggregation.
* Aggregation uses only valid sample 10:10 (`min_memfree = 300000`, `max_memfree = 300000`, `avg_memfree = 300000.0`).

---

## TC-MEM-012B: Free memory exceeds total memory

### Test data

```text
10:00 memory_free = 700000  memory_total = 512000
10:10 memory_free = 200000  memory_total = 512000
10:20 memory_free = 300000  memory_total = 512000
```

### Expected result

```json
{
  "min_memfree": 200000,
  "max_memfree": 300000,
  "avg_memfree": 250000.0
}
```

* When `memory_total` is present and `memory_free > memory_total`, the sample is treated as corrupted telemetry and excluded.
* If all samples are excluded by this rule, the memory summary uses the same empty-sample response as `TC-MEM-005`.

---

## TC-MEM-013: Data from another gateway on the same board

### Test data

* Gateway A and Gateway B belong to the same board.
* Both have memory samples.

### Expected result

* Only rows where `serialNumber` matches the requested `routerId` are included.
* Gateway B data does not affect Gateway A's result.

---

## TC-MEM-014: Data from the same gateway under a different board record

### Expected result

* Normal metric lookup uses the currently resolved board and matching serial number.
* Unrelated board data is not mixed into the summary.

---

## TC-MEM-015: Schema migration preserves old timepoints

### Steps

1. Start with a database created before `resource_data` existed.
2. Run the upgrade.
3. Insert new memory-enabled rows.
4. Call the API.

### Expected result

* Old records remain readable.
* Old missing values are ignored.
* New records contribute to the summary.
* Migration does not delete historical timepoints.

---

# 6. Radio Temperature Summary Test Cases

## Endpoint

```http
GET /api/v1/devices/{routerId}/radio-temperature-summary
```

Expected response:

```json
{
  "min_wifi_temp_2.4G": 62,
  "max_wifi_temp_2.4G": 70,
  "avg_wifi_temp_2.4G": 66.64,
  "min_wifi_temp_5G": 56,
  "max_wifi_temp_5G": 65,
  "avg_wifi_temp_5G": 60.38
}
```

---

## TC-TEMP-001: Aggregate 2.4 GHz and 5 GHz separately

### Test data

```text
2.4 GHz: 60, 65, 70
5 GHz:   50, 55, 60
```

### Expected result

```json
{
  "min_wifi_temp_2.4G": 60,
  "max_wifi_temp_2.4G": 70,
  "avg_wifi_temp_2.4G": 65.0,
  "min_wifi_temp_5G": 50,
  "max_wifi_temp_5G": 60,
  "avg_wifi_temp_5G": 55.0
}
```

---

## TC-TEMP-002: Only 2.4 GHz radio exists

### Expected result

```json
{
  "min_wifi_temp_2.4G": 60,
  "max_wifi_temp_2.4G": 70,
  "avg_wifi_temp_2.4G": 65.0,
  "min_wifi_temp_5G": null,
  "max_wifi_temp_5G": null,
  "avg_wifi_temp_5G": null
}
```

---

## TC-TEMP-003: Only 5 GHz radio exists

### Expected result

* All 2.4 GHz fields are `null`.
* 5 GHz fields contain the calculated values.

---

## TC-TEMP-004: No radio temperature data

### Expected result

```json
{
  "min_wifi_temp_2.4G": null,
  "max_wifi_temp_2.4G": null,
  "avg_wifi_temp_2.4G": null,
  "min_wifi_temp_5G": null,
  "max_wifi_temp_5G": null,
  "avg_wifi_temp_5G": null
}
```

* HTTP `200 OK`.

---

## TC-TEMP-005: Missing temperature field

### Test data

```json
{
  "band": 2
}
```

### Expected result

* Sample is ignored.
* No synthetic temperature is inserted.
* The value `20` is not generated automatically.

---

## TC-TEMP-006: Null temperature field

### Test data

```json
{
  "band": 5,
  "wifi_temp": null
}
```

### Expected result

* Sample is excluded from aggregation.

---

## TC-TEMP-007: Pre-cutover temperature record

### Test data

```text
temperatureMigrationCutoverTime = 2026-07-29T10:00:00Z
API requested startTime = 2026-07-29T10:00:00Z
Database contains historical row: sample time = 2026-07-29T09:59:59Z, wifi_temp = 62
```

### Expected result

* The pre-cutover database sample is excluded by the database query filter `timestamp >= startTime`.
* It does not affect minimum, maximum or average.
* Note: If API requested `startTime` were before `temperatureMigrationCutoverTime` (e.g. `09:55:00Z`), the request would return `400 Bad Request` per `TC-TEMP-017`.

---

## TC-TEMP-008: Zero temperature treated as missing-sensor sentinel

### Preconditions

The sample contains:

```text
wifi_temp = 0
```

### Expected result

* `wifi_temp = 0` is treated as an uninitialized or missing-sensor sentinel across all OpenWiFi telemetry.
* The sample is excluded from temperature aggregation and does not contribute to min, max, or average calculations.
* If all samples for a band have `wifi_temp = 0`, the response returns `null` for that band's min, max, and average fields.

---

## TC-TEMP-009: Post-cutover missing `wifi_temp`

### Expected result

* Temperature is excluded when `wifi_temp` is missing or `null`.
* No synthetic fallback value is generated.

---

## TC-TEMP-010: Post-cutover valid numeric `wifi_temp`

### Preconditions

A post-cutover sample contains a numeric `wifi_temp` value.

### Expected result

* A post-cutover numeric `wifi_temp` is included only when all of the following hold:
  * `-40 <= wifi_temp <= 125`
  * `wifi_temp != 0`
  * `wifi_temp != 255`
* Explicit boundary values `-40` and `125` are valid inclusive measurements and are included in aggregation.
* Sentinels and out-of-range values (`0`, `255`, `< -40`, and `> 125`) are excluded.

---

## TC-TEMP-011: Pre-cutover temperature equal to 20

### Preconditions

* `temperatureMigrationCutoverTime = 2026-07-29T10:00:00Z`.
* API requested `startTime = 2026-07-29T10:00:00Z`.
* Database contains pre-cutover record timestamp `2026-07-29T09:59:00Z` with `wifi_temp = 20`.

### Expected result

* The pre-cutover database row is excluded by `timestamp >= startTime` filtering because historical temperature values cannot reliably distinguish measured values from synthetic fallback values.
* It does not affect minimum, maximum or average.

---

## TC-TEMP-012: Pre-cutover temperature other than 20

### Test data

```text
temperatureMigrationCutoverTime = 2026-07-29T10:00:00Z
API requested startTime = 2026-07-29T10:00:00Z
Database contains pre-cutover record timestamp 2026-07-29T09:55:00Z with wifi_temp = 62
```

### Expected result

* The sample is excluded by `timestamp >= startTime` filtering because all pre-cutover temperature records are ignored.

---

## TC-TEMP-013: Post-cutover temperature equal to 20

### Test data

```text
Record timestamp is at or after temperature_migration_cutover_time
wifi_temp = 20
```

### Expected result

* The sample is included.
* Post-cutover samples are not rejected only because the measured value is `20`.

---

## TC-TEMP-014: Mix of valid and invalid samples

### Test data

`temperatureMigrationCutoverTime` = `2026-07-29T10:00:00Z`
API requested window: `startTime = 2026-07-29T10:00:00Z`, `endTime = 2026-07-29T10:05:00Z`

```text
2.4 GHz database samples:
09:59:00Z  wifi_temp = 20   (pre-cutover DB row -> excluded by timestamp >= startTime)
10:01:00Z  wifi_temp = 60   (post-cutover valid sample -> included)
10:02:00Z  wifi_temp = 65   (post-cutover valid sample -> included)
10:03:00Z  wifi_temp = null (missing sample -> excluded)
10:04:00Z  wifi_temp = 70   (post-cutover valid sample -> included)
```

### Expected result

```text
min = 60
max = 70
avg = 65
```

Only post-cutover valid samples `60`, `65`, and `70` are included. Pre-cutover DB row (`09:59:00Z`) and `null` are excluded.

---

## TC-TEMP-015: 6 GHz radio data is present

### Test data

```text
band = 6
wifi_temp = 58
```

### Expected result

* 6 GHz data does not enter the 2.4 GHz or 5 GHz fields.
* No incorrect band mapping occurs.

---

## TC-TEMP-016: Multiple radios using the same band

### Test data

Two 5 GHz radios publish valid temperatures.

### Expected result

* Samples from both 5 GHz radios are included in the 5 GHz summary.
* They are not overwritten by the last radio entry.

---

## TC-TEMP-017: Requested range starts before temperature migration cutover

### Test data

Query requested `startTime` is strictly before `temperatureMigrationCutoverTime` (`startTime < temperatureMigrationCutoverTime`).

### Expected result

* HTTP `400 Bad Request`.
* Response JSON envelope:

```json
{
  "error": "temperature_range_before_cutover",
  "message": "The requested summary interval starts before the temperature migration cutover timestamp."
}
```
* No truncated or partial temperature summary is returned for pre-cutover intervals.

---

## TC-TEMP-018: Decimal average

### Test data

```text
Temperatures: 60, 61
```

### Expected result

```text
average = 60.5
```

Average is not truncated.

---

## TC-TEMP-019: Malformed `radio_data`

### Test data

A timepoint contains invalid JSON in `radio_data`.

### Expected result

* The malformed record is skipped and logged.
* Other valid records in the requested range are processed.
* Service does not crash and does not return an internal error solely because one row is malformed.
* Partial invalid data must not produce fabricated temperatures.

---

## TC-CONFIG-TEMP-001: Valid file configuration starts service

### Preconditions

`temperature.migration_cutover_time = "2026-07-01T00:00:00Z"` in configuration file. `TEMPERATURE_MIGRATION_CUTOVER_TIME` environment variable is unset.

### Expected result

* Service initializes successfully.
* `temperatureMigrationCutoverTime` is set to `2026-07-01T00:00:00Z`.

---

## TC-CONFIG-TEMP-002: Valid environment configuration starts service

### Preconditions

`TEMPERATURE_MIGRATION_CUTOVER_TIME = "2026-07-01T00:00:00Z"` in environment. `temperature.migration_cutover_time` configuration file key is unset.

### Expected result

* Service initializes successfully.
* `temperatureMigrationCutoverTime` is set to `2026-07-01T00:00:00Z`.

---

## TC-CONFIG-TEMP-003: Environment configuration takes precedence over file configuration

### Preconditions

* Configuration file: `temperature.migration_cutover_time = "2026-06-01T00:00:00Z"`.
* Environment variable: `TEMPERATURE_MIGRATION_CUTOVER_TIME = "2026-07-01T00:00:00Z"`.

### Expected result

* Service initializes successfully.
* `temperatureMigrationCutoverTime` evaluates to `"2026-07-01T00:00:00Z"` (environment variable takes precedence).

---

## TC-CONFIG-TEMP-004: Missing configuration causes fatal startup failure

### Preconditions

Both `temperature.migration_cutover_time` file key and `TEMPERATURE_MIGRATION_CUTOVER_TIME` environment variable are absent or empty.

### Expected result

* Service fails startup immediately.
* Logs a `FATAL` error: `FATAL: Missing required configuration 'temperature.migration_cutover_time'`.
* Service process terminates with a non-zero exit code.

---

## TC-CONFIG-TEMP-005: Malformed timestamp causes fatal startup failure

### Preconditions

`temperature.migration_cutover_time = "invalid-date-string"`.

### Expected result

* Service fails startup immediately.
* Logs a `FATAL` error: `FATAL: Unparseable configuration 'temperature.migration_cutover_time'`.
* Service process terminates with a non-zero exit code.

---

## TC-CONFIG-TEMP-006: Timezone offset handling

### Preconditions

`temperature.migration_cutover_time = "2026-07-01T05:30:00+05:30"`.

### Expected result

* Timestamp is parsed and normalized to UTC `2026-07-01T00:00:00Z`.
* Service initializes successfully with canonical UTC timestamp.

---

# 7. Wi-Fi Client Usage Summary Test Cases

## Endpoint

```http
GET /api/v1/devices/{routerId}/wifi-clients/usage-summary
```

Expected response:

```json
{
  "requestedTimeWindow": {
    "startTime": "2026-07-26T12:00:00Z",
    "endTime": "2026-07-27T12:00:00Z"
  },
  "resultTimeWindow": {
    "earliestActualStartTime": "2026-07-26T12:00:00Z",
    "latestActualEndTime": "2026-07-27T12:00:00Z",
    "boundaryFallbackUsed": false
  },
  "items": [
    {
      "mac": "e2:51:95:ed:0f:28",
      "rx_bytes": 106487500,
      "tx_bytes": 3851250,
      "total_bytes": 110338750,
      "data_consume_rx": "106.49 MB",
      "data_consume_tx": "3.85 MB",
      "total_data_usage": "110.34 MB",
      "usage_accuracy": "exact",
      "incomplete": false,
      "segment_count": 1,
      "boundary_fallback_used": false
    }
  ],
  "totalClients": 1,
  "truncated": false
}
```

---

## TC-USAGE-001: Basic cumulative-counter calculation

### Test data

```text
Time     RX bytes    TX bytes
10:00    1000        500
10:10    3000        1500
10:20    6000        2500
```

### Calculation

```text
RX delta = (3000 - 1000) + (6000 - 3000) = 5000
TX delta = (1500 - 500) + (2500 - 1500) = 2000
```

### Expected result

```text
RX usage = 5000 bytes
TX usage = 2000 bytes
Total = 7000 bytes
```

The API converts the byte totals to decimal megabytes.

---

## TC-USAGE-002: Pre-window baseline is available

### Test data

```text
09:59 baseline: RX=1000, TX=500
10:10 sample:   RX=3000, TX=1500
10:20 sample:   RX=6000, TX=2500
```

Requested start time:

```text
10:00
```

### Expected result

```text
RX delta = 5000
TX delta = 2000
```

The baseline itself is not counted as in-window traffic, but it is used to calculate the first delta.

---

## TC-USAGE-003: No pre-window baseline

### Test data

First in-window cumulative sample:

```text
RX=500000
TX=100000
```

No earlier sample exists.

### Expected result

* First cumulative sample contributes a delta of zero.
* The API does not assume all `500000` and `100000` bytes were generated inside the requested range.
* Later sample deltas are counted normally.
* The client response has `usage_accuracy: "lower_bound"`.
* The client response has `incomplete: true`.

---

## TC-USAGE-004: Counter increases normally

### Test data

```text
Previous RX = 1000
Current RX = 1500
```

### Expected result

```text
RX delta = 500
```

---

## TC-USAGE-005: Counter decreases because of reset

### Test data

```text
Previous RX = 5000
Current RX = 200
```

No confirmed fixed-width rollover exists.

### Expected result

* Delta for this sample is `0`.
* `200` becomes the new baseline.
* The implementation does not add `200` as traffic automatically.
* The client response has `usage_accuracy: "lower_bound"`.
* The client response has `incomplete: true`.

---

## TC-USAGE-006: Confirmed fixed-width counter rollover

### Preconditions

* Counter width and maximum are known.
* Rollover is positively identified.

### Test data

```text
counterMax = 65535
previous = 65530
current = 10
```

### Expected result

```text
delta = (65535 - 65530) + 10 + 1
delta = 16
```

---

## TC-USAGE-007: Client reconnect creates a new session

### Test data

```text
Session A:
RX 1000 → 5000

Session B:
RX 100 → 600
```

### Expected result

* Deltas are calculated separately per session.
* Session B's first value is not subtracted from Session A's last value.
* Final result combines valid deltas for the same client MAC.

---

## TC-USAGE-008: Client moves between BSSIDs

### Test data

Same station MAC moves:

```text
BSSID A → BSSID B
```

### Expected result

* Final response has one row for the station MAC.
* BSSID A and BSSID B are separate counter streams unless a reliable session ID proves continuity.
* Stream deltas are calculated before aggregation by MAC.

---

## TC-USAGE-009: Client moves between SSIDs

### Expected result

* SSID change creates a stream boundary when no reliable session identifier exists.
* Counter decrease during the move does not create false usage.

---

## TC-USAGE-010: Client moves between 2.4 GHz and 5 GHz

### Expected result

* Radio or band change is used as a stream boundary when required.
* Final result remains grouped by station MAC.

---

## TC-USAGE-011: Exact duplicate samples

### Test data

Two identical samples have the same:

```text
station MAC
timestamp
BSSID
SSID
band
RX
TX
```

### Expected result

* Duplicate is removed before delta calculation.
* Usage is not counted twice.

---

## TC-USAGE-012: Out-of-order sample

### Test data

Samples arrive in this order:

```text
10:20 (RX = 3000, TX = 1500)
10:10 (RX = 2000, TX = 1000)
10:30 (RX = 5000, TX = 2500)
```

### Expected result

* All valid samples presented out of temporal order must be deterministically sorted by timestamp ASC (`10:10 -> 10:20 -> 10:30`) before differential calculation.
* Valid out-of-order historical telemetry must NOT be discarded or treated as stale.
* Deltas are calculated sequentially across the sorted stream: `(3000-2000) + (5000-3000) = 3000` RX bytes.
* Under-counting or dropping valid out-of-order samples is prohibited. Negative or duplicated usage is not produced.

---

## TC-USAGE-013: Same timestamp samples

### Test data

```text
Case A, same stream and identical counters:
10:00 BSSID-A SSID-A radio-1 RX=9000 TX=1000
10:00 BSSID-A SSID-A radio-1 RX=9000 TX=1000

Case B, same stream and different counters:
10:00 BSSID-A SSID-A radio-1 RX=9000 TX=1000
10:00 BSSID-A SSID-A radio-1 RX=1000 TX=500

Case C, different stream keys:
10:00 BSSID-A SSID-A radio-1 RX=9000 TX=1000
10:00 BSSID-B SSID-B radio-2 RX=1000 TX=500
```

### Expected result

* Case A is treated as an exact duplicate and collapses to one sample before delta calculation.
* Case B is ambiguous and is excluded from delta calculation unless a reliable source sequence number proves temporal order.
* Case C is calculated independently by stream key; different streams are not ordered against each other or combined.
* BSSID, SSID, radio, or other stable fields identify streams. They must not be used as tie-breakers to invent temporal order for two different counter values with the same stream key and timestamp.

---

## TC-USAGE-014: Multiple clients

### Test data

Associations contain three different station MAC addresses.

### Expected result

* Response contains one result per normalized MAC.
* Counters are not mixed between clients.

---

## TC-USAGE-015: Same MAC appears in different letter case

### Test data

```text
E2:51:95:ED:0F:28
e2:51:95:ed:0f:28
```

### Expected result

* MAC addresses are normalized to canonical lowercase (`e2:51:95:ed:0f:28`).
* Both records belong to one client result item with `mac: "e2:51:95:ed:0f:28"`.
* Response `mac` field matches `^[0-9a-f]{2}(:[0-9a-f]{2}){5}$`.

---

## TC-USAGE-016: RX traffic only

### Expected result

```text
data_consume_rx > 0
data_consume_tx = 0.00 MB
total_data_usage = RX usage
```

---

## TC-USAGE-017: TX traffic only

### Expected result

```text
data_consume_rx = 0.00 MB
data_consume_tx > 0
total_data_usage = TX usage
```

---

## TC-USAGE-018: No traffic change

### Test data

```text
Previous RX = 5000
Current RX = 5000
Previous TX = 1000
Current TX = 1000
```

### Expected result

```text
RX delta = 0
TX delta = 0
total = 0
```

---

## TC-USAGE-019: No associated clients

### Expected result

```json
{
  "requestedTimeWindow": {
    "startTime": "2026-07-26T12:00:00Z",
    "endTime": "2026-07-27T12:00:00Z"
  },
  "resultTimeWindow": {
    "earliestActualStartTime": null,
    "latestActualEndTime": null,
    "boundaryFallbackUsed": false
  },
  "items": [],
  "totalClients": 0,
  "truncated": false
}
```

* HTTP `200 OK`.

---

## TC-USAGE-020: Association has missing counters

### Expected result

* Missing counter does not cause an exception.
* Invalid sample is skipped or treated according to the explicit ingestion rule.
* It must not produce a very large wrapped unsigned value.

---

## TC-USAGE-021: Byte-to-megabyte conversion

### Test data

```text
1,000,000 bytes
```

### Expected result

```text
1.00 MB
```

The implementation uses:

```text
bytes / 1,000,000
```

---

## TC-USAGE-022: Unit label matches calculation

### Expected result

* Decimal megabyte conversion is labelled `MB`.
* The response must not use `Mb`, which commonly means megabits.
* If binary-byte conversion is used instead, it must be labelled `MiB`.
* Response formatting follows the API contract.

---

## TC-USAGE-023: Total equals RX plus TX

### Expected result

For every client, the total usage invariant is defined on raw byte counters, and each display field is derived using the documented unit formatter:

```text
total_bytes = rx_bytes + tx_bytes
data_consume_rx  = format_bytes(rx_bytes)
data_consume_tx  = format_bytes(tx_bytes)
total_data_usage = format_bytes(total_bytes)
```

Direct string addition on formatted fields (e.g. `"10.00 MB" + "20.00 MB"`) is invalid because independent rounding of component display strings can differ from formatted raw byte totals. The test suite asserts `total_bytes = rx_bytes + tx_bytes` on raw integer counters, and verifies `data_consume_rx`, `data_consume_tx`, and `total_data_usage` independently against `format_bytes(...)`.

---

## TC-USAGE-024: Very large cumulative counters

### Expected result

* No integer overflow.
* Deltas and conversion remain correct for 64-bit counters.

---

## TC-USAGE-025: Gateway filter prevents cross-device mixing

### Preconditions

The same client MAC connects to two gateways.

### Expected result

* Requested gateway's result includes only associations from:

```text
boardId = resolvedBoardId
serialNumber = routerId
```

* Traffic from the second gateway is excluded.

---

## TC-USAGE-026: Malformed `ssid_data`

### Expected result

* Service does not crash.
* Malformed record is skipped and logged.
* Other valid records in the requested range are processed.
* Fabricated usage is not returned.

---

## TC-USAGE-027: Client with only outside-window boundary fallback samples

### Test data

* Requested time window: `[10:00:00Z, 11:00:00Z)`.
* Database contains a sample for station MAC `aa:bb:cc:dd:ee:ff` at `09:58:00Z` (pre-window boundary sample within 2x interval tolerance).
* Database contains NO in-window observations (`[10:00:00Z, 11:00:00Z)`) and no proven session overlap during `[10:00:00Z, 11:00:00Z)` for station `aa:bb:cc:dd:ee:ff`.

### Expected result

* Station MAC `aa:bb:cc:dd:ee:ff` is EXCLUDED from `items[]` and NOT counted in `totalClients`.
* Outside-window fallback samples are used solely for calculating boundary differentials for clients with proven in-window presence or session overlap.
* Response returns `items: []`, `totalClients: 0`, and `truncated: false`.

---

## TC-USAGE-028: Independent directional RX/TX counter calculation

### Test data

* Samples for client `11:22:33:44:55:66`:
  * `10:00:00Z`: `rx_bytes = 1000000`, `tx_bytes = -1` (invalid/missing TX)
  * `10:10:00Z`: `rx_bytes = 3000000`, `tx_bytes = 500000`

### Expected result

* RX bytes are calculated independently: `3000000 - 1000000 = 2000000` bytes (`data_consume_rx = "2.00 MB"`).
* TX bytes are uncalculable for 10:00:00Z sample; TX returns `0` bytes (required non-null integer in `calculation_segments`) and formatted `"0.00 MB"` in summary strings.
* Segment `total_bytes` = `2000000 + 0 = 2000000`.
* The stream usage accuracy is classified as `lower_bound`.
* Client `11:22:33:44:55:66` is included in `items[]` and counted in `totalClients`.

---

## TC-USAGE-029: Calculation details flag and segment non-null integer invariants

### Test data

Query with `includeCalculationDetails=true` vs `includeCalculationDetails=false` (or omitted).

### Expected result

* When `includeCalculationDetails=false` or omitted: `items[]` contain `mac`, `data_consume_rx`, `data_consume_tx`, `total_data_usage`, and `usage_accuracy`; `calculation_segments` array is omitted.
* When `includeCalculationDetails=true`: `items[]` additionally expose `calculation_segments[]`. Each segment object has non-null required integer fields `rx_bytes`, `tx_bytes`, and `total_bytes`.
* The byte invariant `rx_bytes + tx_bytes == total_bytes` holds for every segment.

---

## TC-USAGE-030: Fallback boundary sampling 2x interval tolerance threshold

### Test data

* Expected collection interval = 300 seconds (5 minutes). Tolerance threshold = 2 * 300 = 600 seconds (10 minutes).
* Client A has pre-start fallback sample at 09:51:00Z (9 minutes before 10:00:00Z start, within tolerance threshold).
* Client B has pre-start fallback sample at 09:48:00Z (12 minutes before 10:00:00Z start, outside tolerance threshold).

### Expected result

* Client A uses the 09:51:00Z sample as boundary baseline (accuracy `exact` or `bounded_interval`).
* Client B cannot use the 09:48:00Z sample because it exceeds 2x collection interval tolerance; Client B starts delta calculation from the first in-window sample at 10:05:00Z (accuracy `lower_bound`).

---

## TC-USAGE-031: Result time window calculation

### Test data

Observations for requested window `[10:00:00Z, 11:00:00Z)` occur at `10:05:00Z` and `10:55:00Z`.

### Expected result

* `resultTimeWindow.startTime` = `"2026-07-27T10:05:00Z"` (earliest contributing sample in window).
* `resultTimeWindow.endTime` = `"2026-07-27T10:55:00Z"` (latest contributing sample in window).
* `requestedTimeWindow` remains `"2026-07-27T10:00:00Z"` to `"2026-07-27T11:00:00Z"`.

---

## TC-USAGE-032: Client truncation threshold and deterministic ordering

### Test data

501 active clients exist in the requested window.

### Expected result

* `totalClients` = 501.
* `items[]` array length = 500.
* `truncated` = `true`.
* Returned 500 items are deterministically sorted by `total_data_usage` DESC, then station `mac` ASC (canonical lowercase) as tie-breaker.

---

# 8. Wi-Fi Client RSSI Summary Test Cases

## Endpoint

```http
GET /api/v1/devices/{routerId}/wifi-clients/rssi-summary
```

RSSI categories:

```text
Excellent: RSSI >= -55
Good:      -67 <= RSSI < -55
Fair:      -75 <= RSSI < -67
Poor:      RSSI < -75
```

Invalid RSSI:

```text
0
positive values
values below -127
NULL
```

---

## TC-RSSI-001: Excellent RSSI boundary

### Test data

```text
RSSI = -55
```

### Expected result

* Sample is classified as `excellent`.

---

## TC-RSSI-002: RSSI above excellent boundary

### Test data

```text
RSSI = -40
```

### Expected result

* Sample is classified as `excellent`.

---

## TC-RSSI-003: Good upper boundary

### Test data

```text
RSSI = -56
```

### Expected result

* Sample is classified as `good`.

---

## TC-RSSI-004: Good lower boundary

### Test data

```text
RSSI = -67
```

### Expected result

* Sample is classified as `good`.

---

## TC-RSSI-005: Fair upper boundary

### Test data

```text
RSSI = -68
```

### Expected result

* Sample is classified as `fair`.

---

## TC-RSSI-006: Fair lower boundary

### Test data

```text
RSSI = -75
```

### Expected result

* Sample is classified as `fair`.

---

## TC-RSSI-007: Poor boundary

### Test data

```text
RSSI = -76
```

### Expected result

* Sample is classified as `poor`.

---

## TC-RSSI-008: Minimum accepted RSSI

### Test data

```text
RSSI = -127
```

### Expected result

* Sample is valid.
* It is classified as `poor`.

---

## TC-RSSI-009: RSSI below valid range

### Test data

```text
RSSI = -128
```

### Expected result

* Sample is ignored.

---

## TC-RSSI-010: RSSI equals zero

### Expected result

* Sample is ignored.
* It does not enter any quality bucket.

---

## TC-RSSI-011: Positive RSSI

### Test data

```text
RSSI = 20
```

### Expected result

* Sample is ignored.

---

## TC-RSSI-012: Null RSSI

### Expected result

* Sample is ignored.

---

## TC-RSSI-013: Calculate percentages for one client

### Test data

```text
Excellent samples: 4
Good samples:      3
Fair samples:      2
Poor samples:      1
```

### Expected result

```json
{
  "rssi_excellent_pct": 40.0,
  "rssi_good_pct": 30.0,
  "rssi_fair_pct": 20.0,
  "rssi_poor_pct": 10.0,
  "rssi_total_samples": 10
}
```

---

## TC-RSSI-014: Percentages require decimal rounding

### Test data

```text
Excellent: 1
Good: 1
Fair: 1
Total: 3
```

### Expected result

```text
Excellent = 33.33
Good = 33.33
Fair = 33.33
Poor = 0.00
```

Percentages are rounded to two decimal places.

---

## TC-RSSI-015: Percentage sum after rounding

### Expected result

* Percentages are independently rounded to two decimal places.
* A minor rounding result such as `99.99` or `100.01` is acceptable only if documented.
* The implementation must not silently assign the rounding difference to an arbitrary category unless specified.

---

## TC-RSSI-016: All samples are excellent

### Expected result

```text
excellent_pct = 100
good_pct = 0
fair_pct = 0
poor_pct = 0
```

---

## TC-RSSI-017: All samples are poor

### Expected result

```text
excellent_pct = 0
good_pct = 0
fair_pct = 0
poor_pct = 100
```

---

## TC-RSSI-018: Valid and invalid samples mixed

### Test data

```text
-50
-60
-70
-80
0
20
-128
NULL
```

### Expected result

* Total valid samples: `4`.
* Each valid quality category has one sample.
* Every percentage is `25%`.
* Invalid samples do not affect `rssi_total_samples`.

---

## TC-RSSI-019: Multiple clients

### Expected result

* Each normalized station MAC has an independent sample count and percentages.
* No RSSI samples are shared between clients.

---

## TC-RSSI-020: Same MAC with different case

### Expected result

* MAC is normalized.
* Samples are aggregated into one response row.

---

## TC-RSSI-021: Client moves between BSSIDs

### Expected result

* RSSI samples remain grouped by station MAC.
* BSSID movement does not create duplicate final client rows.

---

## TC-RSSI-022: No clients

### Expected result

```json
{
  "requestedTimeWindow": {
    "startTime": "2026-07-26T12:00:00Z",
    "endTime": "2026-07-27T12:00:00Z"
  },
  "resultTimeWindow": {
    "firstSampleTime": null,
    "lastSampleTime": null,
    "totalSamples": 0
  },
  "items": [],
  "totalClients": 0,
  "truncated": false
}
```

* HTTP `200 OK`.

---

## TC-RSSI-023: Client has only invalid samples

### Expected result

* The client is omitted from the response.
* No percentage division by zero occurs.
* `NaN` or infinity is never returned.

---

## TC-RSSI-024: Samples outside requested range

### Expected result

* Out-of-range RSSI samples are excluded.

---

## TC-RSSI-025: Gateway filtering

### Preconditions

The same client MAC appears on two gateways.

### Expected result

* Only samples associated with the requested gateway's timepoints are included.

---

## TC-RSSI-026: Malformed association entry

### Expected result

* Invalid association is skipped and logged.
* Other valid associations in the requested range are processed.
* Invalid association data does not crash the API.

---

# 9. Cross-API Consistency Test Cases

## TC-CROSS-001: Same time range across all APIs

### Steps

Call all five APIs using identical:

```text
routerId
timestampTill
lookbackHours
```

### Expected result

* Every API calculates the same requested `startTime` and `endTime` from `timestampTill` and `lookbackHours`.
* Memory, usage, and RSSI apply the requested half-open aggregation window: `startTime <= sample_time < endTime`.
* Temperature requires `startTime >= temperatureMigrationCutoverTime`; if requested `startTime < temperatureMigrationCutoverTime`, the temperature request is rejected with HTTP `400 Bad Request` (`error: "temperature_range_before_cutover"`) matching TC-TEMP-017 rather than clipping the requested range.
* Availability applies the requested event window only when `startTime >= availabilityValidFrom`; otherwise the availability request is rejected according to the API contract.

---

## TC-CROSS-002: Router resolution reuse across separate MCP HTTP requests

### Preconditions

Five separate MCP metric HTTP requests are made for the same gateway `routerId`.

### Expected result

* Separate MCP metric calls are separate HTTP requests and do not share request-scoped state.
* Router resolution reuse across different metric endpoints occurs through `VenueCoordinator`'s maintained `routerId -> boardId` map or the process-level router-resolution cache.
* OWPROV is not queried on every request when the local `VenueCoordinator` map or unexpired process-level cache entry is present.

---

## TC-CROSS-003: Gateway has no metric data

### Expected result

```text
Memory API:      null summary fields
Temperature API: null summary fields
Usage API:       object envelope with items: [], totalClients: 0, truncated: false
RSSI API:        object envelope with items: [], totalClients: 0, truncated: false
Availability:    data.fetch_status = success, data.offline_count = 0, meta.coverage = full
```

* All metric responses use HTTP `200 OK` when queries succeed but return no data.
* Availability returns `data.fetch_status = "success"` and `data.offline_count = 0` as an exact result only when `startTime >= availabilityValidFrom`, `meta.coverage = "full"`, and `meta.availabilityCoverage.processedThrough >= endTime`.
* If `startTime < availabilityValidFrom`, Availability returns `400 Bad Request` with `error: "availability_range_before_cutover"`.

---

## TC-CROSS-004: Gateway is offline during the requested period

### Expected result

* Availability API reports the observed offline transition.
* Other APIs return data available before shutdown within the requested range.
* Lack of samples after shutdown does not erase earlier valid data.
* Missing later samples are not converted into zero memory, zero temperature or zero RSSI.

---

## TC-CROSS-005: Data from another gateway is present

### Expected result

* Every API filters by the requested gateway identity.
* Results from different gateways are not mixed.

---

## TC-CROSS-006: Board ownership changes during history window

### Expected result

* Memory, temperature, usage and RSSI follow the currently defined board-resolution/query design.
* Availability history remains durable by `serialNumber`.
* No historical availability event is lost due to reassignment.

---

## TC-CROSS-007: Database migration from previous release

### Steps

1. Start with a previous-version database.
2. Run all required migrations.
3. Retain old timepoints.
4. Insert new-format timepoints and availability events.
5. Call all APIs.

### Expected result

* Service starts successfully.
* Existing data remains available.
* Missing new fields in old rows are handled safely.
* New fields are stored correctly.
* All four availability storage tables (`device_availability_state`, `device_availability_events`, `device_availability_ingestion_checkpoint`, `device_availability_ingestion_gaps`), both sequence columns (`source_sequence`, `last_source_sequence`), and associated sequence-aware indexes/constraints are created.
* No existing data is deleted unintentionally.

---

# 10. Response Contract Test Cases

## TC-CONTRACT-001: Memory response field names

Expected exact fields:

```text
min_memfree
max_memfree
avg_memfree
```

---

## TC-CONTRACT-002: Temperature response field names

Expected exact fields:

```text
min_wifi_temp_2.4G
max_wifi_temp_2.4G
avg_wifi_temp_2.4G
min_wifi_temp_5G
max_wifi_temp_5G
avg_wifi_temp_5G
```

All temperature fields are reported in degrees Celsius.

---

## TC-CONTRACT-003: Usage response field names

Expected exact fields:

```text
mac
rx_bytes
tx_bytes
total_bytes
data_consume_rx
data_consume_tx
total_data_usage
usage_accuracy
incomplete
segment_count
boundary_fallback_used
```

---

## TC-CONTRACT-004: RSSI response field names

Expected exact fields:

```text
mac
rssi_excellent_pct
rssi_good_pct
rssi_fair_pct
rssi_poor_pct
rssi_total_samples
```

---

## TC-CONTRACT-005: Availability response field names

Expected exact fields:

```text
meta
data
```

Expected `data` fields:

```text
gw_uuid
fetch_status
offline_count
```

`offline_count` is an integer for `coverage = "full"` or `coverage = "partial"`.
It is `null` for `coverage = "none"` and `accuracy = "not_applicable"`.

Expected availability-specific `meta` fields:

```text
requestedWindow
observedWindow
sourceWindow
contributingWindow
selection
coverage
accuracy
sampleCount
offlineEventCount
effectiveSamplingIntervalSeconds
allowedGapSeconds
boundarySamplesUsed
availabilityCoverage
```

Expected `meta.availabilityCoverage` fields:

```text
coverageStart
processedThrough
allowedIngestionDelaySeconds
ingestionGapKnown
proofSource
```

`coverageStart` and `processedThrough` are derived from durable per-serial
availability ingestion checkpoint state, and `ingestionGapKnown` reflects
persisted gaps that overlap the requested interval.

For availability responses, `sampleCount` is retained for response-shape
consistency with the other summary APIs. When `meta.coverage` is `"full"` or `"partial"`, `sampleCount` is defined as:

```text
sampleCount = offlineEventCount = offline_count
```

`sampleCount` and `offlineEventCount` both mean the number of offline transition rows in
`device_availability_events` contributing to `offline_count`. Online recovery transition rows
(`event_type = 'online'`) are stored in transition history for state tracking and `observedWindow` /
`sourceWindow` bounds, but they do not contribute to `sampleCount`, `offlineEventCount`, or `offline_count`.
For `"full"` or `"partial"` coverage, `sampleCount` always equals `offlineEventCount`.

When `meta.coverage = "none"` and `meta.accuracy = "not_applicable"`, `sampleCount`, `offlineEventCount`,
and `offline_count` are all `null`. `boundarySamplesUsed` is data-dependent: `beforeStart` is `true` only
when a pre-start boundary event was actually selected by the query setup, and event counting does not require
a pre-start boundary event.

---

## TC-CONTRACT-006: Client summary envelope shape

### Expected result

* Usage (`GET /api/v1/devices/{routerId}/wifi-clients/usage-summary`) and RSSI (`GET /api/v1/devices/{routerId}/wifi-clients/rssi-summary`) summary responses are object envelopes containing `requestedTimeWindow`, `resultTimeWindow`, `items`, `totalClients`, and `truncated`.
* When no clients match the requested interval, `items` is an empty array `[]`, `totalClients` is `0`, and `truncated` is `false`.

---

## TC-CONTRACT-007: Client MAC address format

### Expected result

* Usage and RSSI `mac` fields match `^[0-9a-f]{2}(:[0-9a-f]{2}){5}$`.
* MAC addresses are returned as canonical lowercase colon-separated six-octet values.
* Upper-case hex or arbitrary strings are not valid client MAC addresses in the response contract.

---

## TC-CONTRACT-008: Usage byte totals

### Expected result

For every usage summary item:

```text
total_bytes = rx_bytes + tx_bytes
```

---

## TC-CONTRACT-009: No request body

### Expected result

* All five APIs work as GET requests without a request body.
* Inputs are accepted only through the path and query parameters.

---

## TC-CONTRACT-010: JSON data types

### Expected result

* Memory min and max are numeric or `null`.
* Memory average is numeric or `null`.
* Temperature fields are numeric or `null`.
* Usage summary client objects use these exact data types:

```text
mac                 string
rx_bytes            non-negative integer
tx_bytes            non-negative integer
total_bytes         non-negative integer
data_consume_rx     formatted string using the documented unit
data_consume_tx     formatted string using the documented unit
total_data_usage    formatted string using the documented unit
usage_accuracy      enum string: "exact", "bounded_interval", or "lower_bound"
incomplete          boolean
segment_count       non-negative integer
boundary_fallback_used boolean
```

* RSSI percentages are numeric.
* RSSI sample count is an integer.
* Offline count is a non-negative integer when coverage is "full" or "partial".
* It is null when coverage is "none" and accuracy is "not_applicable".

---

# 11. Acceptance Criteria

The PR implementation is functionally accepted when:

1. All five endpoints are available in OpenAPI.
2. Every endpoint uses the gateway serial number as `routerId`.
3. Router ownership resolves correctly from the maintained local map.
4. OWPROV fallback resolution distinguishes `404` and `409` status outcomes.
5. Child-venue gateway resolution works.
6. Timestamp and lookback validation is consistent.
7. Memory aggregation ignores missing historical fields instead of treating them as zero.
8. Temperature aggregation excludes synthetic or invalid fallback values.
9. Client bandwidth is calculated from reset-safe counter deltas.
10. A pre-window baseline is used when available.
11. Counter resets, reconnects, BSSID changes and duplicates do not inflate usage.
12. RSSI thresholds and boundary values are classified correctly.
13. Invalid RSSI values are ignored.
14. RSSI percentages are calculated per client.
15. Usage and RSSI client-summary responses use the documented object envelope shape (`requestedTimeWindow`, `resultTimeWindow`, `items`, `totalClients`, `truncated`) matching TC-CONTRACT-006.
16. Gateway shutdown and network loss create one offline transition each.
17. Repeated pings and disconnections do not create duplicate transitions.
18. Availability events remain queryable after board reassignment.
19. Availability success responses include `fetch_status: "success"`.
20. Empty successful queries return the documented empty response.
21. Response field names and data types match the MCP contract exactly.
22. Data from one gateway never appears in another gateway's response.
