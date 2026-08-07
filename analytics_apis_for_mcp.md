# Analytics APIs for MCP Tools

This document defines the request format, response format, and implementation logic for the following MCP tools in the `ra-wlan-cloud-analytics` service:

- `get_gateway_free_memory`
- `get_gateway_wifi_temp`
- `get_device_bandwidth_consumption`
- `get_device_rssi_quality`
- `get_gateway_offline_count`

The specification is structured across three distinct component layers:
1. **Public API Contract Specifications**: External OpenAPI schemas (`openapi/owanalytics.yaml`) defining HTTP requests, parameter validation, and response envelopes.
2. **Persistence & Pipeline Architecture Design**: Internal storage structures (`device_availability_events`, `device_availability_state`, `device_availability_ingestion_checkpoint`, `device_availability_ingestion_gaps`), Kafka event consumption/ordering, and cutover semantics.
3. **Test Specifications Matrix**: Independent verification matrix documented in `analytics_mcp_api_test_cases.md`.

> [!IMPORTANT]
> The specified production architecture changes—including four new availability persistence structures (`device_availability_events`, `device_availability_state`, `device_availability_ingestion_checkpoint`, `device_availability_ingestion_gaps`), Kafka event ordering/checkpointing rules, cutover migration rules, and OpenAPI v2.7.0 endpoint schemas—constitute a production architecture specification. Approving or merging this test specification PR does NOT bypass separate explicit architecture design sign-off for backend schema additions and production Kafka pipeline changes prior to production deployment.

The API contracts match the MCP tool names and response fields from the provided CSV.

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

All REST handlers must enforce request validation in three distinct sequential phases:

1. **Phase 1: Pure Request Parsing & Input Validation (No DB or I/O lookups)**
   - Validate `routerId` syntax (1–64 characters matching `^[a-zA-Z0-9_-]+$`) -> HTTP `400 invalid_router_id` if malformed.
   - Inspect raw query collection for exact-once presence of `timestampTill` and `lookbackHours` -> HTTP `400 invalid_timestamp` / `invalid_lookback_hours` if missing or repeated.
   - Parse `timestampTill` shape & UTC semantics -> HTTP `400 invalid_timestamp` if malformed or invalid date/time.
   - Parse `lookbackHours` strict positive integer -> HTTP `400 invalid_lookback_hours` if zero, negative, or non-numeric.
   - Checked epoch calculation: Compute `start_time = end_time - (lookback_hours * 3600)` using signed 64-bit integers. Verify `start_time >= 0` (minimum supported Unix epoch `1970-01-01T00:00:00Z`) -> HTTP `400 invalid_timestamp` if underflowing before epoch.

2. **Phase 2: Router Ownership & Serial Resolution**
   - Resolve `routerId` to `boardId` via local cache / OWPROV -> HTTP `404 not_found` if router serial does not exist in OWPROV.
   - Load router scope monitoring configuration (`monitoringDuration`) to derive `maxLookbackHours = floor(monitoringDuration / 3600)`.

3. **Phase 3: Duration, Retention & Domain Cutover Validation**
   - Validate duration against scope maximum: `lookbackHours > maxLookbackHours` -> HTTP `400 invalid_lookback_hours`.
   - Validate requested range against data retention window: requested window outside retention -> HTTP `400 lookback_outside_retention`.
   - Validate `start_time` against temperature cutover threshold (`start_time < temperatureMigrationCutoverTime`) -> HTTP `400 temperature_range_before_cutover`.
   - Validate `start_time` against availability cutover threshold (`start_time < availabilityValidFrom`) -> HTTP `400 availability_range_before_cutover`.

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
  dataset retrieval = all_with_baseline
  response = offline transition count
```

For `all`, retrieve every record in the requested range. For state-transition
calculations, also retrieve the latest valid state at or before the requested
start; this is `all_with_baseline`.

For `differential`, return the change in cumulative counter values over the
requested time range:

```text
delta = ending metric value - starting metric value
```

Use exact effective-boundary samples when available. Effective boundaries are
the requested boundaries clipped to any proven session lifetime. Otherwise, use
the latest available starting sample at or before the effective start and the
earliest available ending sample at or after the effective end. Each fallback
boundary sample must be within two times the configured telemetry sampling
interval from the effective boundary. When fallback samples are used, the
default response exposes the requested time range, aggregate result time window,
usage accuracy, segment count, and fallback status. Effective boundaries and
per-segment actual sample timestamps are included only when
`includeCalculationDetails=true`.

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
lookbackHours must be present exactly once.
lookbackHours must parse as a strict whole decimal integer with no trailing characters.
lookbackHours must be greater than 0.
maxLookbackHours = floor(configured monitoringDuration / 3600).
lookbackHours must be less than or equal to maxLookbackHours.
startTime must be less than endTime.
```

All APIs in this document derive `maxLookbackHours` from the configured `monitoringDuration` for the resolved router ownership scope. All APIs share this limit unless an endpoint section explicitly names a stricter limit. No endpoint currently defines a separate limit.

For a one-year monitoring configuration, `maxLookbackHours` is `365 * 24` only when the configured monitoring duration is exactly 365 days. Do not assume every calendar year is 8760 hours; use the configured duration and effective retention timestamps when validating the request.

Return validation errors with field-specific error codes:

```text
invalid_timestamp:
  timestampTill is missing, malformed, not UTC `Z` format, or otherwise unsupported.

invalid_lookback_hours:
  lookbackHours is missing, repeated, empty, non-numeric, fractional, partially numeric, overflowing,
  zero, negative, or greater than maxLookbackHours.

lookback_outside_retention:
  the calculated [startTime, endTime) window is outside the configured retention window.
```

A valid timestamp with an invalid `lookbackHours` value must not return `invalid_timestamp`. Internally, query `timepoints.timestamp` and `wificlienthistory.timestamp` using epoch seconds.

### Monitoring Configuration

Handlers must load the monitoring configuration for the resolved router ownership scope before querying metric storage.

```text
monitoringDuration = configured monitoring retention duration
retentionEnd = min(currentTime, monitoringConfigurationExpiry)
retentionStart = retentionEnd - monitoringDuration

If monitoring has been enabled for less time than monitoringDuration:
  retentionStart = max(monitoringEnabledAt, retentionEnd - monitoringDuration)
```

Use the configured duration and effective retention timestamps instead of calendar assumptions so leap years, partial-year retention, recently enabled monitoring, and changed retention policies behave consistently.

The requested half-open range must fit inside the configured retention window:

```text
startTime >= retentionStart
endTime <= retentionEnd
```

If the requested range is outside the configured retention window, return `400 Bad Request` with `error: "lookback_outside_retention"`.

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

For cumulative-counter differential summaries, exact effective-boundary samples
are preferred. For endpoints with proven session lifetimes, effective boundaries
are the requested boundaries clipped to the proven session start and end. For
endpoints without session lifetimes, effective boundaries equal the requested
boundaries. Otherwise, the handler must use the latest available sample at or
before `effective_start` and the earliest available sample at or after
`effective_end`, provided each fallback sample is within two times the configured
telemetry sampling interval from the effective boundary. These fallback samples
can fall outside `[startTime, endTime)`, so the response must expose:

```text
requestedTimeWindow.startTime = startTime
requestedTimeWindow.endTime = endTime
resultTimeWindow.earliestActualStartTime =
  earliest actual starting sample used by any calculable segment contributing
  to returned clients
resultTimeWindow.latestActualEndTime =
  latest actual ending sample used by any calculable segment contributing
  to returned clients
resultTimeWindow.boundaryFallbackUsed =
  true when any calculable segment contributing to returned clients used a
  fallback boundary sample
```

When no calculable usage segment exists for an observed client, the client
response uses `segment_count: 0` instead of inventing effective or actual
boundary timestamps. If `includeCalculationDetails=true`, that client has
`calculation_segments: []`.

Example:

```text
Request 1: 10:00 <= timestamp < 11:00
Request 2: 11:00 <= timestamp < 12:00
```

A sample exactly at `11:00` belongs only to Request 2.

### Authorization

All device metric handlers must enforce authorization before returning data:

```text
1. Authenticate the bearer token.
   If the token is missing, invalid, or expired, return 401 Unauthorized.
2. Resolve the caller's accessible entity, venue, and board scope from the token.
3. Resolve routerId to its current router ownership context.
4. Verify that the resolved router belongs to a board or venue the caller may read.
5. Return 404 Not Found when the router exists but the caller cannot access it.
6. Return 404 Not Found when the router does not exist.
```

Required permission:

```text
analytics.gateway_metrics.read
```

