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

The test cases cover:
* API contract tests for request shapes, HTTP status codes, response schemas, parameter validation, filtering, and half-open time-range window semantics `[startTime, endTime)`.
* Service integration tests for Kafka topic consumption, OWPROV fallback resolution, `VenueCoordinator` maintained ownership map, process-level router resolution cache, and PostgreSQL storage queries.
* Database/white-box tests for `device_availability_events`, `timepoints`, and `wificlienthistory` table schemas, constraints, and event counting behavior.
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
timepoints
wificlienthistory
```

8. Gateway availability uses the existing persisted gateway state record as the authoritative state and `device_availability_events` as the transition log. Kafka `disconnection` messages are treated as `offline`; Kafka `ping` and `capabilities` messages are treated as `online`. Every newer message updates that record's `lastContact`; pings also update `lastPing`, and real offline transitions update `lastDisconnection`. This does not require a table named `device` or `devices`.
9. Database records for the test gateway are cleared or isolated before each independent test.
10. A valid authorization token is available for testing endpoint access.

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

Call an API with a syntactically invalid router ID:

```http
GET /api/v1/devices/unknown-router/memory-summary
    ?timestampTill=2026-07-29T12:00:00Z
    &lookbackHours=24
```

### Expected result

* HTTP `400 Bad Request`.
* Error is `invalid_router_id`.
* OWPROV ownership lookup and metric aggregation are not executed.

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
    ?timestampTill=2026-07-29T12:00:00+05:30
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

## TC-COMMON-012: Semantically invalid timestamp

### Request

```http
?timestampTill=2026-99-99T88:77:66Z
&lookbackHours=24
```

### Expected result

* HTTP `400 Bad Request`.
* The value is rejected even though it matches the timestamp shape.
* No database aggregation is performed.

---

## TC-COMMON-013: Zero lookback

### Request

```http
?timestampTill=2026-07-29T12:00:00Z
&lookbackHours=0
```

### Expected result

* HTTP `400 Bad Request`.

---

## TC-COMMON-014: Negative lookback

### Request

```http
?timestampTill=2026-07-29T12:00:00Z
&lookbackHours=-1
```

### Expected result

* HTTP `400 Bad Request`.

---

## TC-COMMON-015: Lookback exceeds configured maximum

### Request

```http
?timestampTill=2026-07-29T12:00:00Z
&lookbackHours=10000
```

### Expected result

* HTTP `400 Bad Request`.
* Error indicates that the maximum supported lookback was exceeded.

---

## TC-COMMON-016: Missing timestamp

### Request

```http
?lookbackHours=24
```

### Expected result

* HTTP `400 Bad Request`.

---

## TC-COMMON-017: Missing lookback

### Request

```http
?timestampTill=2026-07-29T12:00:00Z
```

### Expected result

* HTTP `400 Bad Request`.

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

### Persisted gateway state check

```text
Lock and read the existing persisted gateway state record for:
serialNumber = 60cf84f22290

Verify these fields:
connected
lastContact
lastPing
lastConnection
lastDisconnection
```

### Latest transition event query

```sql
SELECT *
FROM device_availability_events
WHERE serialNumber = '60cf84f22290'
ORDER BY event_time DESC
LIMIT 1;
```

### Availability history query

```sql
SELECT *
FROM device_availability_events
WHERE serialNumber = '60cf84f22290'
ORDER BY event_time ASC;
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

Verify that the first observed ping initializes the persisted gateway state record as online without creating an online transition event.

### Preconditions

* No availability event exists for the gateway.
* The persisted gateway state record has no prior availability contact state:

```text
connected is unset or false only because the row is uninitialized
lastContact is unset or 0
```

### Steps

1. Start the gateway.
2. Wait for the gateway to publish a `ping` message.
3. Wait for Analytics to consume the message.
4. Query the persisted gateway state record.
5. Query `device_availability_events`.

### Expected result

* No `online` event is inserted.
* No `offline` event is inserted.
* The persisted gateway state record is updated:

```text
connected = true
lastContact = ping timestamp
lastPing = ping timestamp
```

* The availability API returns:

```json
{
  "gw_uuid": "60cf84f22290",
  "fetch_status": "success",
  "offline_count": 0
}
```

---

## TC-AVAIL-002: Repeated pings while already online do not create duplicates

### Objective

Verify that regular gateway pings do not create repeated online transition events.

### Preconditions

* The persisted gateway state record has:

```text
connected = true
lastContact < latest ping timestamp
```

### Steps

