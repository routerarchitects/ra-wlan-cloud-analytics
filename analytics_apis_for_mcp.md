# Analytics APIs for MCP Tools

This document defines the request format, response format, and implementation logic for the following MCP tools in the `ra-wlan-cloud-analytics` service:

- `get_gateway_free_memory`
- `get_gateway_wifi_temp`
- `get_device_bandwidth_consumption`
- `get_device_rssi_quality`
- `get_gateway_offline_count`

This specification is structured across two in-scope component areas:
1. **MCP Analytics Behavior Specification**: Intended HTTP requests, parameter validation, response envelopes, and ownership/error semantics for future MCP-facing Analytics handlers.
2. **Persistence & Pipeline Architecture Design**: Internal storage structures (`device_availability_events`, `device_availability_state`), Kafka event consumption/ordering, Kafka offset commit semantics, and cutover semantics.

Follow-up deliverables:
- OpenAPI contract/schema updates in `openapi/owanalytics.yaml`.
- Independent test specification matrix in `analytics_mcp_api_test_cases.md`.

> [!IMPORTANT]
> This PR does not update `openapi/owanalytics.yaml` and does not publish a
> functional OpenAPI contract for these MCP analytics endpoints. The OpenAPI
> contract/schema update is a follow-up deliverable. The specified production
> architecture changes—including two new availability persistence structures
> (`device_availability_events`, `device_availability_state`), Kafka event
> ordering and offset commit rules, and cutover migration rules—constitute a production architecture
> specification. Approving or merging this behavior specification PR does NOT bypass
> separate explicit architecture design sign-off for backend schema additions and
> production Kafka pipeline changes prior to production deployment.

The intended API behavior matches the MCP tool names and response fields from the provided CSV.

---

# Common Input Parameters

Each MCP tool receives:

```text
router_id: str
timestamp_till: str
lookback_hours: int
```

`router_id` is the gateway serial number value from the CSV. Analytics APIs expose the same value as the path variable `routerId`.

```text
MCP input:              router_id
Analytics path variable: routerId
routerId = router_id
Internal storage field: serialNumber
serialNumber = routerId
```

