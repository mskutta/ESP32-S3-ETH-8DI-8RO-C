#pragma once

#include <Arduino.h>

void MDNS_Configure(const char *hostname, uint16_t oscTcpPort);
void MDNS_Loop(void);
bool MDNS_QlabAvailable(void);
size_t MDNS_QlabCount(void);
IPAddress MDNS_QlabIP(size_t index);
uint16_t MDNS_QlabPort(size_t index);
bool MDNS_QlabTarget(size_t index, char *ipString, size_t ipStringSize, uint16_t *port);