The permission is evaluated against the resolved board first, then the resolved venue, then the parent entity. Child-venue access is allowed only when the caller's venue permission explicitly includes descendant venues according to the same venue hierarchy rules used by OWPROV and `VenueCoordinator`; otherwise access is limited to the exact resolved venue or board.

Authorization must not rely on the caller-supplied `routerId` alone. A caller must not be able to request an arbitrary gateway serial number and retrieve metrics without proving access to the router's current ownership scope. The external API intentionally returns `404 Not Found` for both nonexistent routers and routers outside the caller's authorized scope to avoid exposing router existence. Internal logs and metrics should preserve the exact reason.

For historical availability, authorize by current router ownership before querying `device_availability_events` by `serialNumber`. The event-time `board_id` field is historical context only and must not be the sole authorization source. If current ownership cannot be resolved for a non-operator caller, do not serve serial-number-only availability history. A privileged operator-level permission may bypass current ownership resolution only if explicitly implemented and audited.

Privileged operator bypass, if implemented, must use a separate permission:

```text
analytics.gateway_metrics.read_any
```

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
2. Read the current VenueCoordinator ownershipVersion.
3. Check the process-level router resolution cache.
4. Use an unexpired positive cache entry only if its ownershipVersion matches
   the current VenueCoordinator ownershipVersion.
5. Resolve board ownership from the maintained routerId -> boardId map.
6. If the map has a current entry, use it as resolvedBoardId.
7. On cache/map miss or expired cache entry, call OWPROV inventory with status-aware handling:
     GET /api/v1/inventory/{routerId}
8. Read inventoryTag.venue as venueId.
9. Use the OWPROV venue result to verify or refresh board ownership.
10. Store the resolved result in the process-level cache.
11. Query Analytics storage with resolvedBoardId and serialNumber = routerId.
```

Separate MCP metric calls are separate HTTP requests. They do not share an HTTP request context, so router resolution reuse across different metric endpoints must come from the maintained `VenueCoordinator` ownership map or the process-level cache, not from request-scoped state.

Implementation notes:

```text
OWPROV source:
  /api/v1/inventory/{routerId}

OWPROV response field:
  InventoryTag.venue

Analytics local source:
  VenueCoordinator maintained routerId -> boardId map
  BoardInfo records for venue metadata and validation
  BoardInfo.venueList[].id
  BoardInfo.venueList[].monitorSubVenues
```

Board ownership resolution must use the maintained `routerId -> boardId` map as the primary design. `VenueCoordinator` should maintain this map from the same board device lists used by `VenueWatcher`, updating it when boards start, reconcile, or change device membership.

OWPROV inventory lookup is not the normal path for every metric query. It is used when the local map has no entry, when the entry needs verification, or when ownership may be stale. Board ownership refresh must account for child venues. Do not only check whether `inventoryTag.venue` is directly present in `BoardInfo.venueList`.

Primary ownership algorithm:

```text
Look up routerId in VenueCoordinator's maintained routerId -> boardId map.

If exactly one current board mapping exists:
  resolvedBoardId = mapped boardId
  return Success

If multiple current board mappings exist:
  return MultipleBoards
  log a configuration error

If no mapping exists:
  perform status-aware OWPROV inventory lookup
  read InventoryTag.venue
  refresh candidate board ownership from current venue device lists
  update the maintained routerId -> boardId map when ownership is determined
```

This mirrors the existing watcher behavior, where board devices are fetched from OWPROV with the board venue's `monitorSubVenues` setting.

A router must have exactly one authoritative board mapping. Never resolve multiple candidates by list order, oldest board, most recently updated board, or direct venue preference. If multiple active mappings exist, return `409 Conflict` and log a configuration error.

Refresh algorithm for cache/map misses:

```text
For each local BoardInfo record that can own the inventory venue:
  for each VenueInfo in BoardInfo.venueList:
    call SDK::Prov::Venue::GetDevices(
      client,
      VenueInfo.id,
      VenueInfo.monitorSubVenues,
      venueDeviceList,
      venueExists)

    if routerId is present in venueDeviceList.devices:
      add BoardInfo.info.id as a candidate board

If exactly one candidate exists:
  resolvedBoardId = candidate board
  update routerId -> boardId map

If more than one candidate exists:
  return MultipleBoards
  do not choose a candidate by list order or heuristic tie-breaker
```

Failure handling:

```text
OWPROV inventory not found              -> 404 Not Found
Inventory exists but venue is empty     -> 404 Not Found
No Analytics board configured for venue -> 404 Not Found
Multiple matching boards                -> 409 Conflict
OWPROV unavailable or invalid response  -> 502 Bad Gateway after cache fallback is exhausted
```

The resolver must return a status-bearing result, not a boolean, so handlers can map each failure to the correct HTTP response.

```cpp
enum class RouterIdResolutionStatus {
    Success,
    InvalidRouterId,
    InventoryNotFound,
    EmptyVenue,
    BoardNotConfigured,
    MultipleBoards,
    AccessDenied,
    MonitoringNotConfigured,
    MonitoringDisabled,
    OwprovUnavailable,
    OwprovInvalidResponse
};

struct RouterIdResolutionResult {
    RouterIdResolutionStatus status = RouterIdResolutionStatus::OwprovUnavailable;
    std::string routerId;
    std::string venueId;
    std::string resolvedBoardId;
    uint64_t resolvedAt = 0;
    uint64_t ownershipVersion = 0;
    std::string message;
};
```

Status mapping:

```text
Success               -> continue request
InvalidRouterId       -> 400 Bad Request
InventoryNotFound     -> 404 Not Found
EmptyVenue            -> 404 Not Found
BoardNotConfigured    -> 404 Not Found
MultipleBoards        -> 409 Conflict
AccessDenied          -> 404 Not Found
MonitoringNotConfigured -> 404 Not Found
MonitoringDisabled    -> 409 Conflict
OwprovUnavailable     -> 502 Bad Gateway
OwprovInvalidResponse -> 502 Bad Gateway
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

Router resolution process cache:

```text
Scope:
  thread-safe, process-level cache shared by all REST handlers

Key:
  routerId

Positive value:
  venueId
  boardId
  resolvedAt
  ownershipVersion

Negative value:
  resolution status
  resolvedAt
  ownershipVersion when available

Positive TTL:
  5 minutes, absolute from resolvedAt

Negative TTL:
  30 seconds or less, absolute from resolvedAt

Maximum size:
  10000 routerId entries per process

Eviction:
  expire entries by TTL first, then evict least-recently-used entries when the
  maximum size is reached

On unexpired positive cache hit:
  return cached venueId and boardId if ownershipVersion still matches the
  current VenueCoordinator ownershipVersion

On expired entry:
  resolve again before serving the request

On OWPROV failure:
  an unexpired positive local ownership entry may be used if ownershipVersion
  still matches
  an expired entry must not be extended silently
  if no usable unexpired entry exists, return OwprovUnavailable or
  OwprovInvalidResponse according to the failure

Negative-result caching:
  cache InventoryNotFound, EmptyVenue, BoardNotConfigured, MultipleBoards, and
  MonitoringNotConfigured for the negative TTL
  do not cache OwprovUnavailable or OwprovInvalidResponse as ownership facts

Invalidation:
  invalidate the routerId entry immediately when OWPROV reports a different
  venue for the router
  invalidate affected routerId entries when VenueCoordinator detects board
  membership changes
  invalidate affected routerId entries on board deletion or board venue
  reconfiguration
  invalidate affected routerId entries when board configuration changes
  invalidate affected routerId entries when venue monitoring settings change
  invalidate the routerId entry when router assignment changes
  invalidate the routerId entry when the router is removed
  invalidate the routerId entry when ownershipVersion changes

Concurrency:
  cache reads and writes must be synchronized
  concurrent misses for the same routerId should coalesce to one refresh when
  practical
  if coalescing is not implemented, racing refreshes must not publish older
  ownershipVersion results over newer results
```

The cache is secondary to `VenueCoordinator` ownership. A cache hit must not override a newer `VenueCoordinator` ownershipVersion.

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

