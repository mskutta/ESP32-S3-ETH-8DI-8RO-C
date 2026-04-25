#pragma once
#include <Arduino.h>

#define ETH_SPI_SCK  15
#define ETH_SPI_MISO 14
#define ETH_SPI_MOSI 13
#define ETH_PHY_ADDR 1
#define ETH_PHY_CS   16
#define ETH_PHY_IRQ  12
#define ETH_PHY_RST  39

void ETH_Init(void);
void ETH_Loop(void);
bool ETH_Connected(void);
IPAddress ETH_LocalIP(void);
void ETH_SetHostname(const char *hostname);
