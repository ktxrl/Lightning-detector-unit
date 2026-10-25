// Lightning emulator: drives a coil with 500 kHz bursts shaped like a lightning flash.
// Wiring: GPIO 18 -> 220 ohm resistor -> coil -> GND
//         Push button: one leg -> GPIO 27, other leg -> GND (no resistor needed)
// Press the external button (or the onboard BOOT button) to fire one flash.
//
// Tune the flash shape live from the Serial Monitor (115200 baud, "Newline" line ending):
//   strokes 3      number of strokes in one flash (1-6)
//   decay 600      how long each stroke takes to fade, in microseconds
//   gap 50         time between strokes, in milliseconds
//   power 64       starting strength, 1-64 (64 = strongest)
//   test 5         COIL TEST: steady 500 kHz for 5 seconds (detector should report "Noise")
//   show           print current settings

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

// SIMULATION MODE (ESP-NOW, channel 1):
//   - sends a small "beacon" 5 times a second so the station can track how close the wand is
//   - each button press sends a "simulated strike"; strength = how long the button is held
//     (quick tap = weak, 2 seconds or longer = 100%)
// The station labels these as SIMULATED on its dashboard.
uint8_t BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

#define COIL_PIN   18
#define BUTTON_PIN 0        // onboard BOOT button
#define FIRE_PIN   27       // external "lightning" button, wired to GND
#define FREQ_HZ    500000
#define RES_BITS   7        // 7-bit duty: 0-127, so 64 = 50% duty = strongest 500 kHz signal

int numStrokes = 3;
int decayUs    = 600;
int gapMs      = 50;
int power      = 64;

void setDuty(int duty) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(COIL_PIN, duty);
#else
  ledcWrite(0, duty);
#endif
}

// One stroke: jump to full strength, then fade out exponentially
void stroke(int startDuty) {
  const int stepUs = 20;
  for (int t = 0; t < decayUs * 3; t += stepUs) {
    int duty = (int)(startDuty * exp(-(float)t / decayUs));
    if (duty < 1) break;
    setDuty(duty);
    delayMicroseconds(stepUs);
  }
  setDuty(0);
}

void sendBeacon() {
  static unsigned long last = 0;
  if (millis() - last < 200) return;
  last = millis();
  esp_now_send(BROADCAST, (const uint8_t*)"LBCN", 4);
}

void sendSim(uint8_t strength) {
  uint8_t msg[5] = {'L', 'S', 'I', 'M', strength};
  esp_err_t r = esp_now_send(BROADCAST, msg, 5);
  Serial.printf(r == ESP_OK ? "Simulated strike sent (strength %d%%)\n" : "Send failed\n", strength);
}

void fireFlash() {
  Serial.println("FLASH!");
  int duty = power;
  for (int i = 0; i < numStrokes; i++) {
    stroke(duty);
    duty = max(1, (int)(duty * 0.6));   // later strokes are usually weaker
    delay(gapMs);
  }
}

void showSettings() {
  Serial.printf("strokes=%d  decay=%dus  gap=%dms  power=%d\n", numStrokes, decayUs, gapMs, power);
}

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(FIRE_PIN, INPUT_PULLUP);    // internal pull-up: reads HIGH until pressed
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(COIL_PIN, FREQ_HZ, RES_BITS);
#else
  ledcSetup(0, FREQ_HZ, RES_BITS);
  ledcAttachPin(COIL_PIN, 0);
#endif
  setDuty(0);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);   // same channel as the station's network
  if (esp_now_init() == ESP_OK) {
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, BROADCAST, 6);
    peer.channel = 1;
    peer.encrypt = false;
    esp_now_add_peer(&peer);
  } else {
    Serial.println("ESP-NOW init failed");
  }
  Serial.println("Lightning emulator ready. Press the button (GPIO 27) or BOOT to fire.");
  showSettings();
}

void loop() {
  sendBeacon();
  if (digitalRead(BUTTON_PIN) == LOW || digitalRead(FIRE_PIN) == LOW) {
    delay(20);  // debounce: ignore the button's contact bounce
    if (digitalRead(BUTTON_PIN) == LOW || digitalRead(FIRE_PIN) == LOW) {
      if (digitalRead(FIRE_PIN) == LOW) Serial.println("Button pressed (GPIO 27)");
      else                              Serial.println("Button pressed (BOOT)");
      unsigned long pressStart = millis();
      fireFlash();
      // wait for release (keep beaconing); hold time sets the strike strength
      unsigned long lastChg = 0;
      while (digitalRead(BUTTON_PIN) == LOW || digitalRead(FIRE_PIN) == LOW) {
        sendBeacon();
        if (millis() - lastChg >= 100) {             // live "charging" level, 10x per second
          lastChg = millis();
          unsigned long h = millis() - pressStart;
          uint8_t c[5] = {'L', 'C', 'H', 'G', (uint8_t)constrain((long)(10 + h * 90 / 2000), 10L, 100L)};
          esp_now_send(BROADCAST, c, 5);
        }
        delay(10);
      }
      unsigned long held = millis() - pressStart;
      uint8_t strength = (uint8_t)constrain((long)(10 + held * 90 / 2000), 10L, 100L);
      Serial.printf("Button released after %lu ms\n", held);
      sendSim(strength);
      delay(200);
    }
  }

  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    int space = cmd.indexOf(' ');
    String key = (space > 0) ? cmd.substring(0, space) : cmd;
    int val = (space > 0) ? cmd.substring(space + 1).toInt() : 0;

    if (key == "test") {
      int secs = constrain(val > 0 ? val : 5, 1, 30);
      Serial.printf("COIL TEST: steady 500 kHz for %d s...\n", secs);
      setDuty(64);                       // 50% duty = strongest 500 kHz
      delay(secs * 1000UL);
      setDuty(0);
      Serial.println("COIL TEST done");
    }
    else if (key == "strokes") numStrokes = constrain(val, 1, 6);
    else if (key == "decay") decayUs = constrain(val, 50, 20000);
    else if (key == "gap") gapMs = constrain(val, 5, 500);
    else if (key == "power") power = constrain(val, 1, 64);
    showSettings();
  }
}
