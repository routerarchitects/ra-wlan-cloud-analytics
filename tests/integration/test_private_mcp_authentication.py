"""Live C++ private-router tests; require an isolated DB and fake OWSEC/OWPROV.

Set OWANALYTICS_TEST_PRIVATE_URL, OWANALYTICS_TEST_PRIVATE_KEY,
OWANALYTICS_TEST_PRIVATE_CA, and the existing OWANALYTICS_TEST_DB_DSN.
These tests do not substitute an HTTP mock for the Analytics router.
"""

import json
import os
import ssl
import urllib.error
import urllib.parse
import urllib.request

import pytest

from test_memory_summary_api import (
    UNAUTHORIZED_ROUTER_ID,
    default_router_id,
    env_or_skip,
    recent_timestamp,
    seeded_board,
    valid_token,
)


pytestmark = pytest.mark.integration

ENDPOINTS = [
    "memory-summary",
    "radio-temperature-summary",
    "wifi-clients/usage-summary",
    "wifi-clients/rssi-summary",
    "availability-summary",
]


def private_request(endpoint, token, key, name, router=None):
    base = env_or_skip("OWANALYTICS_TEST_PRIVATE_URL")
    query = urllib.parse.urlencode(
        {"timestampTill": recent_timestamp(), "lookbackHours": "1"}
    )
    router = router or default_router_id()
    url = f"{base}/api/v1/devices/{router}/{endpoint}?{query}"
    headers = {"Accept": "application/json"}
    if token is not None:
        headers["Authorization"] = "Bearer " + token
    if key is not None:
        headers["X-API-KEY"] = key
    if name is not None:
        headers["X-INTERNAL-NAME"] = name
    context = ssl.create_default_context(cafile=os.environ.get("OWANALYTICS_TEST_PRIVATE_CA"))
    request = urllib.request.Request(url, headers=headers)
    try:
        with urllib.request.urlopen(request, timeout=10, context=context) as response:
            return response.status, json.loads(response.read())
    except urllib.error.HTTPError as error:
        return error.code, json.loads(error.read())


@pytest.mark.parametrize("endpoint", ENDPOINTS)
def test_private_router_requires_service_and_user_authorization(endpoint, seeded_board):
    env_or_skip("OWANALYTICS_TEST_PRIVATE_URL")
    key = env_or_skip("OWANALYTICS_TEST_PRIVATE_KEY")
    caller = "https://mango-integration.example.com"

    status, _ = private_request(endpoint, valid_token(), key, caller)
    assert status == 200

    # A valid user's token cannot replace the private interface's service key.
    for supplied_key, supplied_name in [(None, caller), ("invalid-key", caller), (key, None), (key, "")]:
        status, _ = private_request(endpoint, valid_token(), supplied_key, supplied_name)
        assert status == 403

    # A valid service key cannot replace or repair the user's Bearer token.
    for token in [None, "invalid-user-token"]:
        status, _ = private_request(endpoint, token, key, caller)
        assert status == 401

    # The existing resolver intentionally masks OWPROV's resource denial as 404.
    status, body = private_request(endpoint, valid_token(), key, caller, UNAUTHORIZED_ROUTER_ID)
    assert status == 404
    assert body["error"] == "not_found"