```cpp
AnalyticsObjects::DeviceResourceTimePoint resource;

// Validate numeric type, integral value, non-negative range (>= 0), and 64-bit non-overflow before unsigned conversion.
// Sample-level rejection policy for negative telemetry: If ANY present memory field (free, total, cached, buffered) is negative (< 0), non-numeric, or invalid, mark the entire memory sample invalid and omit resource_data so the entire corrupted memory timepoint is excluded during aggregation.
bool sampleValid = true;
auto parseMemoryField = [&sampleValid](const Poco::JSON::Object::Ptr &obj, const std::string &key) -> std::optional<uint64_t> {
    if (!obj->has(key) || obj->isNull(key)) return std::nullopt;
    try {
        if (!obj->isNumeric(key)) { sampleValid = false; return std::nullopt; }
        int64_t val = obj->getElement<int64_t>(key);
        if (val < 0) { sampleValid = false; return std::nullopt; }
        return static_cast<uint64_t>(val);
    } catch (...) {
        sampleValid = false;
        return std::nullopt;
    }
};

auto freeVal = parseMemoryField(memory, "free");
auto totalVal = parseMemoryField(memory, "total");
auto cachedVal = parseMemoryField(memory, "cached");
auto bufferedVal = parseMemoryField(memory, "buffered");

if (sampleValid) {
    if (freeVal)     resource.memory_free = *freeVal;
    if (totalVal)    resource.memory_total = *totalVal;
    if (cachedVal)   resource.memory_cached = *cachedVal;
    if (bufferedVal) resource.memory_buffered = *bufferedVal;
    DTP.resource_data = resource;
}
```

### Aggregation Logic

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
  include the sample only when memory_free is present
  exclude the sample if any present memory value is negative
  exclude the sample if memory_total is present and memory_free > memory_total

Return:
  min_memfree = min(memory_free samples)
  max_memfree = max(memory_free samples)
  avg_memfree = sum(memory_free samples) / sample_count
```

Memory values are bytes and must be nonnegative. If `memory_total` is present, `memory_free` must not exceed `memory_total`; corrupted samples that violate this consistency rule are ignored rather than contributing to the summary. For successful responses with samples, the invariant is:

```text
min_memfree <= avg_memfree <= max_memfree
```

Do not query `device_timepoints.memory_free` unless a separate normalized table and migration are introduced.

### Handler Flow

```text
Validate routerId
    ↓
Call ResolveRouterIdContext(client, routerId)
    ↓
If status is not Success, map RouterIdResolutionStatus to HTTP response
    ↓
Use result.venueId and result.resolvedBoardId
    ↓
Parse timestampTill
    ↓
Calculate start_time
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
  "min_wifi_temp_2.4G": 62,
  "max_wifi_temp_2.4G": 70,
  "avg_wifi_temp_2.4G": 66.64,
  "min_wifi_temp_5G": 56,
  "max_wifi_temp_5G": 65,
  "avg_wifi_temp_5G": 60.38
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
However, wifi_temp = 0 is defined as an uninitialized or missing-sensor sentinel across all OpenWiFi telemetry. A sample with wifi_temp = 0, null, 255, or outside the valid range [-40, 125] is treated as a missing sensor reading and MUST be excluded from aggregation for all samples regardless of timestamp.
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
      if radio.wifi_temp is present, non-null, != 0, and -40 <= radio.wifi_temp <= 125:
        add radio.wifi_temp to that band's sample list

For each band:
  min_temperature = min(samples)
  max_temperature = max(samples)
  avg_temperature = sum(samples) / sample_count
```

A valid temperature sample is:

```text
record timestamp is at or after temperatureMigrationCutoverTime
radio.wifi_temp is present, non-null, != 0, and within range [-40, 125]
```

If all samples for a band are invalid or missing, return `null` for that band's min, max, and average fields.

Do not query `radio_timepoints` unless a separate normalized table and migration are introduced.

### Handler Logic

```cpp
if (radio.band == 2) {
    response.min_wifi_temp_2_4G = summary.min;
    response.max_wifi_temp_2_4G = summary.max;
    response.avg_wifi_temp_2_4G = summary.avg;
}

if (radio.band == 5) {
    response.min_wifi_temp_5G = summary.min;
    response.max_wifi_temp_5G = summary.max;
    response.avg_wifi_temp_5G = summary.avg;
}
```

### Missing Band Example

```json
{
  "min_wifi_temp_2.4G": 62,
  "max_wifi_temp_2.4G": 70,
  "avg_wifi_temp_2.4G": 66.64,
  "min_wifi_temp_5G": null,
  "max_wifi_temp_5G": null,
  "avg_wifi_temp_5G": null
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
    &includeCalculationDetails=false
```

`includeCalculationDetails` is optional and defaults to `false`. Set it to
`true` only for diagnostic calculation provenance.

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

By default, usage-summary returns concise calculation quality metadata for MCP
consumers. Segment-level provenance is verbose and is omitted unless
`includeCalculationDetails=true` is requested.

```json
{
  "requestedTimeWindow": {
    "startTime": "2026-07-26T12:00:00Z",
    "endTime": "2026-07-27T12:00:00Z"
  },
  "resultTimeWindow": {
    "earliestActualStartTime": "2026-07-26T11:55:00Z",
    "latestActualEndTime": "2026-07-27T12:05:00Z",
    "boundaryFallbackUsed": true
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
    },
    {
      "mac": "28:39:26:a1:7c:a5",
      "rx_bytes": 30071250,
      "tx_bytes": 16486250,
      "total_bytes": 46557500,
      "data_consume_rx": "30.07 MB",
      "data_consume_tx": "16.49 MB",
      "total_data_usage": "46.56 MB",
      "usage_accuracy": "bounded_interval",
      "incomplete": false,
      "segment_count": 1,
      "boundary_fallback_used": true
    },
    {
      "mac": "54:6c:0e:44:11:09",
      "rx_bytes": 4200000,
      "tx_bytes": 600000,
      "total_bytes": 4800000,
      "data_consume_rx": "4.20 MB",
      "data_consume_tx": "0.60 MB",
      "total_data_usage": "4.80 MB",
      "usage_accuracy": "exact",
      "incomplete": false,
      "segment_count": 2,
      "boundary_fallback_used": false
    }
  ],
  "totalClients": 3,
  "truncated": false
}
```

`resultTimeWindow` is aggregate response metadata:

```text
earliestActualStartTime =
  minimum actual_start_time among the server's internal calculable segments
  contributing to returned clients

latestActualEndTime =
  maximum actual_end_time among the server's internal calculable segments
  contributing to returned clients

boundaryFallbackUsed =
  true when any internal calculable segment contributing to returned clients
  used a fallback boundary sample
```

It describes only the overall result envelope. It does not mean every client or
segment used the entire envelope, and it is identical whether or not
`includeCalculationDetails` is enabled.

When no clients are returned:

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

When a client is observed but no usage interval can be calculated, such as when
only one cumulative-counter sample exists in or bounding the requested range,
return the safely provable minimum with `segment_count: 0`:

```json
{
  "requestedTimeWindow": {
    "startTime": "2026-08-05T12:00:00Z",
    "endTime": "2026-08-05T13:00:00Z"
  },
  "resultTimeWindow": {
    "earliestActualStartTime": null,
    "latestActualEndTime": null,
    "boundaryFallbackUsed": false
  },
  "items": [
    {
      "mac": "e2:51:95:ed:0f:28",
      "rx_bytes": 0,
      "tx_bytes": 0,
      "total_bytes": 0,
      "data_consume_rx": "0.00 MB",
      "data_consume_tx": "0.00 MB",
      "total_data_usage": "0.00 MB",
      "usage_accuracy": "lower_bound",
      "incomplete": true,
      "segment_count": 0,
      "boundary_fallback_used": false
    }
  ],
  "totalClients": 1,
  "truncated": false
}
```

## Calculation Details

Use `includeCalculationDetails=true` only for diagnostics, troubleshooting, or
calculation provenance. Ordinary MCP decisions should use `usage_accuracy`,
`segment_count`, `boundary_fallback_used`, and the aggregate `resultTimeWindow`.

```http
GET /api/v1/devices/60cf84f22290/wifi-clients/usage-summary?timestampTill=2026-07-27T12:00:00Z&lookbackHours=24&includeCalculationDetails=true
Authorization: Bearer <token>
```

When details are enabled, every returned calculation segment has non-null
`actual_start_time` and `actual_end_time`. `stream_id` and `segment_id` are
opaque identifiers scoped to the response. Clients must not persist them,
compare them across requests, parse them, or depend on their format; the
examples below are illustrative only. If no differential can be calculated, the
client still has `segment_count: 0` and
`calculation_segments: []`.

Detailed response invariants:

```text
client.rx_bytes == SUM(calculation_segments[].rx_bytes)
client.tx_bytes == SUM(calculation_segments[].tx_bytes)
client.total_bytes == SUM(calculation_segments[].total_bytes)
client.segment_count == calculation_segments.length
client.boundary_fallback_used ==
  true when any calculation segment has boundary_fallback_used = true
