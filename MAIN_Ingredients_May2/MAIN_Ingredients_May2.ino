/*************************************************************
  Let Em Cook - Ingredient Station / Pantry

  - Uses LetEmCook shared protocol types
  - Boots straight into ESP-NOW gameplay mode
  - No Wi-Fi blocking during normal game mode
  - Ingredient station owns overwriting/resetting plates
  - Supports shared maintenance / OTA flow
  - Uses ingredient LEDs as OTA/update indicators
*************************************************************/

#define LEC_DEBUG 1

#include <LetEmCook.h>

#include <esp_now.h>
#include <WiFi.h>
#include <MFRC522.h>
#include <SPI.h>
#include <DYPlayerArduino.h>

extern "C" {
  #include "esp_wifi.h"
}

/*************************************************************
  PIN DEFINITIONS
*************************************************************/

// RFID reader pins
#define SS_PIN 21
#define RST_PIN 22

// Button pins
#define BUTTON_TOMATO 25
#define BUTTON_LETTUCE 34
#define BUTTON_CHEESE 27
#define BUTTON_MEAT 14
#define BUTTON_APPLE 26
#define BUTTON_DOUGH 35

// LED pin definitions
#define LED_LETTUCE 12
#define LED_DOUGH 13
#define LED_MEAT 32
#define LED_CHEESE 33
#define LED_APPLE 2
#define LED_TOMATO 4

// Audio module pins
#define AUDIO_TX 16
#define AUDIO_RX 17

/*************************************************************
  LOCAL CONSTANTS
*************************************************************/

#define INGREDIENT_DEBOUNCE_MS 300
#define HELLO_RETRY_MS 3000

/*************************************************************
  HARDWARE INSTANCES
*************************************************************/

MFRC522 rfid(SS_PIN, RST_PIN);

HardwareSerial audioSerial(2);
DY::Player audioModule(&audioSerial);

/*************************************************************
  NETWORK / PACKETS
*************************************************************/

uint8_t serverAddress[6];

LecPacket outgoingPacket;
LecPacket incomingPacket;

// CHANGED: Maintenance packets are queued from onDataRecv()
// and processed in loop(), so OTA does not run inside ESP-NOW callback.
LecPacket pendingMaintenancePacket;
volatile bool pendingMaintenancePacketAvailable = false;

volatile bool dataReceived = false;
bool requestSent = false;
unsigned long lastHelloSentAt = 0;

/*************************************************************
  GAME STATE
*************************************************************/

char lastIngredientPressed[INGREDIENT_LENGTH] = "";
char pendingRfid[RFID_LENGTH] = "";

bool gameRunning = false;
bool onFire = false;

unsigned long lastFlashTime = 0;
bool flashState = false;

int activeLED = -1;
int currentLEDToLightPin = -1;

enum IngredientProcessState {
  INGREDIENT_IDLE,
  INGREDIENT_WAITING_FOR_RFID,
  INGREDIENT_PROCESSING_RFID
};

IngredientProcessState currentState = INGREDIENT_IDLE;
unsigned long rfidStartTime = 0;

/*************************************************************
  BUTTON STATE
*************************************************************/

// Previous state variables for edge detection
bool previousButtonTomato = false;
bool previousButtonLettuce = false;
bool previousButtonCheese = false;
bool previousButtonMeat = false;
bool previousButtonApple = false;
bool previousButtonDough = false;

// Debounce tracking
unsigned long lastDebounceTomato = 0;
unsigned long lastDebounceLettuce = 0;
unsigned long lastDebounceCheese = 0;
unsigned long lastDebounceMeat = 0;
unsigned long lastDebounceApple = 0;
unsigned long lastDebounceDough = 0;

/*************************************************************
  FUNCTION PROTOTYPES
*************************************************************/

void initializePins();
void initializeEspNow();
void initializeAudio();

