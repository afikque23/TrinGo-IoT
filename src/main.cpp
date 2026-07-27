#include <Arduino.h>
#include <TinyGPSPlus.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>

// --- Konfigurasi MPU6050 ---
Adafruit_MPU6050 mpu;
bool mpuReady = false;
float est_velocity_mps = 0.0;
float est_distance_m = 0.0;
unsigned long last_mpu_time = 0;
float mpu_offset_accel = 0.0; // Kalibrasi gravitasi/kemiringan awal

// --- Konfigurasi WiFi ---
// ⚠️ ISI DENGAN SSID DAN PASSWORD WIFI ANDA
const char* WIFI_SSID     = "^_^";
const char* WIFI_PASSWORD = "18345672";

// --- Konfigurasi MQTT ---
const char* MQTT_BROKER   = "tringgo.site";
const int   MQTT_PORT     = 1883;
const char* MQTT_USER     = "tringgo_mqtt";
const char* MQTT_PASS     = "erenvsreiner";
String mqttTopic; // Akan diisi otomatis dengan MAC Address: vehicle/{MAC}/telemetry

// --- Konfigurasi Pin GPS ---
#define RXD2 16  // Dihubungkan ke TX pada GPS Neo 7M
#define TXD2 17  // Dihubungkan ke RX pada GPS Neo 7M
#define GPS_BAUD 9600

// --- Konfigurasi Reverse Geocoding ---
#define GEOCODE_INTERVAL 30000  // Request alamat setiap 30 detik
#define MIN_DISTANCE_M   50     // Atau jika berpindah > 50 meter

TinyGPSPlus gps;
WiFiClient espClient;
PubSubClient mqtt(espClient);

// Diagnostik
unsigned long totalCharsReceived = 0;
unsigned long lastCharCount = 0;
bool gpsModuleDetected = false;
bool rawModeDone = false;
bool is_initial_fix_done = false;

// Reverse Geocoding
String currentAddress = "Menunggu GPS fix...";
String mapsUrl = "-";
unsigned long lastGeocodeTime = 0;
double lastGeocodeLat = 0;
double lastGeocodeLng = 0;
bool wifiConnected = false;

// Hitung jarak antara 2 koordinat (meter)
double haversineDistance(double lat1, double lon1, double lat2, double lon2) {
  double dLat = radians(lat2 - lat1);
  double dLon = radians(lon2 - lon1);
  double a = sin(dLat / 2) * sin(dLat / 2) +
             cos(radians(lat1)) * cos(radians(lat2)) *
             sin(dLon / 2) * sin(dLon / 2);
  double c = 2 * atan2(sqrt(a), sqrt(1 - a));
  return 6371000.0 * c;  // Radius bumi dalam meter
}

void connectWiFi() {
  Serial.print(F("🔗 Menghubungkan ke WiFi: "));
  Serial.println(WIFI_SSID);
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(F("."));
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.println();
    Serial.print(F("✅ WiFi Terhubung! IP: "));
    Serial.println(WiFi.localIP());
    
    // Set MQTT Topic berdasarkan MAC Address
    mqttTopic = "vehicle/" + WiFi.macAddress() + "/telemetry";
    Serial.print(F("📡 Topik MQTT Anda: "));
    Serial.println(mqttTopic);
  } else {
    wifiConnected = false;
    Serial.println();
    Serial.println(F("❌ WiFi GAGAL terhubung! (Akan terus mencoba di latar belakang)"));
  }
}

unsigned long lastMqttReconnectAttempt = 0;

void reconnectMqtt() {
  if (!wifiConnected || WiFi.status() != WL_CONNECTED) return;
  
  if (!mqtt.connected()) {
    unsigned long now = millis();
    if (now - lastMqttReconnectAttempt < 5000) return; // Coba tiap 5 detik
    lastMqttReconnectAttempt = now;

    Serial.print(F("🔄 Menghubungkan ke MQTT... "));
    String clientId = "ESP32Client-" + WiFi.macAddress();
    
    if (mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASS)) {
      Serial.println(F("✅ Berhasil!"));
    } else {
      Serial.print(F("❌ Gagal, rc="));
      Serial.print(mqtt.state());
      Serial.println(F(" (Akan dicoba lagi dalam 5 detik)"));
    }
  }
}

