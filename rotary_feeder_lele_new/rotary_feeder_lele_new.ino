
#define BLYNK_TEMPLATE_ID "TMPL6QhHfbNhK"
#define BLYNK_TEMPLATE_NAME "Rotary Feeder"
#define BLYNK_AUTH_TOKEN "CrTfIoWqIVcGmbxT_1iJlnEnSN2KQmc3"


#define BLYNK_PRINT Serial

#include <WiFi.h>               
#include <WiFiClient.h>         
#include <BlynkSimpleEsp32.h>   
#include <ESP32Servo.h>         
#include <time.h>        



const char* ssid     = "Ardi Pebriyan";
const char* password = "12345678910";         

const char* ntpServer      = "pool.ntp.org"; 
const long  gmtOffset      = 7 * 3600;       
const int   daylightOffset = 0;              


#define SERVO_PIN 25   


#define SERVO_POS_CLOSED    0   
                                
#define SERVO_OPEN_DEFAULT 150  
                        
#define SERVO_OPEN_MIN      10  
                            
#define SERVO_OPEN_MAX     180  



#define SERVO_MOVE_TIME  300  
#define SERVO_OPEN_HOLD 500   



#define FEED_WINDOW_MINUTES 15

Servo          feederServo;      
BlynkTimer     timer;            
      
WidgetTerminal terminal(V2);    



int scheduleMorningHour   = -1; 
int scheduleMorningMinute =  0; 
int scheduleEveningHour   = -1; 
int scheduleEveningMinute =  0; 


int servoOpenAngle = SERVO_OPEN_DEFAULT;


int countMorning = 0; 
int countEvening = 0; 




int  lastLogDay  = -1;   
bool morningDone = false; 
bool eveningDone = false; 


bool lastWifiConnected = false; 
bool lastServoAttached = false; 
bool ntpSynced         = false; 



bool servoReady     = false; 
bool feedInProgress = false;





String getTimestamp() {
  struct tm t;                        
  if (!getLocalTime(&t, 0)) 
    return "[??:??:??]";               
  char buf[12];                       
  snprintf(buf, sizeof(buf),        
           "[%02d:%02d:%02d]",        
           t.tm_hour,                  
           t.tm_min,                   
           t.tm_sec);                  
  return String(buf);                  
}


