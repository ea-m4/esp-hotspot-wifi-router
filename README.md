# ESP32 WiFi Hotspot Bridge

Turn any ESP32 into a WiFi repeater — connects to an upstream router and re-shares the internet through its own Access Point. Fully configurable at runtime via a built-in web UI; no reflashing required.

```
[Phone / PC]
     |  WiFi (AP client)
[ESP32 Access Point]
     |  NAT (LWIP NAPT)
[ESP32 Station]
     |  WiFi (upstream client)
[Your Router] ── Internet
```

---

## Features

| Feature | Details |
|---|---|
| **WiFi repeater / NAT bridge** | AP+STA dual mode with LWIP NAPT |
| **Web configuration portal** | `http://192.168.4.1` — no app needed |
| **Persists across reboots** | All settings stored in NVS flash |
| **DNS selection** | Router auto / Cloudflare 1.1.1.1 / AdGuard / Custom |
| **Local DNS ad filter** | ~35 ad-network domains blocked on-device |
| **WiFi 6 (802.11ax)** | Auto-enabled on C6 / C61 / C5 chips |
| **Dual-band 2.4 + 5 GHz** | Auto-enabled on C5 chip |
| **HT40 / 40 MHz channels** | Enabled where supported (~2× throughput) |
| **WPA2 + WPA3 transition mode** | AP accepts both WPA2 and WPA3 clients |

### Ad blocking — what works and what doesn't

DNS filtering blocks banner ads, tracking pixels, and pop-ups on most websites. It **cannot** block YouTube in-stream video ads — YouTube serves ads from the same domains (`youtube.com`, `googlevideo.com`) as actual video content. Use the [uBlock Origin](https://ublockorigin.com) browser extension for YouTube.

For broader DNS-level filtering without local flash storage, select **AdGuard DNS** in the web UI — it filters tens of thousands of domains server-side.

---

## Supported hardware

Any ESP32 variant supported by ESP-IDF 5.x:

| Chip | WiFi generation | Bands |
|---|---|---|
| ESP32, S2, S3, C3 | WiFi 4 (802.11n) | 2.4 GHz |
| ESP32-C6, C61 | WiFi 6 (802.11ax) | 2.4 GHz |
| ESP32-C5 | WiFi 6 (802.11ax) | 2.4 + 5 GHz |

---

## Requirements

- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/) **v5.2 or later**
- Any ESP32-series development board

---

## Quick start

### 1 — Clone and enter the project

```bash
git clone https://github.com/ea-m4/esp-hotspot-wifi-router.git
cd esp-hotspot-wifi-router
```

### 2 — Set up the IDF environment

```bash
. $IDF_PATH/export.sh
```

### 3 — Set your target chip

```bash
idf.py set-target esp32s3   # or esp32, esp32c3, esp32c6, …
```

### 4 — Build and flash

```bash
idf.py build flash monitor
```

### 5 — Configure

1. On your phone or PC, connect to the WiFi network **`ESP32-Bridge`** (password: `bridge123`).
2. Open **`http://192.168.4.1`** in a browser.
3. Fill in your upstream router's SSID and password, choose a hotspot name/password, pick a DNS mode.
4. Press **Save & Restart**.
5. Reconnect to the (possibly renamed) hotspot — internet access is now bridged.

---

## Project layout

```
esp-hotspot-wifi-router/
├── main/
│   ├── wifi-hotspot_bridge.c   # app_main — init & orchestration only
│   ├── config.h / config.c     # bridge_config_t, NVS load/save
│   ├── wifi_bridge.h / .c      # WiFi AP+STA setup, NAT, event handler
│   ├── web_config.h / .c       # HTTP server, HTML config page
│   ├── dns_filter.h / .c       # DNS proxy + ad-block list
│   └── CMakeLists.txt
├── sdkconfig.defaults           # NAPT + TCP/UDP buffer tuning
├── CMakeLists.txt
├── .gitignore
├── LICENSE
└── README.md
```

---

## Configuration reference

All fields are saved to NVS and survive reboots. Defaults used on first boot:

| Field | Default | Notes |
|---|---|---|
| Upstream SSID | *(empty)* | Set via web UI |
| Upstream password | *(empty)* | |
| Hotspot SSID | `ESP32-Bridge` | |
| Hotspot password | `bridge123` | Leave empty for open network |
| AP channel | 1 | 1–13 |
| Max AP clients | 5 | 1–10 |
| DNS mode | Router auto | See options below |
| Custom DNS | `8.8.8.8` | Active only when DNS = Custom |
| Ad filter | Off | Local proxy on 192.168.4.1:53 |

### DNS modes

| Mode | Server | Notes |
|---|---|---|
| Router auto | Upstream DHCP-assigned | Default |
| Cloudflare | `1.1.1.1` | Fast, privacy-focused |
| AdGuard | `94.140.14.14` | Server-side ad/tracker blocking |
| Custom | User-defined IP | Any valid DNS server |

---

## Performance tuning

`sdkconfig.defaults` sets several LWIP tunables at first `idf.py build`:

```
CONFIG_LWIP_IP_FORWARD=y
CONFIG_LWIP_IPV4_NAPT=y
CONFIG_LWIP_TCP_SND_BUF_DEFAULT=65535
CONFIG_LWIP_TCP_WND_DEFAULT=65535
CONFIG_LWIP_TCP_RECVMBOX_SIZE=12
CONFIG_LWIP_UDP_RECVMBOX_SIZE=12
CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=16
CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=32
```

To apply after a first build: `idf.py fullclean && idf.py build`.

---

## Resetting to factory defaults

Hold the **EN/RST** button or run from the IDF monitor:

```bash
idf.py erase-flash
idf.py flash
```

This wipes NVS and restores the built-in defaults above.

---

## License

MIT — see [LICENSE](LICENSE).