1. Keep the gateway online.
2. Allow it to send at least three ping messages.
3. Wait for Analytics to process all messages.
4. Query the persisted gateway state record.
5. Query `device_availability_events`.

Example messages:

```text
12:00 ping
12:02 ping
12:04 ping
```

### Expected result

* No additional `online` event is inserted after the first online state.
* `lastContact` is updated to the latest newer ping timestamp.
* `lastPing` is updated to the latest newer ping timestamp.
* `connected` remains `true`.
* `offline_count` remains `0`.

---

## TC-AVAIL-003: Device is physically powered off

### Objective

Verify that physically powering off the gateway creates one offline transition.

### Preconditions

* Gateway is online.
* The persisted gateway state record has `connected = true`.

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
* The persisted gateway state record is updated:

```text
connected = false
lastContact = disconnection message timestamp
lastDisconnection = disconnection message timestamp
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
  "gw_uuid": "60cf84f22290",
  "fetch_status": "success",
  "offline_count": 1
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
* The persisted gateway state record is updated:

```text
connected = true
lastContact = ping or capabilities timestamp
lastPing = ping timestamp when message type is ping
lastConnection = ping or capabilities timestamp
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
  "gw_uuid": "60cf84f22290",
  "fetch_status": "success",
  "offline_count": 1
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
* `connected` becomes `true`.
* `lastContact` and the applicable online timestamp field are updated.
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
12:00 ping          → online
12:10 shutdown      → offline
12:15 boot          → online
13:00 disconnect    → offline
13:05 reconnect     → online
14:00 power off     → offline
14:10 power on      → online
```

Call the API for a time range covering the entire sequence.

### Expected result

The event table contains:

```text
12:00 online
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
  "gw_uuid": "60cf84f22290",
  "fetch_status": "success",
  "offline_count": 3
}
```

---

## TC-AVAIL-011: Repeated disconnection while already offline is ignored

### Objective

Verify that repeated offline messages do not create duplicate offline transitions.

### Preconditions

* The gateway has already produced one offline transition.
* The persisted gateway state record has `connected = false`.

### Steps

1. Keep the gateway offline after the first disconnection.
2. Deliver another newer `disconnection` message for the same gateway while `connected = false`.
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
* For newer repeated disconnection messages while `connected = false`, `lastContact` advances.
* `lastDisconnection` remains the timestamp of the actual offline transition.
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
* The duplicate delivery does not move `connected`, `lastContact`, `lastPing`, or `lastDisconnection`.
* `offline_count` is `1`.

---

## TC-AVAIL-013: Repeated ping while already online is ignored

### Objective

Verify that repeated online messages after an offline-to-online transition do not create duplicate online rows.

### Steps

1. Reconnect or power on the gateway from an offline state.
2. Capture the first ping that creates the online transition.
3. Deliver another newer ping while the persisted gateway state record has `connected = true`.
4. Query availability storage.

### Expected result

* Exactly one online transition event is inserted.
* No additional offline event is inserted.
* The newer repeated ping updates `lastContact` and `lastPing`.
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

### Steps

1. Select a time range containing no offline events.
2. Call the API.

### Expected result

```json
{
  "gw_uuid": "60cf84f22290",
  "fetch_status": "success",
  "offline_count": 0
}
```

The API must not treat an empty result as an error.

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

```text
offline_count = 0
```

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

Verify that syntactically invalid router IDs (such as non-hexadecimal values or invalid punctuation) are rejected with HTTP 400 before OWPROV ownership lookup or availability database queries are executed.

### Requests

1. Handler-level validation requests (non-hexadecimal or invalid punctuation):

```http
GET /api/v1/devices/unknown-router/availability-summary
    ?timestampTill=2026-07-29T12:00:00Z
    &lookbackHours=24

GET /api/v1/devices/:::/availability-summary
    ?timestampTill=2026-07-29T12:00:00Z
    &lookbackHours=24