void resetOutgoingPacket();
void sendHelloToServer();
void sendDataToServer(LecPacket* packet);
void processHelloHeartbeat();

void processPendingMaintenancePacket();

void sendMaintenanceResultToServer(
  LecPacketType packetType,
  LecPacketResult result,
  const char* status,
  const char* payload
);

void handleMaintenanceStatus(const char* status);
void handleMaintenanceProgress(uint8_t percent);

void showMaintenanceProgress(uint8_t percent);
void showMaintenanceActivity();
void showMaintenanceSuccess();
void showMaintenanceError();

String checkButtons();
int getLEDPin(const char* ingredient);

bool readCurrentRfid(char* outRfid);
bool PICC_IsAnyCardPresent();

void handlePlateStateResponse();
void handleFireLedEffect();

void playSuccessFeedback();
void playOverwriteFeedback();
void playErrorFeedback(const char* currentIngredient);

void setAllIngredientLeds(bool state);
void clearAllIngredientLeds();

/*************************************************************
  ESP-NOW CALLBACKS
*************************************************************/

void onDataSent(const wifi_tx_info_t* tx_info, esp_now_send_status_t status) {
  Serial.print("Send status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAILED");
}

void onDataRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (len != sizeof(LecPacket)) {
    Serial.println("Received data size mismatch.");
    return;
  }

  LecPacket received;
  memcpy(&received, data, sizeof(received));

  if (!lecIsValidPacket(received, len)) {
    Serial.println("Invalid Let Em Cook packet received.");
    return;
  }

  Serial.println("=== Packet received from server ===");
  Serial.print("Packet Type: ");
  Serial.println(static_cast<int>(received.packetType));
  Serial.print("Round: ");
  Serial.println(static_cast<int>(received.round));
  Serial.print("RFID: ");
  Serial.println(received.rfid);
  Serial.print("Ingredient: ");
  Serial.println(received.ingredient);
  Serial.print("Recipe: ");
  Serial.println(received.recipeName);
  Serial.print("Result: ");
  Serial.println(static_cast<int>(received.result));
  Serial.println("===================================");

  // CHANGED: Queue maintenance/update packets instead of running OTA here.
  if (LecMaintenance::shouldHandlePacket(received)) {
    memcpy(&pendingMaintenancePacket, &received, sizeof(LecPacket));
    pendingMaintenancePacketAvailable = true;
    return;
  }

  memcpy(&incomingPacket, &received, sizeof(LecPacket));

  switch (incomingPacket.packetType) {
    case LEC_PKT_START_ROUND:
      if (incomingPacket.round == LEC_ROUND_1 ||
          incomingPacket.round == LEC_ROUND_2) {
        gameRunning = true;
        onFire = false;
        clearAllIngredientLeds();
        Serial.println("Ingredient station enabled for round.");
      }
      break;

    case LEC_PKT_END_ROUND:
    case LEC_PKT_END_GAME:
    case LEC_PKT_REINITIALIZE:
      gameRunning = false;
      onFire = false;
      requestSent = false;
      dataReceived = false;
      currentState = INGREDIENT_IDLE;
      clearAllIngredientLeds();
      Serial.println("Ingredient station stopped.");
      break;

    case LEC_PKT_ON_FIRE:
      onFire = true;
      Serial.println("Ingredient station is on FIRE.");
      break;

    case LEC_PKT_EXTINGUISH_FIRE:
      onFire = false;
      clearAllIngredientLeds();
      Serial.println("Ingredient station fire extinguished.");
      break;

    case LEC_PKT_PLATE_STATE:
      if (requestSent) {
        dataReceived = true;
      }
      break;

    default:
      break;
  }
}

/*************************************************************
  SETUP
*************************************************************/

