<p align="center">
    <img src="images/project/logo.svg" width="200"/>
</p>

# OpenWiFI Analytics Service (OWANALYTICS)

## What is it?
The OpenWiFi Analytics Service is a service for the TIP OpenWiFi CloudSDK (OWSDK).
OWANALYTICS gathers statistics about device used in OpenWiFI and groups them according to their 
provisioning (OWPROV) entities or venues. OWANALYTICS, like all other OWSDK microservices, is
defined using an OpenAPI definition and uses the ucentral communication protocol to interact with Access Points. To use
the OWANALYTICS, you need to use the Docker.

## OpenAPI
The OWANALYTICS REST API is defined in [`openapi/owanalytics.yaml`](https://github.com/routerarchitects/wlan-cloud-analytics/blob/main/openapi/owanalytics.yaml). You can use this OpenAPI definition to generate static API documentation, inspect the available endpoints, or build client SDKs.

## Docker
To use the CLoudSDK deployment please follow [here](https://github.com/Telecominfraproject/wlan-cloud-ucentral-deploy)

### OWANALYTICS Service Configuration
The configuration is kept in a file called `owanalytics.properties`. To understand the content of this file,
please look [here](https://github.com/routerarchitects/ra-wlan-cloud-analytics/blob/main/owanalytics.properties)

## Kafka topics
To read more about Kafka, follow the [document](https://github.com/Telecominfraproject/wlan-cloud-ucentralgw/blob/main/KAFKA.md)

| OWANALYTICS | Analytics Service | [here](https://github.com/routerarchitects/wlan-cloud-analytics) | [here](https://github.com/routerarchitects/wlan-cloud-analytics/blob/main/openapi/owanalytics.yaml) |
| OWSUB | Subscriber Service | [here](https://github.com/routerarchitects/wlan-cloud-userportal) | [here](https://github.com/routerarchitects/wlan-cloud-userportal/blob/main/openapi/userportal.yaml) |