```

2. Framework / route-level security requests (path dot-segments):

```http
GET /api/v1/devices/./availability-summary
GET /api/v1/devices/../availability-summary
```

### Expected result

* For handler-level validation requests (`unknown-router`, `:::`):
  * HTTP `400 Bad Request`.
  * Response resembles:

```json
{
  "error": "invalid_router_id",
  "message": "routerId must be a valid 12-29 hex character gateway serial number"
}
```

  * OWPROV resolution is not called.
  * No availability query is executed.

* For framework / route-level dot-segment requests (`.`, `..`):
  * Requests are normalized or rejected by the HTTP server/framework layer before reaching the handler.
  * Dot-segments are never passed to OWPROV ownership resolution.

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

* Request is rejected according to the API contract.
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
* Error indicates invalid lookback value.

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
* Error indicates that the supported lookback limit was exceeded.

---



## 4.5. Concurrency and Reliability Test Cases

## TC-AVAIL-028: Two consumers process concurrent disconnection messages

### Objective

Verify that concurrent processing cannot create duplicate offline transitions.

### Preconditions

* Gateway is online.

### Steps

1. Start with the persisted gateway state record locked state as `connected = true`.
2. Deliver two disconnection messages for the same gateway concurrently.
3. Query event storage.

### Expected result

* Gateway state check and event insertion happen inside one database transaction.
* The persisted gateway state record is locked by `serialNumber` while processing.
* Exactly one offline event row is inserted.
* The persisted gateway state record ends with `connected = false` and `lastContact` equal to the accepted disconnection timestamp.
* Offline count is `1`.

---



## TC-AVAIL-029: Stale out-of-order event is ignored

### Objective

Verify that an event older than the persisted gateway state record's `lastContact` cannot change availability history.

### Steps

1. Set the persisted gateway state record to:

```text
connected = true
lastContact = 12:10
lastPing = 12:10
```

2. Deliver a delayed `disconnection` message for the same gateway at `12:05`.
3. Query the persisted gateway state record.
4. Query `device_availability_events`.

### Expected result

* The stale `12:05` event is ignored.
* No offline event row is inserted.
* The persisted gateway state record remains:

```text
connected = true
lastContact = 12:10
lastPing = 12:10
```

---

## TC-AVAIL-030: First event is a disconnection

### Objective

Verify that a first observed `disconnection` message creates an offline transition.

### Preconditions

* No availability event exists for the gateway.
* The persisted gateway state record has:

```text
connected = true
lastContact < disconnection timestamp
```

### Steps

1. Deliver a `disconnection` message.
3. Query `device_availability_events`.
4. Call availability-summary for a window containing the event.

### Expected result

* One `offline` event row is inserted.
* The event uses the gateway `serialNumber`.
* The persisted gateway state record is updated to `connected = false`.
* `offline_count` is `1`.

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
12:00 online
12:10 offline
12:15 online
12:30 offline
```

2. Call availability-summary for a window containing the whole sequence.

### Expected result

* The API counts only the two stored `offline` rows.
* Stored `online` rows are ignored by the offline count.
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
Initial ping                 → online event
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
  "gw_uuid": "60cf84f22290",
  "fetch_status": "success",
  "offline_count": 2
}
```

---



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
temperature_migration_cutover_time = 2026-07-29T10:00:00Z
sample time = 2026-07-29T09:59:59Z
wifi_temp = 62
```

### Expected result

* Sample is excluded because it is before `temperature_migration_cutover_time`.
* It does not affect minimum, maximum or average.

---

## TC-TEMP-008: Post-cutover measured zero temperature

### Preconditions

The sample is at or after `temperature_migration_cutover_time`:

```text
wifi_temp = 0
```

### Expected result

* The sample is included as a valid measured value.
* It is not replaced with `20`.

---

## TC-TEMP-009: Post-cutover missing `wifi_temp`

### Expected result

* Temperature is excluded when `wifi_temp` is missing or `null`.
* No synthetic fallback value is generated.

---

## TC-TEMP-010: Post-cutover present `wifi_temp`

### Expected result

* Numeric `wifi_temp` is included.

---

## TC-TEMP-011: Pre-cutover temperature equal to 20

### Preconditions

* Record timestamp is before `temperature_migration_cutover_time`.
* `wifi_temp = 20`.

### Expected result

* The pre-cutover value is excluded because historical temperature values cannot reliably distinguish measured values from synthetic fallback values.
* It does not affect minimum, maximum or average.

---

## TC-TEMP-012: Pre-cutover temperature other than 20

### Test data

```text
Record timestamp is before temperature_migration_cutover_time
wifi_temp = 62
```

### Expected result

* The sample is excluded because all pre-cutover temperature records are ignored.

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

```text
2.4 GHz:
60 valid
20 historical ambiguous
65 valid
null invalid
70 valid
```

### Expected result

```text
min = 60
max = 70
avg = 65
```

Only `60`, `65`, and `70` are included.

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

## TC-TEMP-017: Temperature samples outside the requested range

### Expected result

* Only samples where `startTime <= sample_time < endTime` are included.

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

