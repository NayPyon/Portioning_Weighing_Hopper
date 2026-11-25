#include <Arduino.h>
#include <Wire.h>
#include <Servo.h>
#include <HX711.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// =========================================================================
#define PIN_POT       A0  // Input analog putaran target berat
#define PIN_TARE      2   // Tombol kalibrasi nol (Tare)
#define PIN_START     3   // Tombol Mulai / Darurat Berhenti (Toggle)
#define PIN_HX_DT     4   // Data dari modul loadcell HX711
#define PIN_HX_SCK    5   // Clock untuk modul HX711
#define PIN_LED_GRN   6   // Indikator hijau: sistem aman / pengisian selesai
#define PIN_LED_RED   7   // Indikator merah: proses jalan / bahaya overload
#define PIN_BUZZER    8   // Alarm bunyi notifikasi
#define PIN_SERVO     9   // Sinyal PWM penggerak pintu katup hopper

// =========================================================================
// KALIBRASI MEKANIK & SENSOR
// =========================================================================
// Sudut bukaan servo 
const int VALVE_CLOSE = 0;   // Tertutup rapat
const int VALVE_SLOW  = 25;  // Bukaan kecil (mode tetes/halus biar akurat)
const int VALVE_FAST  = 90;  // Bukaan penuh (ngebut di awal pengisian)

// Faktor pembagi HX711
const float LOADCELL_SCALE = 420.0;

// Inisialisasi objek perangkat keras
HX711 scale;
Servo hopperGate;
Adafruit_SSD1306 display(128, 64, &Wire, -1); // OLED 128x64 tanpa pin Reset fisik

// =========================================================================
// VARIABEL STATE MACHINE & STATUS SISTEM
// =========================================================================
enum SystemState { IDLE, FAST_FILL, SLOW_FILL, DONE, OVERLOAD };
SystemState state = IDLE;

bool isRunning = false;          // Status mesin: true = sedang mengisi, false = berhenti
float currentWeightKg = 0.0;     // Berat real-time dari timbangan
float targetWeightKg  = 0.50;    // Target berat yang diinginkan (default 0.5 kg)
int valveAngle = VALVE_CLOSE;    // Posisi sudut servo saat ini

// Timer non-blocking pengganti delay() biar tombol tetap responsif
unsigned long lastReadTime = 0;
unsigned long lastBtnTime  = 0;

// Helper singkat buat bunyikan buzzer tanpa bikin program macet (non-blocking)
void beep(int freq, int duration) {
  tone(PIN_BUZZER, freq, duration);
}

// =========================================================================
// FUNGSI RENDER LAYAR OLED
// =========================================================================
void updateDisplay() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // Baris paling atas: Status kerja sistem saat ini
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(F("HOPPER:"));
  display.setCursor(48, 0);
  switch (state) {
    case IDLE:      display.print(F("READY")); break;
    case FAST_FILL: display.print(F("FAST MODE")); break;
    case SLOW_FILL: display.print(F("SLOW MODE")); break;
    case DONE:      display.print(F("COMPLETE")); break;
    case OVERLOAD:  display.print(F("OVERLOAD!")); break;
  }
  display.drawLine(0, 10, 128, 10, SSD1306_WHITE); // Garis pemisah header

  // Info berat target (2 angka di belakang koma)
  display.setCursor(0, 14);
  display.print(F("Target : "));
  display.print(targetWeightKg, 2);
  display.println(F(" kg"));

  // Info berat aktual hasil bacaan sensor
  display.setCursor(0, 25);
  display.print(F("Aktual : "));
  display.print(currentWeightKg, 2);
  display.println(F(" kg"));

  // Konversi sudut servo (0-90 derajat) jadi persentase (0-100%) biar enak dibaca operator
  int valvePct = map(valveAngle, 0, 90, 0, 100);
  display.setCursor(0, 36);
  display.print(F("Katup  : "));
  display.print(valvePct);
  display.println(F(" %"));

  // Gambar kotak progress bar pengisian di bagian bawah layar
  display.drawRect(0, 49, 128, 13, SSD1306_WHITE);
  float ratio = constrain(currentWeightKg / max(targetWeightKg, 0.01f), 0.0, 1.0);
  int barWidth = (int)(ratio * 124); // Lebar maksimal isi bar 124 pixel (sisakan border)
  display.fillRect(2, 51, barWidth, 9, SSD1306_WHITE);

  display.display(); // Dorong semua buffer gambar ke layar fisik
}

