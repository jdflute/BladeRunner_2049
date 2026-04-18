// --- FS selection & TJpg_Decoder mapping ---
#define FS_NO_GLOBALS
#include <Arduino.h>
#include <FS.h>
using fs::File;            // allow 'File' with FS_NO_GLOBALS
#include <LittleFS.h>

#include <TJpg_Decoder.h>
#include <TFT_eSPI.h>
#include <vector>

// ---------- User Configuration ----------
#define SLIDES_DIR       "/slides"      // Folder in LittleFS containing JPEGs
#define DISPLAY_SECONDS  1.5              // Seconds per image (default speed)
#define ROTATION         3              // 0-3; 1 = landscape for many ILI9488 modules
#define BACKGROUND_COLOR TFT_BLACK      // Background fill

// --- NEW: Two speed presets based on filename tags ---
#define FAST_SECONDS     0.5              // e.g. for files containing "fast"
#define SLOW_SECONDS     3              // e.g. for files containing "slow"
// Change the numbers above to whatever you like.
// ---------------------------------------

TFT_eSPI tft = TFT_eSPI();  // Pins set in TFT_eSPI User_Setup.h
std::vector<String> imageList;

// ---- TJpg_Decoder output callback ----
static bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap)
{
  // Continue decoding even if a block starts off-screen (avoid aborting)
  if (x >= tft.width() || y >= tft.height()) return true;
  tft.pushImage(x, y, w, h, bitmap);
  return true;
}

// ==============================
// === Buttons & Playback State ===
// ==============================

// --- Button pins (change to suit your wiring) ---
// Wire each button between the pin and GND. We use INPUT_PULLUP so idle=HIGH, pressed=LOW.
#define BTN_PLAY_PAUSE   26
#define BTN_NEXT         32   // Updated to avoid GPIO 27
#define BTN_PREV         33   // Updated to avoid GPIO 27
#define DEBOUNCE_MS      35

struct DebouncedButton {
  uint8_t pin;
  bool stable = HIGH;     // stable (debounced) logic level
  bool lastRead = HIGH;   // last raw read
  uint32_t lastChange = 0;
  bool pressedEvent = false; // latched on rising press event (HIGH->LOW since pull-up)

  void begin(uint8_t p) {
    pin = p;
    pinMode(pin, INPUT_PULLUP);
    stable = HIGH;
    lastRead = HIGH;
    lastChange = millis();
    pressedEvent = false;
  }

  void update() {
    bool r = digitalRead(pin);
    if (r != lastRead) {            // raw edge -> reset debounce timer
      lastRead = r;
      lastChange = millis();
    }
    // If stable for DEBOUNCE_MS, accept new state and generate events
    if ((millis() - lastChange) >= DEBOUNCE_MS) {
      if (stable != lastRead) {
        // Edge detected
        pressedEvent = (stable == HIGH && lastRead == LOW);  // button press (active LOW)
        stable = lastRead;
      } else {
        pressedEvent = false;
      }
    } else {
      pressedEvent = false;
    }
  }

  bool pressed() const { return pressedEvent; }  // true one loop after a clean press
};

DebouncedButton btnPlayPause, btnNext, btnPrev;

// Playback state
static size_t idx = 0;             // current image index
bool playing = true;               // play/pause toggle
uint32_t lastAdvanceMs = 0;        // timestamp when the current slide started showing

// --- NEW: Per-slide interval (ms), decided by filename ---
uint32_t currentIntervalMs = DISPLAY_SECONDS * 1000UL;

// Forward declarations
void drawCenteredJpeg(const String& path);

// ---- Helpers ----
bool endsWithIgnoreCase(const String& s, const char* ext)
{
  int sl = s.length();
  int el = strlen(ext);
  if (el > sl) return false;
  String tail = s.substring(sl - el);
  tail.toLowerCase();
  String e(ext); e.toLowerCase();
  return tail == e;
}

bool isJpegFile(const String& name)
{
  return endsWithIgnoreCase(name, ".jpg") || endsWithIgnoreCase(name, ".jpeg");
}

void scanDirForJpegs(const char* dirPath)
{
  File dir = LittleFS.open(dirPath);
  if (!dir || !dir.isDirectory()) {
    Serial.printf("Directory not found or not a directory: %s\n", dirPath);
    return;
  }

  File entry;
  while ((entry = dir.openNextFile())) {
    String p = String(entry.path());
    if (entry.isDirectory()) {
      scanDirForJpegs(p.c_str());
      entry.close();
      continue;
    }
    if (isJpegFile(p)) {
      imageList.push_back(p);
      Serial.printf("Found JPEG: %s (%u bytes)\n", p.c_str(), (unsigned)entry.size());
    }
    entry.close();
  }
}

