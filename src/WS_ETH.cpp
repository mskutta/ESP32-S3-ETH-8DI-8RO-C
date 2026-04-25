#include "WS_ETH.h"
#include <WiFi.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_eth.h"
#include "esp_eth_com.h"
#include "esp_eth_netif_glue.h"
#include "esp_event.h"
#include "esp_interface.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_defaults.h"

static bool eth_connected = false;
static bool eth_started = false;
static IPAddress eth_ip;
static String eth_hostname = "esp32-eth0";
static esp_eth_handle_t eth_handle = nullptr;
static esp_netif_t *eth_netif = nullptr;
static spi_device_handle_t spi_handle = nullptr;
static esp_eth_netif_glue_handle_t eth_glue = nullptr;

extern void add_esp_interface_netif(esp_interface_t interface, esp_netif_t *esp_netif);

static void handleEthEvent(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  (void)arg;
  (void)event_base;
  (void)event_data;

  switch (event_id) {
    case ETHERNET_EVENT_START:
      Serial.println("ETH Started");
      if (eth_netif != nullptr) {
        esp_netif_set_hostname(eth_netif, eth_hostname.c_str());
      }
      break;

    case ETHERNET_EVENT_CONNECTED:
      Serial.println("ETH Connected");
      if (eth_netif != nullptr) {
        esp_err_t dhcp_err = esp_netif_dhcpc_start(eth_netif);
        if (dhcp_err != ESP_OK && dhcp_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
          Serial.printf("ETH: dhcpc_start failed (%d)\n", dhcp_err);
        } else {
          Serial.println("ETH: DHCP client started");
        }
      }
      break;

    case ETHERNET_EVENT_DISCONNECTED:
      Serial.println("ETH Disconnected");
      eth_connected = false;
      eth_ip = IPAddress();
      break;

    case ETHERNET_EVENT_STOP:
      Serial.println("ETH Stopped");
      eth_connected = false;
      eth_started = false;
      eth_ip = IPAddress();
      break;

    default:
      break;
  }
}

static void handleIpEvent(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  (void)arg;
  (void)event_base;

  if (event_id != IP_EVENT_ETH_GOT_IP || event_data == nullptr) {
    return;
  }

  auto *event = static_cast<ip_event_got_ip_t *>(event_data);
  eth_connected = true;
  eth_ip = IPAddress(event->ip_info.ip.addr);
  Serial.printf("ETH Got IP: %u.%u.%u.%u\n", eth_ip[0], eth_ip[1], eth_ip[2], eth_ip[3]);
}

void ETH_Init(void) {
  if (eth_started) {
    return;
  }

  Serial.println("ETH: Starting W5500 via ESP-IDF");

  esp_err_t err = esp_netif_init();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    Serial.printf("ETH: esp_netif_init failed (%d)\n", err);
    return;
  }

  err = esp_event_loop_create_default();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    Serial.printf("ETH: esp_event_loop_create_default failed (%d)\n", err);
    return;
  }

  spi_bus_config_t bus_config = {};
  bus_config.miso_io_num = ETH_SPI_MISO;
  bus_config.mosi_io_num = ETH_SPI_MOSI;
  bus_config.sclk_io_num = ETH_SPI_SCK;
  bus_config.quadwp_io_num = -1;
  bus_config.quadhd_io_num = -1;
  bus_config.max_transfer_sz = 4096;

  err = spi_bus_initialize(SPI2_HOST, &bus_config, SPI_DMA_CH_AUTO);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    Serial.printf("ETH: spi_bus_initialize failed (%d)\n", err);
    return;
  }

  spi_device_interface_config_t dev_config = {};
  dev_config.command_bits = 16;
  dev_config.address_bits = 8;
  dev_config.mode = 0;
  dev_config.clock_speed_hz = 12 * 1000 * 1000;
  dev_config.spics_io_num = ETH_PHY_CS;
  dev_config.queue_size = 20;

  err = spi_bus_add_device(SPI2_HOST, &dev_config, &spi_handle);
  if (err != ESP_OK) {
    Serial.printf("ETH: spi_bus_add_device failed (%d)\n", err);
    return;
  }

  err = gpio_install_isr_service(0);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    Serial.printf("ETH: gpio_install_isr_service failed (%d)\n", err);
    return;
  }

  eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
  eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(spi_handle);
  w5500_config.int_gpio_num = ETH_PHY_IRQ;
  esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);

  eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
  phy_config.phy_addr = ETH_PHY_ADDR;
  phy_config.reset_gpio_num = ETH_PHY_RST;
  esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);

  if (mac == nullptr || phy == nullptr) {
    Serial.println("ETH: failed to create MAC/PHY");
    return;
  }

  esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
  err = esp_eth_driver_install(&eth_config, &eth_handle);
  if (err != ESP_OK) {
    Serial.printf("ETH: esp_eth_driver_install failed (%d)\n", err);
    return;
  }

  uint8_t eth_mac[6];
  err = esp_read_mac(eth_mac, ESP_MAC_ETH);
  if (err == ESP_OK) {
    err = esp_eth_ioctl(eth_handle, ETH_CMD_S_MAC_ADDR, eth_mac);
    if (err != ESP_OK) {
      Serial.printf("ETH: set MAC failed (%d)\n", err);
    } else {
      Serial.printf(
        "ETH MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
        eth_mac[0], eth_mac[1], eth_mac[2], eth_mac[3], eth_mac[4], eth_mac[5]
      );
    }
  } else {
    Serial.printf("ETH: esp_read_mac failed (%d)\n", err);
  }

  esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
  eth_netif = esp_netif_new(&netif_config);
  if (eth_netif == nullptr) {
    Serial.println("ETH: esp_netif_new failed");
    return;
  }

  err = esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &handleEthEvent, nullptr);
  if (err != ESP_OK) {
    Serial.printf("ETH: ETH event register failed (%d)\n", err);
    return;
  }

  err = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &handleIpEvent, nullptr);
  if (err != ESP_OK) {
    Serial.printf("ETH: IP event register failed (%d)\n", err);
    return;
  }

  eth_glue = esp_eth_new_netif_glue(eth_handle);
  err = esp_netif_attach(eth_netif, eth_glue);
  if (err != ESP_OK) {
    Serial.printf("ETH: esp_netif_attach failed (%d)\n", err);
    return;
  }

  add_esp_interface_netif(ESP_IF_ETH, eth_netif);

  esp_netif_set_hostname(eth_netif, eth_hostname.c_str());

  err = esp_eth_start(eth_handle);
  if (err != ESP_OK) {
    Serial.printf("ETH: esp_eth_start failed (%d)\n", err);
    return;
  }

  eth_started = true;
}

void ETH_Loop(void) {
  // Event-driven on ESP32; kept for compatibility with the main loop.
}

bool ETH_Connected(void) {
  return eth_connected;
}

IPAddress ETH_LocalIP(void) {
  return eth_ip;
}

void ETH_SetHostname(const char *hostname) {
  if (hostname && hostname[0] != '\0') {
    eth_hostname = hostname;
  }
}
