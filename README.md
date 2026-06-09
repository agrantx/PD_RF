<p align="center">
  <img src="images/banner.png" width="800">
</p>

<h1 align="center">📡 PD RF</h1>
<p align="center">
Portable RF Toolkit based on ESP32-C3
</p>



```text
██████╗ ██████╗     ██████╗ ███████╗
██╔══██╗██╔══██╗    ██╔══██╗██╔════╝
██████╔╝██║  ██║    ██████╔╝█████╗
██╔═══╝ ██║  ██║    ██╔══██╗██╔══╝
██║     ██████╔╝    ██║  ██║██║
╚═╝     ╚═════╝     ╚═╝  ╚═╝╚═╝
```

---------------------------------------------------------------------------------

## 🌐 Web Flasher 🌐 (beta)
[🌐 Open Web Flasher](https://agrantx.github.io/PD_RF/pd-rf-flasher.html)

---------------------------------------------------------------------------------

## 🌐 Web menu preview 🌐
[🌐 Open menu preview](https://agrantx.github.io/PD_RF/pd_rf_menu_preview.html)

---------------------------------------------------------------------------------

# 🔌 Pinout

### 📡 CC1101

| Signal | GPIO |
|---------|---------|
| CSN | GPIO 5 |
| GDO0 | GPIO 4 |
| GDO2 | GPIO 3 |
| MOSI | GPIO 7 |
| MISO | GPIO 2 |
| SCK | GPIO 6 |

### 🖥 OLED SSD1306

| Signal | GPIO |
|---------|---------|
| SDA | GPIO 9 |
| SCL | GPIO 10 |

### 💾 SD Card

| Signal | GPIO |
|---------|---------|
| CS | GPIO 8 |
| MOSI | GPIO 7 |
| MISO | GPIO 2 |
| SCK | GPIO 6 |

### 🔘 Buttons

| Button | GPIO |
|---------|---------|
| UP | GPIO 0 |
| DOWN | GPIO 1 |
| OK | GPIO 21 |

# ⚠️ Power Notes

NRF24L01 modules can be sensitive to power quality.

* Use a stable 3.3V supply
* Add a 10µF–100µF capacitor between VCC and GND near the module
* Do not connect VCC to 5V

# 📦 Hardware

* ESP32-C3
* CC1101
* OLED SSD1306 (I2C)
* MicroSD Card Module
* 3 Push Buttons

## Upgrading from Standard CC1101 to E07-433M20S

This project can be upgraded from a standard CC1101 module to the **E07-433M20S** high-power RF transceiver.

### What changes are required?

#coming soon

### Recommended additions

The E07-433M20S consumes significantly more power than a standard CC1101 module.

Recommended:

* Stable 3.3V power supply
* Dedicated voltage regulator
* 100µF–470µF capacitor between VCC and GND
* Quality 433MHz antenna
* Short power wires and solid grounding

The module can draw around 100mA during transmission at maximum power.

### Performance Improvements

The E07-433M20S is based on the CC1101 transceiver and includes a built-in Power Amplifier (PA).

Compared to a standard CC1101 module:

* Higher transmit power
* Better long-range performance
* Improved signal stability
* External antenna support
* Better performance in noisy RF environments

The module supports up to **20 dBm (100mW)** output power and receiver sensitivity down to **-109 dBm**. EBYTE specifies communication distances of up to approximately **2 km** under ideal open-field conditions with a suitable antenna.

### Why upgrade?

✔ More transmit power

✔ Better range

✔ Stronger signal reception

✔ External antenna support

✔ Improved reliability

For users who want maximum Sub-GHz performance, the **E07-433M20S** is a significant upgrade over a standard CC1101 module and is the recommended choice for long-range applications.