void setup() {
  Serial.begin(115200);
  delay(200);

  Serial.println();
  Serial.println("Booting Let Em Cook Ingredient Station...");

  memcpy(serverAddress, LEC_DEFAULT_SERVER_MAC, 6);

  WiFi.mode(WIFI_STA);
  delay(100);

  Serial.print("Ingredient Station MAC: ");
  Serial.println(WiFi.macAddress());

  SPI.begin();
  rfid.PCD_Init();

  esp_wifi_set_channel(LEC_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

  Serial.print("ESP-NOW Channel: ");
  Serial.println(LEC_ESPNOW_CHANNEL);

  initializePins();
  initializeEspNow();
  initializeAudio();

  resetOutgoingPacket();
  memset(&incomingPacket, 0, sizeof(incomingPacket));
  memset(&pendingMaintenancePacket, 0, sizeof(pendingMaintenancePacket));

  sendHelloToServer();

  // Pantry stays ESP-NOW only during normal gameplay.
  // Wi-Fi is only used when an OTA/update packet is received.
  LecMaintenance::begin(
    LEC_DEVICE_INGREDIENT_STATION,
    handleMaintenanceStatus,
    handleMaintenanceProgress,
    sendMaintenanceResultToServer
  );

  Serial.print("Size of LecPacket: ");
  Serial.println(sizeof(LecPacket));

  Serial.println("Ingredient Station ready.");
}

/*************************************************************
  LOOP
*************************************************************/

void loop() {
  LecMaintenance::tick();

  processPendingMaintenancePacket();

  processHelloHeartbeat();

  // CHANGED: During maintenance/update, do not process pantry gameplay.
  if (LecMaintenance::isMaintenanceMode()) {
    delay(25);
    return;
  }

  if (!gameRunning) {
    delay(50);
    return;
  }

  if (onFire) {
    handleFireLedEffect();
    delay(10);
    return;
  }

  String ingredient = checkButtons();

  if (ingredient.length() > 0) {
    strncpy(lastIngredientPressed, ingredient.c_str(), INGREDIENT_LENGTH - 1);
    lastIngredientPressed[INGREDIENT_LENGTH - 1] = '\0';

    int ledPin = getLEDPin(lastIngredientPressed);

    if (ledPin != -1) {
      digitalWrite(ledPin, HIGH);
      activeLED = ledPin;
    }

    if (currentState == INGREDIENT_IDLE) {
      currentState = INGREDIENT_WAITING_FOR_RFID;
      rfidStartTime = millis();

      if (readCurrentRfid(pendingRfid)) {
        Serial.print("RFID detected: ");
        Serial.println(pendingRfid);

        lecInitPacket(
          outgoingPacket,
          LEC_PKT_PLATE_LOOKUP,
          LEC_DEVICE_INGREDIENT_STATION,
          LEC_ROLE_INGREDIENT_STATION
        );

        lecSetRfid(outgoingPacket, pendingRfid);

        memset(&incomingPacket, 0, sizeof(incomingPacket));

        sendDataToServer(&outgoingPacket);

        requestSent = true;
        dataReceived = false;

        currentState = INGREDIENT_PROCESSING_RFID;
        rfidStartTime = millis();
      } else {
        Serial.println("No RFID card detected.");

        currentState = INGREDIENT_IDLE;

        if (activeLED != -1) {
          digitalWrite(activeLED, LOW);
          activeLED = -1;
        }
      }
    }
  }

  if (currentState == INGREDIENT_PROCESSING_RFID) {
    if (dataReceived &&
        requestSent &&
        incomingPacket.packetType == LEC_PKT_PLATE_STATE) {
      handlePlateStateResponse();
    } else if (millis() - rfidStartTime > LEC_RFID_TIMEOUT_MS) {
      Serial.println("Timeout waiting for server plate response.");

      if (activeLED != -1) {
        digitalWrite(activeLED, LOW);
        activeLED = -1;
      }

      requestSent = false;
      dataReceived = false;
      currentState = INGREDIENT_IDLE;
    }
  }

  if (currentLEDToLightPin != -1) {
    digitalWrite(currentLEDToLightPin, HIGH);
    delay(500);
    digitalWrite(currentLEDToLightPin, LOW);
    currentLEDToLightPin = -1;
  }
}

/*************************************************************
  INITIALIZATION HELPERS
*************************************************************/

void initializePins() {
  pinMode(BUTTON_TOMATO, INPUT_PULLDOWN);

  // GPIO 34 and GPIO 35 are input-only pins and do not support internal pull-down.
  // These require external resistors if your button circuit needs a defined LOW state.
  pinMode(BUTTON_LETTUCE, INPUT);
  pinMode(BUTTON_DOUGH, INPUT);

  pinMode(BUTTON_CHEESE, INPUT_PULLDOWN);
  pinMode(BUTTON_MEAT, INPUT_PULLDOWN);
  pinMode(BUTTON_APPLE, INPUT_PULLDOWN);

  pinMode(LED_LETTUCE, OUTPUT);
  pinMode(LED_DOUGH, OUTPUT);
  pinMode(LED_MEAT, OUTPUT);
  pinMode(LED_CHEESE, OUTPUT);
  pinMode(LED_APPLE, OUTPUT);
  pinMode(LED_TOMATO, OUTPUT);

  clearAllIngredientLeds();
}

void initializeEspNow() {
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW.");
    return;
  }

  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataRecv);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, serverAddress, 6);
  peerInfo.channel = LEC_ESPNOW_CHANNEL;
  peerInfo.encrypt = false;

  if (esp_now_is_peer_exist(serverAddress)) {
    Serial.println("Server peer already exists.");
  } else if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to add server peer.");
    return;
  }

  Serial.println("ESP-NOW initialized.");
}

