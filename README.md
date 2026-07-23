<p align="center">
  <img src="images/project/logo.svg" height="170" align="middle" alt="TIP OpenWiFi Logo" />
  &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;
  <img src="images/project/mango-logo.png" height="90" align="middle" alt="Mango Cloud Logo" />
</p>

# OpenWiFi Analytics Service (OWANALYTICS)

## Overview
The OpenWiFi Analytics Service (OWANALYTICS) is a core microservice within the Telecom Infra Project (TIP) OpenWiFi CloudSDK (OWSDK) ecosystem.

OWANALYTICS gathers real-time statistics and telemetry reports from connected devices, aggregating metrics (like CPU load, memory usage, radio noise, and client associations) and grouping them logically according to their provisioning entities or venues.

## Role in Mango Cloud
This service is part of [Mango Cloud](https://www.mangowifi.cloud/), Router Architects’ open-source platform for managed Wi-Fi and connectivity operations.

Within Mango Cloud, **OWANALYTICS** serves as the central **Device Analytics & Telemetry Aggregator**.

Key integrations include:
* **AP Status Tracker**: Listens to periodic status/telemetry payloads sent by Access Points via the Controller Service (`owgw`).
* **KPI Metrics Calculator**: Aggregates CPU load, memory, temperature, and channel noise profiles, grouping them by MDU property or venue to generate historical graphs.
* **Client Association Logger**: Records client connection history, data bandwidth (Rx/Tx bytes), signal strengths (RSSI), and MCS rates across 2G/5G/6G radio bands.
* **Intra-Microservice Auth**: Uses access tokens issued by the Security Service (`owsec`) to secure telemetry validation APIs.

### Resources
* [Mango Cloud Website](https://www.mangowifi.cloud/)
* [Mango Cloud Deployment Guide](https://github.com/routerarchitects/mango-cloud-deployment)
* [Router Architects GitHub Organization](https://github.com/routerarchitects)

### Telemetry Guides
* [Telemetry & Monitoring](https://www.mangowifi.cloud/docs/operations/device-operations-owgw/telemetry-monitoring)

## OpenAPI
The OpenAPI definition is available directly in the [GitHub repository](openapi/owanalytics.yaml).
You can load the [raw OpenAPI definition file](https://raw.githubusercontent.com/routerarchitects/ra-wlan-cloud-analytics/main/openapi/owanalytics.yaml) into [Swagger UI](https://petstore.swagger.io/) to view interactive API docs.

## Building
To build the microservice from source, please follow the instructions in [BUILDING.md](./BUILDING.md).

## Docker
To run this service as part of the CloudSDK containerized deployment, refer to the [mango-cloud-deployment](https://github.com/routerarchitects/mango-cloud-deployment) repository instructions.

## Firewall Considerations
The microservice exposes three main network interfaces. Ensure your firewall or container ingress rules are configured accordingly:

| Port | Description | Configurable |
| :--- | :--- | :---: |
| `16009` | Default port for public REST API Access to the OWANALYTICS | yes |
| `17009` | Default port for internal REST API Access (intra-service communications) | yes |
| `16109` | Default port for Application Load Balancer (ALB) Health Checks | yes |

### OWANALYTICS Service Configuration
All configuration parameters are stored in the `owanalytics.properties` file. For a complete guide to all available properties, check [CONFIGURATION.md](./CONFIGURATION.md).

## Kafka topics
The service uses Kafka to ingest device telemetry payloads and coordinate with other microservices. To read more about the Kafka integration, review [KAFKA.md](https://github.com/Telecominfraproject/wlan-cloud-ucentralgw/blob/main/KAFKA.md).

## Contributions
We welcome and appreciate community contributions. Please review [CONTRIBUTING.md](./CONTRIBUTING.md) to get started.

## Pull Requests
When submitting pull requests:
1. Create a branch named after the JIRA issue you are resolving or the feature you are implementing.
2. Submit your pull request targeting the `main` branch.

## Additional OWSDK Microservices
Here is the index of additional OpenWiFi microservices:

| Name | Description | Link | OpenAPI |
| :--- | :--- | :---: | :---: |
| **OWSEC** | Security Service | [here](https://github.com/routerarchitects/ra-wlan-cloud-ucentralsec) | [here](https://github.com/routerarchitects/ra-wlan-cloud-ucentralsec/blob/main/openapi/owsec.yaml) |
| **OWGW** | Controller Service | [here](https://github.com/routerarchitects/ra-wlan-cloud-ucentralgw) | [here](https://github.com/routerarchitects/ra-wlan-cloud-ucentralgw/blob/main/openapi/owgw.yaml) |
| **OWFMS** | Firmware Management Service | [here](https://github.com/routerarchitects/ra-wlan-cloud-ucentralfms) | [here](https://github.com/routerarchitects/ra-wlan-cloud-ucentralfms/blob/main/openapi/owfms.yaml) |
| **OWPROV** | Provisioning Service | [here](https://github.com/routerarchitects/ra-wlan-cloud-owprov) | [here](https://github.com/routerarchitects/ra-wlan-cloud-owprov/blob/main/openapi/owprov.yaml) |
| **OWANALYTICS** | Analytics Service | [here](https://github.com/routerarchitects/ra-wlan-cloud-analytics) | [here](https://github.com/routerarchitects/ra-wlan-cloud-analytics/blob/main/openapi/owanalytics.yaml) |
| **OWSUB** | Subscriber Portal Service | [here](https://github.com/routerarchitects/ra-wlan-cloud-userportal) | [here](https://github.com/routerarchitects/ra-wlan-cloud-userportal/blob/main/openapi/userportal.yaml) |
| **NW-Topology** | Network Topology Service | [here](https://github.com/routerarchitects/ra-openlan-nw-topology) | [here](https://github.com/routerarchitects/ra-openlan-nw-topology/blob/main/openapi/nwtopology.yaml) |