// Reverse geocoding via OpenStreetMap Nominatim
void reverseGeocode(double lat, double lng) {
  if (!wifiConnected || WiFi.status() != WL_CONNECTED) {
    // Coba reconnect
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println(F("⚠️ WiFi terputus, mencoba reconnect..."));
      WiFi.reconnect();
      delay(2000);
      if (WiFi.status() != WL_CONNECTED) {
        currentAddress = "WiFi tidak terhubung";
        return;
      }
    }
  }

  HTTPClient http;
  
  // Bangun URL Nominatim
  String url = "https://nominatim.openstreetmap.org/reverse?format=json&lat=";
  url += String(lat, 6);
  url += "&lon=";
  url += String(lng, 6);
  url += "&zoom=18&addressdetails=1&accept-language=id";

  Serial.println(F("🌐 Mengambil alamat dari OpenStreetMap..."));
  
  http.begin(url);
  http.addHeader("User-Agent", "ESP32-GPS-Tracker/1.0");
  http.setTimeout(10000);
  
  int httpCode = http.GET();
  
  if (httpCode == 200) {
    String payload = http.getString();
    
    // Parse JSON
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);
    
    if (!error) {
      // Ambil display_name sebagai alamat lengkap
      const char* displayName = doc["display_name"];
      if (displayName) {
        currentAddress = String(displayName);
      }
      
      // Juga tampilkan detail alamat jika ada
      JsonObject addr = doc["address"];
      if (!addr.isNull()) {
        String shortAddr = "";
        
        if (addr.containsKey("road")) shortAddr += String((const char*)addr["road"]);
        if (addr.containsKey("village")) {
          if (shortAddr.length() > 0) shortAddr += ", ";
          shortAddr += String((const char*)addr["village"]);
        } else if (addr.containsKey("suburb")) {
          if (shortAddr.length() > 0) shortAddr += ", ";
          shortAddr += String((const char*)addr["suburb"]);
        }
        if (addr.containsKey("city")) {
          if (shortAddr.length() > 0) shortAddr += ", ";
          shortAddr += String((const char*)addr["city"]);
        } else if (addr.containsKey("town")) {
          if (shortAddr.length() > 0) shortAddr += ", ";
          shortAddr += String((const char*)addr["town"]);
        }
        if (addr.containsKey("state")) {
          if (shortAddr.length() > 0) shortAddr += ", ";
          shortAddr += String((const char*)addr["state"]);
        }
        
        if (shortAddr.length() > 0) {
          Serial.print(F("📍 Alamat Singkat     : "));
          Serial.println(shortAddr);
        }
      }
      
      lastGeocodeLat = lat;
      lastGeocodeLng = lng;
      
      // Update Google Maps URL
      mapsUrl = "https://www.google.com/maps?q=" + String(lat, 6) + "," + String(lng, 6);
      Serial.println(F("✅ Alamat berhasil diambil!"));
    } else {
      Serial.print(F("❌ JSON parse error: "));
      Serial.println(error.c_str());
    }
  } else {
    Serial.print(F("❌ HTTP error: "));
    Serial.println(httpCode);
  }
  http.end();
}