void initializeAudio() {
  audioSerial.begin(9600, SERIAL_8N1, AUDIO_RX, AUDIO_TX);
  audioModule.begin();
  audioModule.setCycleMode(DY::PlayMode::OneOff);
  audioModule.setVolume(24);
  audioModule.stop();

  Serial.println("Audio initialized.");
}

/*************************************************************
  MAINTENANCE / OTA HELPERS
*************************************************************/

void processPendingMaintenancePacket() {
  if (!pendingMaintenancePacketAvailable) {
    return;
  }

  LecPacket packetToHandle;
  memcpy(&packetToHandle, &pendingMaintenancePacket, sizeof(LecPacket));

  pendingMaintenancePacketAvailable = false;

  LecMaintenance::handlePacket(packetToHandle);
}

void sendMaintenanceResultToServer(
  LecPacketType packetType,
  LecPacketResult result,
  const char* status,
  const char* payload
) {
  LecPacket response;

  lecInitPacket(
    response,
    packetType,
    LEC_DEVICE_INGREDIENT_STATION,
    LEC_ROLE_INGREDIENT_STATION
  );

  response.result = result;
  response.runMode = LecMaintenance::getRunMode();

  if (status != nullptr) {
    lecSetStatus(response, status);
  }

  if (payload != nullptr) {
    lecSetPayload(response, payload);
  }

  sendDataToServer(&response);
}

void handleMaintenanceStatus(const char* status) {
  if (status == nullptr) return;

  Serial.print("Maintenance status: ");
  Serial.println(status);

  if (strcmp(status, "maintenance-requested") == 0 ||
      strcmp(status, "net-config-saved") == 0 ||
      strcmp(status, "wifi-connecting") == 0 ||
      strcmp(status, "ota-starting") == 0 ||
      strcmp(status, "ota-http-begin") == 0 ||
      strcmp(status, "ota-update-begin") == 0) {
    showMaintenanceActivity();
  }
  else if (strcmp(status, "ota-success") == 0) {
    showMaintenanceSuccess();
  }
  else if (strcmp(status, "ota-failed") == 0 ||
           strcmp(status, "wifi-failed") == 0 ||
           strcmp(status, "maintenance-idle-timeout") == 0) {
    showMaintenanceError();
  }
  else if (strcmp(status, "maintenance-exit") == 0) {
    clearAllIngredientLeds();
  }
}

