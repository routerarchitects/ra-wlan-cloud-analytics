# Analytics APIs for MCP Tools

This document defines the request format, response format, and implementation logic for the following MCP tools in the `ra-wlan-cloud-analytics` service:

- `get_gateway_free_memory`
- `get_gateway_wifi_temp`
- `get_device_bandwidth_consumption`
- `get_device_rssi_quality`
- `get_gateway_offline_count`

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

The Analytics API should calculate the requested time range as:

```text
end_time = parse(timestamp_till)
start_time = end_time - (lookback_hours * 3600)
```

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

The public MCP-facing parameters use ISO-8601/RFC3339 time, but the existing Analytics API style uses integer `fromDate` and `endDate` timestamps. Handlers should convert the request as:

```text
timestampTill: RFC3339 UTC string, for example 2026-07-27T12:00:00Z
endTime:       Unix epoch seconds parsed from timestampTill
startTime:     endTime - (lookbackHours * 3600)
```

Validation rules:

```text
timestampTill must parse as a valid timestamp.
lookbackHours must be greater than 0.
maxLookbackHours = floor(configured monitoringDuration / 3600).
lookbackHours must be less than or equal to maxLookbackHours.
startTime must be less than endTime.
```

All APIs in this document derive `maxLookbackHours` from the configured `monitoringDuration` for the resolved router ownership scope. All APIs share this limit unless an endpoint section explicitly names a stricter limit. No endpoint currently defines a separate limit.

For a one-year monitoring configuration, `maxLookbackHours` is `365 * 24` only when the configured monitoring duration is exactly 365 days. Do not assume every calendar year is 8760 hours; use the configured duration and effective retention timestamps when validating the request.

Return `400 Bad Request` for invalid timestamps, unsupported timezones, non-positive `lookbackHours`, or values above the applicable maximum. Internally, query `timepoints.timestamp` and `wificlienthistory.timestamp` using epoch seconds.

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
if (memory.has("free")) {
    resource.memory_free = memory["free"].as<uint64_t>();
}
if (memory.has("total")) {
    resource.memory_total = memory["total"].as<uint64_t>();
}
if (memory.has("cached")) {
    resource.memory_cached = memory["cached"].as<uint64_t>();
}
if (memory.has("buffered")) {
    resource.memory_buffered = memory["buffered"].as<uint64_t>();
}
DTP.resource_data = resource;
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

Return:
  min_memfree = min(memory_free samples)
  max_memfree = max(memory_free samples)
  avg_memfree = sum(memory_free samples) / sample_count
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

Required ingestion rule:

```text
If the source radio temperature is present and non-null:
  store radios[].wifi_temp = source temperature

If the source radio temperature is missing or null:
  omit radios[].wifi_temp or store radios[].wifi_temp = null

Do not synthesize a numeric fallback temperature for missing data.
Do not store a placeholder in wifi_temp for a missing temperature.
```

Migration boundary rule:

```text
Define a fixed temperature_migration_cutover_time as the deployment/migration
timestamp where radios[].wifi_temp starts being written consistently.

Only use temperature records created at or after temperature_migration_cutover_time.
Ignore all earlier records because historical temperature values cannot reliably
distinguish measured values from synthetic fallback values.

If the requested range starts before temperature_migration_cutover_time:
  effective_start_time = temperature_migration_cutover_time
else:
  effective_start_time = startTime

Do not filter out post-cutover samples only because the measured value is 20°C.
After the cutover, a present wifi_temp value is treated as a legitimate
measurement.
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

Use the current `timepoints.radio_data` JSON array:

```text
Load TimePointDB records where:
  boardId == resolvedBoardId
  stored serialNumber == request routerId
  timestamp >= effective_start_time
  timestamp < endTime

For each record:
  parse radio_data
  for each radio in radio_data:
    if radio.band is 2 or 5:
      if radio.wifi_temp is present and non-null:
        add radio.wifi_temp to that band's sample list

For each band:
  min_temperature = min(samples)
  max_temperature = max(samples)
  avg_temperature = sum(samples) / sample_count
```

A valid temperature sample is:

```text
record timestamp is at or after temperature_migration_cutover_time
radio.wifi_temp is present and non-null
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

data_consume_rx = SUM(stream rx_bytes segment differentials or lower-bound deltas)
data_consume_tx = SUM(stream tx_bytes segment differentials or lower-bound deltas)
total_data_usage = data_consume_rx + data_consume_tx

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
`lower_bound`. Include a client in the response when it has at least one
qualifying sample in or bounding the requested interval, even if the safely
provable lower-bound delta is 0. If no differential segment can be calculated
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
  BSSID/SSID/band/radio tie-breakers

Deduplicate exact duplicate samples before calculating deltas.

Discard out-of-order samples for the same calculated stream.

When an association/session identifier is available:
  calculate deltas only within the same session.

When no session identifier is available:
  use station MAC as the result grouping key,
  but use BSSID/SSID/band/radio changes as stream boundaries when possible.

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

If 10:10 is a confirmed new session that started inside the request window:
  add 200
  total rx_bytes = 700
  usage_accuracy = exact, unless another stream is incomplete

If 10:10 is an ambiguous decrease:
  add 0
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

The MCP output expects strings such as:

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