void sendDataToMQTT() {
  if (!mqtt.connected()) return;

  JsonDocument doc;
  
  // Data wajib sesuai struktur Laravel Anda
  doc["device_id"] = WiFi.macAddress();
  doc["timestamp"] = millis(); 
  
  bool is_fix = gps.location.isValid() && gps.satellites.value() >= 4;
  doc["has_fix"] = is_fix;

  if (is_fix) {
    doc["lat"] = gps.location.lat();
    doc["lng"] = gps.location.lng();
    
    if (gps.speed.isValid()) doc["speed_kmh"] = gps.speed.kmph();
    if (gps.altitude.isValid()) doc["alt_gps_m"] = gps.altitude.meters();
    if (gps.course.isValid()) doc["course_deg"] = gps.course.deg();
    
    doc["address"] = currentAddress;
    doc["maps_url"] = mapsUrl;
  } else {
    // Jika tidak fix, kirimkan hasil Dead Reckoning MPU6050
    if (mpuReady) {
      doc["est_speed_kmh"] = est_velocity_mps * 3.6;
      doc["est_distance_m"] = est_distance_m;
    }
  }
  
  if (gps.satellites.isValid()) doc["sat"] = gps.satellites.value();
  if (gps.hdop.isValid()) doc["hdop"] = gps.hdop.hdop();

  // ── TAMBAHAN: Data MPU6050 selalu dikirim (untuk Tabel 4.3 dan analisis) ──
  if (mpuReady) {
    sensors_event_t a, g, temp;
    mpu.getEvent(&a, &g, &temp);

    // Hitung g-force total (resultante 3 sumbu)
    float gForceTotal = sqrt(
      a.acceleration.x * a.acceleration.x +
      a.acceleration.y * a.acceleration.y +
      a.acceleration.z * a.acceleration.z
    ) / 9.81; // Konversi m/s² → g

    // Hitung g-force horizontal (X & Y) — sama seperti dead reckoning
    float gForceHoriz = sqrt(
      a.acceleration.x * a.acceleration.x +
      a.acceleration.y * a.acceleration.y
    ) / 9.81;

    // is_moving: true jika g-force horizontal melebihi threshold (getaran/gerakan)
    bool isMoving = (gForceHoriz - (mpu_offset_accel / 9.81)) > 0.05;

    doc["mpu_g_force"]   = gForceTotal;   // g-force total (semua sumbu)
    doc["mpu_is_moving"] = isMoving;       // status gerak

    // Debug ke Serial Monitor
    Serial.print(F("MPU G-Force Total    : "));
    Serial.print(gForceTotal, 4);
    Serial.println(F(" g"));
    Serial.print(F("MPU G-Force Horiz    : "));
    Serial.print(gForceHoriz, 4);
    Serial.println(F(" g"));
    Serial.print(F("MPU Is Moving        : "));
    Serial.println(isMoving ? F("MOVING ✅") : F("IDLE 🔵"));
  }
  // ─────────────────────────────────────────────────────────────────────────

  // Convert JSON to String
  String payload;
  serializeJson(doc, payload);

  // Publish ke topik
  if (mqtt.publish(mqttTopic.c_str(), payload.c_str())) {
    Serial.println(F("🚀 [MQTT] Data berhasil dikirim ke server!"));
  } else {
    Serial.println(F("❌ [MQTT] Gagal mengirim data."));
  }
}



void computeDeadReckoning() {
  if (!mpuReady) return;

  unsigned long now = millis();
  // Batasi pembacaan MPU maksimal setiap 50ms (20Hz) agar CPU dan I2C tidak kewalahan
  if (now - last_mpu_time < 50) return;

  float dt = (now - last_mpu_time) / 1000.0;
  last_mpu_time = now;

  if (gps.location.isValid() && gps.satellites.value() >= 4) {
    // Jika GPS valid, sinkronkan nilai estimasi dari GPS
    est_velocity_mps = gps.speed.mps();
    est_distance_m = 0; // Jarak relatif selama dead reckoning direset
  } else {
    // GPS Loss -> Lakukan dead reckoning menggunakan MPU6050
    sensors_event_t a, g, temp;
    mpu.getEvent(&a, &g, &temp);

    // Ambil percepatan horizontal (Asumsi sensor mendatar, gaya Z = gravitasi)
    float a_horiz = sqrt(a.acceleration.x * a.acceleration.x + a.acceleration.y * a.acceleration.y);
    
    // Kurangi dengan nilai kalibrasi (kemiringan awal saat diam)
    float net_accel = a_horiz - mpu_offset_accel;
    
    // Threshold noise statis (sisa noise/getaran kecil)
    if (abs(net_accel) < 0.5) {
      net_accel = 0.0;
    }

    // Integral ke-1: Kecepatan (v = v0 + a * dt)
    est_velocity_mps += net_accel * dt;
    if (est_velocity_mps < 0) est_velocity_mps = 0; // Tidak bisa mundur


    // Friksi/Decay: Kurangi kecepatan perlahan jika tidak ada percepatan
    if (net_accel == 0.0 && est_velocity_mps > 0) {
      est_velocity_mps -= 1.0 * dt; // Asumsi perlambatan 1 m/s^2
      if (est_velocity_mps < 0) est_velocity_mps = 0;
    }

    // Integral ke-2: Jarak (s = v * dt)
    est_distance_m += est_velocity_mps * dt;
  }
}

// Tampilkan data NMEA mentah selama 10 detik pertama
void showRawNMEA() {
  while (Serial2.available() > 0) {
    char c = Serial2.read();
    Serial.print(c);
    totalCharsReceived++;
  }
}