void handleMaintenanceProgress(uint8_t percent) {
  showMaintenanceProgress(percent);
}

void showMaintenanceActivity() {
  for (int i = 0; i < 2; i++) {
    setAllIngredientLeds(true);
    delay(120);
    setAllIngredientLeds(false);
    delay(120);
  }
}

void showMaintenanceProgress(uint8_t percent) {
  if (percent > 100) {
    percent = 100;
  }

  int ledCount = map(percent, 0, 100, 0, 6);

  digitalWrite(LED_LETTUCE, ledCount >= 1 ? HIGH : LOW);
  digitalWrite(LED_TOMATO,  ledCount >= 2 ? HIGH : LOW);
  digitalWrite(LED_CHEESE,  ledCount >= 3 ? HIGH : LOW);
  digitalWrite(LED_MEAT,    ledCount >= 4 ? HIGH : LOW);
  digitalWrite(LED_APPLE,   ledCount >= 5 ? HIGH : LOW);
  digitalWrite(LED_DOUGH,   ledCount >= 6 ? HIGH : LOW);
}

void showMaintenanceSuccess() {
  for (int i = 0; i < 3; i++) {
    setAllIngredientLeds(true);
    delay(150);
    setAllIngredientLeds(false);
    delay(150);
  }
}

void showMaintenanceError() {
  for (int i = 0; i < 5; i++) {
    setAllIngredientLeds(true);
    delay(80);
    setAllIngredientLeds(false);
    delay(80);
  }
}

/*************************************************************
  PACKET HELPERS
*************************************************************/

void resetOutgoingPacket() {
  lecInitPacket(
    outgoingPacket,
    LEC_PKT_NONE,
    LEC_DEVICE_INGREDIENT_STATION,
    LEC_ROLE_INGREDIENT_STATION
  );

  lastIngredientPressed[0] = '\0';
}

void sendHelloToServer() {
  LecPacket hello;

  lecInitPacket(
    hello,
    LEC_PKT_HELLO,
    LEC_DEVICE_INGREDIENT_STATION,
    LEC_ROLE_INGREDIENT_STATION
  );

  lecSetStatus(hello, "ingredient-online");

  lastHelloSentAt = millis();

  sendDataToServer(&hello);
}

void processHelloHeartbeat() {
  if (LecMaintenance::isMaintenanceMode()) {
    return;
  }

  // Keep announcing while the pantry is not actively in a game.
  if (gameRunning) {
    return;
  }

  if (millis() - lastHelloSentAt < HELLO_RETRY_MS) {
    return;
  }

  lastHelloSentAt = millis();

  Serial.println("Sending ingredient HELLO heartbeat to server...");
  sendHelloToServer();
}

void sendDataToServer(LecPacket* packet) {
  if (packet == nullptr) return;

  packet->protocolVersion = LEC_PROTOCOL_VERSION;
  packet->packetSize = sizeof(LecPacket);
  packet->uptimeMs = millis();
  packet->deviceClass = LEC_DEVICE_INGREDIENT_STATION;
  packet->role = LEC_ROLE_INGREDIENT_STATION;

  // Report actual mode, GAME / MAINT / BULK_OTA / REBOOTING.
  packet->runMode = LecMaintenance::getRunMode();

  esp_err_t result = esp_now_send(
    serverAddress,
    reinterpret_cast<uint8_t*>(packet),
    sizeof(LecPacket)
  );

  if (result == ESP_OK) {
    Serial.println("Packet sent to server.");
  } else {
    Serial.print("Error sending packet: ");
    Serial.println(result);
  }
}

/*************************************************************
  GAME LOGIC
*************************************************************/

