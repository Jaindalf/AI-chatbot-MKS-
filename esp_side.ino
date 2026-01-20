// =================================================================================================
// TRINITY VOICE ASSISTANT - SD VERSION (Non-PSRAM Safe, SWITCH CONTROLLED)
// =================================================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <SD.h>
#include <SPI.h>
#include <driver/i2s.h>

// =================================================================================================
// 1. CONFIGURATION
// =================================================================================================

// ---- Wi-Fi ----
const char* WIFI_SSID = "motoedge60fusion_8370";
const char* WIFI_PASS = "Ascii@008";

// ---- Server ----
const char* SERVER_URL = "http://10.132.106.197:5002/voice_input";

// ---- Audio ----
#define SAMPLE_RATE       16000
#define BITS_PER_SAMPLE   16
#define CHANNELS          1

// ---- Recording ----
#define MAX_RECORD_SECONDS  6
#define I2S_READ_CHUNK_SIZE 2048
const size_t MAX_RECORD_BYTES =
  SAMPLE_RATE * MAX_RECORD_SECONDS * (BITS_PER_SAMPLE / 8) * CHANNELS;

const char* AUDIO_PATH = "/recorded.pcm";

// =================================================================================================
// 2. PIN DEFINITIONS
// =================================================================================================

// === I2S Microphone (INMP441) ===
#define I2S_MIC_PORT I2S_NUM_0
#define PIN_I2S_MIC_BCLK 33
#define PIN_I2S_MIC_LRCK 25
#define PIN_I2S_MIC_DIN  34

// === I2S Speaker (MAX98357A) ===
#define I2S_SPK_PORT I2S_NUM_1
#define PIN_I2S_SPK_BCLK 27
#define PIN_I2S_SPK_LRCK 14
#define PIN_I2S_SPK_DOUT 26

// === SD Card (VSPI default pins used internally) ===
#define SD_CS   5

// === UI ===
#define PIN_LED_REC   16
#define PIN_LED_PLAY  17
#define PIN_SWITCH    32   // ACTIVE LOW (INPUT_PULLUP)

// =================================================================================================
// 3. GLOBALS
// =================================================================================================

bool isRecording = false;
bool lastSwitchState = false;
size_t recordedBytes = 0;

File audioFile;
HTTPClient httpClient;

// =================================================================================================
// 4. SD DEBUG HELPERS
// =================================================================================================

bool sdWriteTest() {
  Serial.println("[SD TEST] Creating /test.txt");

  File test = SD.open("/test.txt", FILE_WRITE);
  if (!test) {
    Serial.println("[SD TEST] ❌ FAILED");
    return false;
  }

  test.println("SD write OK");
  test.close();

  Serial.println("[SD TEST] ✅ PASSED");
  return true;
}

// =================================================================================================
// 5. I2S INITIALIZATION
// =================================================================================================

void i2s_mic_init() {
  const i2s_config_t config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = 0,
    .dma_buf_count = 4,
    .dma_buf_len = 128,
    .use_apll = false,
    .fixed_mclk = 0
  };

  const i2s_pin_config_t pins = {
    .bck_io_num = PIN_I2S_MIC_BCLK,
    .ws_io_num  = PIN_I2S_MIC_LRCK,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num  = PIN_I2S_MIC_DIN
  };

  i2s_driver_install(I2S_MIC_PORT, &config, 0, NULL);
  i2s_set_pin(I2S_MIC_PORT, &pins);
  i2s_zero_dma_buffer(I2S_MIC_PORT);

  Serial.println("[INIT] I2S0 (Mic) ready.");
}

void i2s_spk_init() {
  const i2s_config_t config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format =
      (i2s_comm_format_t)(I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB),
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 512,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = -1
  };

  const i2s_pin_config_t pins = {
    .mck_io_num = I2S_PIN_NO_CHANGE,
    .bck_io_num = PIN_I2S_SPK_BCLK,
    .ws_io_num  = PIN_I2S_SPK_LRCK,
    .data_out_num = PIN_I2S_SPK_DOUT,
    .data_in_num  = I2S_PIN_NO_CHANGE
  };

  i2s_driver_install(I2S_SPK_PORT, &config, 0, NULL);
  i2s_set_pin(I2S_SPK_PORT, &pins);
  i2s_set_clk(I2S_SPK_PORT, SAMPLE_RATE,
              I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);
  i2s_zero_dma_buffer(I2S_SPK_PORT);

  Serial.println("[INIT] I2S1 (Speaker) ready.");
}

// =================================================================================================
// 6. SERVER COMMUNICATION
// =================================================================================================