void printDiagnostics() {
  Serial.println(F("--- DIAGNOSTIK KONEKSI GPS ---"));
  Serial.print(F("Total karakter diterima : "));
  Serial.println(totalCharsReceived);

  unsigned long charsPerInterval = totalCharsReceived - lastCharCount;
  lastCharCount = totalCharsReceived;
  Serial.print(F("Karakter per 3 detik    : "));
  Serial.println(charsPerInterval);

  Serial.print(F("Sentences valid (NMEA)  : "));
  Serial.println(gps.sentencesWithFix());
  Serial.print(F("Sentences gagal checksum: "));
  Serial.println(gps.failedChecksum());
  Serial.print(F("Sentences lulus checksum: "));
  Serial.println(gps.passedChecksum());

  if (totalCharsReceived == 0) {
    Serial.println(F("\n⚠️  TIDAK ADA DATA DARI GPS!"));
  } else if (gps.passedChecksum() == 0 && gps.failedChecksum() > 0) {
    Serial.println(F("\n⚠️  DATA DITERIMA TAPI CHECKSUM GAGAL!"));
  } else if (totalCharsReceived > 0 && gps.passedChecksum() > 0) {
    gpsModuleDetected = true;
    Serial.println(F("\n✅ GPS MODUL TERDETEKSI & KOMUNIKASI OK!"));
  }
  
  // Status WiFi & MQTT
  Serial.print(F("WiFi Status          : "));
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("Terhubung ("));
    Serial.print(WiFi.RSSI());
    Serial.println(F(" dBm)"));
  } else {
    Serial.println(F("Tidak terhubung"));
  }
  
  Serial.print(F("MQTT Status          : "));
  Serial.println(mqtt.connected() ? F("Terhubung ✅") : F("Terputus ❌"));
}

void printGPSData() {
  Serial.println(F("=== Data GPS Neo 7M ==="));

  // Status koneksi
  Serial.print(F("Status Modul         : "));
  Serial.println(gpsModuleDetected ? F("TERHUBUNG ✅") : F("BELUM TERDETEKSI ❌"));

  // Satelit
  Serial.print(F("Satelit Terhubung    : "));
  if (gps.satellites.isValid()) {
    Serial.print(gps.satellites.value());
    if (gps.satellites.value() == 0) {
      Serial.println(F(" (Mencari Sinyal... Bawa ke tempat terbuka!)"));
    } else if (gps.satellites.value() < 4) {
      Serial.println(F(" (Sinyal lemah, butuh min. 4 untuk fix)"));
    } else {
      Serial.println(F(" (Sinyal bagus! ✅)"));
    }
  } else {
    Serial.println(F("N/A"));
  }

  // HDOP (akurasi)
  if (gps.hdop.isValid()) {
    Serial.print(F("HDOP (Akurasi)       : "));
    Serial.print(gps.hdop.hdop());
    if (gps.hdop.hdop() < 2.0) {
      Serial.println(F(" (Sangat Bagus)"));
    } else if (gps.hdop.hdop() < 5.0) {
      Serial.println(F(" (Bagus)"));
    } else {
      Serial.println(F(" (Kurang Akurat)"));
    }
  }

  // Lokasi
  if (gps.location.isValid()) {
    Serial.print(F("Latitude             : "));
    Serial.println(gps.location.lat(), 6);
    Serial.print(F("Longitude            : "));
    Serial.println(gps.location.lng(), 6);

    Serial.print(F("Link Google Maps     : "));
    Serial.println(mapsUrl);
    
    // Alamat real
    Serial.print(F("📍 Alamat            : "));
    Serial.println(currentAddress);
  } else {
    Serial.println(F("Lokasi               : BELUM VALID (Tunggu GPS Fix)"));
  }

  // Ketinggian GPS
  if (gps.altitude.isValid()) {
    Serial.print(F("Ketinggian (mdpl)    : "));
    Serial.print(gps.altitude.meters());
    Serial.println(F(" m"));
  } else {
    Serial.println(F("Ketinggian           : BELUM VALID"));
  }

  // Kecepatan
  if (gps.speed.isValid()) {
    Serial.print(F("Kecepatan            : "));
    Serial.print(gps.speed.kmph());
    Serial.println(F(" km/jam"));
  }

  // Arah
  if (gps.course.isValid()) {
    Serial.print(F("Arah                 : "));
    Serial.print(gps.course.deg());
    Serial.println(F("°"));
  }

  // Tanggal & Waktu
  if (gps.date.isValid() && gps.time.isValid()) {
    Serial.print(F("Tanggal & Waktu (UTC): "));
    if (gps.date.day() < 10) Serial.print(F("0"));
    Serial.print(gps.date.day());
    Serial.print(F("/"));
    if (gps.date.month() < 10) Serial.print(F("0"));
    Serial.print(gps.date.month());
    Serial.print(F("/"));
    Serial.print(gps.date.year());
    Serial.print(F(" "));
    if (gps.time.hour() < 10) Serial.print(F("0"));
    Serial.print(gps.time.hour());
    Serial.print(F(":"));
    if (gps.time.minute() < 10) Serial.print(F("0"));
    Serial.print(gps.time.minute());
    Serial.print(F(":"));
    if (gps.time.second() < 10) Serial.print(F("0"));
    Serial.println(gps.time.second());

    // Waktu WIB (UTC+7)
    int wibHour = (gps.time.hour() + 7) % 24;
    Serial.print(F("Waktu WIB            : "));
    if (wibHour < 10) Serial.print(F("0"));
    Serial.print(wibHour);
    Serial.print(F(":"));
    if (gps.time.minute() < 10) Serial.print(F("0"));
    Serial.print(gps.time.minute());
    Serial.print(F(":"));
    if (gps.time.second() < 10) Serial.print(F("0"));
    Serial.println(gps.time.second());
  } else {
    Serial.println(F("Waktu                : BELUM VALID"));
  }

  // Data MPU6050 (Dead Reckoning)
  Serial.println(F("\n--- Data MPU6050 (Dead Reckoning) ---"));
  if (mpuReady) {
    bool is_fix = gps.location.isValid() && gps.satellites.value() >= 4;
    if (is_fix) {
      Serial.println(F("Status               : STANDBY (GPS Fix OK)"));
    } else {
      Serial.println(F("Status               : AKTIF (GPS Loss)"));
      Serial.print(F("Estimasi Kecepatan   : "));
      Serial.print(est_velocity_mps * 3.6);
      Serial.println(F(" km/jam"));
      Serial.print(F("Estimasi Jarak       : "));
      Serial.print(est_distance_m);
      Serial.println(F(" meter"));
    }
  } else {
    Serial.println(F("Status               : ERROR / KABEL LEPAS"));
  }

  // Uptime
  Serial.print(F("Uptime               : "));
  unsigned long sec = millis() / 1000;
  Serial.print(sec / 60);
  Serial.print(F(" menit "));
  Serial.print(sec % 60);
  Serial.println(F(" detik"));
}