void handlePlateStateResponse() {
  dataReceived = false;
  requestSent = false;

  Serial.println("Handling plate state response.");

  bool plateHadIngredient =
    incomingPacket.ingredient[0] != '\0' &&
    strcmp(incomingPacket.ingredient, "none") != 0;

  lecInitPacket(
    outgoingPacket,
    LEC_PKT_PLATE_UPDATE,
    LEC_DEVICE_INGREDIENT_STATION,
    LEC_ROLE_INGREDIENT_STATION
  );

  lecSetRfid(outgoingPacket, pendingRfid);
  lecSetIngredient(outgoingPacket, lastIngredientPressed);

  outgoingPacket.chopCount = INVALID_COUNT;
  outgoingPacket.cookCount = INVALID_COUNT;
  outgoingPacket.bakeCount = INVALID_COUNT;
  outgoingPacket.playerScoreDelta = 0;

  // Ingredient station handles resetting/overwriting plate ingredient.
  outgoingPacket.resetPlate = plateHadIngredient;
  outgoingPacket.success = true;

  sendDataToServer(&outgoingPacket);

  currentLEDToLightPin = getLEDPin(lastIngredientPressed);

  if (plateHadIngredient) {
    Serial.print("Overwriting existing ingredient: ");
    Serial.println(incomingPacket.ingredient);
    playOverwriteFeedback();
  } else {
    Serial.println("Writing new ingredient to plate.");
    playSuccessFeedback();
  }

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  resetOutgoingPacket();
  memset(&incomingPacket, 0, sizeof(incomingPacket));
  pendingRfid[0] = '\0';

  if (activeLED != -1) {
    digitalWrite(activeLED, LOW);
    activeLED = -1;
  }

  currentState = INGREDIENT_IDLE;
}

/*************************************************************
  BUTTONS
*************************************************************/

String checkButtons() {
  String ingredient = "";
  unsigned long currentTime = millis();

  if (digitalRead(BUTTON_TOMATO) == HIGH && !previousButtonTomato) {
    if (currentTime - lastDebounceTomato > INGREDIENT_DEBOUNCE_MS) {
      lastDebounceTomato = currentTime;
      ingredient = "tomato";
      Serial.println("Button pressed: TOMATO");
    }
  }
  previousButtonTomato = digitalRead(BUTTON_TOMATO) == HIGH;

  if (digitalRead(BUTTON_LETTUCE) == HIGH && !previousButtonLettuce) {
    if (currentTime - lastDebounceLettuce > INGREDIENT_DEBOUNCE_MS) {
      lastDebounceLettuce = currentTime;
      ingredient = "lettuce";
      Serial.println("Button pressed: LETTUCE");
    }
  }
  previousButtonLettuce = digitalRead(BUTTON_LETTUCE) == HIGH;

  if (digitalRead(BUTTON_CHEESE) == HIGH && !previousButtonCheese) {
    if (currentTime - lastDebounceCheese > INGREDIENT_DEBOUNCE_MS) {
      lastDebounceCheese = currentTime;
      ingredient = "cheese";
      Serial.println("Button pressed: CHEESE");
    }
  }
  previousButtonCheese = digitalRead(BUTTON_CHEESE) == HIGH;

  if (digitalRead(BUTTON_MEAT) == HIGH && !previousButtonMeat) {
    if (currentTime - lastDebounceMeat > INGREDIENT_DEBOUNCE_MS) {
      lastDebounceMeat = currentTime;
      ingredient = "meat";
      Serial.println("Button pressed: MEAT");
    }
  }
  previousButtonMeat = digitalRead(BUTTON_MEAT) == HIGH;

  if (digitalRead(BUTTON_APPLE) == HIGH && !previousButtonApple) {
    if (currentTime - lastDebounceApple > INGREDIENT_DEBOUNCE_MS) {
      lastDebounceApple = currentTime;
      ingredient = "apple";
      Serial.println("Button pressed: APPLE");
    }
  }
  previousButtonApple = digitalRead(BUTTON_APPLE) == HIGH;

  if (digitalRead(BUTTON_DOUGH) == HIGH && !previousButtonDough) {
    if (currentTime - lastDebounceDough > INGREDIENT_DEBOUNCE_MS) {
      lastDebounceDough = currentTime;
      ingredient = "dough";
      Serial.println("Button pressed: DOUGH");
    }
  }
  previousButtonDough = digitalRead(BUTTON_DOUGH) == HIGH;

  return ingredient;
}