void debugListSlides()
{
  Serial.println("---- Listing LittleFS /slides ----");
  if (!LittleFS.exists(SLIDES_DIR)) {
    Serial.println("Directory /slides does NOT exist.");
    return;
  }

  File dir = LittleFS.open(SLIDES_DIR);
  if (!dir || !dir.isDirectory()) {
    Serial.println("/slides is not a directory.");
    return;
  }

  File f;
  while ((f = dir.openNextFile())) {
    String path = String(f.path());
    uint32_t size = f.size();
    Serial.printf("Found: %s (%u bytes)\n", path.c_str(), (unsigned)size);

    // Existence check
    bool exists = LittleFS.exists(path.c_str());
    Serial.printf("  LittleFS.exists: %s\n", exists ? "true" : "false");

    // Decoder header parse check (PASS LittleFS and check JRESULT)
    uint16_t w = 0, h = 0;
    JRESULT jr = TJpgDec.getFsJpgSize(&w, &h, path.c_str(), LittleFS);
    Serial.printf("  TJpg size parse: %s -> %ux%u (code=%d)\n",
                  (jr == JDR_OK ? "OK" : "ERR"), w, h, jr);

    f.close();
  }
  Serial.println("-----------------------------------");
}

void drawErrorCard(const String& title, const String& message, uint16_t bg = TFT_RED, uint16_t fg = TFT_WHITE)
{
  tft.fillScreen(bg);
  tft.setTextColor(fg, bg);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(title, tft.width()/2, tft.height()/2 - 20);
  tft.drawString(message, tft.width()/2, tft.height()/2 + 10);
}

void drawCenteredJpeg(const String& path)
{
  // Check file existence on-device
  if (!LittleFS.exists(path.c_str())) {
    Serial.printf("LittleFS: file not found -> %s\n", path.c_str());
    drawErrorCard("File not found", path, TFT_RED, TFT_WHITE);
    delay(1000);
    return;
  }

  // Read JPEG header to get dimensions (PASS LittleFS and check JRESULT)
  uint16_t img_w = 0, img_h = 0;
  JRESULT sres = TJpgDec.getFsJpgSize(&img_w, &img_h, path.c_str(), LittleFS);
  if (sres != JDR_OK || img_w == 0 || img_h == 0) {
    Serial.printf("Decoder could not read JPEG header -> %s (code=%d)\n", path.c_str(), sres);
    drawErrorCard("Invalid/Unsupported JPEG", path, TFT_ORANGE, TFT_BLACK);
    delay(1000);
    return;
  }

  // Choose scale (valid: 1,2,4,8) that fits the screen
  uint8_t chosenScale = 1;
  for (uint8_t s : {1, 2, 4, 8}) {
    if ((img_w / s) <= tft.width() && (img_h / s) <= tft.height()) { chosenScale = s; break; }
  }
  TJpgDec.setJpgScale(chosenScale);

  int16_t dw = img_w / chosenScale;
  int16_t dh = img_h / chosenScale;
  int16_t x = (tft.width()  - dw) / 2;
  int16_t y = (tft.height() - dh) / 2;

  // Clear background and draw (PASS LittleFS and check JRESULT)
  tft.fillScreen(BACKGROUND_COLOR);

  uint32_t t0 = millis();
  JRESULT dres = TJpgDec.drawFsJpg(x, y, path.c_str(), LittleFS);
  uint32_t dt = millis() - t0;

  if (dres != JDR_OK) {
    Serial.printf("TJpgDec.drawFsJpg error=%d for %s\n", dres, path.c_str());
    drawErrorCard("Decode error", "Code: " + String(dres), TFT_DARKGREY, TFT_WHITE);
  } else {
    Serial.printf("Draw OK: %s (%ux%u / scale %u) at (%d,%d) in %lu ms\n",
                  path.c_str(), img_w, img_h, chosenScale, x, y, (unsigned long)dt);
  }
}

// --- NEW: Decide interval based on filename tokens "fast"/"slow" (case-insensitive)
uint32_t intervalForFile(const String& path) {
  String lower = path;
  lower.toLowerCase();

  if (lower.indexOf("fast") >= 0) {
    return (uint32_t)FAST_SECONDS * 1000UL;
  }
  if (lower.indexOf("slow") >= 0) {
    return (uint32_t)SLOW_SECONDS * 1000UL;
  }
  return (uint32_t)DISPLAY_SECONDS * 1000UL;
}

// Helpers to move index safely
inline void showImageAt(size_t newIdx) {
  if (imageList.empty()) return;
  idx = newIdx % imageList.size();
  drawCenteredJpeg(imageList[idx]);
  currentIntervalMs = intervalForFile(imageList[idx]);   // NEW: set slide-specific interval
  lastAdvanceMs = millis();  // reset auto-advance timer whenever we show an image
  Serial.printf("Slide interval: %lu ms (%s)\n", (unsigned long)currentIntervalMs, imageList[idx].c_str());
}