void processVoiceCommand() {
  Serial.println("[HTTP] Opening recorded file...");

  File file = SD.open(AUDIO_PATH, FILE_READ);
  if (!file) {
    Serial.println("[ERR] Cannot open file for upload!");
    return;
  }

  size_t fileSize = file.size();
  Serial.printf("[HTTP] Uploading %u bytes\n", (unsigned)fileSize);

  httpClient.begin(SERVER_URL);
  httpClient.addHeader("Content-Type", "application/octet-stream");
  httpClient.setTimeout(15000);

  int httpCode = httpClient.sendRequest("POST", &file, fileSize);
  file.close();

  if (httpCode == HTTP_CODE_OK) {
    Serial.println("[HTTP] Server OK, streaming audio...");
    digitalWrite(PIN_LED_PLAY, HIGH);

    WiFiClient* stream = httpClient.getStreamPtr();
    uint8_t buffer[I2S_READ_CHUNK_SIZE];
    size_t bytes_written = 0;

    while (stream->connected()) {
      int bytesRead = stream->readBytes((char*)buffer, sizeof(buffer));
      if (bytesRead > 0) {
        i2s_write(I2S_SPK_PORT, buffer, bytesRead,
                  &bytes_written, portMAX_DELAY);
      } else if (!stream->available()) {
        break;
      }
    }

    digitalWrite(PIN_LED_PLAY, LOW);
    Serial.println("[SPK] Playback finished");
  } else {
    Serial.printf("[ERR] HTTP failed: %d\n", httpCode);
  }

  httpClient.end();
}

// =================================================================================================
// 7. SETUP
// =================================================================================================

void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println("\n=== TRINITY SD VERSION (SWITCH CONTROLLED) ===");

  pinMode(PIN_LED_REC, OUTPUT);
  pinMode(PIN_LED_PLAY, OUTPUT);
  pinMode(PIN_SWITCH, INPUT_PULLUP);

  digitalWrite(PIN_LED_REC, LOW);
  digitalWrite(PIN_LED_PLAY, LOW);

  // ---- Wi-Fi ----
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.printf("\n[OK] Wi-Fi connected. IP: %s\n",
                WiFi.localIP().toString().c_str());

  // ---- SD ----
  if (!SD.begin(SD_CS)) {
    Serial.println("[ERR] SD mount failed!");
  } else {
    Serial.println("[OK] SD mounted");
    sdWriteTest();
  }

  // ---- I2S ----
  i2s_mic_init();
  i2s_spk_init();

  Serial.println("Ready. Use TOGGLE SWITCH to record / play.");
}

// =================================================================================================
// 8. LOOP
// =================================================================================================

void loop() {
  bool switchOn = (digitalRead(PIN_SWITCH) == LOW);

  // ---- START RECORDING ----
  if (switchOn && !lastSwitchState && !isRecording) {
    Serial.println("[REC] Start recording (switch)");
    digitalWrite(PIN_LED_REC, HIGH);
    digitalWrite(PIN_LED_PLAY, LOW);

    if (SD.exists(AUDIO_PATH)) {
      SD.remove(AUDIO_PATH);
    }

    audioFile = SD.open(AUDIO_PATH, FILE_APPEND);
    if (!audioFile) {
      Serial.println("[ERR] Failed to open file!");
      return;
    }

    recordedBytes = 0;
    i2s_start(I2S_MIC_PORT);
    isRecording = true;
  }

  // ---- STOP + UPLOAD + PLAYBACK ----
  if (!switchOn && lastSwitchState && isRecording) {
    Serial.println("[REC] Stop recording (switch)");
    digitalWrite(PIN_LED_REC, LOW);

    i2s_stop(I2S_MIC_PORT);
    isRecording = false;

    audioFile.flush();
    audioFile.close();

    Serial.printf("[REC] Total bytes: %u\n", (unsigned)recordedBytes);
    processVoiceCommand();
  }

  lastSwitchState = switchOn;

  // ---- RECORDING LOOP ----
  if (isRecording) {
    uint8_t chunk[I2S_READ_CHUNK_SIZE];
    size_t bytesRead = 0;

    if (i2s_read(I2S_MIC_PORT, chunk, sizeof(chunk),
                 &bytesRead, 10 / portTICK_PERIOD_MS) == ESP_OK &&
        bytesRead > 0) {

      audioFile.write(chunk, bytesRead);
      recordedBytes += bytesRead;

      if (recordedBytes % 32768 == 0) {
        audioFile.flush();
      }

      if (recordedBytes >= MAX_RECORD_BYTES) {
        Serial.println("[WARN] Max duration reached");
        i2s_stop(I2S_MIC_PORT);
        isRecording = false;
        audioFile.close();
        digitalWrite(PIN_LED_REC, LOW);
        processVoiceCommand();
      }
    }
  }

  delay(10);
}