```

Example detailed response for the same calculated result:

```json
{
  "requestedTimeWindow": {
    "startTime": "2026-07-26T12:00:00Z",
    "endTime": "2026-07-27T12:00:00Z"
  },
  "resultTimeWindow": {
    "earliestActualStartTime": "2026-07-26T11:55:00Z",
    "latestActualEndTime": "2026-07-27T12:05:00Z",
    "boundaryFallbackUsed": true
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
      "boundary_fallback_used": false,
      "calculation_segments": [
        {
          "stream_id": "e2:51:95:ed:0f:28|bssid=18:34:af:01:02:03|ssid=Corp|band=5G",
          "segment_id": "e2:51:95:ed:0f:28|session=42|segment=0",
          "segment_start_reason": "window_start",
          "segment_end_reason": "window_end",
          "effective_start_time": "2026-07-26T12:00:00Z",
          "effective_end_time": "2026-07-27T12:00:00Z",
          "actual_start_time": "2026-07-26T12:00:00Z",
          "actual_end_time": "2026-07-27T12:00:00Z",
          "boundary_fallback_used": false,
          "accuracy": "exact",
          "rx_bytes": 106487500,
          "tx_bytes": 3851250,
          "total_bytes": 110338750
        }
      ]
    },
    {
      "mac": "28:39:26:a1:7c:a5",
      "rx_bytes": 30071250,
      "tx_bytes": 16486250,
      "total_bytes": 46557500,
      "data_consume_rx": "30.07 MB",
      "data_consume_tx": "16.49 MB",
      "total_data_usage": "46.56 MB",
      "usage_accuracy": "bounded_interval",
      "incomplete": false,
      "segment_count": 1,
      "boundary_fallback_used": true,
      "calculation_segments": [
        {
          "stream_id": "28:39:26:a1:7c:a5|bssid=18:34:af:04:05:06|ssid=Corp|band=5G",
          "segment_id": "28:39:26:a1:7c:a5|session=99|segment=0",
          "segment_start_reason": "window_start",
          "segment_end_reason": "window_end",
          "effective_start_time": "2026-07-26T12:00:00Z",
          "effective_end_time": "2026-07-27T12:00:00Z",
          "actual_start_time": "2026-07-26T11:55:00Z",
          "actual_end_time": "2026-07-27T12:05:00Z",
          "boundary_fallback_used": true,
          "accuracy": "bounded_interval",
          "rx_bytes": 30071250,
          "tx_bytes": 16486250,
          "total_bytes": 46557500
        }
      ]
    },
    {
      "mac": "54:6c:0e:44:11:09",
      "rx_bytes": 4200000,
      "tx_bytes": 600000,
      "total_bytes": 4800000,
      "data_consume_rx": "4.20 MB",
      "data_consume_tx": "0.60 MB",
      "total_data_usage": "4.80 MB",
      "usage_accuracy": "exact",
      "incomplete": false,
      "segment_count": 2,
      "boundary_fallback_used": false,
      "calculation_segments": [
        {
          "stream_id": "54:6c:0e:44:11:09|bssid=18:34:af:07:08:09|ssid=Corp|band=5G",
          "segment_id": "54:6c:0e:44:11:09|session=10|segment=0",
          "segment_start_reason": "window_start",
          "segment_end_reason": "session_end",
          "effective_start_time": "2026-07-26T12:00:00Z",
          "effective_end_time": "2026-07-26T18:30:00Z",
          "actual_start_time": "2026-07-26T12:00:00Z",
          "actual_end_time": "2026-07-26T18:30:00Z",
          "boundary_fallback_used": false,
          "accuracy": "exact",
          "rx_bytes": 1800000,
          "tx_bytes": 250000,
          "total_bytes": 2050000
        },
        {
          "stream_id": "54:6c:0e:44:11:09|bssid=18:34:af:07:08:09|ssid=Corp|band=5G",
          "segment_id": "54:6c:0e:44:11:09|session=11|segment=0",
          "segment_start_reason": "session_start",
          "segment_end_reason": "window_end",
          "effective_start_time": "2026-07-26T19:00:00Z",
          "effective_end_time": "2026-07-27T12:00:00Z",
          "actual_start_time": "2026-07-26T19:00:00Z",
          "actual_end_time": "2026-07-27T12:00:00Z",
          "boundary_fallback_used": false,
          "accuracy": "exact",
          "rx_bytes": 2400000,
          "tx_bytes": 350000,
          "total_bytes": 2750000
        }
      ]
    }
  ],
  "totalClients": 3,
  "truncated": false
}
```

When a client is observed in-window (`[startTime, endTime)`) but no usage interval can be calculated, such as when
only one cumulative-counter sample exists in or bounding the requested range,
return the safely provable minimum with no calculation segments:

```json
{
  "requestedTimeWindow": {
    "startTime": "2026-08-05T12:00:00Z",
    "endTime": "2026-08-05T13:00:00Z"
  },
  "resultTimeWindow": {
    "earliestActualStartTime": null,
    "latestActualEndTime": null,
    "boundaryFallbackUsed": false
  },
  "items": [
    {
      "mac": "e2:51:95:ed:0f:28",
      "rx_bytes": 0,
      "tx_bytes": 0,
      "total_bytes": 0,
      "data_consume_rx": "0.00 MB",
      "data_consume_tx": "0.00 MB",
      "total_data_usage": "0.00 MB",
      "usage_accuracy": "lower_bound",
      "incomplete": true,
      "segment_count": 0,
      "boundary_fallback_used": false,
      "calculation_segments": []
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
  mark the stream lower_bound

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
  exact sample at effective_start, if available
  otherwise latest available sample at or before effective_start

end_sample:
  exact sample at effective_end, if available
  otherwise earliest available sample at or after effective_end

boundary tolerance:
  abs(effective_start - actual_start_time) <= 2 * expected_collection_interval
  abs(actual_end_time - effective_end) <= 2 * expected_collection_interval

actual_start_time = start_sample.timestamp
actual_end_time = end_sample.timestamp
boundary_fallback_used =
  actual_start_time != effective_start ||
  actual_end_time != effective_end

uninterrupted segment differential:
  counterDelta(end_sample.rx_bytes, start_sample.rx_bytes)
  counterDelta(end_sample.tx_bytes, start_sample.tx_bytes)

samples between start_sample and end_sample:
  use to detect counter resets, confirmed rollovers, duplicate or
  out-of-order telemetry, missing-sample gaps, and session boundaries
  do not sum raw cumulative values as usage
  do sum proven segment deltas when resets or session splits are confirmed

rx_bytes    = SUM(stream rx_bytes segment differentials or lower-bound deltas)
tx_bytes    = SUM(stream tx_bytes segment differentials or lower-bound deltas)
total_bytes = rx_bytes + tx_bytes

data_consume_rx  = format_bytes(rx_bytes)
data_consume_tx  = format_bytes(tx_bytes)
total_data_usage = format_bytes(total_bytes)

stream accuracy =
  exact when the segment has authoritative counter evidence at effective_start
    and effective_end, and every internal sub-segment is fully proven
  bounded_interval when the segment uses immediate fallback samples within
    tolerance and every internal sub-segment is fully proven
  lower_bound when the stream lacks a usable boundary pair, exceeds boundary
    tolerance, has an ambiguous reset/session change, or only has partially
    observed segments

client usage_accuracy precedence =
  lower_bound if any contributing stream is lower_bound
  otherwise bounded_interval if any contributing stream is bounded_interval
  otherwise exact

incomplete = true when usage_accuracy is lower_bound
```

Calculate counter deltas independently for RX and TX per `stream_key` and
internal calculable segment. After segment deltas are calculated, aggregate the
resulting RX/TX deltas by station MAC for the response. Client `rx_bytes` and
`tx_bytes` must equal the sum of internal segment RX/TX deltas; each internal
segment's `total_bytes` must equal its `rx_bytes + tx_bytes`. The default
response exposes `segment_count` and `boundary_fallback_used` instead of the
segment objects. When `includeCalculationDetails=true`, the detailed
`calculation_segments[]` array must satisfy the same byte-sum invariants. If
any segment contributing to a station MAC is lower_bound, that station's usage
is lower_bound.

Client MAC values in API responses must be normalized canonical lowercase colon-separated MAC addresses matching `^[0-9a-f]{2}(:[0-9a-f]{2}){5}$`.

Usage accuracy contract:

```text
Returned usage is exact only when every contributing segment has enough
information to account for its overlap with the requested window:
  effective_start = max(requested_start, proven_session_start), and
  effective_end = min(requested_end, proven_session_end), and
  authoritative counter evidence exists exactly at effective_start and
  effective_end, and any counter rollover is confirmed and handled with
  rollover arithmetic.

Returned usage is bounded_interval when exact boundary samples are unavailable but
the latest available starting sample at or before `effective_start` and the
earliest available ending sample at or after `effective_end` are available
within two times the configured telemetry sampling interval, and no reset or gap
prevents the segment differential from being calculated. The response must
include requested timestamps and aggregate actual sample timestamps. When
`includeCalculationDetails=true`, it must also include the effective and actual
timestamps for each returned segment. This is usage over an interval containing
the segment's overlap with the requested interval; when counters are monotonic
and uninterrupted, the returned value is greater than or equal to usage in that
overlap.

When a stream lacks a usable bounding sample pair or has an ambiguous counter
decrease/session change, or when either fallback boundary exceeds tolerance, the
returned usage is a lower-bound estimate. The algorithm must avoid overcounting
unknown traffic, so it adds only safely provable nonnegative consecutive segment
deltas inside the requested range and treats the result as incomplete/estimated.
If no positive interval can be safely proven, return 0 bytes for that stream with
`lower_bound`. Include a client in `items[]` and count it in `totalClients` only when it has at least one in-window observation (`[startTime, endTime)`) or proven session overlap during the requested interval, even if the safely provable lower-bound delta is 0. Outside-window bounding samples (e.g. pre-window baseline or post-window fallback samples) are used exclusively as boundary calculation aids for clients that have proven in-window presence or session overlap. A station MAC with only outside-window samples and no in-window observation or session overlap during `[startTime, endTime)` must be excluded from `items[]` and `totalClients`. If no differential segment can be calculated
for that client, return `segment_count: 0` and `boundary_fallback_used: false`;
do not create a segment with fabricated effective or actual boundary timestamps.
If details are enabled, return `calculation_segments: []` for that client.

The response must expose `usage_accuracy`, `segment_count`, and
`boundary_fallback_used` per client:
  exact: all contributing streams are fully accounted for
  bounded_interval: at least one contributing segment used fallback samples
    within tolerance that bound the requested range
  lower_bound: at least one contributing stream used a conservative zero delta
    for an ambiguous interval, missing usable boundary pair, or out-of-tolerance
    boundary

When `usage_accuracy` is `lower_bound`, the reported byte and formatted usage
values are the safely observed minimum. Actual usage may be higher.
```

Differential response decision table:

| Condition | Result |
|---|---|
| Exact effective start and effective end counter evidence exists, no reset/session ambiguity | `exact` differential |
| Fallback start and end samples bound the effective segment range and both are within `2 * expected_collection_interval`, no reset/session ambiguity | `bounded_interval` differential |
| Session starts inside the requested window with proven zero/session baseline and authoritative end evidence | `exact` if effective boundaries are fully proven |
| Session ends inside the requested window with authoritative start and proven session-end evidence | `exact` if effective boundaries are fully proven |
| Start sample missing and session start inside the requested window is confirmed but complete effective-boundary evidence is unavailable | sum safely provable nonnegative segment deltas; `lower_bound` |
| Start sample missing and session origin is unknown | `lower_bound` |
| End sample missing | sum safely provable nonnegative consecutive segment deltas through the latest usable sample; `lower_bound` |
| Both boundary samples missing | sum safely provable nonnegative consecutive segment deltas inside the requested range; `lower_bound` |
| Only one in-window sample exists | return 0 bytes, `lower_bound`, `segment_count: 0`, and `boundary_fallback_used: false`; if details are enabled, return `calculation_segments: []` |
| Client appears during the window without reliable session-start proof | sum safely provable post-appearance segment deltas only; `lower_bound` |
| Client disappears before the end boundary | sum safely provable segment deltas through the last usable sample; `lower_bound` |
| Client reconnects and counters reset | split only when session identity proves independent streams; otherwise `lower_bound` |
| Multiple sessions for the same MAC | calculate per proven session stream, then aggregate by MAC; mark client `lower_bound` if any contributing stream is incomplete |
| Ambiguous counter reset, stale sample, duplicate conflict, or out-of-order telemetry | `lower_bound` |

A fixed-width rollover is confirmed only when the counter width is known for that source, the observed decrease is consistent with a rollover for that counter, and the maximum possible counter increase during the observation gap cannot exceed one full counter range. If the counter width is unknown, if multiple wraps may have occurred, or if the decrease could also be a reset/reconnect/stale sample, treat it as ambiguous and return `lower_bound`.

If the expected collection interval is missing, zero, invalid, or cannot be resolved reliably for the requested retention period, fallback boundary samples cannot qualify as `bounded_interval`; exact boundary samples are required to avoid `lower_bound`.

### Reset-Safe Delta Logic

```cpp
struct CounterDeltaResult {
    uint64_t delta;
    bool incomplete;
};

CounterDeltaResult counterDelta(uint64_t current,
                                uint64_t previous,
                                bool confirmedFixedWidthRollover,
                                bool singleRolloverBoundProven,
                                uint64_t counterMax) {
    if (current >= previous) {
        return {current - previous, false};
    }

    if (confirmedFixedWidthRollover && singleRolloverBoundProven) {
        return {(counterMax - previous) + current + 1, false};
    }

    return {0, true};
}
```

Session starts are handled by segment construction, not by blindly adding the
current counter on every decrease. When reliable session evidence proves a new
session started inside the requested window, use that session's first valid
counter sample as the segment baseline and sum only subsequent proven segment
deltas. If there is no subsequent usable sample, that segment contributes 0 with
`lower_bound`.

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
   - TX bytes return 0 as a required non-null integer in calculation_segments (and formatted "0.00 MB" in summary strings).
   - Total segment bytes total_bytes = rx_bytes + 0 = rx_bytes.
   - The stream accuracy is classified as lower_bound.
3. If TX is present and valid but RX is missing/invalid:
   - TX bytes are calculated normally and included in data_consume_tx.
   - RX bytes return 0 as a required non-null integer in calculation_segments (and formatted "0.00 MB" in summary strings).
   - Total segment bytes total_bytes = 0 + tx_bytes = tx_bytes.
   - The stream accuracy is classified as lower_bound.
4. If a boundary sample has RX but not TX (or vice versa), the missing direction cannot form a calculable boundary delta; that direction returns 0 bytes and is marked uncalculable (lower_bound).
5. segment_count is incremented if at least one direction (RX or TX) produces a valid calculable segment delta.
6. A client MAC is included in totalClients and items[] if it has proven in-window presence or session overlap, regardless of whether one counter direction was missing or uncalculable.

If current < previous:
  if a new association/session is confirmed:
    if the new session start time is within [startTime, endTime):
      close the previous segment at the last pre-reset sample
      start a new segment with current as its baseline
      add only subsequent proven nonnegative deltas for the new segment
      mark the stream lower_bound unless both session segments can be fully proven
    else:
      treat current as the new baseline, add delta 0, and mark the result
      incomplete/estimated
  else if fixed-width rollover is known, one-wrap bound is proven, and can be confirmed:
    apply rollover math
  else:
    treat current as the new baseline, add delta 0, and mark the result
    incomplete/estimated
```

For usage differential calculation, choose the start and end samples first, then
calculate the boundary differential for each segment. Exact usage requires
authoritative counter evidence exactly at each segment's `effective_start` and
`effective_end`, where effective boundaries are clipped to proven session
lifetime and the requested window. If fallback samples within tolerance are used,
classify the result as `bounded_interval`. The default response exposes the
requested time window, aggregate result time window, and fallback summary
fields. When `includeCalculationDetails=true`, each returned segment
additionally exposes its effective and actual boundary timestamps. Do not add a
cumulative counter directly unless it is known to represent traffic that started inside the
segment's effective interval.

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
  usage_accuracy = exact, unless another stream is incomplete

If 10:10 is a confirmed new session that started inside the request window WITHOUT authoritative proof of starting at zero:
  treat 200 as the new segment baseline (delta = 0 for 10:10 sample alone)
  total rx_bytes = 500
  usage_accuracy = lower_bound (until a subsequent sample in Session B establishes a proven delta)

If 10:10 is an ambiguous decrease:
  add 0 (treat 200 as baseline)
  total rx_bytes = 500
  usage_accuracy = lower_bound

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
Return wrapped response with requestedTimeWindow, resultTimeWindow, items,
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
  "requestedTimeWindow": {
    "startTime": "2026-07-26T12:00:00Z",
    "endTime": "2026-07-27T12:00:00Z"
  },
  "resultTimeWindow": {
    "firstSampleTime": "2026-07-26T12:01:00Z",
    "lastSampleTime": "2026-07-27T11:58:00Z",
    "totalSamples": 170
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

`resultTimeWindow` for RSSI is scoped to returned `items[]` after applying the
500-client response limit:

```text
firstSampleTime =
  earliest valid RSSI sample contributing to items[]

lastSampleTime =
  latest valid RSSI sample contributing to items[]

totalSamples =
  SUM(items[].rssi_total_samples)
```

For empty RSSI results:

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
      "firstSampleAt": "2026-07-26T13:15:00Z",
      "lastSampleAt": "2026-07-27T09:45:00Z"
    },
    "sourceWindow": {
      "firstSampleAt": "2026-07-26T11:55:00Z",
      "lastSampleAt": "2026-07-27T12:00:00Z"
    },
    "contributingWindow": {
      "firstSampleAt": "2026-07-26T13:15:00Z",
      "lastSampleAt": "2026-07-27T09:45:00Z"
    },
    "selection": "boundary_assisted",
    "coverage": "full",
    "accuracy": "exact",
    "sampleCount": 6,
    "offlineEventCount": 6,
    "effectiveSamplingIntervalSeconds": 0,
    "allowedGapSeconds": 0,
    "boundarySamplesUsed": {
      "beforeStart": true,
      "atStart": false,
      "atEnd": false,
      "afterEnd": false
    },
    "availabilityCoverage": {
      "coverageStart": "2026-07-26T11:55:00Z",
      "stateKnownFrom": "2026-07-26T11:55:00Z",
      "processedThrough": "2026-07-27T12:00:00Z",
      "allowedIngestionDelaySeconds": 0,
      "ingestionGapKnown": false,
      "proofSource": "serial_partition_checkpoint"
    }
  },
  "data": {
    "gw_uuid": "60cf84f22290",
    "fetch_status": "success",
    "offline_count": 6
  }
}
```

## API Logic

This API can use gateway events from the existing `connection` topic.

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
metadata
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
  source_sequence BIGINT NULL
  reason          TEXT
  connection_ip   TEXT
  session_id      TEXT
  event_id        TEXT
  idempotency_key TEXT NOT NULL
  metadata        TEXT

Indexes:
  availability_serial_time_index:
    serialNumber ASC
    event_time ASC
    source_sequence ASC NULLS FIRST

  availability_board_serial_time_index:
    board_id ASC
    serialNumber ASC
    event_time ASC
    source_sequence ASC NULLS FIRST

Unique constraints:
  availability_idempotency_key_unique:
    idempotency_key ASC

Optional indexes:
  availability_event_id_index:
    event_id ASC
```

SQL composite ordering semantics:

```text
When querying device_availability_events by (event_time, source_sequence):
- Ascending queries use ORDER BY event_time ASC, source_sequence ASC NULLS FIRST so unsequenced events (source_sequence = NULL) at a timestamp sort before sequenced events at the same timestamp.
- Descending queries use ORDER BY event_time DESC, source_sequence DESC NULLS LAST so the event with the highest sequence number at a timestamp sorts first, and unsequenced events sort last.
```

`event_id` stores the normalized source event id when the payload provides one, using `payload.ping.uuid`, `payload.uuid`, or `payload.disconnection.uuid` only when that `uuid` is stable for the same logical source event.

`serialNumber` is the durable identity for availability history. `board_id` is event-time context only and must be nullable because connection events can arrive when the router is not currently assigned to an Analytics board, when OWPROV is unavailable, or after ownership has changed. Do not drop availability events only because current board ownership cannot be resolved.

Add a persisted current-state table for restart-safe transition detection:

```text
device_availability_state
-------------------------
serialNumber
board_id
current_state
last_event_time
last_source_sequence
last_idempotency_key
updated_at
metadata
```

Required fields and constraints:

```text
Fields:
  serialNumber         TEXT primary id
  board_id             TEXT NULL
  current_state        TEXT
  last_event_time      BIGINT
  last_source_sequence BIGINT NULL
  last_idempotency_key TEXT
  updated_at           BIGINT
  metadata             TEXT

Indexes:
  availability_state_board_index:
    board_id ASC

Constraints:
  current_state must be one of: online, offline, unknown
```

`device_availability_events` stores only online/offline transition history. `device_availability_state` stores the authoritative current state and the latest accepted source availability ordering key.

`last_event_time` and `last_source_sequence` form the persisted source ordering key after events have passed the required per-serial ordering mechanism. `last_event_time` must be populated from the normalized Kafka source timestamp (`event_time`), and `last_source_sequence` must be populated from a reliable source sequence, source offset within the producing system, or another deterministic 64-bit numeric value (`BIGINT`) when one exists. `source_sequence` and `last_source_sequence` are stored as `BIGINT NULL` so SQL and application-layer index and comparator operations preserve numeric order (`10 > 2`). The composite key is used for stale detection only after Analytics has established that no older transition for the same serialNumber can still be accepted. `DeviceInfo.lastContact` may remain local contact or processing metadata, but it must not be used to order source events because Kafka delivery can be delayed.

Add persisted ingestion coverage tables for availability exactness:

```text
device_availability_ingestion_checkpoint
----------------------------------------
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

device_availability_ingestion_gaps
----------------------------------
serialNumber
gap_start_event_time
gap_end_event_time
reason
detected_at
metadata
```

`processed_through_event_time` is a source-event-time watermark, not a Kafka processing-time or `DeviceInfo.lastContact` value. `state_known_from` stores the earliest source event timestamp from which gateway state is authoritatively established. When initial state is unproven (such as a first observed disconnection initializing state without prior state history), `state_known_from` is set to the timestamp of the first observed transition or explicit state proof (or an unproven gap is persisted in `device_availability_ingestion_gaps` for `[coverage_start, initial_observation_time)`). Kafka topic, partition, and offset are stored only as proof metadata.

For availability coverage decisions over a requested interval `[startTime, endTime)`:

```text
coverageTargetEnd = endTime - allowedIngestionDelaySeconds
```

An exact result (`meta.coverage = "full"`, `meta.accuracy = "exact"`) is allowed only when the serialNumber checkpoint proves `coverage_start <= startTime`, `state_known_from <= startTime` (or no unproven initial gap), `processed_through_event_time >= endTime`, and no persisted gap overlaps `[startTime, endTime)`.

Evaluating coverage against `coverageTargetEnd = endTime - allowedIngestionDelaySeconds` does not prove that there were no offline transitions in `[coverageTargetEnd, endTime)`. Therefore, if `processed_through_event_time < endTime` (even when `processed_through_event_time >= coverageTargetEnd`), the requested interval `[startTime, endTime)` is not fully covered up to `endTime` and must be reported as partial coverage (`meta.coverage = "partial"`, `meta.accuracy = "lower_bound"`) with the effective/observed window covered so far ending at `processed_through_event_time` (or `coverageTargetEnd`). Set `allowedIngestionDelaySeconds` to the configured maximum tolerated ingestion/source-message delay for availability queries, and return it in `meta.availabilityCoverage`.

The checkpoint must be advanced durably with event processing. For a message that changes availability state or updates same-state metadata, the state update, optional transition insert, and checkpoint update must commit in one database transaction before the corresponding Kafka offset is committed. Rebalances and restarts must reload the checkpoint and any persisted reorder-buffer state needed by the selected ordering strategy. If the implementation cannot prove continuity after a restart, rebalance, topic retention truncation, skipped unparseable connection message, reorder-window overflow, or offset discontinuity, it must persist an ingestion gap and stop returning exact coverage for overlapping ranges.

When one serialNumber has no recent messages, do not infer coverage from an empty event table. The API may return an exact zero only if that serialNumber has a durable checkpoint whose `processed_through_event_time` reaches `endTime`; otherwise the result is partial/lower-bound or unavailable according to the coverage rules. A partition-level or source-level watermark may be used instead of per-message checkpoint advancement only if it is durable, serialNumber-scoped for the queried gateway, and can prove that no earlier source event for that gateway remains unprocessed.

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
  include storage/storage_device_availability_ingestion_checkpoint.h
  include storage/storage_device_availability_ingestion_gaps.h
  add DeviceAvailabilityEventsDB accessor
  add DeviceAvailabilityStateDB accessor
  add DeviceAvailabilityIngestionCheckpointDB accessor
  add DeviceAvailabilityIngestionGapsDB accessor
  add std::unique_ptr<DeviceAvailabilityEventsDB>
  add std::unique_ptr<DeviceAvailabilityStateDB>
  add std::unique_ptr<DeviceAvailabilityIngestionCheckpointDB>
  add std::unique_ptr<DeviceAvailabilityIngestionGapsDB>

src/StorageService.cpp
  construct DeviceAvailabilityEventsDB
  construct DeviceAvailabilityStateDB
  construct DeviceAvailabilityIngestionCheckpointDB
  construct DeviceAvailabilityIngestionGapsDB
  call DeviceAvailabilityEventsDB->Create()
  call DeviceAvailabilityStateDB->Create()
  call DeviceAvailabilityIngestionCheckpointDB->Create()
  call DeviceAvailabilityIngestionGapsDB->Create()
  include availability retention cleanup if retention should match board timepoint cleanup
```

Add a DB upgrade/migration path for the new tables. Existing deployments will start with no historical availability events. Define a fixed `availabilityValidFrom` timestamp as the deployment/migration time when availability-event persistence starts. The API should return `offline_count: 0` as an exact result only for successful empty queries whose requested range starts at or after `availabilityValidFrom` and is covered by the per-serial ingestion checkpoint; do not infer old events from `lastDisconnection`.

Add all availability storage files to `CMakeLists.txt`, including `storage_device_availability_events.*`, `storage_device_availability_state.*`, `storage_device_availability_ingestion_checkpoint.*`, and `storage_device_availability_ingestion_gaps.*`. Register database migration/upgrade logic for all availability tables so fresh databases and upgraded deployments create the event table, state table, checkpoint table, gap table, indexes, uniqueness constraints, and state constraints consistently.

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
  set last_idempotency_key = idempotency_key
  do not insert an offline transition event because the prior state is unknown

First observed ping/capabilities:
  insert state row with current_state = online
  set last_event_time = event_time
  set last_idempotency_key = idempotency_key
  do not insert an online transition event

First observed unrecognized connection message:
  insert or update state row with current_state = unknown only when useful
  set last_event_time only from a valid source timestamp
  do not insert a counted event
```

In-memory transition checking is only an optimization. It is not sufficient for correctness because Kafka can redeliver messages and the service can restart after losing in-memory state. Transition detection must use `device_availability_state.current_state` and `device_availability_state.last_event_time` under the same transaction before writing `device_availability_events`. Duplicate connection messages must be rejected by source timestamp validation and storage-level idempotency before they can affect `offline_count`.

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
  event_time is required and must have the highest precision available from the source

Offline events derived from disconnection:
  reason or source disconnect cause is required when available
  if reason/cause and connection_ip are missing:
    prefer stable source_event_id from payload.disconnection.uuid
    otherwise derive the key from serialNumber, message_type, event_type, and event_time
  if reason/cause and connection_ip are missing and event_time precision is coarser than seconds:
    do not write a counted offline event unless a stable source_event_id or session_id exists

Online events derived from ping/capabilities:
  session_id is required when available
  if session_id is unavailable:
    event_time plus message type must be precise enough to distinguish separate logical reconnects
  if event_time is missing or too coarse to distinguish separate reconnects:
    update current-state metadata only; do not write a counted online transition event

Unrecognized connection messages:
  do not write counted availability events
  store only current-state metadata when useful
```

If the minimum fields for the event type are not present, the implementation must not create a counted `device_availability_events` row. It may update non-counted metadata only when doing so cannot move `device_availability_state` backward or create a false transition.

Do not include `board_id` in the idempotency key unless it is part of a stable source event id. Board ownership can change between delivery and redelivery, and including current ownership in a derived key can turn one logical event into multiple stored events.

Do not include Kafka topic, partition, or offset in the derived logical `idempotency_key`. Kafka coordinates identify a broker record, not the logical connection event. They dedupe redelivery of the same Kafka record, but they do not dedupe the same logical connection event produced as a second Kafka record with a different offset. Store topic, partition, offset, message key, and consumer timestamp in `metadata` for tracing only.

The same delivered logical connection event must always produce the same `idempotency_key`. If the implementation cannot build a stable or deterministic key for an event, it must not write that event as a counted availability transition; log it as an ingestion error instead.

Ordering rule:

```text
Availability transition detection must process connection events in source-event
order for each serialNumber.

The preferred implementation is to produce Kafka connection messages in
source-event order with serialNumber as the Kafka message key, so all events for
one gateway are in one partition and retain gateway event order. If the source
topic cannot provide that guarantee, Analytics must add a bounded event-time
reorder window or an equivalent serialNumber-scoped ordering mechanism before
applying the transition state machine.

A newer same-state online event must not advance last_event_time in a way that
causes an older offline transition for the same serialNumber to be discarded
silently. If a deployment intentionally chooses best-effort counting instead of
per-serial ordering, that lower-accuracy behavior must be documented separately
and surfaced to callers; it must not be reported as exact availability counting.
```

Atomic transition and insert rule:

```text
For each valid connection message emitted by the per-serial ordering stage:
  BEGIN

  acquire a serialNumber-scoped transaction lock before reading state
  load device_availability_state row for serialNumber with write isolation
  if no state row exists:
    atomically create or lock the serialNumber state slot using one of:
      INSERT ... ON CONFLICT ... DO UPDATE/NOTHING followed by SELECT ... FOR UPDATE
      PostgreSQL advisory transaction lock
      serializable transaction with retry
    treat previous state as unknown

  -- Explicit scalar ordering comparison logic:
  if event_time < last_event_time:
    treat the message as stale
    do not insert an availability event
    do not update device_availability_state
    COMMIT
    stop processing this message

  if event_time == last_event_time:
    if both source_sequence and last_source_sequence are present (non-null BIGINT):
      if source_sequence < last_source_sequence:
        treat the message as stale
        do not insert an availability event
        do not update device_availability_state
        COMMIT
        stop processing this message

      if source_sequence == last_source_sequence:
        if idempotency_key equals last_idempotency_key:
          treat the message as an exact duplicate
        else:
          persist an ingestion ambiguity gap for this serialNumber, event_time, and source_sequence
          treat the message as ambiguous, not exact
        do not insert an availability event
        do not update device_availability_state
        COMMIT
        stop processing this message
    else:
      -- Equal timestamp without comparable non-null BIGINT sequences
      if idempotency_key equals last_idempotency_key:
        treat the message as an exact duplicate
      else:
        persist an ingestion ambiguity gap for this serialNumber and event_time
        treat the message as unordered / ambiguous, not exact
      do not insert an availability event
      do not update device_availability_state
      COMMIT
      stop processing this message

  -- event_time > last_event_time, or event_time == last_event_time with source_sequence > last_source_sequence:
  determine incomingState from message_type:
    ping or capabilities -> online
    disconnection -> offline

  if incomingState is online:
    if current_state is offline:
      INSERT online transition event with conflict-safe semantics
      if the insert created a row:
        update device_availability_state:
          current_state = online
          last_event_time = event_time
          last_source_sequence = source_sequence
          last_idempotency_key = idempotency_key
          updated_at = processing time
      else:
        treat as duplicate and do not update state
    else if current_state is online:
      update device_availability_state metadata, last_event_time, last_source_sequence, last_idempotency_key, and updated_at
      do not insert a transition event
    else:
      update device_availability_state to online, last_event_time, and last_source_sequence
      do not insert an initial online transition event

  if incomingState is offline:
    if current_state is online:
      INSERT offline transition event with conflict-safe semantics
      if the insert created a row:
        update device_availability_state:
          current_state = offline
          last_event_time = event_time
          last_source_sequence = source_sequence
          last_idempotency_key = idempotency_key
          updated_at = processing time
      else:
        treat as duplicate and do not update state
    else if current_state is offline:
      update device_availability_state metadata, last_event_time, last_source_sequence, last_idempotency_key, and updated_at
      do not insert a transition event
    else:
      update device_availability_state to offline, last_event_time, and last_source_sequence
      do not insert an initial offline transition event because the prior state is unknown

  COMMIT

or the equivalent database-specific "insert if absent" operation.

The storage method must return whether a row was inserted. Duplicate events that hit
the unique idempotency constraint must not be treated as new offline transitions and
must not move `device_availability_state` backward or forward.

This transaction shape assumes the selected ordering strategy has already made
the message eligible for state-machine processing. Do not apply this scalar
`last_event_time` staleness check directly to raw Kafka arrival order unless the
connection topic is keyed by serialNumber and source-event ordered.
```

Equivalent transaction shape:

```text
BEGIN;

Acquire a serialNumber-scoped transaction lock.
Atomically create or lock the device_availability_state row for :serialNumber.
Read:
  current_state
  last_event_time
  last_source_sequence
  last_idempotency_key

-- Validate scalar timestamp and sequence ordering key and determine whether state changed.
-- Insert into device_availability_events only if state changed.

Update device_availability_state only when timestamp, sequence, and idempotency checks allow it:
  current_state = :newState
  last_event_time = :eventTime
  last_source_sequence = :sourceSequence
  last_idempotency_key = :idempotencyKey
  updated_at = :processingTime
  board_id = :boardId when known, otherwise keep existing or NULL according to metadata policy
  metadata = source and processing metadata

COMMIT;
```

Staleness rule after ordering:

```text
An event is stale when `event_time < last_event_time`, or when `event_time == last_event_time` and both `source_sequence` and `last_source_sequence` are present (non-null BIGINT) with `source_sequence < last_source_sequence`.

Stale or non-advancing events must not insert counted availability events and must
not update device_availability_state, even if their idempotency_key has not been
seen before.

Two different logical events with the same `event_time` must be ordered by a
deterministic 64-bit numeric `source_sequence` (`BIGINT`). Exact Kafka redelivery
should be identified by the idempotency key. If two different logical events share
the same `event_time` and lack comparable non-null source sequences, or have identical
source sequences, Analytics must persist an ingestion ambiguity gap for that source time
and downgrade overlapping availability queries to partial/lower-bound coverage instead
of silently discarding one transition as non-advancing or guessing order arbitrarily.
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

Every ping should not be treated as a new online transition. However, every source-newer ping that is accepted by the per-serial ordering stage must still update `device_availability_state.last_event_time` and metadata.

Ping handling:

```text
current_state=online  + ping -> update last_event_time and metadata only
current_state=offline + ping after a prior offline state -> insert online event, set current_state=online, update last_event_time
no state row + ping -> create current_state=online, update last_event_time, do not insert an online event
```

Disconnection handling:

```text
current_state=online  + disconnection -> insert offline event, set current_state=offline, update last_event_time
current_state=offline + disconnection -> update last_event_time and metadata only when the source message is newer; do not insert another offline event
no state row + disconnection -> create current_state=offline, update last_event_time, do not insert an offline transition event
```

Example:

```text
12:00 first ping initializes current_state=online with no transition event
12:05 disconnection source event
12:10 ping source event
```

If Kafka delivers the `12:10` ping before the `12:05` disconnection and the
topic does not guarantee serialNumber ordering, Analytics must not immediately
advance `last_event_time` to `12:10` and then reject the delayed `12:05`
disconnection. With a bounded reorder window, both messages are ordered by
source time before transition processing:

```text
12:05 disconnection -> insert offline event, current_state=offline, last_event_time=12:05
12:10 ping          -> insert online event, current_state=online,  last_event_time=12:10
```

The final current state is online, and the outage from `12:05` to `12:10` remains
present in transition history. If the implementation lacks serial-keyed
source-event ordering or a bounded reorder window, this case is only best-effort
and must not be surfaced as exact availability counting.

Kafka delay example:

```text
Ping source time:          12:00
Ping processed:            12:05
Disconnection source time: 12:03
Disconnection processed:   12:06
```

The `12:03` disconnection is not stale because it is newer than `device_availability_state.last_event_time = 12:00`. It must not be rejected by comparing against processing-time `DeviceInfo.lastContact = 12:05`.

Review conclusion:

```text
The implementation requires `device_availability_state` unless a future design explicitly names an existing persistent table with the same fields and locking guarantees. `last_event_time` must be updated for every source-newer ping, capabilities, or disconnection message accepted by the per-serial ordering stage, even when no availability transition event is inserted. `DeviceInfo.lastContact` remains contact/processing metadata and is not the source-event ordering field.
```

### Calculation

```text
offline_count =
    COUNT(event_type = 'offline')
```

Availability migration boundary:

```text
availabilityValidFrom is a durable, deployment-wide timestamp loaded on service startup from:
  1. Configuration file property `availability_valid_from` (in /etc/ucentral/owanalytics.json)
  2. Environment variable `ANALYTICS_AVAILABILITY_VALID_FROM`
  3. Persistent database migration metadata table (`system_properties` key `availability_valid_from`) populated during initial table creation.

All service replicas and process restarts MUST load the identical `availabilityValidFrom` timestamp from these durable configuration/DB sources to guarantee multi-replica and restart consistency. Dynamic process-start timestamps (e.g. std::chrono::system_clock::now() at startup) are strictly prohibited.

If startTime < availabilityValidFrom:
  reject the request
  do not query device_availability_events
  do not return offline_count: 0

If startTime >= availabilityValidFrom:
  query device_availability_events normally
```

For `meta.coverage = "none"` and `meta.accuracy = "not_applicable"`, return
`data.offline_count = null`. Do not return a numeric `0` when the API has no
coverage proof for the requested interval.

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
      "firstSampleAt": null,
      "lastSampleAt": null
    },
    "sourceWindow": {
      "firstSampleAt": "2026-07-26T11:55:00Z",
      "lastSampleAt": "2026-07-26T11:55:00Z"
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
      "beforeStart": true,
      "atStart": false,
      "atEnd": false,
      "afterEnd": false
    },
    "availabilityCoverage": {
      "coverageStart": "2026-07-20T00:00:00Z",
      "stateKnownFrom": "2026-07-20T00:00:00Z",
      "processedThrough": "2026-07-27T12:01:00Z",
      "allowedIngestionDelaySeconds": 60,
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

An exact zero is valid only when availability coverage proves the complete requested interval up to `endTime`: `coverageStart <= startTime`, `processedThrough >= endTime`, `ingestionGapKnown = false`, and `proofSource != "unavailable"`. When `processedThrough < endTime` (even if `processedThrough >= endTime - allowedIngestionDelaySeconds`), the requested interval is not fully covered up to `endTime` and a zero matching event count must be reported as partial/lower-bound (`meta.coverage = "partial"`), or as unavailable with `offline_count: null` when no coverage proof exists.

For the availability endpoint, `sampleCount` is retained for response-shape
consistency with the other summary APIs. When `meta.coverage` is `"full"` or `"partial"`, `sampleCount` is defined as:

```text
sampleCount = offlineEventCount = offline_count
```

`sampleCount` is the number of offline transition rows in `device_availability_events`
contributing to `offline_count`. Online recovery transition rows (`event_type = 'online'`)
are stored in transition history for state tracking and `observedWindow` / `sourceWindow`
bounds, but they do not contribute to `sampleCount`, `offlineEventCount`, or `offline_count`.
For `"full"` or `"partial"` coverage, `sampleCount` always equals `offlineEventCount`.

When `meta.coverage = "none"` and `meta.accuracy = "not_applicable"`, `sampleCount`, `offlineEventCount`,
and `offline_count` are all `null`.

`boundarySamplesUsed.beforeStart` is `true` only when a pre-start boundary event
was actually selected. Event counting does not require a pre-start boundary
event when durable availability coverage proves the requested interval.

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

## OpenAPI

Update:

```text
openapi/owanalytics.yaml
```

Add:

```yaml
/devices/{routerId}/memory-summary:
/devices/{routerId}/radio-temperature-summary:
/devices/{routerId}/availability-summary:
/devices/{routerId}/wifi-clients/usage-summary:
/devices/{routerId}/wifi-clients/rssi-summary:
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

JSON field names should match the MCP CSV where the API is directly returning MCP fields. `usage-summary` additionally returns raw byte totals and usage accuracy fields so callers can distinguish exact usage from lower-bound estimates.

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

The helper must use the current `routerId -> boardId` map maintained by `VenueCoordinator` as the primary ownership source. OWPROV inventory lookup must be status-aware and is used to verify or refresh ownership on cache/map misses. Any refresh path must read `InventoryTag.venue` and resolve board ownership with `monitorSubVenues` support, using candidate board device lists through `SDK::Prov::Venue::GetDevices(..., VenueInfo.monitorSubVenues, ...)` when needed.

`availability-summary` is the storage-query exception, not an authorization exception. It must authenticate the caller, resolve current router ownership, and verify the caller has `analytics.gateway_metrics.read` on the resolved board, venue, or parent entity before querying availability storage. After authorization succeeds, the historical count must query availability storage by durable `serialNumber`, not by mandatory current `resolvedBoardId`. Event-time `board_id` is historical context only and must not authorize the request by itself.

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
| `get_device_bandwidth_consumption` | `GET /devices/{routerId}/wifi-clients/usage-summary` | `timepoints.ssid_data[].associations[]` | Resolve routerId to venueId and boardId, then calculate reset-safe cumulative-counter differentials with concise quality metadata by default; include segment provenance only when `includeCalculationDetails=true` |
| `get_device_rssi_quality` | `GET /devices/{routerId}/wifi-clients/rssi-summary` | `timepoints.ssid_data[].associations[].rssi` | Resolve routerId to venueId and boardId, then classify RSSI samples |
| `get_gateway_offline_count` | `GET /devices/{routerId}/availability-summary` | Existing gateway `connection` topic plus `device_availability_events` and `device_availability_state` | Use routerId as durable serialNumber, use `device_availability_state.current_state` and `last_event_time` for restart-safe transition detection, then count offline events by serialNumber |

---

# Recommended Implementation Order

1. `get_gateway_wifi_temp`
2. `get_device_rssi_quality`
3. `get_device_bandwidth_consumption`
4. `get_gateway_free_memory`
5. `get_gateway_offline_count`