### Query Flow

```text
Resolve boardId from routerId
    ↓
Load TimePointDB records for resolvedBoardId and stored serialNumber == request routerId
    ↓
Include records from startTime up to but not including endTime
    ↓
Also load the latest pre-window association sample for each calculated stream_key
    ↓
Parse ssid_data associations
    ↓
Build stream_key values and sort samples by stream_key and timestamp
    ↓
Calculate reset-safe RX/TX deltas
    ↓
Aggregate stream-level deltas by station MAC
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
  "gw_uuid": "60cf84f22290",
  "fetch_status": "success",
  "offline_count": 6
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

Add a dedicated storage class for availability events:

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
  metadata        TEXT

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
  last_idempotency_key TEXT
  updated_at           BIGINT
  metadata             TEXT

Indexes:
  availability_state_board_index:
    board_id ASC

Constraints:
  current_state must be one of: online, offline, unknown
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

Add a DB upgrade/migration path for the new table. Existing deployments will start with no historical availability events. Define a fixed `availabilityValidFrom` timestamp as the deployment/migration time when availability-event persistence starts. The API should return `offline_count: 0` only for successful empty queries whose requested range starts at or after `availabilityValidFrom`; do not infer old events from `lastDisconnection`.

Add the files to `CMakeLists.txt`.

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

Bootstrap rule when no persisted state exists:

```text
First observed disconnection:
  insert one offline event
  set current_state = offline

First observed ping/capabilities:
  set current_state = online
  do not insert an online event

First observed unrecognized connection message:
  set current_state = unknown
  do not insert a counted event
```

In-memory transition checking is only an optimization. It is not sufficient for correctness because Kafka can redeliver messages and the service can restart after losing in-memory state. Transition detection must use `device_availability_state` or derive the latest known state from `device_availability_events` under the same transaction before writing. Duplicate connection messages must be rejected by storage-level idempotency before they can affect `offline_count`.

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

Atomic transition and insert rule:

```text
For each valid connection message:
  begin transaction
  load the persisted state row for serialNumber with write/transaction isolation
  if no state row exists:
    treat previous state as unknown
  determine the new state from the message
  if state row exists and event_time is older than last_event_time:
    treat the message as stale
    do not insert an availability event
    do not update device_availability_state
    commit transaction
    stop processing this message
  if state row exists and event_time equals last_event_time and no reliable tie-breaker proves this event is later:
    treat the message as duplicate or non-advancing
    do not insert a counted availability event
    do not update device_availability_state
    commit transaction
    stop processing this message
  if previous state is unknown:
    if new state is offline:
      insert one offline event with conflict-safe semantics
      if the insert created a row:
        update device_availability_state to offline and last_event_time
      else:
        do not update device_availability_state
    else if new state is online:
      update device_availability_state to online and last_event_time
      do not insert an online event
    else:
      update device_availability_state to unknown and last_event_time
      do not insert a counted event
  else if previous state differs from new state:
    insert availability event with conflict-safe semantics:
      INSERT ... ON CONFLICT(idempotency_key) DO NOTHING
    if the insert created a row:
      update device_availability_state to the new state and last_event_time
    else:
      treat it as a duplicate
      do not update device_availability_state
  else if previous state equals new state:
    update last contact metadata only when event_time is newer than last_event_time
    do not insert a counted transition event
  commit transaction

or the equivalent database-specific "insert if absent" operation.

The storage method must return whether a row was inserted. Duplicate events that hit
the unique idempotency constraint must not be treated as new offline transitions and
must not move `device_availability_state` backward or forward.
```

Staleness rule:

```text
An event is stale when its event_time is older than the persisted state's
last_event_time for the same serialNumber. An event with the same event_time is
non-advancing unless a deterministic source tie-breaker proves it happened later.

Stale or non-advancing events must not insert counted availability events and must
not update device_availability_state, even if their idempotency_key has not been
seen before.

For equal timestamps, use deterministic tie-breakers if the source provides them.
If no reliable tie-breaker exists, process at most one state-changing event for that
serialNumber and timestamp.
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

Every ping should not be treated as a new online transition.

### Calculation

```text
offline_count =
    COUNT(event_type = 'offline')
```

Availability migration boundary:

```text
availabilityValidFrom = deployment timestamp when device_availability_events
persistence became active

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
  "gw_uuid": "60cf84f22290",
  "fetch_status": "success",
  "offline_count": 0
}
```

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

Do not return `fetch_status: failed` with HTTP 200 for internal failures.

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
struct ClientRssiQuality;
struct GatewayOfflineSummary;
```

JSON field names should match the MCP CSV where the API is directly returning MCP fields. `usage-summary` additionally returns raw byte totals and usage accuracy fields so callers can distinguish exact usage from lower-bound estimates.

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
| `get_gateway_offline_count` | `GET /devices/{routerId}/availability-summary` | Existing gateway `connection` topic plus `device_availability_events` | Use routerId as durable serialNumber, persist restart-safe state transitions, then count offline events by serialNumber |

---

# Recommended Implementation Order

1. `get_gateway_wifi_temp`
2. `get_device_rssi_quality`
3. `get_device_bandwidth_consumption`
4. `get_gateway_free_memory`
5. `get_gateway_offline_count`