void setup() {
  Serial.begin(115200);
  Serial2.begin(GPS_BAUD, SERIAL_8N1, RXD2, TXD2);

  Serial.println(F("\n╔═══════════════════════════════════════╗"));
  Serial.println(F("║  TEST GPS NEO 7M + ESP32 DevKit v2.0  ║"));
  Serial.println(F("║     + Reverse Geocoding (Alamat)       ║"));
  Serial.println(F("║     + Mengirim Data ke MQTT Server     ║"));
  Serial.println(F("╚═══════════════════════════════════════╝"));
  
  // Hubungkan WiFi
  connectWiFi();
  
  // Hubungkan MQTT
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setBufferSize(1024); // ⬅️ TAMBAHKAN INI: Perbesar ukuran maksimal paket pengiriman
  
  
  // Inisialisasi MPU6050
  Wire.begin(22, 21); // SDA = 22, SCL = 21 (Sesuai request)
  if (!mpu.begin()) {
    Serial.println(F("⚠️ MPU6050 TIDAK TERDETEKSI! Cek kabel I2C (SDA=22, SCL=21)"));
  } else {
    Serial.println(F("✅ MPU6050 Terdeteksi!"));
    mpu.setAccelerometerRange(MPU6050_RANGE_2_G);
    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
    mpuReady = true;
    
    // Proses Kalibrasi Otomatis (Mencari nilai gravitasi/kemiringan saat diam)
    Serial.println(F("⚙️ Mengkalibrasi MPU6050... JANGAN GERAKKAN ALAT (2 DETIK)!"));
    delay(2000); // Tunggu alat stabil
    float sum_accel = 0;
    for (int i = 0; i < 50; i++) {
      sensors_event_t a, g, temp;
      mpu.getEvent(&a, &g, &temp);
      sum_accel += sqrt(a.acceleration.x * a.acceleration.x + a.acceleration.y * a.acceleration.y);
      delay(10);
    }
    mpu_offset_accel = sum_accel / 50.0;
    Serial.print(F("✅ Kalibrasi selesai! Offset percepatan horizontal: "));
    Serial.println(mpu_offset_accel);

    last_mpu_time = millis();
  }

  Serial.println();
  Serial.println(F("📡 Menampilkan data NMEA mentah selama 10 detik..."));
  Serial.println(F("   (Jika tidak ada karakter muncul = GPS tidak terhubung)\n"));
}