# 7. Wi-Fi Client Usage Summary Test Cases

## Endpoint

```http
GET /api/v1/devices/{routerId}/wifi-clients/usage-summary
```

Expected response:

```json
[
  {
    "mac": "e2:51:95:ed:0f:28",
    "rx_bytes": 106487500,
    "tx_bytes": 3851250,
    "total_bytes": 110338750,
    "data_consume_rx": "851.90 Mb",
    "data_consume_tx": "30.81 Mb",
    "total_data_usage": "882.71 Mb",
    "usage_accuracy": "exact",
    "incomplete": false
  }
]
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

The API converts the byte totals to decimal megabits.

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
10:20
10:10
10:30
```

### Expected result

* Samples are ordered by timestamp before processing, or stale records are discarded according to the stream rule.
* Negative or duplicated usage is not produced.

---

## TC-USAGE-013: Same timestamp with deterministic tie-breakers

### Expected result

* BSSID, SSID, radio or other stable fields provide deterministic ordering.
* Exact duplicates are removed.
* Two unrelated streams are not combined incorrectly.

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

* MAC addresses are normalized.
* Both records belong to one client result.

---

## TC-USAGE-016: RX traffic only

### Expected result

```text
data_consume_rx > 0
data_consume_tx = 0.00 Mb
total_data_usage = RX usage
```

---

## TC-USAGE-017: TX traffic only

### Expected result

```text
data_consume_rx = 0.00 Mb
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
[]
```

* HTTP `200 OK`.

---

## TC-USAGE-020: Association has missing counters

### Expected result

* Missing counter does not cause an exception.
* Invalid sample is skipped or treated according to the explicit ingestion rule.
* It must not produce a very large wrapped unsigned value.

---

## TC-USAGE-021: Byte-to-megabit conversion

### Test data

```text
1,000,000 bytes
```

### Expected result

```text
8.00 Mb
```

The implementation uses:

```text
bytes * 8 / 1,000,000
```

---

## TC-USAGE-022: Unit label matches calculation

### Expected result

* Decimal megabit conversion is labelled `Mb`.
* If binary-byte conversion is used instead, it must not still be labelled `Mb`.
* Response formatting follows the API contract.

---

## TC-USAGE-023: Total equals RX plus TX

### Expected result

For every client:

```text
total_bytes = rx_bytes + tx_bytes
total_data_usage =
    data_consume_rx + data_consume_tx
```

The total must be calculated before display rounding or with consistent rounding rules.

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
[]
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
* Temperature applies an effective aggregation window where `effective_start_time = max(startTime, temperature_migration_cutover_time)`, so pre-cutover temperature records are ignored even if they fall inside the requested window.
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
Usage API:       []
RSSI API:        []
Availability:    fetch_status = success, offline_count = 0 (when startTime >= availabilityValidFrom)
```

* All metric responses use HTTP `200 OK` when queries succeed but return no data.
* Availability returns `fetch_status = "success"` and `offline_count = 0` only when `startTime >= availabilityValidFrom`.
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
* Availability tables and indexes are created.
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
gw_uuid
fetch_status
offline_count
```

---

## TC-CONTRACT-006: Bounded client summary arrays

### Expected result

* Usage and RSSI summary responses are arrays, not wrapper objects.
* Each response contains at most 500 client summary items.
* Results are ordered by normalized client MAC address ascending before the 500-client cap is applied.
* If more than 500 clients match, the response contains the first 500 clients in that deterministic order.
* The endpoints do not accept or require `limit` or `cursor` query parameters.

---

## TC-CONTRACT-007: Client MAC address format

### Expected result

* Usage and RSSI `mac` fields match `^[A-Fa-f0-9]{2}(:[A-Fa-f0-9]{2}){5}$`.
* MAC addresses are returned as colon-separated six-octet values.
* Arbitrary strings are not valid client MAC addresses in the response contract.

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
* Usage fields are formatted strings using the documented unit.
* RSSI percentages are numeric.
* RSSI sample count is an integer.
* Offline count is a non-negative integer.

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
15. Usage and RSSI client-summary arrays are ordered by normalized MAC address and capped at 500 clients.
16. Gateway shutdown and network loss create one offline transition each.
17. Repeated pings and disconnections do not create duplicate transitions.
18. Availability events remain queryable after board reassignment.
19. Availability success responses include `fetch_status: "success"`.
20. Empty successful queries return the documented empty response.
21. Response field names and data types match the MCP contract exactly.
22. Data from one gateway never appears in another gateway's response.
