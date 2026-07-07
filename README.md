# TrinGo-IoT 🏍️📡

Sistem pelacakan (tracking) IoT untuk kendaraan bermotor menggunakan **ESP32** dan modul GPS **U-Blox Neo 7M**. Sistem ini tidak hanya membaca titik koordinat, tetapi juga menerjemahkannya menjadi alamat jalan nyata secara otomatis (*Reverse Geocoding*) dan mengirimkan datanya secara *real-time* ke server/database berbasis Laravel menggunakan protokol MQTT.

## ✨ Fitur Utama
- **Real-Time GPS Tracking**: Mengambil data lokasi (Latitude, Longitude, Kecepatan, Ketinggian, Arah) akurat dari satelit.
- **Reverse Geocoding**: Otomatis mengubah koordinat GPS menjadi alamat jalan nyata memanfaatkan *OpenStreetMap Nominatim API*.
- **Konektivitas MQTT**: Mengirimkan data telemetri dalam format JSON secara ringan dan cepat ke broker MQTT (mendukung integrasi langsung ke sistem backend).
- **Auto-Reconnect**: Sistem pintar yang akan otomatis mencoba menghubungkan ulang jika sinyal WiFi atau server MQTT terputus di tengah jalan.
- **Smart Data Sending**: Mendeteksi jika kendaraan berpindah posisi cukup jauh (minimal 50 meter) sebelum mengirim ulang *request API* alamat untuk menghemat bandwidth dan kuota request API.

## 🛠️ Perangkat Keras (Hardware) yang Dibutuhkan
1. **ESP32 Development Board** (Misal: DOIT ESP32 DEVKIT V1)
2. **Modul GPS U-Blox Neo 7M** (atau seri 6M / 8M)
3. Kabel Jumper secukupnya
4. Sumber Daya (Power Bank atau modul Step-Down ke Aki Motor)

### 🔌 Skema Rangkaian (Wiring)
| Modul GPS Neo 7M | ESP32 Board |
| :---: | :---: |
| **VCC** | `3V3` atau `VIN/5V` |
| **GND** | `GND` |
| **TX**  | `GPIO 16` (RX2) |
| **RX**  | `GPIO 17` (TX2) |

> ⚠️ **Catatan Penting**: Modul GPS membutuhkan pandangan langsung ke langit (*Line of Sight*). Pastikan antena GPS berada di luar ruangan atau tidak terhalang atap beton yang tebal agar bisa cepat mendapat sinyal satelit (*GPS Fix*).

## 📚 Kebutuhan Library
Pastikan library berikut sudah terinstal di proyek Anda:
- `mikalhart/TinyGPSPlus` (v1.0.3+)
- `bblanchon/ArduinoJson` (v7.0.0+)
- `knolleary/PubSubClient` (v2.8+)

*(Catatan: Jika Anda menggunakan **PlatformIO**, library di atas sudah otomatis terunduh melalui file `platformio.ini`)*.

## ⚙️ Konfigurasi & Cara Penggunaan
1. Clone repository ini dan buka folder proyeknya menggunakan VS Code (PlatformIO).
2. Buka file `src/main.cpp`.
3. Sesuaikan konfigurasi **WiFi** dengan jaringan/hotspot Anda:
   ```cpp
   const char* WIFI_SSID     = "Nama_WiFi_Anda";
   const char* WIFI_PASSWORD = "Password_WiFi_Anda";
   ```
4. Sesuaikan konfigurasi **MQTT Broker** Anda (Host, Port, Username, Password):
   ```cpp
   const char* MQTT_BROKER   = "tringgo.site";
   const int   MQTT_PORT     = 1883;
   const char* MQTT_USER     = "user_mqtt_anda";
   const char* MQTT_PASS     = "password_mqtt_anda";
   ```
5. **Upload** kode ke ESP32 Anda.
6. Buka **Serial Monitor** (Baud Rate: `115200`). Anda akan melihat proses ESP32 terhubung ke WiFi, Broker MQTT, melakukan diagnostik pembacaan NMEA mentah, dan mulai menyajikan data.

## 📨 Format Payload MQTT
Topik akan dikirim secara dinamis mengikuti MAC Address perangkat (sehingga mendukung banyak kendaraan sekaligus tanpa bentrok):  
👉 Topik: `vehicle/[MAC_ADDRESS_ESP32]/telemetry`

**Contoh Payload JSON yang dikirimkan ke MQTT:**
```json
{
  "device_id": "1C:C3:AB:C1:92:C4",
  "timestamp": 12345678,
  "has_fix": true,
  "lat": -7.057655,
  "lng": 110.429658,
  "speed_kmh": 20.5,
  "alt_gps_m": 233.5,
  "course_deg": 180.0,
  "address": "Jalan Tirto Usodo Timur, RW 03, Pedalangan, Banyumanik, Kota Semarang, Jawa Tengah, Jawa, 50268, Indonesia",
  "maps_url": "https://www.google.com/maps?q=-7.057655,110.429658",
  "sat": 8,
  "hdop": 0.95
}
```

## 📜 Lisensi
Dikembangkan secara khusus untuk ekosistem backend **TrinGo-IoT**.