void terminalLog(const String& msg) {
  String line = getTimestamp() + " " + msg + "\n"; // Gabungkan timestamp + pesan + newline
  Serial.print(line);                  
  if (Blynk.connected()) {             
    terminal.print(line);        
    terminal.flush();      
  }`
}


// =============================================================
//  FUNGSI NTP
// =============================================================

/*
 * checkNtp()
 * ──────────
 * Memeriksa apakah NTP sudah tersinkronisasi.
 * Dipanggil setiap 5 detik dari checkSystemStatus().
 *
 * Setelah ntpSynced = true, fungsi ini langsung return
 * tanpa melakukan apapun (tidak ada overhead).
 *
 * Log waktu aktual dikirim ke terminal hanya SEKALI
 * saat sinkronisasi pertama berhasil.
 */
void checkNtp() {
  if (ntpSynced) return;               // Sudah sync sebelumnya → skip
  struct tm t;                         // Struct untuk menampung waktu
  if (getLocalTime(&t, 0)) {           // Coba ambil waktu, ms=0 non-blocking
    ntpSynced = true;                  // Tandai NTP sudah tersinkronisasi
    char buf[22];                      // Buffer untuk string tanggal+waktu
    strftime(buf, sizeof(buf),         // Format waktu ke string
             "%H:%M:%S %d/%m/%Y", &t); // Format: "08:00:00 11/05/2026"
    terminalLog("NTP OK: " + String(buf)); // Log ke terminal sekali saja
  }
  // Jika getLocalTime gagal → diam, tidak spam terminal
  // Akan dicoba lagi 5 detik kemudian
}


// =============================================================
//  FUNGSI HELPER JADWAL
// =============================================================

/*
 * isWithinWindow()
 * ────────────────
 * Mengecek apakah waktu (nowHour:nowMin) berada dalam window
 * pakan yang dimulai dari (schedHour:schedMin).
 *
 * Logika:
 *   Konversi semua ke total menit sejak 00:00.
 *   diff = selisih menit antara sekarang dan jadwal.
 *   Window aktif jika: 0 ≤ diff < FEED_WINDOW_MINUTES
 *
 * Contoh FEED_WINDOW_MINUTES=15, jadwal 08:00:
 *   08:00 → diff=0  → 0≥0 && 0<15  → true  ✅
 *   08:07 → diff=7  → 7≥0 && 7<15  → true  ✅
 *   08:15 → diff=15 → 15≥0 && 15<15 → false ❌
 *   07:59 → diff=-1 → -1≥0          → false ❌
 *
 * Return: true jika tombol boleh digunakan, false jika tidak
 */
bool isWithinWindow(int nowHour,   // Jam sekarang  (dari NTP)
                    int nowMin,    // Menit sekarang (dari NTP)
                    int schedHour, // Jam jadwal    (dari Time Input)
                    int schedMin){ // Menit jadwal  (dari Time Input)
  int nowTotal   = nowHour   * 60 + nowMin;   // Waktu sekarang dalam menit
  int schedTotal = schedHour * 60 + schedMin; // Waktu jadwal dalam menit
  int diff       = nowTotal - schedTotal;     // Selisih dalam menit
  return (diff >= 0 && diff < FEED_WINDOW_MINUTES); // true = dalam window
}

/*
 * formatJadwal()
 * ──────────────
 * Memformat jam dan menit menjadi string "H:MM".
 * Menambahkan leading zero pada menit jika < 10.
 * Contoh: formatJadwal(8, 5)  → "8:05"
 *         formatJadwal(15, 30) → "15:30"
 */
String formatJadwal(int h, int m) {
  return String(h) + ":"              // Jam tanpa leading zero
         + (m < 10 ? "0" : "")       // Tambah "0" jika menit < 10
         + String(m);                 // Nilai menit
}


// =============================================================
//  FUNGSI LOG HARIAN
// =============================================================

/*
 * sendDailyLog()
 * ──────────────
 * Mengirim ringkasan pemberian pakan hari ini ke terminal.
 * Dipanggil dari checkDailyReset() tepat sebelum counter
 * direset tengah malam.
 *
 * Format output di terminal:
 *   ========================
 *   LOG HARIAN DD/MM/YYYY
 *   Pagi  : X kali
 *   Sore  : X kali
 *   Total : X kali
 *   Sudut : X deg
 *   ========================
 *
 * Parameter: t = struct tm berisi waktu saat ini
 */
void sendDailyLog(struct tm& t) {
  char dateStr[12];                    // Buffer untuk string tanggal
  strftime(dateStr, sizeof(dateStr),   // Format tanggal ke string
           "%d/%m/%Y", &t);            // Format: "11/05/2026"
  int total = countMorning + countEvening; // Total pakan hari ini

  // Kirim semua baris log ke terminal
  terminalLog("========================");
  terminalLog("LOG HARIAN " + String(dateStr));        // Tanggal hari ini
  terminalLog("Pagi  : " + String(countMorning) + " kali"); // Counter pagi
  terminalLog("Sore  : " + String(countEvening) + " kali"); // Counter sore
  terminalLog("Total : " + String(total) + " kali");        // Total keduanya
  terminalLog("Sudut : " + String(servoOpenAngle) + " deg");// Sudut servo
  terminalLog("========================");
}

/*
 * checkDailyReset()
 * ─────────────────
 * Fungsi manajemen log harian dan reset counter.
 * Dipanggil setiap 5 detik dari checkSystemStatus().
 *
 * Tugas:
 *   1. Kirim log "Sesi PAGI selesai" setelah window pagi lewat
 *   2. Kirim log "Sesi SORE selesai" setelah window sore lewat
 *   3. Kirim log harian lengkap & reset counter di 00:00
 *
 * Menggunakan flag morningDone/eveningDone agar log sesi
 * hanya dikirim SEKALI meskipun fungsi dipanggil berulang.
 * Menggunakan lastLogDay agar reset harian hanya terjadi
 * SEKALI per hari meskipun dipanggil di menit yang sama.
 *
 * Parameter: t = struct tm berisi waktu saat ini
 */
void checkDailyReset(struct tm& t) {
  int nowTotal = t.tm_hour * 60 + t.tm_min; // Waktu sekarang dalam menit

  // ── Log setelah window PAGI selesai ───────────────────────
  if (scheduleMorningHour >= 0   // Jadwal pagi sudah diset
      && !morningDone) {         // Belum dilog hari ini
    // Hitung menit saat window pagi berakhir
    int endTotal = scheduleMorningHour * 60
                   + scheduleMorningMinute
                   + FEED_WINDOW_MINUTES;  // Akhir window
    if (nowTotal >= endTotal) {            // Sudah melewati window?
      morningDone = true;                  // Tandai sesi pagi selesai
      terminalLog("Sesi PAGI selesai. Total: "
                  + String(countMorning) + " kali pemberian.");
    }
  }

  // ── Log setelah window SORE selesai ───────────────────────
  if (scheduleEveningHour >= 0   // Jadwal sore sudah diset
      && !eveningDone) {         // Belum dilog hari ini
    int endTotal = scheduleEveningHour * 60
                   + scheduleEveningMinute
                   + FEED_WINDOW_MINUTES;  // Akhir window
    if (nowTotal >= endTotal) {            // Sudah melewati window?
      eveningDone = true;                  // Tandai sesi sore selesai
      terminalLog("Sesi SORE selesai. Total: "
                  + String(countEvening) + " kali pemberian.");
    }
  }

  // ── Reset harian tepat 00:00 ──────────────────────────────
  if (t.tm_hour == 0              // Jam 00
      && t.tm_min == 0            // Menit 00
      && lastLogDay != t.tm_mday) // Belum direset hari ini
  {
    sendDailyLog(t);              // Kirim log harian SEBELUM reset

    countMorning = 0;             // Reset counter pagi
    countEvening = 0;             // Reset counter sore
    Blynk.virtualWrite(V3, 0);   // Update widget counter pagi di app
    Blynk.virtualWrite(V4, 0);   // Update widget counter sore di app

    morningDone = false;          // Reset flag agar sesi bisa dilog lagi
    eveningDone = false;          // Reset flag agar sesi bisa dilog lagi
    lastLogDay  = t.tm_mday;     // Catat hari ini → cegah reset berulang

    terminalLog("Counter direset. Hari baru dimulai.");
  }
}

// =============================================================
//  FUNGSI SERVO
// =============================================================

/*
 * finishFeeding()
 * ───────────────
 * Callback akhir siklus pakan. Dipanggil via timer.setTimeout()
 * dari closeServo() setelah SERVO_MOVE_TIME ms berlalu.
 *
 * Mereset feedInProgress = false sehingga tombol V0 bisa
 * digunakan kembali untuk pakan berikutnya.
 */
void finishFeeding() {
  feedInProgress = false;              // Izinkan pakan berikutnya
  terminalLog("Servo kembali ke "
              + String(SERVO_POS_CLOSED)
              + " deg. Siap pakan berikutnya.");
}

/*
 * closeServo()
 * ────────────
 * Callback penutupan servo. Dipanggil via timer.setTimeout()
 * dari activateFeeder() setelah (SERVO_MOVE_TIME+SERVO_OPEN_HOLD) ms.
 *
 * Mengirim sinyal 0° ke servo lalu menjadwalkan finishFeeding()
 * setelah SERVO_MOVE_TIME ms (waktu servo sampai posisi tutup).
 */
void closeServo() {
  feederServo.write(SERVO_POS_CLOSED); // Kirim sinyal tutup (0°) ke servo
  terminalLog("Menutup servo...");     // Log ke terminal
  timer.setTimeout(                   // Jadwalkan finishFeeding() setelah
    SERVO_MOVE_TIME,                  // servo mencapai posisi tutup
    finishFeeding);                   // Callback yang dipanggil
}

/*
 * initServo()
 * ───────────
 * Menginisialisasi servo pada SERVO_PIN.
 * Aman dipanggil dari setup() maupun dari checkSystemStatus()
 * saat servo terdeteksi terlepas (tidak ada delay() di sini).
 *
 * Pulse width 500–2400 µs sesuai spesifikasi SG90:
 *   500  µs → 0°
 *   1450 µs → 90°
 *   2400 µs → 180°
 */
void initServo() {
  // attach() mengalokasikan channel LEDC PWM ESP32
  // Return true jika berhasil (TIDAK mendeteksi fisik servo)
  bool attached = feederServo.attach(SERVO_PIN, 500, 2400);

  if (attached) {
    servoReady        = true;          // Servo siap digunakan
    lastServoAttached = true;          // Update status tracking
    feederServo.write(SERVO_POS_CLOSED); // Posisikan ke 0° (tutup)
    terminalLog("Servo: SIAP | Tutup="
                + String(SERVO_POS_CLOSED) + " deg | Buka="
                + String(servoOpenAngle) + " deg");
  } else {
    servoReady        = false;         // Servo tidak bisa digunakan
    lastServoAttached = false;         // Update status tracking
    terminalLog("Servo: ERROR (gagal alokasi PWM)");
  }
}

/*
 * activateFeeder()
 * ────────────────
 * Mengeksekusi satu siklus pemberian pakan secara non-blocking:
 *
 *   t=0ms     : Buka servo ke servoOpenAngle
 *   t=300ms   : (SERVO_MOVE_TIME) servo sudah di posisi buka
 *   t=3300ms  : (300+3000) closeServo() dipanggil → servo tutup
 *   t=3600ms  : (300+3000+300) finishFeeding() → feedInProgress=false
 *
 * Seluruh proses menggunakan timer.setTimeout() sehingga
 * loop() tidak pernah diblokir.
 *
 * Parameter:
 *   isMorning = true  → sesi pagi, counter V3 bertambah
 *   isMorning = false → sesi sore, counter V4 bertambah
 */
void activateFeeder(bool isMorning) {
  if (!servoReady) {                   // Cek servo siap
    terminalLog("ERROR: Servo tidak siap!");
    return;
  }
  if (feedInProgress) {                // Cek tidak ada pakan berjalan
    terminalLog("INFO: Proses pakan sedang berjalan, harap tunggu.");
    return;
  }

  feedInProgress = true;               // Kunci agar tidak bisa dipanggil ganda

  feederServo.write(servoOpenAngle);   // Buka servo ke sudut yang diset V7
  terminalLog("Servo membuka ke "
              + String(servoOpenAngle) + " deg...");

  // Jadwalkan penutupan setelah servo selesai terbuka
  // Total delay = waktu gerak ke posisi buka + waktu tahan terbuka
  timer.setTimeout(
    SERVO_MOVE_TIME + SERVO_OPEN_HOLD, // 300 + 3000 = 3300ms
    closeServo);                        // Callback penutupan

  // Update counter dan widget dashboard sesuai sesi
  if (isMorning) {
    countMorning++;                    // Tambah counter pagi
    Blynk.virtualWrite(V3, countMorning); // Update widget V3
    terminalLog("Pakan PAGI ke-"
                + String(countMorning) + " berhasil.");
  } else {
    countEvening++;                    // Tambah counter sore
    Blynk.virtualWrite(V4, countEvening); // Update widget V4
    terminalLog("Pakan SORE ke-"
                + String(countEvening) + " berhasil.");
  }

  // Kirim notifikasi push ke HP
  // Event "pakan_sukses" harus dibuat di Blynk Console
  // (Template → Events & Notifications → + New Event)
  Blynk.logEvent("pakan_sukses");
}


// =============================================================
//  FUNGSI TIMER – dipanggil otomatis setiap 5 detik
// =============================================================

/*
 * checkSystemStatus()
 * ───────────────────
 * Fungsi pemantauan utama sistem.
 * Didaftarkan ke BlynkTimer dengan interval 5 detik di setup().
 *
 * Urutan tugas:
 *   0. Cek & update status NTP
 *   1. Cek & laporkan perubahan status WiFi
 *   2. Cek & laporkan perubahan status Servo
 *   3. Kelola log harian & reset counter (checkDailyReset)
 *   4. Update tampilan status V1
 */
void checkSystemStatus() {

  // ── 0. CEK NTP ────────────────────────────────────────────
  checkNtp(); // Coba sync NTP jika belum; diam jika sudah

  // ── 1. CEK STATUS WiFi ────────────────────────────────────
  bool wifiNow = (WiFi.status() == WL_CONNECTED); // Status WiFi saat ini
  if (wifiNow != lastWifiConnected) {  // Apakah status BERUBAH?
    lastWifiConnected = wifiNow;       // Simpan status terbaru
    terminalLog("WiFi: "               // Log perubahan ke terminal
                + String(wifiNow ? "TERKONEKSI" : "TERPUTUS"));
  }
  // Jika tidak berubah → diam (tidak spam terminal)

  // ── 2. CEK STATUS SERVO ───────────────────────────────────
  // Jangan cek saat servo sedang bergerak – bisa salah baca
  if (!feedInProgress) {
    bool attachedNow = feederServo.attached(); // Status attach saat ini

    if (!attachedNow && lastServoAttached) {
      // Servo BARU SAJA terlepas (sebelumnya OK, sekarang tidak)
      terminalLog("Servo: terlepas, mencoba re-attach...");
      initServo();                     // Coba attach ulang otomatis
    }
    else if (attachedNow && !lastServoAttached) {
      // Servo BARU SAJA berhasil attach (setelah sebelumnya error)
      lastServoAttached = true;        // Update status tracking
      servoReady        = true;        // Izinkan penggunaan servo
      terminalLog("Servo: SIAP kembali.");
    }
    // Jika tidak ada perubahan → diam
  }

  // ── Cek NTP sebelum lanjut ke bagian waktu ────────────────
  if (!ntpSynced) return; // NTP belum sync → skip semua bagian waktu

  struct tm t;
  if (!getLocalTime(&t, 0)) return; // Gagal baca waktu → skip

  // ── 3. LOG HARIAN & RESET COUNTER ────────────────────────
  checkDailyReset(t); // Kelola log sesi + reset tengah malam

  // ── 4. UPDATE STATUS DISPLAY V1 ──────────────────────────
  // Tampilkan "WAKTUNYA MAKAN" hanya jika waktu sekarang
  // berada dalam window jadwal pagi ATAU sore.
  // Konsisten dengan logika gating di BLYNK_WRITE(V0).
  bool inWindow = false;

  if (scheduleMorningHour >= 0 &&      // Jadwal pagi sudah diset?
      isWithinWindow(t.tm_hour, t.tm_min,
                     scheduleMorningHour, scheduleMorningMinute))
    inWindow = true;                   // Sekarang dalam window pagi

  if (scheduleEveningHour >= 0 &&      // Jadwal sore sudah diset?
      isWithinWindow(t.tm_hour, t.tm_min,
                     scheduleEveningHour, scheduleEveningMinute))
    inWindow = true;                   // Sekarang dalam window sore

  // Kirim status ke widget Label V1 di aplikasi
  Blynk.virtualWrite(V1, inWindow ? "WAKTUNYA MAKAN" : "STANDBY");
}


// =============================================================
//  BLYNK CALLBACK – menerima data dari aplikasi
// =============================================================

/*
 * BLYNK_WRITE(V5) – Jadwal Pagi
 * ──────────────────────────────
 * Dipanggil otomatis oleh library Blynk setiap kali user
 * mengubah nilai di widget Time Input V5 di aplikasi.
 *
 * TimeInputParam adalah tipe data khusus Blynk untuk Time Input.
 * hasStartTime() = true jika user sudah mengisi waktu mulai.
 * getStartHour() = jam yang dipilih user (0–23)
 * getStartMinute() = menit yang dipilih user (0–59)
 */
BLYNK_WRITE(V5) {
  TimeInputParam t(param);            // Parse parameter dari Blynk
  if (t.hasStartTime()) {             // Pastikan waktu mulai ada
    scheduleMorningHour   = t.getStartHour();   // Simpan jam
    scheduleMorningMinute = t.getStartMinute(); // Simpan menit
    morningDone = false;              // Reset flag – jadwal baru
                                      // berarti sesi pagi belum dilog
    terminalLog("Jadwal PAGI diset: "
                + formatJadwal(scheduleMorningHour, scheduleMorningMinute)
                + " (window " + String(FEED_WINDOW_MINUTES) + " menit)");
  }
}

/*
 * BLYNK_WRITE(V6) – Jadwal Sore
 * ──────────────────────────────
 * Sama seperti V5 tapi untuk jadwal sore.
 */
BLYNK_WRITE(V6) {
  TimeInputParam t(param);            // Parse parameter dari Blynk
  if (t.hasStartTime()) {             // Pastikan waktu mulai ada
    scheduleEveningHour   = t.getStartHour();   // Simpan jam
    scheduleEveningMinute = t.getStartMinute(); // Simpan menit
    eveningDone = false;              // Reset flag – jadwal baru
    terminalLog("Jadwal SORE diset: "
                + formatJadwal(scheduleEveningHour, scheduleEveningMinute)
                + " (window " + String(FEED_WINDOW_MINUTES) + " menit)");
  }
}

/*
 * BLYNK_WRITE(V7) – Slider Sudut Buka Servo
 * ──────────────────────────────────────────
 * Dipanggil setiap kali user menggeser Slider V7 di aplikasi.
 *
 * Nilai dari slider (10–180) disimpan ke servoOpenAngle.
 * Validasi dilakukan agar tidak keluar dari batas aman.
 *
 * FITUR PREVIEW: Jika servo tidak sedang bergerak, servo akan
 * bergerak ke sudut baru selama 1 detik lalu kembali ke posisi
 * tutup. Ini membantu kalibrasi sudut mekanik feeder.
 */
BLYNK_WRITE(V7) {
  int angle = param.asInt();          // Baca nilai integer dari slider

  // Validasi batas – clamp ke range yang aman
  if (angle < SERVO_OPEN_MIN) angle = SERVO_OPEN_MIN; // Minimal 10°
  if (angle > SERVO_OPEN_MAX) angle = SERVO_OPEN_MAX; // Maksimal 180°

  servoOpenAngle = angle;             // Simpan sudut yang sudah divalidasi

  terminalLog("Sudut buka diubah: " + String(servoOpenAngle) + " deg"
              + " (min=" + String(SERVO_OPEN_MIN)
              + " max=" + String(SERVO_OPEN_MAX) + ")");

  // Preview gerakan servo (hanya jika tidak sedang pakan)
  if (servoReady && !feedInProgress) {
    feederServo.write(servoOpenAngle);  // Gerak ke sudut baru
    terminalLog("Preview: " + String(servoOpenAngle) + " deg...");

    // Kembali ke posisi tutup setelah 1 detik
    // Lambda function sebagai callback timer (C++11)
    timer.setTimeout(1000L, []() {
      feederServo.write(SERVO_POS_CLOSED); // Tutup kembali
      terminalLog("Preview selesai. Servo kembali tutup.");
    });
  }
}

/*
 * BLYNK_WRITE(V0) – Tombol Pakan (Time-Gated Control)
 * ─────────────────────────────────────────────────────
 * Dipanggil setiap kali nilai widget Button V0 berubah.
 * Nilai 1 = tombol ditekan, nilai 0 = tombol dilepas.
 *
 * Alur keamanan (time-gating):
 *   1. Abaikan jika btnState != 1 (tombol dilepas)
 *   2. Debounce 2 detik (cegah spam tap)
 *   3. Cek NTP sudah sync
 *   4. Ambil waktu saat ini
 *   5. Cek apakah waktu dalam window pagi ATAU sore
 *   6. Jika ya → activateFeeder()
 *   7. Jika tidak → log DITOLAK dengan info waktu lengkap
 */
BLYNK_WRITE(V0) {
  int btnState = param.asInt();        // 1=ditekan, 0=dilepas
  if (btnState != 1) return;           // Abaikan event saat dilepas

  // Debounce: tolak panggilan ulang dalam 2 detik
  // 'static' agar nilai lastPress persisten antar pemanggilan
  static unsigned long lastPress = 0;
  if (millis() - lastPress < 2000) return; // Terlalu cepat → abaikan
  lastPress = millis();                // Catat waktu tekan terakhir

  // Pastikan NTP sudah sync sebelum cek jadwal
  if (!ntpSynced) {
    terminalLog("ERROR: Waktu NTP belum tersinkronisasi.");
    return;
  }

  struct tm t;
  if (!getLocalTime(&t, 0)) {          // Ambil waktu saat ini
    terminalLog("ERROR: Gagal membaca waktu.");
    return;
  }

  int  nowHour   = t.tm_hour;          // Jam saat ini (0–23)
  int  nowMin    = t.tm_min;           // Menit saat ini (0–59)
  bool isMorning = false;              // Flag: apakah ini sesi pagi?
  bool gateOpen  = false;              // Flag: apakah tombol diizinkan?

  // Cek apakah waktu sekarang dalam window jadwal PAGI
  if (scheduleMorningHour >= 0 &&      // Jadwal pagi sudah diset?
      isWithinWindow(nowHour, nowMin,
                     scheduleMorningHour, scheduleMorningMinute)) {
    isMorning = true;                  // Ini sesi pagi
    gateOpen  = true;                  // Tombol diizinkan
  }

  // Cek apakah waktu sekarang dalam window jadwal SORE
  if (scheduleEveningHour >= 0 &&      // Jadwal sore sudah diset?
      isWithinWindow(nowHour, nowMin,
                     scheduleEveningHour, scheduleEveningMinute)) {
    gateOpen = true;                   // Tombol diizinkan
    // isMorning tetap true jika jadwal pagi & sore di jam+menit sama
    // (prioritas pagi)
  }

  // Tolak jika di luar semua window jadwal
  if (!gateOpen) {
    // Pesan detail membantu debugging jadwal
    String nowStr = formatJadwal(nowHour, nowMin); // Waktu sekarang
    String pagStr = (scheduleMorningHour >= 0)     // Jadwal pagi
                    ? formatJadwal(scheduleMorningHour, scheduleMorningMinute)
                    : "--:--";                     // Belum diset
    String sorStr = (scheduleEveningHour >= 0)     // Jadwal sore
                    ? formatJadwal(scheduleEveningHour, scheduleEveningMinute)
                    : "--:--";                     // Belum diset
    terminalLog("DITOLAK: Bukan jadwal pakan!"
                " Skrg=" + nowStr
                + " Pagi=" + pagStr
                + "~+" + String(FEED_WINDOW_MINUTES) + "m"
                + " Sore=" + sorStr
                + "~+" + String(FEED_WINDOW_MINUTES) + "m");
    return;
  }

  // Semua cek lolos → jalankan pakan
  activateFeeder(isMorning);
}



/*
 * BLYNK_CONNECTED()
 * ─────────────────
 * Dipanggil otomatis setiap kali ESP32 berhasil terhubung
 * atau terhubung KEMBALI ke server Blynk setelah terputus.
 *
 * syncVirtual() meminta server Blynk mengirim ulang nilai
 * terakhir dari pin yang disebutkan ke device ini.
 * Ini penting agar jadwal (V5,V6) dan sudut (V7) tidak
 * hilang saat ESP32 restart atau koneksi terputus.
 */
BLYNK_CONNECTED() {
  // Minta server kirim ulang nilai terakhir semua pin penting
  Blynk.syncVirtual(V3, V4, V5, V6, V7); // Counter, jadwal, sudut servo

  terminalLog("Blynk terhubung. Data disinkronisasi.");
  terminalLog("Sistem AKTIF | Window="
              + String(FEED_WINDOW_MINUTES) + "m | Sudut buka="
              + String(servoOpenAngle) + " deg");
}


// =============================================================
//  SETUP – dijalankan SEKALI saat ESP32 pertama menyala
// =============================================================
void setup() {
  Serial.begin(115200);               // Mulai komunikasi serial 115200 baud
  Serial.println("\n=== Sistem Pemberi Pakan Ikan ===");

  // ── Koneksi WiFi ─────────────────────────────────────────
  WiFi.begin(ssid, password);         // Mulai proses koneksi WiFi
  Serial.print("Menghubungkan WiFi");

  // Loop tunggu WiFi maksimal 15 detik
  // Menggunakan millis() agar tidak hang selamanya
  unsigned long wifiTimeout = millis() + 15000; // Batas waktu 15 detik
  while (WiFi.status() != WL_CONNECTED  // Belum terhubung?
         && millis() < wifiTimeout) {   // Belum timeout?
    delay(500);                         // Tunggu 500ms
    Serial.print(".");                  // Indikator progress
  }
  Serial.println();                     // Newline setelah titik-titik

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi OK: " + WiFi.localIP().toString());
    lastWifiConnected = true;           // Update status tracking

    // ── Sinkronisasi NTP ───────────────────────────────────
    // configTime() mengatur timezone dan memulai request ke NTP server
    // Parameter: gmtOffset (detik), daylightOffset (detik), server
    configTime(gmtOffset, daylightOffset, ntpServer);
    Serial.print("Sinkronisasi NTP");

    // Tunggu NTP berhasil maksimal 10 detik
    // getLocalTime(ms=200) = coba selama 200ms per iterasi
    struct tm timeInfo;
    unsigned long ntpTimeout = millis() + 10000; // Batas 10 detik
    while (!getLocalTime(&timeInfo, 200) // Coba ambil waktu
           && millis() < ntpTimeout) {   // Belum timeout?
      Serial.print(".");                 // Indikator progress
    }
    Serial.println();

    if (getLocalTime(&timeInfo, 0)) {    // Verifikasi final (ms=0)
      ntpSynced = true;                  // NTP berhasil
      char buf[22];
      strftime(buf, sizeof(buf),
               "%H:%M:%S %d/%m/%Y", &timeInfo);
      Serial.println("NTP OK: " + String(buf)); // Cetak waktu
    } else {
      // NTP gagal di setup, akan dicoba ulang tiap 5 detik
      Serial.println("NTP belum sync – retry otomatis tiap 5 detik.");
    }

  } else {
    // WiFi gagal – sistem tetap jalan tapi tanpa Blynk & NTP
    Serial.println("WiFi GAGAL – melanjutkan tanpa koneksi.");
  }

  // ── Inisialisasi Blynk ────────────────────────────────────
  // config() menyimpan auth token tanpa langsung connect
  Blynk.config(BLYNK_AUTH_TOKEN);
  // connect() memulai koneksi ke server Blynk
  // BLYNK_CONNECTED() akan dipanggil saat berhasil
  Blynk.connect();

  // ── Inisialisasi Servo ────────────────────────────────────
  initServo();          // Attach servo dan posisikan ke 0°
  delay(SERVO_MOVE_TIME); // Tunggu servo benar-benar di 0°
                          // Ini satu-satunya delay() di seluruh kode
                          // Boleh karena masih di setup(), bukan loop()

  // ── Daftarkan timer 5 detik ───────────────────────────────
  // checkSystemStatus() akan dipanggil setiap 5000ms secara otomatis
  timer.setInterval(5000L, checkSystemStatus);

  // ── Inisialisasi tampilan dashboard ──────────────────────
  Blynk.virtualWrite(V1, "STANDBY");   // Status awal = STANDBY
  Blynk.virtualWrite(V3, 0);           // Counter pagi = 0
  Blynk.virtualWrite(V4, 0);           // Counter sore = 0
  Blynk.virtualWrite(V7, servoOpenAngle); // Sinkronkan slider dengan nilai default
}


// =============================================================
//  LOOP – dijalankan TERUS MENERUS setelah setup()
// =============================================================
void loop() {
  Blynk.run(); // Mengelola: koneksi ke server, menerima data dari
               // aplikasi, memanggil BLYNK_WRITE(), BLYNK_CONNECTED()
               // WAJIB dipanggil sesering mungkin agar tidak disconnect

  timer.run(); // Menjalankan semua timer yang terdaftar:
               // - checkSystemStatus() setiap 5 detik
               // - closeServo() setelah SERVO_MOVE_TIME+SERVO_OPEN_HOLD
               // - finishFeeding() setelah SERVO_MOVE_TIME
               // - preview tutup setelah 1000ms (dari V7)
               // WAJIB dipanggil di loop() agar timer bekerja
}