void loop() {
  unsigned long now = millis();

  // Update wifiConnected flag secara dinamis jika ESP32 reconnect ke WiFi di latar belakang
  if (WiFi.status() == WL_CONNECTED && !wifiConnected) {
    wifiConnected = true;
    mqttTopic = "vehicle/" + WiFi.macAddress() + "/telemetry";
    Serial.print(F("\n✅ WiFi kembali Terhubung! IP: "));
    Serial.println(WiFi.localIP());
  } else if (WiFi.status() != WL_CONNECTED && wifiConnected) {
    wifiConnected = false;
    Serial.println(F("\n❌ WiFi terputus!"));
  }

  // Koneksi ulang ke MQTT jika terputus
  if (!mqtt.connected()) {
    reconnectMqtt();
  }
  mqtt.loop(); // Wajib dipanggil untuk menjaga koneksi MQTT

  // Kalkulasi estimasi MPU6050 setiap siklus loop
  computeDeadReckoning();

  // Fase 1: Tampilkan raw NMEA selama 10 detik pertama
  if (!rawModeDone && now < 10000) {
    showRawNMEA();
    return;
  }

  // Transisi dari raw mode
  if (!rawModeDone) {
    rawModeDone = true;
    Serial.println(F("\n\n═══ SELESAI RAW MODE ═══"));
    lastCharCount = totalCharsReceived;
  }

  // Fase 2: Parsing normal
  while (Serial2.available() > 0) {
    char c = Serial2.read();
    gps.encode(c);
    totalCharsReceived++;
  }

  // Reverse geocoding: ambil alamat jika lokasi valid
  if (gps.location.isValid() && wifiConnected) {
    bool timeToGeocode = (now - lastGeocodeTime > GEOCODE_INTERVAL);
    bool movedFarEnough = (lastGeocodeLat == 0 && lastGeocodeLng == 0) ||
                          haversineDistance(lastGeocodeLat, lastGeocodeLng,
                                           gps.location.lat(), gps.location.lng()) > MIN_DISTANCE_M;
    
    if (timeToGeocode && (movedFarEnough || lastGeocodeLat == 0)) {
      lastGeocodeTime = now;
      reverseGeocode(gps.location.lat(), gps.location.lng());
    }
  }

  // Update flag fix pertama kali secara permanen
  if (gps.location.isValid() && gps.satellites.value() >= 4) {
    is_initial_fix_done = true;
  }

  // Tentukan interval pengiriman (Smart Interval Dinonaktifkan sementara)
  // Default: 5 detik (baik kendaraan berhenti maupun berjalan)
  unsigned long currentInterval = 5000; 
  
  if (gps.location.isValid() && gps.satellites.value() >= 4) {
    // Gunakan kecepatan GPS jika valid
    if (gps.speed.isValid() && gps.speed.kmph() > 2.0) {
      currentInterval = 5000;
    }
  } else if (is_initial_fix_done) {
    // Jika GPS hilang tapi sudah pernah fix (Dead Reckoning MPU6050)
    if ((est_velocity_mps * 3.6) > 2.0) {
      currentInterval = 5000;
    }
  }

  // Tampilkan dan kirim data sesuai Smart Interval
  static unsigned long lastPrint = 0;
  if (now - lastPrint > currentInterval || lastPrint == 0) {
    lastPrint = now;

    printGPSData();
    printDiagnostics();
    
    // Kirim data ke MQTT secara terus-menerus (walaupun belum GPS Fix)
    if (!is_initial_fix_done) {
      Serial.println(F("\n⚠️ Menunggu GPS Fix... (Data tetap dikirim dengan kordinat 0/kosong)"));
    }
    
    sendDataToMQTT();
    
    Serial.println(F("═══════════════════════════════════════"));
  }
}