// =========================================================================
// SETUP AWAL (DIJALANKAN SEKALI SAAT POWER ON)
// =========================================================================
void setup() {
  Serial.begin(115200);

  // Pakai internal pull-up, jadi tombol dihubungkan ke GND saat ditekan (Active LOW)
  pinMode(PIN_TARE, INPUT_PULLUP);
  pinMode(PIN_START, INPUT_PULLUP);
  pinMode(PIN_LED_GRN, OUTPUT);
  pinMode(PIN_LED_RED, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);

  // Pastikan posisi awal katup tertutup rapat sebelum timbangan jalan
  hopperGate.attach(PIN_SERVO);
  hopperGate.write(VALVE_CLOSE);

  // Siapkan sensor loadcell & langsung nol-kan beban wadah kosong
  scale.begin(PIN_HX_DT, PIN_HX_SCK);
  scale.set_scale(LOADCELL_SCALE);
  scale.tare();

  // Cek koneksi layar OLED di alamat I2C 0x3C. Kalau gagal, tahan sistem di sini.
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    for (;;); 
  }
  
  beep(1500, 100); // Bunyi "tit" tanda sistem siap tempur
  updateDisplay();
}

// =========================================================================
// LOOP UTAMA
// =========================================================================
void loop() {
  unsigned long now = millis();

  // -----------------------------------------------------------------------
  // 1. CEK TOMBOL FISIK (Pakai jeda debounce 200ms biar tidak double-click)
  // -----------------------------------------------------------------------
  if (now - lastBtnTime > 200) {
    
    // Tombol Tare: Cuma boleh ditekan pas mesin lagi berhenti (!isRunning)
    if (digitalRead(PIN_TARE) == LOW && !isRunning) {
      scale.tare();
      currentWeightKg = 0.0;
      state = IDLE;
      beep(1200, 80);
      lastBtnTime = now;
    }

    // Tombol Start/Stop: Sistem toggle (tekan sekali jalan, tekan lagi berhenti darurat)
    if (digitalRead(PIN_START) == LOW) {
      isRunning = !isRunning;
      if (!isRunning) {
        // Kalau dipaksa berhenti di tengah jalan, langsung tutup katup!
        state = IDLE;
        valveAngle = VALVE_CLOSE;
        hopperGate.write(valveAngle);
        beep(800, 120);
      } else {
        beep(1800, 100);
      }
      lastBtnTime = now;
    }
  }

  // -----------------------------------------------------------------------
  // 2. BACA SENSOR & KENDALI KATUP (Dieksekusi rutin tiap 100ms / 10Hz)
  // -----------------------------------------------------------------------
  if (now - lastReadTime >= 100) {
    lastReadTime = now;

    // Baca potensiometer hanya saat mesin diam (mengunci target selama proses isi)
    int adcVal = analogRead(PIN_POT);
    if (!isRunning) {
      // Mapping nilai ADC (0-1023) menjadi kelipatan 0.05 kg (rentang 0.20 kg - 2.00 kg)
      int steps = map(adcVal, 0, 1023, 4, 40);
      targetWeightKg = steps * 0.05;
    }

    // Ambil data timbangan kalau chip HX711 sudah siap kirim data
    if (scale.is_ready()) {
      currentWeightKg = scale.get_units(1);
      // Redam noise getaran kecil di sekitar angka nol biar layar nggak kedip minus
      if (currentWeightKg < 0.0 && currentWeightKg > -0.01) currentWeightKg = 0.0;
    }

    // Logika utama transisi State Machine pengisian
    if (isRunning) {
      if (currentWeightKg < targetWeightKg * 0.80) {
        // Masih di bawah 80% target -> Buka katup lebar-lebar biar cepat
        state = FAST_FILL;
        valveAngle = VALVE_FAST;
      } else if (currentWeightKg < targetWeightKg) {
        // Sudah masuk 80% - 99% target -> Kecilkan bukaan biar nggak kebablasan
        state = SLOW_FILL;
        valveAngle = VALVE_SLOW;
      } else {
        // Target tercapai! Tutup katup dan matikan siklus
        isRunning = false;
        valveAngle = VALVE_CLOSE;
        // Cek apakah material yang jatuh kebablasan lebih dari toleransi 5%
        state = (currentWeightKg > targetWeightKg * 1.05) ? OVERLOAD : DONE;
        beep(2200, 300);
      }
    } else {
      // Kondisi saat mesin standby / tidak sedang mengisi
      valveAngle = VALVE_CLOSE;
      if (currentWeightKg > targetWeightKg * 1.05) {
        state = OVERLOAD; // Tetap kasih peringatan kalau beban berlebih belum diangkat
      } else if (state != DONE) {
        state = IDLE;
      }
    }

    // Eksekusi perintah sudut ke motor servo
    hopperGate.write(valveAngle);

    // Atur nyala lampu LED sesuai status terakhir
    digitalWrite(PIN_LED_GRN, (state == DONE || state == IDLE) ? HIGH : LOW);
    digitalWrite(PIN_LED_RED, (state == FAST_FILL || state == SLOW_FILL || state == OVERLOAD) ? HIGH : LOW);

    // Segarkan tampilan di layar OLED
    updateDisplay();
  }
}