inline void nextImage() {
  if (imageList.empty()) return;
  idx = (idx + 1) % imageList.size();
  drawCenteredJpeg(imageList[idx]);
  currentIntervalMs = intervalForFile(imageList[idx]);   // NEW
  lastAdvanceMs = millis();
  Serial.printf("Slide interval: %lu ms (%s)\n", (unsigned long)currentIntervalMs, imageList[idx].c_str());
}

inline void prevImage() {
  if (imageList.empty()) return;
  idx = (idx + imageList.size() - 1) % imageList.size();
  drawCenteredJpeg(imageList[idx]);
  currentIntervalMs = intervalForFile(imageList[idx]);   // NEW
  lastAdvanceMs = millis();
  Serial.printf("Slide interval: %lu ms (%s)\n", (unsigned long)currentIntervalMs, imageList[idx].c_str());
}

// ------------------------------
// STEP 2: Verify A2.jpg on-device
// ------------------------------
void verifyA2() {
  const char *path = "/slides/A2.jpg";

  Serial.println("\n-- Verify A2.jpg --");
  bool exists = LittleFS.exists(path);
  Serial.printf("LittleFS.exists(%s) = %s\n", path, exists ? "true" : "false");
  if (!exists) return;

  File f = LittleFS.open(path, "r");
  if (!f) {
    Serial.println("LittleFS.open failed");
    return;
  }
  size_t sz = f.size();
  Serial.printf("Size: %u bytes\n", (unsigned)sz);

  // Read first 16 bytes to confirm JPEG signature
  uint8_t head[16] = {0};
  size_t n = f.read(head, sizeof(head));
  f.close();
  Serial.printf("Read %u bytes; magic = %02X %02X (expect FF D8)\n",
                (unsigned)n, head[0], head[1]);
}

void setup()
{
  Serial.begin(115200);
  delay(500);
  Serial.println("\nESP32 ILI9488 JPEG Slideshow (LittleFS + file checks + button playback + filename speeds)");

  // Mount LittleFS (set formatOnFail=true only if you want automatic formatting)
  if (!LittleFS.begin(/*formatOnFail=*/false)) {
    Serial.println("LittleFS mount failed! (Try set begin(true) once to format, then upload files again.)");
    tft.init();
    tft.setRotation(ROTATION);
    drawErrorCard("LittleFS mount failed", "Check Tools->Partition & upload FS");
    while (true) delay(1000);
  }

  // Init TFT
  tft.init();
  tft.setRotation(ROTATION);
  tft.setSwapBytes(true); // Important when pushing RGB565 blocks

  // Init JPEG decoder
  TJpgDec.setCallback(tft_output);

  // Optional diagnostics: list slides and header parse
  debugListSlides();

  // Scan for images
  imageList.clear();
  scanDirForJpegs(SLIDES_DIR);

  // Init buttons
  btnPlayPause.begin(BTN_PLAY_PAUSE);
  btnNext.begin(BTN_NEXT);
  btnPrev.begin(BTN_PREV);

  if (imageList.empty()) {
    tft.fillScreen(BACKGROUND_COLOR);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, BACKGROUND_COLOR);
    tft.drawString("No JPEGs found in " SLIDES_DIR, tft.width()/2, tft.height()/2);
    Serial.println("No JPEG files found. Add images to /data/slides and upload LittleFS.");
  } else {
    Serial.printf("Total JPEGs discovered: %u\n", (unsigned)imageList.size());
    // Show the first image and start the timer
    showImageAt(0);               // sets currentIntervalMs based on filename
    playing = true;
  }
}

void loop()
{
  // If no images, just idle
  if (imageList.empty()) {
    delay(50);
    return;
  }

  // Update debounced buttons
  btnPlayPause.update();
  btnNext.update();
  btnPrev.update();

  // --- Handle button events ---
  if (btnPlayPause.pressed()) {
    playing = !playing;
    Serial.printf("Playback %s\n", playing ? "RESUMED" : "PAUSED");
    // Optional: show a tiny overlay or flash border to indicate pause/resume
    lastAdvanceMs = millis();  // reset timer to avoid immediate auto-advance on resume
  }

  if (btnNext.pressed()) {
    Serial.println("Next image");
    nextImage();               // always step forward one on press
  }

  if (btnPrev.pressed()) {
    Serial.println("Previous image");
    prevImage();               // always step back one on press
  }

  // --- Auto-advance when playing (per-slide interval) ---
  if (playing) {
    if (millis() - lastAdvanceMs >= currentIntervalMs) {
      nextImage();
    }
  }

  // Friendly yield
  delay(5);
  yield();
}