int getLEDPin(const char* ingredient) {
  if (strcmp(ingredient, "lettuce") == 0) return LED_LETTUCE;
  if (strcmp(ingredient, "tomato") == 0) return LED_TOMATO;
  if (strcmp(ingredient, "cheese") == 0) return LED_CHEESE;
  if (strcmp(ingredient, "meat") == 0) return LED_MEAT;
  if (strcmp(ingredient, "apple") == 0) return LED_APPLE;
  if (strcmp(ingredient, "dough") == 0) return LED_DOUGH;

  return -1;
}

/*************************************************************
  RFID
*************************************************************/

bool readCurrentRfid(char* outRfid) {
  if (outRfid == nullptr) return false;

  if (!PICC_IsAnyCardPresent()) {
    return false;
  }

  if (!rfid.PICC_ReadCardSerial()) {
    return false;
  }

  snprintf(
    outRfid,
    RFID_LENGTH,
    "%02X%02X%02X%02X",
    rfid.uid.uidByte[0],
    rfid.uid.uidByte[1],
    rfid.uid.uidByte[2],
    rfid.uid.uidByte[3]
  );

  outRfid[RFID_LENGTH - 1] = '\0';

  return true;
}

bool PICC_IsAnyCardPresent() {
  byte bufferATQA[2];
  byte bufferSize = sizeof(bufferATQA);

  rfid.PCD_WriteRegister(rfid.TxModeReg, 0x00);
  rfid.PCD_WriteRegister(rfid.RxModeReg, 0x00);
  rfid.PCD_WriteRegister(rfid.ModWidthReg, 0x26);

  MFRC522::StatusCode result = rfid.PICC_WakeupA(bufferATQA, &bufferSize);

  return result == MFRC522::STATUS_OK || result == MFRC522::STATUS_COLLISION;
}

/*************************************************************
  LED / FIRE / AUDIO FEEDBACK
*************************************************************/

void handleFireLedEffect() {
  if (millis() - lastFlashTime > 500) {
    lastFlashTime = millis();
    flashState = !flashState;

    setAllIngredientLeds(flashState);
  }
}

void setAllIngredientLeds(bool state) {
  digitalWrite(LED_LETTUCE, state ? HIGH : LOW);
  digitalWrite(LED_DOUGH, state ? HIGH : LOW);
  digitalWrite(LED_TOMATO, state ? HIGH : LOW);
  digitalWrite(LED_MEAT, state ? HIGH : LOW);
  digitalWrite(LED_CHEESE, state ? HIGH : LOW);
  digitalWrite(LED_APPLE, state ? HIGH : LOW);
}

void clearAllIngredientLeds() {
  setAllIngredientLeds(false);
}

void playSuccessFeedback() {
  audioModule.stop();
  audioModule.playSpecified(1);
}

void playOverwriteFeedback() {
  audioModule.stop();
  audioModule.playSpecified(1);
}

void playErrorFeedback(const char* currentIngredient) {
  audioModule.stop();
  audioModule.playSpecified(2);

  for (int blink = 0; blink < 3; blink++) {
    setAllIngredientLeds(true);
    delay(150);
    setAllIngredientLeds(false);
    delay(150);
  }

  int pin = getLEDPin(currentIngredient);

  if (pin != -1) {
    digitalWrite(pin, HIGH);
    delay(2000);
    digitalWrite(pin, LOW);
  }
}