# 📱 ESP32 ANCS OLED Notifier

**ESP32 BLE Apple Notification Center Service (ANCS)** client that connects to your iPhone, displays incoming notifications on a **128×64 SSD1306 OLED**, shows a **real battery meter (Nokia-style vertical icon)**, and falls back to a **clock with animation** when idle.

This project brings smartwatch-like functionality to your ESP32 board — no Wi-Fi needed, just Bluetooth LE.

---

## ✨ Features

- 🧠 **Apple ANCS Client** — receive iPhone notifications over BLE  
- 💬 **WhatsApp & Social Support** — sender + message shown on OLED  
- ⏰ **Clock Screen** — auto-switches after 20 s of inactivity, with animated dot  
- 🔋 **Real Battery Indicator** — vertical “Nokia 3310”-style icon showing live ADC voltage  
- 🔁 **Auto Reconnect** — resumes connection when the iPhone returns  
- 🌐 **Web Interface** — set time, view JSON status, and control battery level  
- 🎨 **OLED UI** — clean layout using U8g2 library

---

## 🧩 Hardware

| Component | Description | Example Pin |
|------------|--------------|--------------|
| **ESP32 DevKitC / S3 / D32** | Main MCU | — |
| **0.96" SSD1306 OLED (128×64, I²C)** | Display notifications | `SDA = 8`, `SCL = 9` |
| **Li-ion Battery (3.0–4.2 V)** | Power source | — |
| **Voltage Divider (100 kΩ + 100 kΩ)** | Battery sense to ADC | Battery + → R1 → ADC 34 → R2 → GND |

---

## ⚙️ Pin Configuration

```cpp
#define SDA_PIN     8     // I²C data
#define SCL_PIN     9     // I²C clock
#define RESET_PIN   U8X8_PIN_NONE

---

## 🪄 Usage
1️⃣ Pairing

- Upload firmware to ESP32.

- On iPhone → Settings › Bluetooth → find ANCS → pair.

- Accept the pairing and allow notifications.

2️⃣ Notifications

When a message arrives (e.g. WhatsApp), the OLED shows:
Terranova
Hallo

---

## 💡 Future Ideas

✅ Add scroll for long messages

🕹 Touch or button to switch screens manually

🌈 Animated icons per app category

💾 Save pairing info in NVS

📶 Add Bluetooth RSSI indicator