Public `routerId` validation requires a path-safe string: 1 to 64 alphanumeric characters, hyphens, or underscores (matching `^[a-zA-Z0-9_-]+$`). Syntax validation intentionally rejects path dot-segments such as `.` and `..`, path separators (`/`, `\`), spaces, control characters, and URL-encoded path separators (`%2F`) before OWPROV ownership resolution. Any path-safe serial format (whether hexadecimal or non-hex serial string) passes syntax validation and is sent to OWPROV, letting OWPROV serve as the authoritative entity for verifying serial existence.

The Analytics API calculates the requested time range using checked 64-bit signed epoch arithmetic:

```text
end_time = parse(timestamp_till)
start_time = end_time - (lookback_hours * 3600)
```

Validation & Processing Order:

All REST handlers must enforce request validation in four distinct sequential phases:

0. **Phase 0: Request Authentication (Bearer Token)**
   - Extract and validate HTTP `Authorization` header (`Bearer <token>`).
   - If missing, malformed, expired, or invalid: return HTTP `401 Unauthorized` (`error: "unauthorized"`) immediately.
   - Authentication takes precedence over parameter validation; unauthenticated callers receive HTTP `401 Unauthorized` regardless of whether `routerId`, `timestampTill`, or `lookbackHours` are malformed, missing, or invalid.

1. **Phase 1: Pure Request Parsing & Input Validation (No DB or I/O lookups)**
   - Validate `routerId` syntax (1–64 characters matching `^[a-zA-Z0-9_-]+$`) -> HTTP `400 invalid_router_id` if malformed.
   - Inspect raw query collection for exact-once presence of `timestampTill` and `lookbackHours` -> HTTP `400 invalid_timestamp` / `invalid_lookback_hours` if missing or repeated.
   - Parse `timestampTill` shape & UTC semantics -> HTTP `400 invalid_timestamp` if malformed or invalid date/time.
   - Capture `currentServerTime` once for the request and verify `end_time <= currentServerTime + allowedClockSkewSeconds` -> HTTP `400 invalid_timestamp` if `timestampTill` is too far in the future.
   - Parse `lookbackHours` strict positive integer -> HTTP `400 invalid_lookback_hours` if zero, negative, or non-numeric.
   - Checked epoch calculation: Compute `start_time = end_time - (lookback_hours * 3600)` using signed 64-bit integers. Verify `start_time >= 0` (minimum supported Unix epoch `1970-01-01T00:00:00Z`) -> HTTP `400 invalid_timestamp` if underflowing before epoch.

2. **Phase 2: Router Ownership & Serial Resolution**
   - Resolve `routerId` to `boardId` via local cache / OWPROV.
   - Return HTTP `404 not_found` if the router serial does not exist in OWPROV.
   - Return HTTP `404 not_found` if the router serial exists in OWPROV but is outside the authenticated caller's accessible entity, venue, or board scope.
   - Do not return HTTP `403 Forbidden` for the "existing router outside caller scope" case because it would disclose that the router serial exists.
   - Load router scope monitoring configuration (`monitoringDuration`) to derive `maxLookbackHours = floor(monitoringDuration / 3600)`.

3. **Phase 3: Duration, Retention & Endpoint Cutover Validation**
   - Validate duration against scope maximum (all endpoints): `lookbackHours > maxLookbackHours` -> HTTP `400 invalid_lookback_hours`.
   - Validate requested range against data retention window (all endpoints): requested window outside retention -> HTTP `400 lookback_outside_retention`.
   - Validate endpoint-specific domain cutover thresholds:
     - `radio-temperature-summary` endpoint only: `start_time < temperatureMigrationCutoverTime` -> HTTP `400 temperature_range_before_cutover`.
     - `availability-summary` endpoint only: `start_time < availabilityValidFrom` -> HTTP `400 availability_range_before_cutover`.
     - `memory-summary`, `rssi-summary`, and `bandwidth-consumption` endpoints: no domain cutover validation unless explicitly defined by that endpoint.

Example:

```text
timestamp_till = 2026-07-27T12:00:00Z
lookback_hours = 24

start_time = 2026-07-26T12:00:00Z
end_time   = 2026-07-27T12:00:00Z
```

Recommended query parameters:

```http
?timestampTill=2026-07-27T12:00:00Z
&lookbackHours=24
```

These are `GET` APIs, so no request body is required.

### Internal Retrieval Categories

Retrieval behavior is intrinsic to each endpoint. Do not expose a public
`metricMode` query parameter unless the endpoint also defines separate
mode-specific response schemas.

Endpoint retrieval mapping:

```text
memory-summary:
  dataset retrieval = all
  response = fixed gauge aggregation schema

radio-temperature-summary:
  dataset retrieval = all
  response = fixed gauge aggregation schema

wifi-clients/usage-summary:
  dataset retrieval = differential
  response = calculated cumulative-counter deltas

wifi-clients/rssi-summary:
  dataset retrieval = all
  response = fixed RSSI quality percentage aggregation schema

availability-summary:
  dataset retrieval = all
  response = persisted offline transition count
```

For `all`, retrieve every record in the requested range. `availability-summary`
uses `all` because it counts persisted offline transition rows in
`[startTime, endTime)` and does not need a pre-window baseline.

For `differential`, return the change in cumulative counter values over the
requested time range:

```text
delta = ending metric value - starting metric value
```

Use the valid persisted samples available for the requested range. Effective
boundaries are the requested boundaries clipped to any proven session lifetime.
When samples immediately before or after the requested range are needed to
calculate cumulative-counter deltas, the handler may use them. The response uses
`observedWindow` to show the actual sample timestamps that contributed to the
result; it must not add a result-quality classification.

### Time Conversion and Validation

The public MCP-facing parameters use ISO-8601/RFC3339 UTC time, but the existing Analytics API style uses integer `fromDate` and `endDate` timestamps. Handlers should convert the request as:

```text
timestampTill: RFC3339 UTC string ending with 'Z', for example 2026-07-27T12:00:00Z
endTime:       Unix epoch seconds parsed from timestampTill
startTime:     endTime - (lookbackHours * 3600)
```

Timezone Requirement:
`timestampTill` MUST use the UTC `Z` suffix format (e.g., `2026-07-27T12:00:00Z`). Explicit numeric timezone offsets (such as `+05:30` or `-08:00`) and timestamps without a timezone designator are unsupported and MUST be rejected with `400 Bad Request` and `error: "invalid_timestamp"`.

Validation rules:

```text
timestampTill must parse as a valid UTC timestamp ending with 'Z'.
currentServerTime must be captured once per request after timestampTill parses.
allowedClockSkewSeconds must be the same non-negative service-level value for all MCP analytics handlers.
Unless a deployment explicitly configures another value, allowedClockSkewSeconds defaults to 300 seconds.
endTime must be less than or equal to currentServerTime + allowedClockSkewSeconds.
lookbackHours must be present exactly once.
lookbackHours must parse as a strict whole decimal integer with no trailing characters.
lookbackHours must be greater than 0 and less than or equal to 87600 (10 years API safety cap).
maxLookbackHours = floor(configured monitoringDuration / 3600).
lookbackHours must be less than or equal to maxLookbackHours.
startTime must be less than endTime.
```

If `timestampTill` is beyond `currentServerTime + allowedClockSkewSeconds`, return
`400 Bad Request` with `error: "invalid_timestamp"`. Do not clamp `endTime` to
server time. The returned aggregate must represent the requested half-open
window exactly, not a silently shortened window.

All APIs in this document derive `maxLookbackHours` from the configured `monitoringDuration` for the resolved router venue scope, bounded by an absolute API safety limit of 87600 hours (10 years). Board retention is the authoritative resource bound capping in-memory record loading and parsing; queries requesting lookback windows larger than the resolved board retention (`lookbackHours > maxLookbackHours`) are rejected with `400 invalid_lookback_hours` before querying storage.

For a one-year monitoring configuration, `maxLookbackHours` is `365 * 24` only when the configured monitoring duration is exactly 365 days. Do not assume every calendar year is 8760 hours; use the configured duration and effective retention timestamps when validating the request.

Return validation errors with field-specific error codes:

```text
invalid_timestamp:
  timestampTill is missing, malformed, not UTC `Z` format, beyond
  currentServerTime + allowedClockSkewSeconds, or otherwise unsupported.

invalid_lookback_hours:
  lookbackHours is missing, repeated, empty, non-numeric, fractional, partially numeric, overflowing,
  zero, negative, greater than 87600, or greater than maxLookbackHours.

lookback_outside_retention:
  the calculated [startTime, endTime) window starts before the retained data window
  or ends after the configured request upper bound.
```

A valid timestamp with an invalid `lookbackHours` value must not return `invalid_timestamp`. Internally, query `timepoints.timestamp` and `wificlienthistory.timestamp` using epoch seconds.

### Monitoring Configuration

Handlers must load the monitoring configuration for the resolved router ownership scope before querying metric storage.

```text
monitoringDuration = configured monitoring retention duration
requestEndLimit = min(currentServerTime + allowedClockSkewSeconds, monitoringConfigurationExpiry)
retentionDataEnd = min(currentServerTime, monitoringConfigurationExpiry)
retentionStart = retentionDataEnd - monitoringDuration

If monitoring has been enabled for less time than monitoringDuration:
  retentionStart = max(monitoringEnabledAt, retentionDataEnd - monitoringDuration)
```

Use the configured duration and effective retention timestamps instead of calendar assumptions so leap years, partial-year retention, recently enabled monitoring, and changed retention policies behave consistently.
Do not derive `retentionStart` from `currentServerTime + allowedClockSkewSeconds`;
the skew allowance is a request upper-bound tolerance, not an extension that
slides the retained historical window forward.

The requested half-open range must satisfy:

```text
startTime >= retentionStart
endTime <= requestEndLimit
```

If `endTime > retentionDataEnd` but `endTime <= requestEndLimit`, accept the
request as within clock skew tolerance. Query and report the exact requested
`[startTime, endTime)` window; do not clamp `endTime` to `retentionDataEnd` and
do not reject solely because the request extends slightly beyond
`currentServerTime`.

If the requested range violates `startTime >= retentionStart` or
`endTime <= requestEndLimit`, return `400 Bad Request` with
`error: "lookback_outside_retention"`.

When monitoring is disabled:

```text
stop ingesting new metric and availability records
retain existing records until their existing expiry timestamp
return 409 Conflict for all summary requests
```

Error response:

```json
{
  "error": "monitoring_disabled",
  "message": "Monitoring is disabled for this router scope"
}
```

When monitoring is not configured for the resolved router scope, return:

```http
404 Not Found
```

```json
{
  "error": "monitoring_not_configured",
  "message": "Monitoring is not configured for this router scope"
}
```

### Time Window Semantics

All summary APIs must use a half-open time interval:

```text
[startTime, endTime)

timestamp >= startTime
timestamp < endTime
```

`timestampTill` is the exclusive upper bound. This prevents adjacent requests from double-counting samples or events at the shared boundary.

MCP analytics responses that include window metadata must use common temporal
field names:

```text
requestedWindow.startTime
requestedWindow.endTime
observedWindow.startTime
observedWindow.endTime
```

`observedWindow` must contain only the common temporal fields `startTime` and
`endTime`. Endpoint-specific metadata must not be added inside
`observedWindow`.

For all endpoints, `observedWindow` describes the persisted samples or events
that contributed to the result:

```text
requestedWindow.startTime = startTime
requestedWindow.endTime = endTime

observedWindow.startTime =
  earliest actual persisted sample/event timestamp used in the result
observedWindow.endTime =
  latest actual persisted sample/event timestamp used in the result
```

If no valid persisted data contributes to a result, both
`observedWindow.startTime` and `observedWindow.endTime` are `null`. Analytics
does not attach a result-quality classification to the relationship between
`requestedWindow` and `observedWindow`.

Example:

```text
Request 1: 10:00 <= timestamp < 11:00
Request 2: 11:00 <= timestamp < 12:00
```

A sample exactly at `11:00` belongs only to Request 2.

### Authorization

All device metric handlers must enforce authorization before returning data:

```text
1. Authenticate the bearer token from Authorization: Bearer <token>.
   If the token is missing, invalid, or expired, return 401 Unauthorized.
2. Resolve the caller's accessible entity, venue, and board scope from the token.
3. Resolve routerId to its current router ownership context.
4. Verify that the resolved router belongs to a board or venue the caller may read.
5. Return 404 Not Found when the router exists but the caller cannot access it.
6. Return 404 Not Found when the router does not exist.
```

The MCP analytics endpoints are bearer-token endpoints. Do not allow `X-API-KEY`
authentication for these endpoints unless this specification is explicitly
updated to define API-key semantics and error precedence.

Authorization Model:

The effective router authorization model relies on caller-scoped OWPROV inventory resolution using the caller's Bearer token:

```text
Valid Bearer token
        ↓
OWPROV inventory lookup using caller token
        ↓
Caller allowed to access router?
        ↓
NO  → 404 not_found
YES
        ↓
Resolve Analytics board
        ↓
Read telemetry
```

Authorization must not rely on the caller-supplied `routerId` alone. A caller must not be able to request an arbitrary gateway serial number and retrieve metrics without proving access to the router's current ownership scope. OWPROV is the authoritative source for determining whether the caller can access the router.

If OWPROV returns `404 Not Found`, `401 Unauthorized`, or `403 Forbidden` for the caller's token, Analytics normalizes the response to `404 Not Found` (`{"error": "not_found", "message": "Router was not found"}`). This prevents router existence disclosure.

Do not introduce Analytics-specific metric permissions.

For historical availability, authorize by current router ownership through OWPROV before querying `device_availability_events` by `serialNumber`. The event-time `board_id` field is historical context only and must not be the sole authorization source.

### Current Storage Model

The current service does not store normalized tables named `device_timepoints`, `radio_timepoints`, or `wifi_client_history`.

Current tables are:

```text
timepoints
  boardId
  timestamp
  ap_data      JSON string
  ssid_data    JSON string
  radio_data   JSON string
  device_info  JSON string
  serialNumber

wificlienthistory
  timestamp
  station_id
  bssid
  ssid
  rssi
  rx_bytes
  tx_bytes
  venue_id
  ...
```

Gateway-scoped APIs should accept only `routerId` publicly. The handler must map `routerId` to the existing internal `serialNumber` field and resolve the internal `boardId` before loading `TimePointDB` records. Do not rename the existing `timepoints.serialNumber` field or its indexes to `routerId`.

### RouterId Resolution

The MCP tool cannot pass `boardId`, so Analytics must resolve it from `routerId`, which is the gateway serial number.

Resolution flow:

```text
1. Validate routerId.
2. Call OWPROV inventory with status-aware handling using the requesting
   caller's bearer token:
     GET /api/v1/inventory/{routerId}
3. If OWPROV returns not found, unauthorized, or forbidden, return
   `404 not_found`.
4. Read inventoryTag.venue as venueId.
5. If venueId is empty, return `404 not_found`.
6. Call `BoardsDB.FindBoardVenueRecordsByVenue(venueId)`.
7. If no Analytics board matches the venue, return `404 not_found`.
8. If more than one board matches the venue, return `409 multiple_boards`.
9. Query Analytics storage with resolvedBoardId and serialNumber = routerId.
```

Separate MCP metric calls are separate HTTP requests. Each timepoint-backed handler must resolve the router through the same caller-scoped OWPROV and Analytics board lookup flow before reading stored metrics.

Implementation notes:

```text
OWPROV source:
  /api/v1/inventory/{routerId}

OWPROV response field:
  InventoryTag.venue

Analytics local source:
  BoardInfo / DB board records for venue metadata, retention, and validation
  RouterIdResolver::Result normalized board fields (resolvedBoardId, retention, interval, monitorSubVenues)
```

Primary ownership algorithm:

```text
Call OWPROV inventory for routerId with the current caller token.

If OWPROV denies access or cannot find the router:
  return 404 Not Found

If InventoryTag.venue is empty:
  return 404 Not Found

Find Analytics boards for InventoryTag.venue.

If exactly one board exists:
  return Success

If no board exists:
  return 404 Not Found

If more than one board exists:
  return 409 Conflict
```

Failure handling:

```text
OWPROV inventory not found              -> 404 Not Found
OWPROV inventory outside caller scope   -> 404 Not Found
Inventory exists but venue is empty     -> 404 Not Found
No Analytics board configured for venue -> 404 Not Found
Multiple matching boards                -> 409 Conflict
OWPROV unavailable or invalid response  -> 502 Bad Gateway
```

The existing `SDK::Prov::Device::Get` helper returns only `bool` and does not expose the OWPROV HTTP status. Do not use that bool-only helper when the handler must distinguish `404 Not Found` from OWPROV connectivity or parsing failures. Use one of these instead:

```text
Option A:
  Add a status-aware SDK helper, for example:
    SDK::Prov::Device::GetWithStatus(...)

Option B:
  Call OpenAPIRequestGet(uSERVICE_PROVISIONING, "/api/v1/inventory/{routerId}", ...)
  directly from ResolveRouterIdContext and inspect the HTTP response status.
```

This resolver intentionally does not use the global `VenueCoordinator` device map, monitored-subvenue board scans, or a process-level ownership cache. Any future board lookup optimization must remain secondary to the caller-scoped OWPROV authorization result and must not change memory-summary behavior without an explicit requirement.

---

# 1. `get_gateway_free_memory`

## MCP Tool Signature

```text
get_gateway_free_memory(
    router_id: str,
    timestamp_till: str,
    lookback_hours: int
)
```

## Recommended API

```http
GET /api/v1/devices/{routerId}/memory-summary
    ?timestampTill=2026-07-27T12:00:00Z
    &lookbackHours=24
```

## Example Request

```http
GET /api/v1/devices/60cf84f22290/memory-summary?timestampTill=2026-07-27T12:00:00Z&lookbackHours=24
Authorization: Bearer <token>
```

## Request Body

```text
None
```

## Response

```json
{
  "requestedWindow": {
    "startTime": "2026-07-26T12:00:00Z",
    "endTime": "2026-07-27T12:00:00Z"
  },
  "observedWindow": {
    "startTime": "2026-07-26T12:03:00Z",
    "endTime": "2026-07-27T11:57:00Z"
  },
  "min_memfree": 211374,
  "max_memfree": 215050,
  "avg_memfree": 212074.36
}
```

## API Logic

The gateway state payload contains:

```text
unit.memory.free
unit.memory.total
unit.memory.cached
unit.memory.buffered
```

The current Analytics implementation calculates memory usage percentage from `free` and `total`, but does not preserve all raw memory values as separate historical fields.

### Required Model Change

Add raw memory fields to the device time-point model and persist them with each `timepoints` record:

```cpp
struct DeviceResourceTimePoint {
    std::optional<uint64_t> memory_free;
    std::optional<uint64_t> memory_total;
    std::optional<uint64_t> memory_cached;
    std::optional<uint64_t> memory_buffered;
};

struct DeviceTimePoint {
    ...
    DeviceResourceTimePoint resource_data;
};
```

Required code changes:

```text
src/RESTObjects/RESTAPI_AnalyticsObjects.h
  Add DeviceResourceTimePoint.
  Add DeviceTimePoint::resource_data.

src/RESTObjects/RESTAPI_AnalyticsObjects.cpp
  Add DeviceResourceTimePoint to_json/from_json.
  Add resource_data to DeviceTimePoint to_json/from_json.

src/storage/storage_timepoints.h
  Extend TimePointDBRecordType with a resource_data JSON string field.

src/storage/storage_timepoints.cpp
  Add ORM field resource_data.
  Update Convert(record -> DeviceTimePoint).
  Update Convert(DeviceTimePoint -> record).
  Add an Upgrade migration for existing DBs.

src/APStats.cpp
  Populate DTP.resource_data from unit.memory during state ingestion.
```

Existing rows will not have `resource_data`; treat those rows as missing memory samples rather than zero free memory.

Do not represent missing memory fields as `0`. A missing `memory_free` value and a genuine reported zero must remain distinguishable.

### Ingestion Logic

Ingestion should preserve syntactically and structurally valid source telemetry
and avoid analytics-specific semantic validation. The typed memory resource
model stores byte counts as unsigned values. The accepted range for each memory
field is:

```text
0 <= memory_value <= UINT64_MAX
```

Negative memory values, fractional values, nonnumeric values, and values that
overflow `uint64_t` are structurally invalid for this ingestion contract and are
omitted field-by-field, not preserved in `DeviceResourceTimePoint`. Parse each
memory field independently, persist fields that can be represented as unsigned
64-bit integers, and omit only the malformed or structurally invalid field. Do
not apply cross-field checks such as `memory_free <= memory_total` during
ingestion; those checks belong to the aggregation layer so source telemetry that
is valid for the storage contract remains available for troubleshooting.

```cpp
AnalyticsObjects::DeviceResourceTimePoint resource;

// Validate numeric type, integral value, byte-count range (0..UINT64_MAX), and unsigned 64-bit non-overflow before conversion.
// Field-level preservation policy: malformed or structurally invalid free, total, cached, or buffered values are omitted independently.
// Negative, fractional, nonnumeric, and uint64_t-overflowing values are structurally invalid for this unsigned byte-count storage contract.
// Do not apply analytics-specific semantic checks, such as free <= total, at ingestion time.
auto parseMemoryField = [](const Poco::JSON::Object::Ptr &obj, const std::string &key) -> std::optional<uint64_t> {
    if (!obj->has(key) || obj->isNull(key)) return std::nullopt;
    try {
        Poco::Dynamic::Var raw = obj->get(key);
        if (!raw.isNumeric() || !raw.isInteger()) return std::nullopt;

        uint64_t val = 0;
        raw.convert(val); // Throws on negative values and overflow.
        return val;
    } catch (...) {
        return std::nullopt;
    }
};

auto freeVal = parseMemoryField(memory, "free");
auto totalVal = parseMemoryField(memory, "total");
auto cachedVal = parseMemoryField(memory, "cached");
auto bufferedVal = parseMemoryField(memory, "buffered");

if (freeVal || totalVal || cachedVal || bufferedVal) {
    if (freeVal)     resource.memory_free = *freeVal;
    if (totalVal)    resource.memory_total = *totalVal;
    if (cachedVal)   resource.memory_cached = *cachedVal;
    if (bufferedVal) resource.memory_buffered = *bufferedVal;
    DTP.resource_data = resource;
}
```

### Aggregation Logic

Aggregation decides whether a preserved memory sample is usable for the
free-memory summary. `memory_free` is the required field for this aggregation.
`memory_total` is optional and is used only when present and valid for the
`memory_free <= memory_total` consistency check. `memory_cached` and
`memory_buffered` are unrelated to the free-memory summary and must not affect
sample inclusion.

Use the current `timepoints` table plus parsed JSON fields:

```text
Load TimePointDB records where:
  boardId == resolvedBoardId
  stored serialNumber == request routerId
  timestamp >= startTime
  timestamp < endTime

For each record:
  read record.resource_data.memory_free
  ignore missing resource_data
  include the sample only when memory_free is present and valid
  use memory_total only when present and valid for the free <= total consistency check
  exclude the sample if valid memory_total is present and memory_free > memory_total
  ignore invalid or missing memory_total, memory_cached, and memory_buffered without invalidating memory_free

Return:
  min_memfree = min(memory_free samples)
  max_memfree = max(memory_free samples)
  avg_memfree = sum(memory_free samples) / sample_count
  observedWindow.startTime = earliest timestamp of a memory_free sample contributing to the aggregation
  observedWindow.endTime = latest timestamp of a memory_free sample contributing to the aggregation
```

Memory values are bytes and must satisfy `0 <= memory_value <= UINT64_MAX`.
If both `memory_free` and valid `memory_total` are present, `memory_free` must
not exceed `memory_total`. Samples that violate this consistency rule are
ignored rather than contributing to the summary. Invalid or missing
`memory_cached` or `memory_buffered` values are ignored field-by-field and must
not cause a valid `memory_free` sample to be dropped. For successful responses
with samples, the invariant is:

```text
min_memfree <= avg_memfree <= max_memfree
```

Do not query `device_timepoints.memory_free` unless a separate normalized table and migration are introduced.

### Handler Flow

```text
Authenticate bearer token
    ↓
Validate routerId syntax
    ↓
Inspect raw query parameters for exactly one timestampTill and lookbackHours
    ↓
Parse timestampTill and lookbackHours
    ↓
Calculate start_time with checked epoch arithmetic
    ↓
Call ResolveRouterIdContext(client, routerId)
    ↓
If status is not Success, map RouterIdResolutionStatus to HTTP response
    ↓
Use result.venueId and result.resolvedBoardId
    ↓
Validate lookbackHours against monitoring duration and retention
    ↓
Query timepoints and read resource_data.memory_free samples
    ↓
Calculate min, max and average
    ↓
Return MCP response shape
```

### Empty Result

```json
{
  "requestedWindow": {
    "startTime": "2026-07-26T12:00:00Z",
    "endTime": "2026-07-27T12:00:00Z"
  },
  "observedWindow": {
    "startTime": null,
    "endTime": null
  },
  "min_memfree": null,
  "max_memfree": null,
  "avg_memfree": null
}
```

Use HTTP `200 OK` when the query succeeds but no data exists.

---

# 2. `get_gateway_wifi_temp`

## MCP Tool Signature

```text
get_gateway_wifi_temp(
    router_id: str,
    timestamp_till: str,
    lookback_hours: int
)
```

## Recommended API

```http
GET /api/v1/devices/{routerId}/radio-temperature-summary
    ?timestampTill=2026-07-27T12:00:00Z
    &lookbackHours=24
```

## Example Request

```http
GET /api/v1/devices/60cf84f22290/radio-temperature-summary?timestampTill=2026-07-27T12:00:00Z&lookbackHours=24
Authorization: Bearer <token>
```

## Request Body

```text
None
```

## Response

```json
{
  "requestedWindow": {
    "startTime": "2026-07-26T12:00:00Z",
    "endTime": "2026-07-27T12:00:00Z"
  },
  "observedWindow": {
    "startTime": "2026-07-26T12:04:00Z",
    "endTime": "2026-07-27T11:55:00Z"
  },
  "min_wifi_temp_2.4G": 62,
  "max_wifi_temp_2.4G": 70,
  "avg_wifi_temp_2.4G": 66.64,
  "latest_wifi_temp_2.4G": 68,
  "min_wifi_temp_5G": 56,
  "max_wifi_temp_5G": 65,
  "avg_wifi_temp_5G": 60.38,
  "latest_wifi_temp_5G": 60
}
```

## API Logic

Analytics should store a nullable Wi-Fi temperature value per radio:

```text
radios[].band
radios[].wifi_temp
```

`radios[].wifi_temp` and all `*_wifi_temp_*` response fields use degrees Celsius.

Required ingestion rule:

```text
If the source radio temperature is present and non-null:
  store radios[].wifi_temp = source temperature

If the source radio temperature is missing or null:
  omit radios[].wifi_temp or store radios[].wifi_temp = null

Do not synthesize a numeric fallback temperature for missing data.
Do not store a placeholder in wifi_temp for a missing temperature.
```

Zero-temperature sentinel source of truth:

```text
The source of truth for whether wifi_temp = 0 means unavailable is an explicit
TelemetryTemperatureContract resolved at ingestion time from stable producer
metadata, such as OWPROV inventory deviceType, gateway capabilities platform,
firmware family, or an equivalent service-level contract registry.

The resolved contract must expose:
  wifiTempZeroIsUnavailable: boolean

Persist either the resolved boolean on the radio temperature sample, for example
radio_data[].wifi_temp_zero_is_unavailable, or persist a stable contract id that
can be resolved later to the same boolean. Do not infer zero-sentinel behavior
from the observed temperature value itself.

If no producer/device contract can be resolved for a sample, the default is:
  wifiTempZeroIsUnavailable = false

### Venue Reassignment & Observed Window Rules

When a gateway moves from an old venue (Board A) to a new venue (Board B):
1. `routerId` resolves via OWPROV to the current venue (`Board B`).
2. Analytics queries timepoints filtered by `boardId == Board B` and `serialNumber == routerId`. Historical samples associated with `Board A` are not queried or included.
3. `requestedWindow` reflects the full requested interval (`startTime` to `endTime`).
4. `observedWindow` reflects the first and last valid sample timestamps found for `Board B` within `[startTime, endTime)`.
5. Temperature aggregates (`min`, `max`, `avg`, `latest`) are calculated exclusively from valid samples on `Board B`.
```

Migration boundary configuration & rule:

```text
temperatureMigrationCutoverTime:
  source: Analytics service configuration (file key 'temperature.migration_cutover_time' or ENV 'TEMPERATURE_MIGRATION_CUTOVER_TIME')
  scope: global per deployment
  format: ISO 8601 / RFC 3339 UTC string (e.g. "2026-07-01T00:00:00Z")
  required: true (Analytics service fails startup with a FATAL log if missing or unparseable)

Only use temperature records created at or after temperatureMigrationCutoverTime.
Ignore all earlier records because historical temperature values cannot reliably
distinguish measured values from synthetic fallback values.

If the requested range starts before temperatureMigrationCutoverTime (startTime < temperatureMigrationCutoverTime):
  return 400 Bad Request with JSON error envelope:
  {
    "error": "temperature_range_before_cutover",
    "message": "The requested summary interval starts before the temperature migration cutover timestamp."
  }

Do not filter out post-cutover samples only because the measured value is 20°C.
After the cutover, a present wifi_temp value is treated as a legitimate measurement.
Reject `wifi_temp = 0` only when the corresponding telemetry producer or device
contract defines `0` as an unavailable or uninitialized sensor sentinel.
Otherwise, treat `0°C` as a valid in-range measurement. A sample with
`wifi_temp = null`, `255`, or a value outside the valid range `[-40, 125]` is
treated as a missing sensor reading and MUST be excluded from aggregation.
```

The current radio-band mapping is:

```text
2G → 2
5G → 5
6G → 6
```

Response mapping:

```text
band = 2 → fields ending in _2.4G
band = 5 → fields ending in _5G
```

### Aggregation Logic

After validating `startTime >= temperatureMigrationCutoverTime`, query the `timepoints.radio_data` JSON array:

```text
Load TimePointDB records where:
  boardId == resolvedBoardId
  stored serialNumber == request routerId
  timestamp >= startTime
  timestamp < endTime

For each record:
  parse radio_data
  for each radio in radio_data:
    if radio.band is 2 or 5:
      if radio.wifi_temp is present, non-null, -40 <= radio.wifi_temp <= 125,
         and (radio.wifi_temp != 0 or radio.wifi_temp_zero_is_unavailable is not true):
        add radio.wifi_temp to that band's sample list

For each band:
  min_temperature = min(samples)
  max_temperature = max(samples)
  avg_temperature = sum(samples) / sample_count
  latest_temperature = temperature from the latest valid sample timestamp

observedWindow.startTime =
  earliest timestamp of a valid temperature sample contributing to any returned band aggregation

observedWindow.endTime =
  latest timestamp of a valid temperature sample contributing to any returned band aggregation
```

A valid temperature sample is:

```text
record timestamp is at or after temperatureMigrationCutoverTime
radio.wifi_temp is present, non-null, and within range [-40, 125]
radio.wifi_temp = 0 is excluded only when the persisted/resolved temperature contract has wifiTempZeroIsUnavailable = true
```

If all samples for a band are invalid or missing, return `null` for that band's min, max, average, and latest fields.

Do not query `radio_timepoints` unless a separate normalized table and migration are introduced.

### Handler Logic

```cpp
if (radio.band == 2) {
    response.min_wifi_temp_2_4G = summary.min;
    response.max_wifi_temp_2_4G = summary.max;
    response.avg_wifi_temp_2_4G = summary.avg;
    response.latest_wifi_temp_2_4G = summary.latest;
}

if (radio.band == 5) {
    response.min_wifi_temp_5G = summary.min;
    response.max_wifi_temp_5G = summary.max;
    response.avg_wifi_temp_5G = summary.avg;
    response.latest_wifi_temp_5G = summary.latest;
}
```

### Missing Band Example

```json
{
  "requestedWindow": {
    "startTime": "2026-07-26T12:00:00Z",
    "endTime": "2026-07-27T12:00:00Z"
  },
  "observedWindow": {
    "startTime": "2026-07-26T12:04:00Z",
    "endTime": "2026-07-27T11:55:00Z"
  },
  "min_wifi_temp_2.4G": 62,
  "max_wifi_temp_2.4G": 70,
  "avg_wifi_temp_2.4G": 66.64,
  "latest_wifi_temp_2.4G": 68,
  "min_wifi_temp_5G": null,
  "max_wifi_temp_5G": null,
  "avg_wifi_temp_5G": null,
  "latest_wifi_temp_5G": null
}
```

---

# 3. `get_device_bandwidth_consumption`

## MCP Tool Signature

```text
get_device_bandwidth_consumption(
    router_id: str,
    timestamp_till: str,
    lookback_hours: int
)
```

## Recommended API

```http
GET /api/v1/devices/{routerId}/wifi-clients/usage-summary
    ?timestampTill=2026-07-27T12:00:00Z
    &lookbackHours=24
```

## Example Request

```http
GET /api/v1/devices/60cf84f22290/wifi-clients/usage-summary?timestampTill=2026-07-27T12:00:00Z&lookbackHours=24
Authorization: Bearer <token>
```

## Request Body

```text
None
```

## Response

```json
{
  "requestedWindow": {
    "startTime": "2026-07-26T12:00:00Z",
    "endTime": "2026-07-27T12:00:00Z"
  },
  "observedWindow": {
    "startTime": "2026-07-26T11:55:00Z",
    "endTime": "2026-07-27T12:05:00Z"
  },
  "items": [
    {
      "mac": "e2:51:95:ed:0f:28",
      "rx_bytes": 106487500,
      "tx_bytes": 3851250,
      "total_bytes": 110338750,
      "data_consume_rx": "106.49 MB",
      "data_consume_tx": "3.85 MB",
      "total_data_usage": "110.34 MB"
    },
    {
      "mac": "28:39:26:a1:7c:a5",
      "rx_bytes": 30071250,
      "tx_bytes": 16486250,
      "total_bytes": 46557500,
      "data_consume_rx": "30.07 MB",
      "data_consume_tx": "16.49 MB",
      "total_data_usage": "46.56 MB"
    },
    {
      "mac": "54:6c:0e:44:11:09",
      "rx_bytes": 4200000,
      "tx_bytes": 600000,
      "total_bytes": 4800000,
      "data_consume_rx": "4.20 MB",
      "data_consume_tx": "0.60 MB",
      "total_data_usage": "4.80 MB"
    }
  ],
  "totalClients": 3,
  "truncated": false
}
```

`observedWindow` is aggregate response metadata:

```text
startTime =
  earliest actual persisted sample timestamp used by any returned client
  calculation

endTime =
  latest actual persisted sample timestamp used by any returned client
  calculation
```

It describes only the overall result envelope. It does not mean every client or
internal calculation stream used the entire envelope.

When no clients are returned:

```json
{
  "requestedWindow": {
    "startTime": "2026-07-26T12:00:00Z",
    "endTime": "2026-07-27T12:00:00Z"
  },
  "observedWindow": {
    "startTime": null,
    "endTime": null
  },
  "items": [],
  "totalClients": 0,
  "truncated": false
}
```

When a client is observed but no usage interval can be calculated, such as when
only one cumulative-counter sample exists in or bounding the requested range,
return zero usage for that client without a quality classification:

```json
{
  "requestedWindow": {
    "startTime": "2026-08-05T12:00:00Z",
    "endTime": "2026-08-05T13:00:00Z"
  },
  "observedWindow": {
    "startTime": null,
    "endTime": null
  },
  "items": [
    {
      "mac": "e2:51:95:ed:0f:28",
      "rx_bytes": 0,
      "tx_bytes": 0,
      "total_bytes": 0,
      "data_consume_rx": "0.00 MB",
      "data_consume_tx": "0.00 MB",
      "total_data_usage": "0.00 MB"
    }
  ],
  "totalClients": 1,
  "truncated": false
}
```

## API Logic

For gateway-scoped results, use `timepoints.ssid_data[].associations[]` because `wificlienthistory` currently does not store the gateway `serialNumber`.

`timepoints.ssid_data[].associations[]` already stores per-client:

```text
station MAC
RX bytes
TX bytes
packet counters
connected duration
BSSID
SSID
radio information
```

Alternative implementation:

```text
If wificlienthistory is preferred for performance, first add the gateway `serialNumber` to:
  AnalyticsObjects::WifiClientHistory
  WifiClientHistoryDBRecordType
  storage_wificlients.cpp fields and converters
  APStats.cpp ingestion
  DB upgrade migration
```

The values are cumulative counters, so they must not be summed directly.

### Correct Calculation

```text
For an uninterrupted stream:
  delta = ending metric value - starting metric value

For confirmed rollover:
  apply known-width rollover arithmetic

For confirmed independent session split/reset:
  calculate each proven session segment separately
  sum segment differentials

For ambiguous reset/session split:
  calculate only safely observed nonnegative segment deltas

stream_key = association/session id when available, otherwise:
  station MAC
  BSSID
  SSID
  band/radio when present

requested_start = startTime
requested_end = endTime

effective boundaries:
  effective_start = max(requested_start, proven_session_start)
  effective_end = min(requested_end, proven_session_end)

start_sample:
  sample at effective_start, if available
  otherwise latest available sample at or before effective_start

end_sample:
  sample at effective_end, if available
  otherwise earliest available sample at or after effective_end

start_sample_time = start_sample.timestamp
end_sample_time = end_sample.timestamp

uninterrupted segment differential:
  counterDelta(end_sample.rx_bytes, start_sample.rx_bytes)
  counterDelta(end_sample.tx_bytes, start_sample.tx_bytes)

samples between start_sample and end_sample:
  use to detect counter resets, confirmed rollovers, duplicate or
  out-of-order telemetry, missing-sample gaps, and session boundaries
  do not sum raw cumulative values as usage
  do sum proven segment deltas when resets or session splits are confirmed

rx_bytes    = SUM(stream rx_bytes segment differentials)
tx_bytes    = SUM(stream tx_bytes segment differentials)
total_bytes = rx_bytes + tx_bytes

data_consume_rx  = format_bytes(rx_bytes)
data_consume_tx  = format_bytes(tx_bytes)
total_data_usage = format_bytes(total_bytes)
```

Calculate counter deltas independently for RX and TX per `stream_key` and
internal calculable segment. Streams, sessions, counter reset detection,
boundary samples, and segments are internal implementation mechanics only. After
internal segment deltas are calculated, aggregate the resulting RX/TX deltas by
station MAC for the public response. Client `rx_bytes` and `tx_bytes` must equal
the sum of internal segment RX/TX deltas; each internal segment's `total_bytes`
must equal its `rx_bytes + tx_bytes`.

Client MAC values in API responses must be normalized canonical lowercase colon-separated MAC addresses matching `^[0-9a-f]{2}(:[0-9a-f]{2}){5}$`.

Usage calculation contract:

```text
Use only valid persisted samples to calculate cumulative-counter deltas.
Outside-window samples may be used as boundary samples when needed to calculate
the first or last differential segment for a client observed in the requested
range. The response must include requested timestamps and aggregate actual
sample timestamps through `observedWindow`.

If a stream lacks a usable bounding sample pair or has an ambiguous counter
decrease/session change, skip the ambiguous interval rather than inventing
traffic. Sum only safely calculable nonnegative consecutive segment deltas. If
no differential segment can be calculated for an observed client, return that
client with zero byte totals.

Include a client in `items[]` and count it in `totalClients` only when it has at
least one in-window observation (`[startTime, endTime)`) or proven session
overlap during the requested interval. Outside-window boundary samples are used
only as calculation aids for clients with proven requested-window presence or
session overlap. A station MAC with only outside-window samples and no in-window
observation or session overlap during `[startTime, endTime)` must be excluded
from `items[]` and `totalClients`.
```

Differential data-handling rules:

| Condition | Handling |
|---|---|
| Usable start and end counter samples exist, no reset/session ambiguity | calculate the differential |
| Boundary samples immediately outside the requested range are needed | use them as calculation inputs and expose their timestamps through `observedWindow` |
| Session starts inside the requested window with a usable session baseline and later sample | calculate deltas from the usable session samples |
| Session ends inside the requested window with usable samples | calculate deltas through the last usable session sample |
| Start sample missing | sum safely calculable nonnegative consecutive segment deltas from available samples |
| End sample missing | sum safely calculable nonnegative consecutive segment deltas through the latest usable sample |
| Both boundary samples missing | sum safely calculable consecutive segment deltas inside the requested range |
| Only one in-window sample exists | return zero bytes for that client |
| Client appears during the window without reliable session-start proof | sum safely calculable post-appearance segment deltas only |
| Client disappears before the end boundary | sum safely calculable segment deltas through the last usable sample |
| Client reconnects and counters reset | split only when session identity proves independent streams; otherwise skip the ambiguous interval |
| Multiple sessions for the same MAC | calculate per proven session stream, then aggregate by MAC |
| Ambiguous counter reset, stale sample, duplicate conflict, or out-of-order telemetry | skip the ambiguous interval |

When `current < previous` and the counter width is known for that source, and
the decrease is not explained by a proven session reset/reconnect or ambiguous
stream change, assume at most one counter rollover occurred between the two
samples and apply single-rollover arithmetic:

```text
delta = (counterMax - previous) + current + 1
```

If the counter width is unknown, multiple wraps are plausible, or the decrease
could be a reset/reconnect/stale sample, treat the interval as ambiguous and
skip that interval.

### Reset-Safe Delta Logic

```cpp
struct CounterDeltaResult {
    uint64_t delta;
    bool usable;
};

CounterDeltaResult counterDelta(uint64_t current,
                                uint64_t previous,
                                bool counterWidthKnown,
                                bool decreaseAmbiguous,
                                uint64_t counterMax) {
    if (current >= previous) {
        return {current - previous, true};
    }

    if (counterWidthKnown && !decreaseAmbiguous) {
        return {(counterMax - previous) + current + 1, true};
    }

    return {0, false};
}
```

Session starts are handled by segment construction, not by blindly adding the
current counter on every decrease. When reliable session evidence proves a new
session started inside the requested window, use that session's first valid
counter sample as the segment baseline and sum only subsequent proven segment
deltas. If there is no subsequent usable sample, that segment contributes 0.

Do not treat every lower counter as a reset where `current` should be added. A lower value can mean a new association session, movement between radios/BSSIDs, stale or out-of-order telemetry, duplicate station records inside one timepoint, or counter-width rollover. Adding `current` on every decrease can overcount traffic by attributing unknown pre-window traffic to the requested window.

Required sample handling:

```text
Build a deterministic sample key from the stable fields available in the association:
  station MAC
  timestamp
  BSSID, SSID, band/radio when present

Sort samples by:
  station MAC
  timestamp ASC
  stream identity fields for grouping only

Deduplicate exact duplicate samples before calculating deltas.

For the same calculated stream and same timestamp:
  identical counters are duplicates and collapse to one sample
  different counters are ambiguous and must be excluded from delta calculation
  unless a reliable source sequence number proves their temporal order

Different stream keys are calculated independently. BSSID, SSID, band, and radio
identify counter streams; they must not be used to invent temporal order between
different counter values at the same timestamp.

Deterministically sort all stream samples by timestamp ASC before calculating deltas. Valid out-of-order historical telemetry must be sorted and included in differential calculation; valid samples must not be discarded or treated as stale merely because they arrived out of temporal sequence. Discarding is strictly reserved for exact duplicate samples or ambiguous conflicting counter samples at the same timestamp without sequence proof.

When an association/session identifier is available:
  calculate deltas only within the same session.

When no session identifier is available:
  use station MAC as the result grouping key,
  but use BSSID/SSID/band/radio changes as stream boundaries when possible.

Independent Directional Counter Rules (RX vs TX):
1. RX and TX counters are calculated independently per stream.
2. If RX is present and valid but TX is missing, non-numeric, negative (< 0), or uncalculable:
   - RX bytes are calculated normally and included in data_consume_rx.
   - TX bytes return 0 as a required non-null integer and formatted "0.00 MB" in summary strings.
   - Total segment bytes total_bytes = rx_bytes + 0 = rx_bytes.
3. If TX is present and valid but RX is missing/invalid:
   - TX bytes are calculated normally and included in data_consume_tx.
   - RX bytes return 0 as a required non-null integer and formatted "0.00 MB" in summary strings.
   - Total segment bytes total_bytes = 0 + tx_bytes = tx_bytes.
4. If a boundary sample has RX but not TX (or vice versa), the missing direction cannot form a calculable boundary delta; that direction returns 0 bytes.
5. A client MAC is included in totalClients and items[] if it has proven in-window presence or session overlap, regardless of whether one counter direction was missing or uncalculable.

If current < previous:
  if a new association/session is confirmed:
    if the new session start time is within [startTime, endTime):
      close the previous segment at the last pre-reset sample
      start a new segment with current as its baseline
      add only subsequent proven nonnegative deltas for the new segment
    else:
      treat current as the new baseline and add delta 0
  else if counter width is known and the decrease is not ambiguous:
    assume at most one rollover occurred and apply single-rollover math
  else:
    treat current as the new baseline and add delta 0
```

For usage differential calculation, choose the start and end samples first, then
calculate the boundary differential for each segment. Effective boundaries are
clipped to proven session lifetime and the requested window. The default
response exposes the requested window, observed window, and calculated values.
Do not add a cumulative counter directly unless it is known to represent traffic
that started inside the segment's effective interval.

A new association/session is confirmed only when the source provides reliable session identity or timing evidence. For example, a session id change, association id change, or connected-duration reset may prove a new session if it also proves the session start time is within the requested window. BSSID, SSID, band, or radio changes define separate calculation streams, but they do not by themselves prove that the new cumulative counter started inside the requested window.

Example:

```text
10:00 rx_bytes = 1000
10:05 rx_bytes = 1500
10:10 rx_bytes = 200

Delta 10:00 -> 10:05 = 500

If 10:10 is a confirmed new session that started inside the request window WITH authoritative proof of starting at counter zero (e.g. session_start event with baseline 0 at session origin):
  add 200 (delta from zero baseline)
  total rx_bytes = 700

If 10:10 is a confirmed new session that started inside the request window WITHOUT authoritative proof of starting at zero:
  treat 200 as the new segment baseline (delta = 0 for 10:10 sample alone)
  total rx_bytes = 500

If 10:10 is an ambiguous decrease:
  add 0 (treat 200 as baseline)
  total rx_bytes = 500

If previous = 9900, current = 100, counterMax = 9999, and rollover is confirmed:
  delta = (9999 - 9900) + 100 + 1
  delta = 200
```

### Incorrect Calculation

```sql
SUM(rx_bytes)
SUM(tx_bytes)
```

This would overcount cumulative values.

### Grouping

```text
GROUP BY station_id
```

A client moving between BSSIDs should remain one client in the final response unless a per-BSSID result is explicitly required. However, BSSID/SSID/band/radio changes must define separate calculation streams for counter deltas unless a reliable association/session identifier says they are the same counter stream.

### Unit Conversion

The API returns raw byte counters plus display-only strings such as:

```text
106.49 MB
3.85 MB
```

Recommended conversion:

```cpp
double rxMB = static_cast<double>(rxBytes) / 1000000.0;
double txMB = static_cast<double>(txBytes) / 1000000.0;
```

`MB` means decimal megabytes. If the implementation divides bytes by `1024 * 1024`, label the result as `MiB` instead.

Formatting:

```cpp
fmt::format("{:.2f} MB", value);
```

This uses standard floating point formatting semantics (formatting two decimal places with the `" MB"` suffix) rather than tying calculations to a custom round-half-up implementation.

### Query Flow

```text
Resolve boardId from routerId
    ↓
Load TimePointDB records for resolvedBoardId and stored serialNumber == request routerId
    ↓
Query both boundary candidates for each calculated stream:
  - In-window samples: timestamp >= startTime and timestamp < endTime
  - Start boundary: latest sample at or before effective_start (timestamp <= effective_start)
  - End boundary: earliest sample at or after effective_end (timestamp >= effective_end)
(Outside-window samples serve strictly as boundary calculation aids for streams with in-window observations or proven session overlap; they do NOT make a client eligible for response items)
    ↓
Parse ssid_data associations
    ↓
Build stream_key values and sort samples by stream_key and timestamp ASC
    ↓
Calculate reset-safe RX/TX deltas using effective start and end boundary samples
    ↓
Aggregate stream-level deltas by station MAC (including only clients with in-window observations or proven session overlap), sort clients deterministically by raw total_bytes DESC then normalized mac ASC, and apply the 500-client limit (truncated = totalClients > 500)
    ↓
Convert bytes to decimal megabytes
    ↓
Return wrapped response with requestedWindow, observedWindow, items,
totalClients, and truncated
```

---

# 4. `get_device_rssi_quality`

## MCP Tool Signature

```text
get_device_rssi_quality(
    router_id: str,
    timestamp_till: str,
    lookback_hours: int
)
```

## Recommended API

```http
GET /api/v1/devices/{routerId}/wifi-clients/rssi-summary
    ?timestampTill=2026-07-27T12:00:00Z
    &lookbackHours=24
```

## Example Request

```http
GET /api/v1/devices/60cf84f22290/wifi-clients/rssi-summary?timestampTill=2026-07-27T12:00:00Z&lookbackHours=24
Authorization: Bearer <token>
```

## Request Body

```text
None
```

## Response

```json
{
  "requestedWindow": {
    "startTime": "2026-07-26T12:00:00Z",
    "endTime": "2026-07-27T12:00:00Z"
  },
  "observedWindow": {
    "startTime": "2026-07-26T12:01:00Z",
    "endTime": "2026-07-27T11:58:00Z"
  },
  "items": [
    {
      "mac": "e2:51:95:ed:0f:28",
      "rssi_excellent_pct": 41.67,
      "rssi_good_pct": 50.0,
      "rssi_fair_pct": 1.67,
      "rssi_poor_pct": 6.67,
      "rssi_total_samples": 60
    },
    {
      "mac": "28:39:26:a1:7c:a5",
      "rssi_excellent_pct": 92.73,
      "rssi_good_pct": 7.27,
      "rssi_fair_pct": 0.0,
      "rssi_poor_pct": 0.0,
      "rssi_total_samples": 110
    }
  ],
  "totalClients": 2,
  "truncated": false
}
```

`observedWindow` for RSSI is scoped to returned `items[]` after applying the
500-client response limit:

```text
startTime =
  earliest valid RSSI sample contributing to items[]

endTime =
  latest valid RSSI sample contributing to items[]
```

For empty RSSI results:

```json
{
  "requestedWindow": {
    "startTime": "2026-07-26T12:00:00Z",
    "endTime": "2026-07-27T12:00:00Z"
  },
  "observedWindow": {
    "startTime": null,
    "endTime": null
  },
  "items": [],
  "totalClients": 0,
  "truncated": false
}
```

## API Logic

For gateway-scoped results, read RSSI from `timepoints.ssid_data[].associations[]`. `wificlienthistory` also stores RSSI, but it cannot be filtered by gateway `serialNumber` unless that model is extended as described in the bandwidth section.

### RSSI Thresholds

```text
Excellent: RSSI >= -55
Good:      -67 <= RSSI < -55
Fair:      -75 <= RSSI < -67
Poor:      RSSI < -75
```

### Invalid Samples

Ignore:

```text
RSSI = 0
RSSI > 0
RSSI < -127
NULL
```

### Per-Client Calculation

```text
excellent_count = COUNT(rssi >= -55)

good_count =
    COUNT(rssi >= -67 AND rssi < -55)

fair_count =
    COUNT(rssi >= -75 AND rssi < -67)

poor_count =
    COUNT(rssi < -75)

total_samples =
    excellent_count +
    good_count +
    fair_count +
    poor_count
```

Percentages:

```text
excellent_pct = excellent_count × 100 / total_samples
good_pct      = good_count × 100 / total_samples
fair_pct      = fair_count × 100 / total_samples
poor_pct      = poor_count × 100 / total_samples
```

Round percentages to two decimal places.

The four percentage fields are constrained to `0 <= value <= 100`. Because each percentage is independently rounded to two decimal places, the four values may sum to slightly below or above exactly `100`.

### Current Storage Aggregation Logic

```text
Load TimePointDB records where:
  boardId == resolvedBoardId
  stored serialNumber == request routerId
  timestamp >= startTime
  timestamp < endTime

For each record:
  parse ssid_data
  for each association:
    station_id = normalized association.station MAC
    rssi = association.rssi
    ignore invalid RSSI samples
    increment the station_id bucket for excellent/good/fair/poor

For each station_id:
  total_samples = excellent + good + fair + poor
  calculate percentages from total_samples
```

Do not query `wifi_client_history.board_id`, `wifi_client_history.serialNumber`, or `wifi_client_history.created`; those fields do not exist in the current schema.

The API should return the final percentages directly. The MCP server should not need to perform additional RSSI summarisation.

---

# 5. `get_gateway_offline_count`

## MCP Tool Signature

```text
get_gateway_offline_count(
    router_id: str,
    timestamp_till: str,
    lookback_hours: int
)
```

## Recommended API

```http
GET /api/v1/devices/{routerId}/availability-summary
    ?timestampTill=2026-07-27T12:00:00Z
    &lookbackHours=24
```

## Example Request

```http
GET /api/v1/devices/60cf84f22290/availability-summary?timestampTill=2026-07-27T12:00:00Z&lookbackHours=24
Authorization: Bearer <token>
```

## Request Body

```text
None
```

## Response

```json
{
  "meta": {
    "requestedWindow": {
      "startTime": "2026-07-26T12:00:00Z",
      "endTime": "2026-07-27T12:00:00Z"
    },
    "observedWindow": {
      "startTime": "2026-07-26T13:15:00Z",
      "endTime": "2026-07-27T09:45:00Z"
    },
    "offlineEventCount": 6
  },
  "data": {
    "gw_uuid": "60cf84f22290",
    "fetch_status": "success",
    "offline_count": 6
  }
}
```

For availability responses, `observedWindow` is the actual event-time range
represented by the availability data used to produce the response. It is derived
from offline transition events that contributed to `offline_count`; when no
offline transition events contribute, both `observedWindow.startTime` and
`observedWindow.endTime` are `null`. The response does not report Kafka consumer
progress or lag as availability-domain data.

The `availability-summary` endpoint is an observed-data API. It reports the
availability transition rows currently persisted in Analytics storage at request
time. It does not prove what Analytics has not yet received.

## API Logic

Availability ingestion uses gateway events from the existing `connection` topic.
The REST API query path reads only Analytics storage.

Analytics currently handles gateway-level events:

```text
ping
disconnection
capabilities
```

The current code updates:

```text
connected
lastConnection
lastDisconnection
lastPing
lastContact
```

To calculate historical offline counts, transitions must be persisted.

### Storage Table

```text
device_availability_events
--------------------------
id
board_id
serialNumber
event_type
event_time
reason
connection_ip
session_id
event_id
idempotency_key
```

### Required Storage Implementation

Add dedicated storage classes for availability events and restart-safe availability state:

```text
src/storage/storage_device_availability_events.h
src/storage/storage_device_availability_events.cpp
src/storage/storage_device_availability_state.h
src/storage/storage_device_availability_state.cpp
```

Required ORM fields and indexes:

```text
Fields:
  id              TEXT primary id
  board_id        TEXT NULL
  serialNumber    TEXT
  event_type      TEXT
  event_time      BIGINT
  reason          TEXT
  connection_ip   TEXT
  session_id      TEXT
  event_id        TEXT
  idempotency_key TEXT NOT NULL

Indexes:
  availability_serial_time_index:
    serialNumber ASC
    event_time ASC

  availability_board_serial_time_index:
    board_id ASC
    serialNumber ASC
    event_time ASC

Unique constraints:
  availability_idempotency_key_unique:
    idempotency_key ASC

Optional indexes:
  availability_event_id_index:
    event_id ASC
```

SQL ordering semantics:

```text
When querying device_availability_events, order by event_time only.
```

`event_id` stores the normalized source event id when the payload provides one, using `payload.ping.uuid`, `payload.uuid`, or `payload.disconnection.uuid` only when that `uuid` is stable for the same logical source event.

`serialNumber` is the durable identity for availability history. `board_id` is event-time context only and must be nullable because connection events can arrive when the router is not currently assigned to an Analytics board, when OWPROV is unavailable, or after ownership has changed. Do not drop availability events only because current board ownership cannot be resolved.

Add a persisted current-state table for restart-safe transition detection:

```text
device_availability_state
-------------------------
serialNumber          TEXT PRIMARY KEY
board_id              TEXT NULL
current_state         TEXT
last_event_time       BIGINT
updated_at            BIGINT
```

Required fields and constraints:

```text
Fields:
  serialNumber         TEXT primary id
  board_id             TEXT NULL
  current_state        TEXT
  last_event_time      BIGINT
  updated_at           BIGINT

Indexes:
  availability_state_board_index:
    board_id ASC

Constraints:
  current_state IN ('online', 'offline', 'unknown')
```

`device_availability_events` stores only online/offline transition history.
Historical availability queries read this table and count persisted offline
transition rows.

`device_availability_state` exists only to persist:

```text
- the latest accepted availability state
- the latest accepted source event timestamp
- optional current board context
- the state row update timestamp
```

It prevents repeated ping or disconnection messages from generating duplicate
transitions. The REST handler must not derive historical availability counts from
`device_availability_state`.

`last_event_time` is the persisted source ordering key after events have been
consumed in Kafka partition order for the gateway. It must be populated from the
normalized Kafka source timestamp (`event_time`). `DeviceInfo.lastContact` may
remain local contact or processing metadata, but it must not be used to order
source events.

Availability ingestion responsibilities are separated as follows:

```text
device_availability_state.last_event_time
  -> replay/non-advancing detection at state level

device_availability_events.idempotency_key UNIQUE
  -> storage-level duplicate transition protection

Kafka committed offset
  -> ingestion progress and replay position
```

Do not add Analytics-owned ingestion progress or durable data-loss tables for availability.

Kafka consumer offsets are the source of truth for ingestion progress, replay after service restart, redelivery of uncommitted messages, consumer lag, and consumer-group rebalances. Analytics-owned availability state is limited to:

```text
device_availability_events
device_availability_state
```

Kafka topic, partition, offset, message key, consumer timestamp, message type,
connection IP, disconnect reason, producer information, and other diagnostic
details must not be stored in `device_availability_state`. Keep operational and
debugging information in logs, metrics, traces, or other existing observability
mechanisms. If diagnostic fields are required for a historical transition, model
them explicitly in `device_availability_events` where already defined; do not
add a generic metadata column solely for diagnostics.

Expected Kafka and database processing contract:

```text
Kafka message received
normalize and validate event
BEGIN database transaction
lock/load device_availability_state for serialNumber
validate event_time
insert device_availability_events only on a real state transition
update device_availability_state
COMMIT database transaction
commit Kafka offset
```

This is an at-least-once processing contract. The Kafka offset must not be committed before the database transaction succeeds. If the service crashes before the database commit, Kafka replays the message after restart. If the service crashes after the database commit but before the Kafka offset commit, Kafka may redeliver the message; the `event_time` check must make replay non-advancing, and the unique `device_availability_events.idempotency_key` constraint remains a safety net for duplicate transition inserts. Kafka auto-commit must not acknowledge messages ahead of successful database commits.

Availability transition processing must preserve Kafka partition order. The
default implementation should process messages from one Kafka partition
sequentially for availability state transitions. If batching or concurrent
processing is introduced later, offset commits must remain contiguous per
partition: commit only the highest offset for which that record and every earlier
uncommitted record in the same partition have successfully completed the
database transaction. Never commit past an earlier failed, skipped, or
unprocessed record in the same partition.

Unsafe concurrent commit example:

```text
partition P:
offset 100 -> DB success
offset 101 -> DB fails
offset 102 -> DB success

Do not commit offset 102, because that would skip offset 101 after restart.
The highest committable contiguous offset is 100.
```

Restart and rebalance recovery must use the stable Kafka consumer group:

```text
1. consumer joins the same Kafka consumer group
2. Kafka resumes from the last committed offset for assigned partitions
3. messages after that offset are replayed
4. Analytics loads device_availability_state for each serialNumber as messages arrive
5. event_time checks prevent duplicate transition rows; event-table idempotency remains a storage safety net
6. processing continues and offsets are committed only after DB commit
```

Concrete restart scenario:

```text
offset 100 -> ping -> DB committed, Kafka offset committed
offset 101 -> disconnection -> DB committed, Kafka offset committed
offset 102 -> ping -> DB committed, service crashes before Kafka offset commit

After restart:
consumer rejoins the same consumer group
Kafka resumes from the last committed position
offset 102 may be replayed
device_availability_state already reflects offset 102
event_time == last_event_time makes the replay a duplicate/non-advancing no-op
device_availability_events idempotency remains a storage safety net if a duplicate transition insert is attempted
offset 102 is committed after the replay transaction succeeds or no-ops safely
consumer continues with offset 103
```

Consumer-group rebalance and partition reassignment follow the same rule: Kafka
assigns partitions to a consumer in the stable group, resumes from committed
offsets, and redelivers any messages whose offsets were not committed.

Normal Kafka lag is not an Analytics data gap. If the producer is at offset 5000 and the consumer is at offset 4500, the service should keep consuming until it catches up. Operational monitoring should expose Kafka consumer lag through Kafka or observability metrics, not through availability-domain tables or API fields.

Exceptional Kafka data-loss conditions are operational incidents, not normal availability-domain state. Examples include Kafka retention expiring before the consumer caught up, a topic being deleted or recreated, or offsets being manually reset past unprocessed data. Log, alert, and metric these conditions. Do not introduce Analytics-owned durable data-loss records unless a separate product requirement explicitly calls for historical completeness auditing.

Kafka operational monitoring may expose consumer lag, offset positions, replay
activity, retention incidents, and consumer-group health through Kafka metrics,
Prometheus metrics, service dashboards, logs, alerts, or consumer-group
monitoring. Those signals are separate from the availability-domain API
contract.

Final availability ingestion architecture:

```text
                 Kafka connection topic
                         |
                key = serialNumber
                         |
                         v
                 Kafka consumer group
                         |
                partition ordering
                         |
                         v
             normalize connection event
                         |
                         v
              DB transaction / serial lock
                   /              \
                  v                v
device_availability_state   device_availability_events
       current state          transition history
                  \                /
                   \              /
                    DB COMMIT
                         |
                         v
                commit Kafka offset
```

Final responsibility split:

```text
Kafka:
  reliable delivery/replay according to Kafka's normal consumer-group semantics
  restart handling
  offset management
  lag monitoring

Analytics ingestion:
  restart-safe current availability state
  latest accepted event_time
  transition detection
  idempotent transition persistence

Analytics API:
  query currently persisted transition history
  count persisted offline events
  report observedWindow for those persisted events
```

The key API principle is:

```text
The availability API reports what Analytics has observed and persisted.
It does not prove what Analytics has not yet received.
```

Explicitly out of scope for `availability-summary`:

```text
strong read-after-Kafka-ingestion consistency guarantees
source-to-API delivery SLA guarantees
request-time Kafka consumer position validation
historical source-event delivery proof
server-side availability data-quality classification
durable ingestion data-loss persistence
durable ingestion checkpoint persistence
waiting for Kafka catch-up before answering
high availability across Kafka/DB failure scenarios
```

Add a `DeviceAvailabilityEvent` REST/storage object with `to_json` and `from_json` support in:

```text
src/RESTObjects/RESTAPI_AnalyticsObjects.h
src/RESTObjects/RESTAPI_AnalyticsObjects.cpp
```

Update `StorageService`:

```text
src/StorageService.h
  include storage/storage_device_availability_events.h
  include storage/storage_device_availability_state.h
  add DeviceAvailabilityEventsDB accessor
  add DeviceAvailabilityStateDB accessor
  add std::unique_ptr<DeviceAvailabilityEventsDB>
  add std::unique_ptr<DeviceAvailabilityStateDB>

src/StorageService.cpp
  construct DeviceAvailabilityEventsDB
  construct DeviceAvailabilityStateDB
  call DeviceAvailabilityEventsDB->Create()
  call DeviceAvailabilityStateDB->Create()
  include availability retention cleanup if retention should match board timepoint cleanup
```

Add a DB upgrade/migration path for the new tables. Existing deployments will
start with no historical availability events. Initialize a fixed
`availabilityValidFrom` timestamp when availability-event persistence starts and
persist it as `system_properties.availability_valid_from`. The API should return
`offline_count: 0` for successful empty queries whose requested range starts at
or after the persisted `availabilityValidFrom`; do not infer old events from
`lastDisconnection`.

`availabilityValidFrom` represents the timestamp from which this Analytics
database can reliably claim that availability transition persistence was active.
Once initialized, the persisted `system_properties` value is authoritative.
Changing an environment variable or config file must not silently change the
historical validity boundary.

Add all availability storage files to `CMakeLists.txt`, including `storage_device_availability_events.*` and `storage_device_availability_state.*`. Register database migration/upgrade logic for these availability tables so fresh databases and upgraded deployments create the event table, state table, indexes, uniqueness constraints, and state constraints consistently.

### Ingestion Hook

Persist availability events from connection handling:

```text
src/APStats.cpp
  AP::UpdateConnection(...)
```

Ingestion-time board source:

```text
1. Read board ownership from VenueCoordinator's maintained routerId -> boardId map.
2. If a current mapping exists:
     set board_id to the mapped board id.
3. If no current mapping exists:
     set board_id to NULL.
     still persist the availability state/event by serialNumber when the message is otherwise valid.
4. Do not perform a full OWPROV inventory lookup or board scan for every connection event.
5. A background refresh or later request-time resolution may update current ownership, but it must not rewrite historical event ownership blindly.
```

The ingestion path must not depend on HTTP request-time `ResolveRouterIdContext`. That resolver can use OWPROV to verify or refresh ownership for API requests, but Kafka connection ingestion should use the maintained `VenueCoordinator` ownership map and tolerate missing current board ownership.

For `device_availability_state`, `board_id` is current ownership/context only.
For every accepted source-newer event and every bootstrap state row:

```text
if VenueCoordinator has a current mapping:
  state.board_id = mapped boardId
else:
  state.board_id = NULL
```

Do not keep a previous state-table `board_id` when the current map has no
mapping. `device_availability_events.board_id` remains historical event-time
context.

Rules:

```text
On disconnection:
  store one offline event when the device transitions from connected to disconnected.

On ping/capabilities:
  store one online event only when the device transitions from disconnected to connected.

Do not store online events for every ping.
```

Bootstrap rule when no `device_availability_state` row exists:

```text
First observed disconnection:
  insert state row with current_state = offline
  set last_event_time = event_time
  set board_id from the current VenueCoordinator mapping, or NULL when absent
  set updated_at
  do not insert an offline transition event because the prior state is unknown

First observed ping/capabilities:
  insert state row with current_state = online
  set last_event_time = event_time
  set board_id from the current VenueCoordinator mapping, or NULL when absent
  set updated_at
  do not insert an online transition event

First observed unrecognized connection message:
  insert or update state row with current_state = unknown only when useful
  set last_event_time only from a valid source timestamp
  set board_id from the current VenueCoordinator mapping, or NULL when absent
  set updated_at
  do not insert a counted event
```

In-memory transition checking is only an optimization. It is not sufficient for correctness because Kafka can redeliver messages and the service can restart after losing in-memory state. Transition detection must use `device_availability_state.current_state` and `device_availability_state.last_event_time` under the same transaction before writing `device_availability_events`. Duplicate connection messages must be rejected by `event_time` validation before they can affect `offline_count`; the event-table idempotency constraint remains a storage safety net for duplicate transition inserts.

Normalize connection messages before transition processing:

```text
If payload.ping exists:
  message_type = ping
  event_type = online
  serialNumber = payload.ping.serialNumber
  event_time = payload.ping.timestamp
  source_event_uuid = payload.ping.uuid
  connection_ip = payload.ping.connectionIp

If payload.capabilities exists:
  message_type = capabilities
  event_type = online
  serialNumber = payload.serial
  event_time = payload.timestamp
  source_event_uuid = payload.uuid
  connection_ip = payload.connectionIp
  reason = payload.reason

If payload.disconnection exists:
  message_type = disconnection
  event_type = offline
  serialNumber = payload.disconnection.serialNumber
  event_time = payload.disconnection.timestamp
  source_event_uuid = payload.disconnection.uuid
```

The event writer must include `board_id` from the ingestion-time ownership map when available, normalized `serialNumber`, `event_type`, normalized `event_time`, and a stable `idempotency_key`.

Idempotency key rule:

```text
If the incoming connection message contains a stable source event id:
  source_event_id = payload.ping.uuid
    or payload.uuid
    or payload.disconnection.uuid
  idempotency_key = deterministic hash of:
    system.host
    system.id
    serialNumber
    message_type
    source_event_id

Else if the message contains a stable session id plus event timestamp:
  idempotency_key = deterministic hash of:
    serialNumber
    message_type
    event_type
    event_time
    session_id

Else if connection_ip or reason is available:
  idempotency_key = deterministic hash of the stable logical message identity:
    serialNumber
    message_type
    event_type
    event_time
    reason
    connection_ip

Else:
  idempotency_key = deterministic hash of the minimum stable logical identity:
    serialNumber
    message_type
    event_type
    event_time
```

Treat `uuid` as a source event id only if it is stable for the same logical connection message across Kafka redelivery and unique within the source namespace identified by `system.host` and `system.id`. If `uuid` is only a random per-delivery value, do not use it as `source_event_id`.

Example for exact Kafka redelivery protection, after verifying that `payload.ping.uuid` remains stable for the same logical redelivered message:

```text
hash(
  system.host +
  system.id +
  serialNumber +
  message_type +
  payload.ping.uuid
)
```

Do not use `system.id` alone as the logical event id; it identifies the producing system, not one individual ping.

Minimum fields for derived idempotency keys:

```text
All counted availability events:
  serialNumber is required
  event_type is required
  event_time is required and must satisfy the producer contract of strictly increasing source timestamps per serialNumber

Offline events derived from disconnection:
  reason or source disconnect cause is required when available
  if reason/cause and connection_ip are missing:
    prefer stable source_event_id from payload.disconnection.uuid
    otherwise derive the key from serialNumber, message_type, event_type, and event_time

Online events derived from ping/capabilities:
  session_id is required when available
  if session_id is unavailable:
    derive the key from serialNumber, message_type, event_type, and event_time
  if event_time is missing or violates the strictly increasing per-serial producer contract:
    do not move current state; do not write a counted online transition event

Unrecognized connection messages:
  do not write counted availability events
  do not use device_availability_state as a payload or diagnostics store
```

If the minimum fields for the event type are not present, the implementation must
not create a counted `device_availability_events` row and must not use
`device_availability_state` to store diagnostic details from that message.

Do not include `board_id` in the idempotency key unless it is part of a stable source event id. Board ownership can change between delivery and redelivery, and including current ownership in a derived key can turn one logical event into multiple stored events.

Do not include Kafka topic, partition, or offset in the derived logical
`idempotency_key`. Kafka coordinates identify a broker record, not the logical
connection event. They dedupe redelivery of the same Kafka record, but they do
not dedupe the same logical connection event produced as a second Kafka record
with a different offset. Keep Kafka coordinates and consumer timestamps in logs,
metrics, or traces rather than `device_availability_state`.

The same delivered logical connection event must always produce the same `idempotency_key`. If the implementation cannot build a stable or deterministic key for an event, it must not write that event as a counted availability transition; log it as an ingestion error instead.

Ordering rule:

```text
For each serialNumber:
- Kafka key = serialNumber
- events are produced in source-event order
- source event_time is strictly increasing

Kafka partition ordering is therefore the ordering guarantee for all
availability events belonging to one gateway. Analytics orders source events
using only `event_time`.

Because the producer guarantees strictly increasing source timestamps per
serialNumber and writes those events to Kafka in that same order, normal
processing sees:

incoming event_time > last_event_time

Analytics keeps only defensive checks for stale or duplicate/non-advancing
messages. If a message has `event_time < last_event_time`, ignore it as stale.
If a message has `event_time == last_event_time`, treat it as duplicate or
non-advancing, do not create a transition, and do not move state. Equal source
timestamps are not expected during normal operation.

Under the producer contract, the same `serialNumber` and `event_time` identifies
a duplicate or replay. `device_availability_state` does not store an
idempotency key. Keep the unique `idempotency_key` on
`device_availability_events` as a database safety net for persisted
transitions.

Before a source-newer event updates `device_availability_state`, resolve
`stateBoardId` from the current VenueCoordinator map:

```text
if current mapping exists:
  stateBoardId = mapped boardId
else:
  stateBoardId = NULL
```

If the source topic violates serialNumber keying, per-serial source-event order,
or strictly increasing per-serial `event_time`, treat that as a producer/topic
contract violation. Do not add Analytics PostgreSQL ingestion-progress,
data-loss, or additional durable ordering state to compensate.
```

Atomic transition and insert rule:

```text
For each valid connection message consumed in Kafka partition order:
  BEGIN

  acquire a serialNumber-scoped transaction lock before reading state
  determine incomingState from message_type:
    ping or capabilities -> online
    disconnection -> offline

  load device_availability_state row for serialNumber with write isolation
  if no state row exists:
    atomically create or lock the serialNumber state slot using one of:
      INSERT ... ON CONFLICT ... DO UPDATE/NOTHING followed by SELECT ... FOR UPDATE
      PostgreSQL advisory transaction lock
      serializable transaction with retry
    if another transaction created the state row first, re-read it and continue
    otherwise initialize current_state from incomingState
    set last_event_time = event_time
    set board_id = stateBoardId
    set updated_at
    do not insert an initial transition event
    COMMIT
    stop processing this message

  -- Event-time-only ordering check:
  if event_time < last_event_time:
    treat the message as stale
    do not insert an availability event
    do not update device_availability_state
    COMMIT
    stop processing this message

  if event_time == last_event_time:
    treat the message as duplicate or non-advancing
    do not insert an availability event
    do not update device_availability_state
    COMMIT
    stop processing this message

  -- event_time > last_event_time:
  if incomingState is online:
    if current_state is offline:
      INSERT online transition event with conflict-safe semantics
      if the insert created a row:
        update device_availability_state:
          current_state = online
          last_event_time = event_time
          updated_at = processing time
          board_id = stateBoardId
      else:
        treat as duplicate and do not update state
    else if current_state is online:
      update device_availability_state last_event_time, updated_at, and board_id = stateBoardId
      do not insert a transition event
    else:
      update device_availability_state to online, last_event_time, updated_at, and board_id = stateBoardId
      do not insert an initial online transition event

  if incomingState is offline:
    if current_state is online:
      INSERT offline transition event with conflict-safe semantics
      if the insert created a row:
        update device_availability_state:
          current_state = offline
          last_event_time = event_time
          updated_at = processing time
          board_id = stateBoardId
      else:
        treat as duplicate and do not update state
    else if current_state is offline:
      update device_availability_state last_event_time, updated_at, and board_id = stateBoardId
      do not insert a transition event
    else:
      update device_availability_state to offline, last_event_time, updated_at, and board_id = stateBoardId
      do not insert an initial offline transition event because the prior state is unknown

  COMMIT

or the equivalent database-specific "insert if absent" operation.

The storage method must return whether a row was inserted. Duplicate events that hit
the unique idempotency constraint must not be treated as new offline transitions and
must not move `device_availability_state` backward or forward.

This transaction shape assumes the connection topic is keyed by serialNumber and
the producer publishes per-serial events in source-event order with strictly
increasing source `event_time`. Do not apply this state machine to a topic that
violates that contract unless a separate architecture explicitly defines the
ordering guarantee outside Analytics PostgreSQL ingestion-progress or data-loss
tables.
```

Equivalent transaction shape:

```text
BEGIN;

Acquire a serialNumber-scoped transaction lock.
Atomically create or lock the device_availability_state row for :serialNumber.
Read:
  current_state
  last_event_time

-- Validate event_time, then determine whether state changed.
-- Insert into device_availability_events only if state changed.

Update device_availability_state only when event_time checks allow it:
  current_state = :newState
  last_event_time = :eventTime
  updated_at = :processingTime
  board_id = :stateBoardId, where stateBoardId is the current VenueCoordinator mapping or NULL

COMMIT;
```

Staleness rule after ordering:

```text
An event is stale when `event_time < last_event_time`.

An event is duplicate or non-advancing when `event_time == last_event_time`.

Stale, duplicate, and non-advancing events must not insert counted availability
events and must not update device_availability_state. The equal timestamp case
is defensive only and is not expected during normal operation.
```

### Store Only State Transitions

Correct:

```text
offline → online: store online event
online → offline: store offline event
```

Incorrect:

```text
every ping → store online event
```

Every ping should not be treated as a new online transition. However, every source-newer ping that is accepted after Kafka partition-ordered consumption must still update `device_availability_state.last_event_time`, `updated_at`, and `board_id` from the current VenueCoordinator mapping or `NULL`.

Ping handling:

```text
current_state=online  + ping -> update last_event_time, updated_at, and board_id from the current map or NULL; do not insert a transition event
current_state=offline + ping after a prior offline state -> insert online event, set current_state=online, update last_event_time, updated_at, and board_id from the current map or NULL
no state row + ping -> create current_state=online, update last_event_time, updated_at, and board_id from the current map or NULL; do not insert an online event
```

Disconnection handling:

```text
current_state=online  + disconnection -> insert offline event, set current_state=offline, update last_event_time, updated_at, and board_id from the current map or NULL
current_state=offline + disconnection -> update last_event_time, updated_at, and board_id from the current map or NULL; do not insert another offline event
no state row + disconnection -> create current_state=offline, update last_event_time, updated_at, and board_id from the current map or NULL; do not insert an offline transition event
```

Example:

```text
12:00 first ping initializes current_state=online with no transition event
12:05 disconnection source event
12:10 ping source event
```

With Kafka key = serialNumber and producer-side per-serial ordering, Kafka
preserves the gateway event order:

```text
12:05 disconnection -> insert offline event, current_state=offline, last_event_time=12:05
12:10 ping          -> insert online event, current_state=online,  last_event_time=12:10
```

The final current state is online, and the outage from `12:05` to `12:10` remains
present in transition history. If the topic is not keyed by serialNumber or the
producer publishes events for one serialNumber out of order, Kafka cannot provide
the required per-gateway transition ordering. Treat that as an operational or
producer-contract issue; do not model it with Analytics ingestion-progress or data-loss tables.

Review conclusion:

```text
The implementation requires `device_availability_state` unless a future design explicitly names an existing persistent table with the same fields and locking guarantees. `last_event_time` must be updated for every source-newer ping, capabilities, or disconnection message accepted after Kafka partition-ordered consumption, even when no availability transition event is inserted. `board_id` must be set from the current VenueCoordinator mapping or NULL on every accepted source-newer state update. `DeviceInfo.lastContact` remains contact/processing metadata and is not the source-event ordering field.
```

### Calculation

```text
offline_count =
    count(
      device_availability_events
      where serialNumber = routerId
      and event_type = 'offline'
      and event_time >= startTime
      and event_time < endTime
    )
```

`offline_count` is the number of persisted offline transition rows currently
observed by Analytics for the requested interval. It is not proof that every
source event for the interval has been ingested.

Availability migration boundary:

```text
availabilityValidFrom is a durable, deployment-wide cutover timestamp for
availability history. It represents the timestamp from which this Analytics
database can reliably claim that availability transition persistence was active.

The authoritative persisted source of truth is:
  system_properties.availability_valid_from

Configuration and environment values are first-time initialization inputs only.
They are not equivalent runtime sources once the database property exists.
Changing an environment variable or config file must not silently change the
historical validity boundary.

Initialization input precedence, used only when
system_properties.availability_valid_from does not already exist:
  ANALYTICS_AVAILABILITY_VALID_FROM
    overrides
  availability_valid_from from /etc/ucentral/owanalytics.json

On service startup:
  rawEnvValue =
      ANALYTICS_AVAILABILITY_VALID_FROM if present, else unset

  rawConfigValue =
      availability_valid_from from /etc/ucentral/owanalytics.json if present,
      else unset

  persistedValue =
      read system_properties.availability_valid_from

  if persistedValue exists:
      availabilityValidFrom = persistedValue

      for each supplied rawEnvValue and rawConfigValue:
          configuredValue = parse supplied value
          if configuredValue is invalid:
              fail startup with a clear invalid-configuration error

          if configuredValue != persistedValue:
              fail startup with a clear configuration-mismatch error:
                availability_valid_from configuration mismatch:
                configured value does not match persisted
                system_properties.availability_valid_from

  else:
      rawConfiguredValue =
          rawEnvValue if set
          else rawConfigValue if set
          else unset

      if rawConfiguredValue is unset:
          fail startup with a clear missing-configuration error

      configuredValue = parse rawConfiguredValue
      if configuredValue is invalid:
          fail startup with a clear invalid-configuration error

      insert system_properties.availability_valid_from = configuredValue
      availabilityValidFrom = configuredValue

      if another replica concurrently inserted the property first:
          re-read system_properties.availability_valid_from
          apply the persistedValue-exists mismatch checks above

The persisted value must never be silently overwritten during a normal restart
or deployment. All service replicas and process restarts MUST use the identical
persisted system_properties.availability_valid_from timestamp to guarantee
multi-replica and restart consistency. Dynamic process-start timestamps
(e.g. std::chrono::system_clock::now() at startup) are strictly prohibited.

If startTime < availabilityValidFrom:
  reject the request
  do not query device_availability_events
  do not return offline_count: 0

If startTime >= availabilityValidFrom:
  query device_availability_events normally
```

Rejecting pre-cutover ranges prevents a successful zero response from meaning either
"no outages occurred" or "Analytics was not collecting availability events yet."

### Query

```sql
SELECT COUNT(*) AS offline_count
FROM device_availability_events
WHERE serialNumber = :router_id
  AND event_type = 'offline'
  AND event_time >= :start_time
  AND event_time < :end_time;
```

Do not require `board_id = :resolvedBoardId` for the availability count. `board_id` is nullable event-time context and can differ from the router's current board after reassignment. The durable query identity for gateway availability history is `serialNumber`.

The availability REST request path must not call Kafka, compare committed
offsets to partition high watermarks, inspect consumer lag, wait for the
consumer to catch up, or validate ingestion freshness. Kafka offset state is
operational state, not API response data.

Final availability query flow:

```text
Authenticate request
validate routerId, timestampTill, and lookbackHours
resolve router ownership
validate retention and availabilityValidFrom
query device_availability_events for serialNumber and [startTime, endTime)
filter event_type = offline
offline_count = number of persisted matching rows
derive observedWindow from matching persisted offline rows
return response
```

`observedWindow` for availability is derived only from persisted offline
transition rows contributing to `offline_count`:

```text
observedWindow.startTime =
  earliest persisted offline transition event_time contributing to offline_count

observedWindow.endTime =
  latest persisted offline transition event_time contributing to offline_count
```

Do not set `observedWindow` to the requested range merely because the query
covers that range. It reflects observed persisted transition events, not
server-side data-status classification.

Example:

```text
requestedWindow = [12:00, 14:00)
persisted offline transition event_time values = [12:15, 13:20]

offline_count = 2
observedWindow = [12:15, 13:20]
```

Kafka consumer lag example:

```text
12:00 ping
12:05 offline
12:10 online
12:20 offline event exists upstream but has not yet been consumed

API request at 12:21:
  persisted offline rows = [12:05]
  offline_count = 1
  observedWindow = [12:05, 12:05]

Later, Kafka consumer processes the 12:20 event.

Same historical API request:
  persisted offline rows = [12:05, 12:20]
  offline_count = 2
  observedWindow = [12:05, 12:20]
```

Both responses are valid because the API reports the persisted observations
available at request time.

### Range Before Availability Cutover

Use an HTTP error:

```http
400 Bad Request
```

```json
{
  "error": "availability_range_before_cutover",
  "message": "Availability history is only available for ranges starting at or after availabilityValidFrom"
}
```

### Success Response with No Offline Events

```json
{
  "meta": {
    "requestedWindow": {
      "startTime": "2026-07-26T12:00:00Z",
      "endTime": "2026-07-27T12:00:00Z"
    },
    "observedWindow": {
      "startTime": null,
      "endTime": null
    },
    "offlineEventCount": 0
  },
  "data": {
    "gw_uuid": "60cf84f22290",
    "fetch_status": "success",
    "offline_count": 0
  }
}
```

A zero result means no persisted offline transition was observed in the
requested post-cutover interval based on the data currently available in
Analytics storage. It does not make claims about outages not yet observed,
unconsumed messages, or Kafka consumer position. Kafka consumer lag is
operational state and is not represented as availability-domain response
metadata.

`offlineEventCount` is the number of offline transition rows in
`device_availability_events` that contribute to `offline_count`. Online recovery
transition rows (`event_type = 'online'`) are stored in transition history for
state tracking, but they do not contribute to `observedWindow`,
`offlineEventCount`, or `offline_count`.

### Internal Error

Use an HTTP error:

```http
500 Internal Server Error
```

```json
{
  "error": "availability_query_failed",
  "message": "Unable to retrieve gateway availability history"
}
```

HTTP 200 represents a successful retrieval. Do not return a success-shaped HTTP 200 response for internal failures; return the appropriate HTTP error instead.

---

# Repository Implementation Structure

Repository:

```text
routerarchitects/ra-wlan-cloud-analytics
```

## OpenAPI Follow-up Deliverable

This PR does not make these functional OpenAPI changes. A follow-up PR must
update:

```text
openapi/owanalytics.yaml
```

That follow-up should add:

```yaml
/devices/{routerId}/memory-summary:
/devices/{routerId}/radio-temperature-summary:
/devices/{routerId}/availability-summary:
/devices/{routerId}/wifi-clients/usage-summary:
/devices/{routerId}/wifi-clients/rssi-summary:
```

That follow-up must also ensure each MCP analytics operation explicitly declares
bearer-only security so it does not inherit the existing top-level OpenAPI
`bearerAuth OR ApiKeyAuth` contract:

```yaml
security:
  - bearerAuth: []
```

---

## REST Objects

Update:

```text
src/RESTObjects/RESTAPI_AnalyticsObjects.h
src/RESTObjects/RESTAPI_AnalyticsObjects.cpp
```

Add objects:

```cpp
struct GatewayMemorySummary;
struct GatewayWifiTemperatureSummary;
struct ClientBandwidthConsumption;
struct ClientBandwidthConsumptionResponse;
struct ClientRssiQuality;
struct ClientRssiQualityResponse;
struct GatewayOfflineSummary;
```

JSON field names should match the MCP CSV where the API is directly returning MCP fields. `usage-summary` additionally returns raw byte totals alongside display-formatted usage strings.

Client MAC addresses (`mac`) must be normalized to canonical lowercase colon-separated format matching pattern `^[0-9a-f]{2}(:[0-9a-f]{2}){5}$` across all client metrics endpoints.

---

## REST Handlers

Recommended files:

```text
src/RESTAPI/RESTAPI_device_metrics_summary_handler.h
src/RESTAPI/RESTAPI_device_metrics_summary_handler.cpp

src/RESTAPI/RESTAPI_wifi_client_metrics_handler.h
src/RESTAPI/RESTAPI_wifi_client_metrics_handler.cpp
```

Device metrics handler:

```text
memory-summary
radio-temperature-summary
availability-summary
```

Wi-Fi client metrics handler:

```text
usage-summary
rssi-summary
```

Handler query parsing implementation note:
REST handlers must inspect and parse the raw HTTP query string parameter collection directly rather than relying solely on generated framework parameter binding. Handlers must verify that `lookbackHours` and `timestampTill` appear exactly once in the raw query string, reject repeated parameters, reject fractional numeric values, and reject 32-bit integer overflow.

Timepoint-backed handlers must call a shared serial-resolution helper before storage access:

```cpp
RouterIdResolutionResult ResolveRouterIdContext(
    RESTAPIHandler* client,
    const std::string& routerId);
```

The timepoint-backed handlers are:

```text
memory-summary
radio-temperature-summary
usage-summary
rssi-summary
```

The helper must resolve current ownership through a status-aware OWPROV inventory lookup using the requesting caller's token. If OWPROV returns not found, unauthorized, or forbidden, the helper must return `404 not_found`. After OWPROV returns the visible device, the helper must read `InventoryTag.venue`, call `BoardsDB.FindBoardVenueRecordsByVenue(Device.venue)`, require exactly one matching Analytics board, and return that `boardId` plus `venueId`.

`availability-summary` is the storage-query exception, not an authorization exception. It must authenticate the caller, resolve current router ownership via OWPROV using the caller's token, and verify the router is accessible before querying availability storage. After authorization succeeds, the historical count must query availability storage by durable `serialNumber`, not by mandatory current `resolvedBoardId`. Event-time `board_id` is historical context only and must not authorize the request by itself.

---

## Router Registration

Update:

```text
src/RESTAPI/RESTAPI_routers.cpp
```

Register the new handlers in both:

```text
RESTAPI_Router
RESTAPI_Router_I
```

---

## Build Configuration

Update:

```text
CMakeLists.txt
```

Add all new handler and storage `.cpp` and `.h` files.

---

## Storage Files

Update:

```text
src/storage/storage_timepoints.h
src/storage/storage_timepoints.cpp
src/storage/storage_wificlients.h
src/storage/storage_wificlients.cpp
src/storage/storage_device_availability_events.h
src/storage/storage_device_availability_events.cpp
src/storage/storage_device_availability_state.h
src/storage/storage_device_availability_state.cpp
src/StorageService.h
src/StorageService.cpp
```

`storage_wificlients.*` only needs changes if the implementation chooses to add gateway `serialNumber` to `wificlienthistory`. The default implementation for gateway-scoped client summaries should aggregate `timepoints.ssid_data` to avoid that migration.

Suggested functions:

```cpp
bool GetMemorySummary(
    const std::string& resolvedBoardId,
    const std::string& routerId,
    uint64_t startTime,
    uint64_t endTime,
    AnalyticsObjects::GatewayMemorySummary& result);

bool GetRadioTemperatureSummary(...);

bool GetWifiClientUsageSummary(...);

bool GetWifiClientRssiSummary(...);

bool GetGatewayAvailabilitySummary(...);
```

---

# Final Mapping

| MCP Tool | Analytics API | Data Source | Implementation Status |
|---|---|---|---|
| `get_gateway_free_memory` | `GET /devices/{routerId}/memory-summary` | `timepoints.resource_data.memory_free` | Resolve routerId to venueId and boardId, then aggregate `timepoints` |
| `get_gateway_wifi_temp` | `GET /devices/{routerId}/radio-temperature-summary` | `timepoints.radio_data[].wifi_temp` | Resolve routerId to venueId and boardId, then aggregate present Wi-Fi temperature samples from `timepoints` |
| `get_device_bandwidth_consumption` | `GET /devices/{routerId}/wifi-clients/usage-summary` | `timepoints.ssid_data[].associations[]` | Resolve routerId to venueId and boardId, then calculate reset-safe cumulative-counter differentials from available persisted samples |
| `get_device_rssi_quality` | `GET /devices/{routerId}/wifi-clients/rssi-summary` | `timepoints.ssid_data[].associations[].rssi` | Resolve routerId to venueId and boardId, then classify RSSI samples |
| `get_gateway_offline_count` | `GET /devices/{routerId}/availability-summary` | Existing gateway `connection` topic plus `device_availability_events` and `device_availability_state` | Use routerId as durable serialNumber, use `device_availability_state.current_state` and `last_event_time` for restart-safe transition detection, then count offline events by serialNumber |

---

# Recommended Implementation Order

1. `get_gateway_wifi_temp`
2. `get_device_rssi_quality`
3. `get_device_bandwidth_consumption`
4. `get_gateway_free_memory`
5. `get_gateway_offline_count`
