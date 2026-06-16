/*************************************************************
    Let Em Cook - Generic Client Station

    Handles 4 reusable client roles:
    - Chop
    - Cook
    - Mix
    - Serve
*************************************************************/

#define LEC_DEBUG 1

#include <LetEmCook.h>

#include <esp_now.h>
#include <WiFi.h>
#include <MFRC522.h>
#include <SPI.h>
#include <Adafruit_NeoPixel.h>
#include <DYPlayerArduino.h>

extern "C" {
  #include "esp_wifi.h"
}

/*************************************************************
  PIN DEFINITIONS
*************************************************************/

#define SS_PIN 21
#define RST_PIN 22

#define BUTTON_PIN 13
#define BUTTON_LIGHT_PIN 27

#define NEOPIXEL_PIN 4
#define NEOPIXEL_MOSFET_PIN 26
#define NUM_PIXELS 20

#define AUDIO_TX 16
#define AUDIO_RX 17

#define SPRITE_TX 32
#define SPRITE_RX 33

/*************************************************************
  LOCAL CONSTANTS
*************************************************************/

#define CLIENT_BUTTON_DEBOUNCE_MS 0
#define CLIENT_RESPONSE_TIMEOUT_MS 5000
#define CLIENT_SHORT_ACK_TIMEOUT_MS 2000
#define CLIENT_RFID_TIMEOUT_MS 800
#define ROLE_DELAY_MS 4500
#define CHECK_INTERVAL_MS 500
#define HELLO_RETRY_MS 3000

#define EXTINGUISH_VIDEO_MS 2500UL


/*************************************************************
  HARDWARE
*************************************************************/

MFRC522 rfid(SS_PIN, RST_PIN);
Adafruit_NeoPixel strip(NUM_PIXELS, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

HardwareSerial audioSerial(2);
DY::Player audioModule(&audioSerial);

HardwareSerial spriteSerial(1);

/*************************************************************
  NETWORK / PACKETS
*************************************************************/

uint8_t serverAddress[6];

LecPacket outgoingPacket;
LecPacket serverData;
LecPacket currentRecipeData;

volatile bool dataReceived = false;

unsigned long lastHelloSentAt = 0;

/*************************************************************
  ROLE / GAME STATE
*************************************************************/

LecRole currentRole = LEC_ROLE_NONE;
LecRole previousRole = LEC_ROLE_NONE;

LecRound currentRound = LEC_ROUND_NONE;

bool onFire = false;

bool roleTransitionPending = false;
LecRole roleTransitionTarget = LEC_ROLE_NONE;
unsigned long roleTransitionStart = 0;

volatile uint8_t pendingSpriteCmd = 0;

/*************************************************************
  LOCAL RFID / PLATE STATE
*************************************************************/

char localRFID[RFID_LENGTH] = {0};
char localIngredient[INGREDIENT_LENGTH] = {0};

unsigned long lastCheckTime = 0;
unsigned long currentTime = 0;
unsigned long lastRFIDReadTime = 0;

/*************************************************************
  BUTTON STATE
*************************************************************/

struct ButtonState {
  unsigned long lastButtonPressTime;
  bool lastButtonState;
};

ButtonState stationButtonState = {0, HIGH};

/*************************************************************
  SYSTEM STATES
*************************************************************/

enum SystemState {
  CLIENT_IDLE,
  CLIENT_PROCESSING
};

SystemState chopState = CLIENT_IDLE;
SystemState cookState = CLIENT_IDLE;

enum MixState {
  MIX_IDLE,
  MIX_MIXING
};

MixState mixState = MIX_IDLE;

/*************************************************************
  CHOP STATE
*************************************************************/

int localChopCount = 0;

/*************************************************************
  COOK STATE
*************************************************************/

bool cooking = false;
bool burntPlate = false;

unsigned long lastCookTime = 0;
int localCookCount = 0;

bool isPlayingCookingAudio = false;
bool hasPlayedCookingCompleteAudio = false;

bool resumeScreenAfterExtinguish = false;
unsigned long resumeScreenAt = 0;

/*************************************************************
  MIX / SERVE STATE
*************************************************************/

LecRecipe currentRecipe;
bool currentRecipeValid = false;

char collectedIngredients[MAX_RECIPE_INGREDIENTS][INGREDIENT_LENGTH] = {0};
int collectedIngredientCount = 0;

/*************************************************************
  FUNCTION PROTOTYPES
*************************************************************/

void initializeEspNow();
void initializeHardware();

void sendHelloToServer();
void sendDataToServer(LecPacket* packet);
bool waitForServerResponse(unsigned long timeout);
void processHelloHeartbeat();
void sendDebugLogToServer(const char* status, const char* payload);

void resetPacket(LecPacket& packet, LecPacketType type);
void resetSharedData();
void resetAllRoleData();
void performReinitialization();

void handleRoleAssignment(LecRole newRole);
void applyPendingRoleTransition();

void executeChopStationClient();
void executeCookStationClient();
void executeMixStationClient();
void executeServeStationClient();
bool canCookIngredientForCurrentRecipe(const char* ingredient, const LecPacket& plateData);

bool requestPlateState(const char* rfidUID, unsigned long timeoutMs);

bool sendPlateUpdate(
  const char* rfidUID,
  const char* ingredient,
  int16_t chopCount,
  int16_t cookCount,
  int16_t bakeCount,
  bool resetPlate,
  int16_t scoreDelta,
  unsigned long ackTimeoutMs
);

bool sendServeAttempt(const char* rfidUID, const char* recipeName, unsigned long timeoutMs);
void requestAndUpdateCurrentRecipe();

bool isButtonPressed(ButtonState& buttonState);
bool readRFID(char* rfidUID);
bool PICC_IsAnyCardPresent();
void resetRFIDReader();

void updateChopCountDisplay(int chopCount);
void updateCookLedSection(int cookCount);
void updateMixStationLEDs();

void setStripColor(uint32_t color);
void clearStrip();
void blinkButtonLED();

void graduallyLightUpSection(int startLED, int endLED);
void runFireEffect();

uint8_t getVideoID(const char* recipeName);
void sendSprite(uint8_t cmd);

bool recipeRequirementMet(const LecIngredientRequirement& req, const LecPacket& plateData);

void processResumeScreenAfterExtinguish();
void playCurrentRoleScreen();

void sendMaintenanceResultToServer(
  LecPacketType packetType,
  LecPacketResult result,
  const char* status,
  const char* payload
);

void handleMaintenanceStatus(const char* status);
void handleMaintenanceProgress(uint8_t percent);
void showMaintenanceProgress(uint8_t percent);
void showMaintenanceError();
void showMaintenanceSuccess();

/*************************************************************
  ESP-NOW CALLBACKS
*************************************************************/

void onDataSent(const wifi_tx_info_t* tx_info, esp_now_send_status_t status) {
  Serial.print("Send status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAILED");
}

void onDataRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (len != sizeof(LecPacket)) {
    Serial.println("Received packet size mismatch.");
    return;
  }

  LecPacket incoming;
  memcpy(&incoming, data, sizeof(incoming));

  if (!lecIsValidPacket(incoming, len)) {
    Serial.println("Invalid Let Em Cook packet received.");
    return;
  }

  Serial.println("=== Packet Received From Server ===");
  Serial.print("Packet Type: ");
  Serial.println(static_cast<int>(incoming.packetType));
  Serial.print("Role: ");
  Serial.println(static_cast<int>(incoming.role));
  Serial.print("Round: ");
  Serial.println(static_cast<int>(incoming.round));
  Serial.print("Ingredient: ");
  Serial.println(incoming.ingredient);
  Serial.print("Recipe: ");
  Serial.println(incoming.recipeName);
  Serial.println("===================================");

  if (LecMaintenance::handlePacket(incoming)) {
    return;
  }

  if (roleTransitionPending) {
    if (incoming.packetType == LEC_PKT_REINITIALIZE ||
        incoming.packetType == LEC_PKT_END_GAME) {
      roleTransitionPending = false;
      roleTransitionTarget = LEC_ROLE_NONE; // CHANGED: fully clear pending role target.
    }
  }

  switch (incoming.packetType) {
    case LEC_PKT_ASSIGN_ROLE:
      handleRoleAssignment(incoming.role);
      break;

    case LEC_PKT_START_ROUND:
      // CHANGED: More defensive duplicate check.
      // If we are already in this round OR transitioning into this round, do not restart visuals.

      resumeScreenAfterExtinguish = false;
      if (currentRound == incoming.round &&
          (currentRole != LEC_ROLE_NONE || roleTransitionPending)) {
        Serial.println("Duplicate START_ROUND ignored.");
        break;
      }

      currentRound = incoming.round;

      // CHANGED: clear recipe for the new round so serve station does not use stale round recipe.
      currentRecipeValid = false;
      memset(&currentRecipe, 0, sizeof(currentRecipe));
      memset(&currentRecipeData, 0, sizeof(currentRecipeData));

      // CHANGED: clear old gameplay/LED/RFID state when a new round starts.
      resetAllRoleData();

      // CHANGED: cancel any old role transition from previous round.
      roleTransitionPending = false;
      roleTransitionTarget = LEC_ROLE_NONE;
      currentRole = LEC_ROLE_NONE;

      if (incoming.round == LEC_ROUND_1) {
        // Round 1 has the countdown video.
        pendingSpriteCmd = 0x13;
        Serial.println("Received Start Round 1. Playing countdown 0x13.");
      } 
      else if (incoming.round == LEC_ROUND_2) {
        // Do NOT play tornado here.
        // Tornado 0x16 should only happen from LEC_PKT_ASSIGN_ROLE.
        pendingSpriteCmd = 0x00;
        Serial.println("Received Start Round 2. Waiting for role assignment tornado.");
      }

      break;

    case LEC_PKT_END_ROUND:
      resetAllRoleData();
      currentRole = LEC_ROLE_NONE;         
      previousRole = LEC_ROLE_NONE;        
      roleTransitionPending = false;       
      roleTransitionTarget = LEC_ROLE_NONE;
      currentRound = LEC_ROUND_NONE;
      currentRecipeValid = false;           
      resumeScreenAfterExtinguish = false;
      break;

    case LEC_PKT_END_GAME:
      Serial.println("Received End Game.");
      audioModule.stop();
      clearStrip();
      onFire = false;
      currentRole = LEC_ROLE_NONE;
      previousRole = LEC_ROLE_NONE;
      currentRound = LEC_ROUND_NONE;
      roleTransitionPending = false;        
      roleTransitionTarget = LEC_ROLE_NONE; 
      currentRecipeValid = false;          
      pendingSpriteCmd = 0x0F;
      resumeScreenAfterExtinguish = false;
      resetAllRoleData();
      break;

    case LEC_PKT_REINITIALIZE:
      performReinitialization();
      break;

    case LEC_PKT_PLATE_STATE:
    case LEC_PKT_SERVE_RESULT:
    case LEC_PKT_RECIPE_STATE:
      memcpy(&serverData, &incoming, sizeof(LecPacket));
      dataReceived = true;
      break;

    case LEC_PKT_NEW_RECIPE:
      Serial.println("Received New Recipe.");

      // Ignore recipe packets from another round.
      if (incoming.round != currentRound) {
        Serial.println("Ignoring recipe from different round.");
        break;
      }

      if (currentRecipeValid && strcmp(currentRecipe.name, incoming.recipeName) == 0) {
        // Same recipe can still be a new iteration and should refresh the screen.
        Serial.println("Same recipe received again; refreshing screen/state.");
      }

      if (lecFindRecipeByName(incoming.recipeName, currentRecipe)) {
        currentRecipeValid = true;
        memcpy(&currentRecipeData, &incoming, sizeof(LecPacket));

        Serial.print("Current recipe set to: ");
        Serial.println(currentRecipe.name);

        if (currentRole == LEC_ROLE_MIX_STATION) {
          mixState = MIX_IDLE;
          collectedIngredientCount = 0;
          memset(collectedIngredients, 0, sizeof(collectedIngredients));
          clearStrip();
        }

        if (currentRole == LEC_ROLE_SERVE_STATION) {
          uint8_t videoID = getVideoID(currentRecipe.name);
          if (videoID != 0x00) {
            pendingSpriteCmd = videoID;
          }
        } else {
          // This is expected during tornado transition.
          // The recipe is stored and applyPendingRoleTransition() will use it if this client becomes serve.
          Serial.println("Recipe stored; client is not currently serve station.");
        }
      } else {
        Serial.println("Unknown recipe received.");
      }
      break;

    case LEC_PKT_ON_FIRE:
      Serial.println("Station is now on fire.");
      pendingSpriteCmd = 0x0A;
      audioModule.stop();
      audioModule.playSpecified(4);
      onFire = true;
      break;

    case LEC_PKT_EXTINGUISH_FIRE:
      Serial.println("Station fire extinguished.");

      pendingSpriteCmd = 0x0B;

      audioModule.stop();
      audioModule.playSpecified(9);

      resetAllRoleData();

      onFire = false;

      // After extinguish video, resume current station/recipe screen.
      resumeScreenAfterExtinguish = true;
      resumeScreenAt = millis() + EXTINGUISH_VIDEO_MS;

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
  Serial.println("Booting Let Em Cook Generic Client...");

  memcpy(serverAddress, LEC_DEFAULT_SERVER_MAC, 6);

  WiFi.mode(WIFI_STA);
  delay(100);

  Serial.print("Client MAC Address: ");
  Serial.println(WiFi.macAddress());

  SPI.begin();
  esp_wifi_set_channel(LEC_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

  Serial.print("ESP-NOW Channel: ");
  Serial.println(LEC_ESPNOW_CHANNEL);

  initializeHardware();
  initializeEspNow();

  resetAllRoleData();
  sendHelloToServer();

  LecMaintenance::begin(
    LEC_DEVICE_GENERIC_CLIENT,
    handleMaintenanceStatus,
    handleMaintenanceProgress,
    sendMaintenanceResultToServer
  );

  Serial.print("Size of LecPacket: ");
  Serial.println(sizeof(LecPacket));

  Serial.println("Generic Client ready.");
}

/*************************************************************
  LOOP
*************************************************************/

void loop() {
  LecMaintenance::tick();

  if (LecMaintenance::isMaintenanceMode()) {
    delay(25);
    return;
  }

  processHelloHeartbeat();
  
  if (pendingSpriteCmd) {
    sendSprite(pendingSpriteCmd);
    pendingSpriteCmd = 0;
  }

  processResumeScreenAfterExtinguish();

  if (roleTransitionPending) {
    applyPendingRoleTransition();
    return;
  }

  if (onFire) {
    runFireEffect();
    delay(50);
    return;
  }

  switch (currentRole) {
    case LEC_ROLE_CHOP_STATION:
      executeChopStationClient();
      break;

    case LEC_ROLE_COOK_STATION:
      executeCookStationClient();
      break;

    case LEC_ROLE_MIX_STATION:
      executeMixStationClient();
      break;

    case LEC_ROLE_SERVE_STATION:
      executeServeStationClient();
      break;

    default:
      delay(100);
      break;
  }

  delay(10);
}

/*************************************************************
  INITIALIZATION
*************************************************************/

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

void initializeHardware() {
  rfid.PCD_Init();
  delay(50);
  Serial.println("RFID reader initialized.");

  pinMode(NEOPIXEL_MOSFET_PIN, OUTPUT);
  digitalWrite(NEOPIXEL_MOSFET_PIN, LOW);

  strip.begin();
  strip.show();
  delay(50);
  Serial.println("Neopixel strip initialized.");

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  pinMode(BUTTON_LIGHT_PIN, OUTPUT);
  digitalWrite(BUTTON_LIGHT_PIN, HIGH);

  audioSerial.begin(9600, SERIAL_8N1, AUDIO_RX, AUDIO_TX);
  audioModule.begin();
  audioModule.setCycleMode(DY::PlayMode::OneOff);
  audioModule.setVolume(24);
  audioModule.stop();

  spriteSerial.begin(9600, SERIAL_8N1, SPRITE_RX, SPRITE_TX);
  sendSprite(0x00);

  Serial.println("Hardware initialization complete.");
}

/*************************************************************
  PACKET HELPERS
*************************************************************/

void resetPacket(LecPacket& packet, LecPacketType type) {
  lecInitPacket(
    packet,
    type,
    LEC_DEVICE_GENERIC_CLIENT,
    currentRole
  );

  packet.round = currentRound;
}

void sendDebugLogToServer(const char* status, const char* payload) {
  LecPacket debugPacket;

  lecInitPacket(
    debugPacket,
    LEC_PKT_DEBUG_LOG,
    LEC_DEVICE_GENERIC_CLIENT,
    currentRole
  );

  debugPacket.round = currentRound;

  if (status != nullptr) {
    lecSetStatus(debugPacket, status);
  }

  if (payload != nullptr) {
    lecSetPayload(debugPacket, payload);
  }

  sendDataToServer(&debugPacket);
}

void processHelloHeartbeat() {
  if (roleTransitionPending) {
    return;
  }

  if (currentRound != LEC_ROUND_NONE || currentRole != LEC_ROLE_NONE) {
    return;
  }

  if (millis() - lastHelloSentAt < HELLO_RETRY_MS) {
    return;
  }

  lastHelloSentAt = millis();

  Serial.println("Sending HELLO heartbeat to server...");
  sendHelloToServer();
}

void sendHelloToServer() {
  LecPacket hello;

  lecInitPacket(
    hello,
    LEC_PKT_HELLO,
    LEC_DEVICE_GENERIC_CLIENT,
    currentRole
  );

  hello.round = currentRound;
  lecSetStatus(hello, "generic-client-online");

  lastHelloSentAt = millis();

  sendDataToServer(&hello);
}

void sendDataToServer(LecPacket* packet) {
  if (packet == nullptr) return;

  packet->protocolVersion = LEC_PROTOCOL_VERSION;
  packet->packetSize = sizeof(LecPacket);
  packet->uptimeMs = millis();
  packet->deviceClass = LEC_DEVICE_GENERIC_CLIENT;
  packet->role = currentRole;
  packet->round = currentRound;
  packet->runMode = LecMaintenance::getRunMode();

  Serial.println("=== Sending Packet To Server ===");
  Serial.print("Packet Type: ");
  Serial.println(static_cast<int>(packet->packetType));
  Serial.print("Role: ");
  Serial.println(static_cast<int>(packet->role));
  Serial.print("RFID: ");
  Serial.println(packet->rfid);
  Serial.print("Ingredient: ");
  Serial.println(packet->ingredient);
  Serial.println("================================");

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
    LEC_DEVICE_GENERIC_CLIENT,
    currentRole
  );

  response.result = result;
  response.round = currentRound;
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

  if (strcmp(status, "ota-starting") == 0 ||
      strcmp(status, "wifi-connecting") == 0 ||
      strcmp(status, "ota-http-begin") == 0 ||
      strcmp(status, "ota-update-begin") == 0) {
    // Indicate update/maintenance activity.
    digitalWrite(BUTTON_LIGHT_PIN, LOW);
    setStripColor(strip.Color(0, 0, 255));
  }
  else if (strcmp(status, "ota-success") == 0) {
    showMaintenanceSuccess();
  }
  else if (strcmp(status, "ota-failed") == 0 ||
           strcmp(status, "wifi-failed") == 0) {
    showMaintenanceError();
  }
  else if (strcmp(status, "maintenance-exit") == 0) {
    clearStrip();
    digitalWrite(BUTTON_LIGHT_PIN, HIGH);
  }
}

void handleMaintenanceProgress(uint8_t percent) {
  showMaintenanceProgress(percent);
}

void showMaintenanceProgress(uint8_t percent) {
  if (percent > 100) {
    percent = 100;
  }

  digitalWrite(NEOPIXEL_MOSFET_PIN, HIGH);
  strip.clear();

  int litPixels = (NUM_PIXELS * percent) / 100;

  for (int i = 0; i < litPixels; i++) {
    strip.setPixelColor(i, strip.Color(0, 0, 255));
  }

  strip.show();

  // CHANGED: Blink button light during update.
  digitalWrite(BUTTON_LIGHT_PIN, (millis() / 250) % 2 == 0 ? HIGH : LOW);
}

void showMaintenanceError() {
  digitalWrite(NEOPIXEL_MOSFET_PIN, HIGH);

  for (int flash = 0; flash < 3; flash++) {
    setStripColor(strip.Color(255, 0, 0));
    digitalWrite(BUTTON_LIGHT_PIN, LOW);
    delay(150);

    clearStrip();
    digitalWrite(BUTTON_LIGHT_PIN, HIGH);
    delay(150);
  }
}

void showMaintenanceSuccess() {
  digitalWrite(NEOPIXEL_MOSFET_PIN, HIGH);

  for (int flash = 0; flash < 3; flash++) {
    setStripColor(strip.Color(0, 255, 0));
    digitalWrite(BUTTON_LIGHT_PIN, HIGH);
    delay(150);

    clearStrip();
    digitalWrite(BUTTON_LIGHT_PIN, LOW);
    delay(150);
  }
}

bool waitForServerResponse(unsigned long timeout) {
  unsigned long startTime = millis();
  dataReceived = false;

  while (!dataReceived && (millis() - startTime) < timeout) {
    delay(10);
  }

  return dataReceived;
}

bool requestPlateState(const char* rfidUID, unsigned long timeoutMs) {
  if (!rfidUID) return false;

  resetPacket(outgoingPacket, LEC_PKT_PLATE_LOOKUP);
  lecSetRfid(outgoingPacket, rfidUID);

  memset(&serverData, 0, sizeof(serverData));
  dataReceived = false;

  sendDataToServer(&outgoingPacket);

  if (!waitForServerResponse(timeoutMs)) {
    Serial.println("Timeout waiting for plate state.");
    return false;
  }

  if (serverData.packetType != LEC_PKT_PLATE_STATE) {
    Serial.print("Unexpected packet type: ");
    Serial.println(static_cast<int>(serverData.packetType));
    return false;
  }

  return true;
}

bool sendPlateUpdate(
  const char* rfidUID,
  const char* ingredient,
  int16_t chopCount,
  int16_t cookCount,
  int16_t bakeCount,
  bool resetPlate,
  int16_t scoreDelta,
  unsigned long ackTimeoutMs
) {
  resetPacket(outgoingPacket, LEC_PKT_PLATE_UPDATE);

  lecSetRfid(outgoingPacket, rfidUID);
  lecSetIngredient(outgoingPacket, ingredient);

  outgoingPacket.chopCount = chopCount;
  outgoingPacket.cookCount = cookCount;
  outgoingPacket.bakeCount = bakeCount;
  outgoingPacket.resetPlate = resetPlate;
  outgoingPacket.playerScoreDelta = scoreDelta;

  dataReceived = false;
  sendDataToServer(&outgoingPacket);

  if (ackTimeoutMs == 0) {
    return true;
  }

  if (!waitForServerResponse(ackTimeoutMs)) {
    Serial.println("No ACK from server on plate update.");
    return false;
  }

  return true;
}

bool sendServeAttempt(const char* rfidUID, const char* recipeName, unsigned long timeoutMs) {
  resetPacket(outgoingPacket, LEC_PKT_SERVE_ATTEMPT);

  lecSetRfid(outgoingPacket, rfidUID);
  lecSetRecipeName(outgoingPacket, recipeName);

  memset(&serverData, 0, sizeof(serverData));
  dataReceived = false;

  sendDataToServer(&outgoingPacket);

  if (!waitForServerResponse(timeoutMs)) {
    Serial.println("Timeout waiting for serve result.");
    return false;
  }

  if (serverData.packetType != LEC_PKT_SERVE_RESULT) {
    Serial.print("Unexpected serve result packet type: ");
    Serial.println(static_cast<int>(serverData.packetType));
    return false;
  }

  return true;
}

/*************************************************************
  RESET / ROLE HANDLING
*************************************************************/

void resetSharedData() {
  memset(&outgoingPacket, 0, sizeof(outgoingPacket));
  memset(&serverData, 0, sizeof(serverData));
  dataReceived = false;
}

void resetAllRoleData() {
  resetSharedData();

  chopState = CLIENT_IDLE;
  cookState = CLIENT_IDLE;
  mixState = MIX_IDLE;

  memset(localRFID, 0, sizeof(localRFID));
  memset(localIngredient, 0, sizeof(localIngredient));

  localChopCount = 0;
  localCookCount = 0;

  cooking = false;
  burntPlate = false;
  isPlayingCookingAudio = false;
  hasPlayedCookingCompleteAudio = false;

  collectedIngredientCount = 0;
  memset(collectedIngredients, 0, sizeof(collectedIngredients));

  clearStrip();

  Serial.println("All role-specific data reset.");
}

void performReinitialization() {
  Serial.println("Performing re-initialization...");

  audioModule.stop();

  resetAllRoleData();

  onFire = false;
  currentRole = LEC_ROLE_NONE;
  previousRole = LEC_ROLE_NONE;
  currentRound = LEC_ROUND_NONE;
  roleTransitionPending = false;
  resumeScreenAfterExtinguish = false;

  pendingSpriteCmd = 0x00;

  Serial.println("Re-initialization complete.");
}

void playCurrentRoleScreen() {
  uint8_t playCommand = 0x00;

  switch (currentRole) {
    case LEC_ROLE_CHOP_STATION:
      playCommand = 0x02;
      break;

    case LEC_ROLE_COOK_STATION:
      playCommand = 0x03;
      break;

    case LEC_ROLE_MIX_STATION:
      playCommand = 0x04;
      break;

    case LEC_ROLE_SERVE_STATION:
      if (!currentRecipeValid) {
        Serial.println("Serve resume requested but recipe missing. Requesting recipe.");
        requestAndUpdateCurrentRecipe();
      }

      if (currentRecipeValid) {
        playCommand = getVideoID(currentRecipe.name);
      }
      break;

    default:
      playCommand = 0x00;
      break;
  }

  if (playCommand != 0x00) {
    sendSprite(playCommand);
  }
}

void processResumeScreenAfterExtinguish() {
  if (!resumeScreenAfterExtinguish) return;

  if (millis() >= resumeScreenAt) {
    resumeScreenAfterExtinguish = false;

    Serial.println("Resuming role screen after extinguish video.");

    // Return each client to its current station/recipe screen.
    playCurrentRoleScreen();
  }
}

void handleRoleAssignment(LecRole newRole) {
  Serial.print("Received role assignment: ");
  Serial.println(static_cast<int>(newRole));

  resumeScreenAfterExtinguish = false;

  if (newRole != LEC_ROLE_CHOP_STATION &&
      newRole != LEC_ROLE_COOK_STATION &&
      newRole != LEC_ROLE_MIX_STATION &&
      newRole != LEC_ROLE_SERVE_STATION) {
    Serial.println("WARNING: Invalid gameplay role received by generic client.");
    return;
  }

  // Ignore duplicate role packet if already fully on this role.
  if (!roleTransitionPending && currentRole == newRole) {
    Serial.println("Duplicate role assignment ignored.");
    return;
  }

  // Ignore duplicate role packet if already transitioning to this role.
  if (roleTransitionPending && roleTransitionTarget == newRole) {
    Serial.println("Duplicate pending role assignment ignored.");
    return;
  }

  audioModule.stop();
  audioModule.playSpecified(11);

  // Save old role, but mark client as neutral during transition.
  // This prevents recipe packets or button logic from acting like the old station.
  previousRole = currentRole;
  currentRole = LEC_ROLE_NONE;

  // Reset station state before tornado.
  resetAllRoleData();

  // Tornado should only be triggered here, not by StartRound2.
  pendingSpriteCmd = 0x16;

  roleTransitionPending = true;
  roleTransitionTarget = newRole;
  roleTransitionStart = millis();

  Serial.println("Role transition started. Playing tornado 0x16.");
}

void applyPendingRoleTransition() {
  if (millis() - roleTransitionStart < ROLE_DELAY_MS) {
    return;
  }

  roleTransitionPending = false;
  currentRole = roleTransitionTarget;
  previousRole = roleTransitionTarget;
  roleTransitionTarget = LEC_ROLE_NONE; // clear target after applying.

  uint8_t playCommand = 0x00;

  switch (currentRole) {
    case LEC_ROLE_CHOP_STATION:
      playCommand = 0x02;
      break;

    case LEC_ROLE_COOK_STATION:
      playCommand = 0x03;
      break;

    case LEC_ROLE_MIX_STATION:
      playCommand = 0x04;
      break;

    case LEC_ROLE_SERVE_STATION:
      // If recipe arrived during tornado, this will already be true.
      // If it did not arrive yet, ask server once so station video is less likely to be missed.
      if (!currentRecipeValid) {
        Serial.println("Serve role applied but recipe missing. Requesting recipe.");
        requestAndUpdateCurrentRecipe();
      }

      if (currentRecipeValid) {
        playCommand = getVideoID(currentRecipe.name);
      } else {
        playCommand = 0x00;
        Serial.println("Serve station has no recipe video yet.");
      }
      break;

    default:
      playCommand = 0x00;
      break;
  }

  if (playCommand != 0x00) {
    sendSprite(playCommand);
  }

  Serial.print("Role transition applied. Current role: ");
  Serial.println(static_cast<int>(currentRole));
}

/*************************************************************
  RFID HELPERS
*************************************************************/

bool PICC_IsAnyCardPresent() {
  byte bufferATQA[2];
  byte bufferSize = sizeof(bufferATQA);

  rfid.PCD_WriteRegister(rfid.TxModeReg, 0x00);
  rfid.PCD_WriteRegister(rfid.RxModeReg, 0x00);
  rfid.PCD_WriteRegister(rfid.ModWidthReg, 0x26);

  MFRC522::StatusCode result = rfid.PICC_WakeupA(bufferATQA, &bufferSize);

  return result == MFRC522::STATUS_OK || result == MFRC522::STATUS_COLLISION;
}

bool readRFID(char* rfidUID) {
  if (!rfidUID) return false;

  if (PICC_IsAnyCardPresent() && rfid.PICC_ReadCardSerial()) {
    snprintf(
      rfidUID,
      RFID_LENGTH,
      "%02X%02X%02X%02X",
      rfid.uid.uidByte[0],
      rfid.uid.uidByte[1],
      rfid.uid.uidByte[2],
      rfid.uid.uidByte[3]
    );

    rfidUID[RFID_LENGTH - 1] = '\0';

    resetRFIDReader();
    return true;
  }

  return false;
}

void resetRFIDReader() {
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}

/*************************************************************
  BUTTON / LED HELPERS
*************************************************************/

bool isButtonPressed(ButtonState& buttonState) {
  bool currentButtonState = digitalRead(BUTTON_PIN);
  unsigned long now = millis();

  if (buttonState.lastButtonState == HIGH && currentButtonState == LOW) {
    if (now - buttonState.lastButtonPressTime > CLIENT_BUTTON_DEBOUNCE_MS) {
      buttonState.lastButtonPressTime = now;
      buttonState.lastButtonState = currentButtonState;
      return true;
    }
  }

  buttonState.lastButtonState = currentButtonState;
  return false;
}

void blinkButtonLED() {
  digitalWrite(BUTTON_LIGHT_PIN, LOW);
  delay(100);
  digitalWrite(BUTTON_LIGHT_PIN, HIGH);
}

void forceStripOffOnBoot() {
  for (int i = 0; i < 3; i++) {
    strip.clear();
    strip.show();
    delay(20);
  }

  digitalWrite(NEOPIXEL_MOSFET_PIN, LOW);
}

void setStripColor(uint32_t color) {
  digitalWrite(NEOPIXEL_MOSFET_PIN, HIGH);
  strip.clear();

  for (int i = 0; i < NUM_PIXELS; i++) {
    strip.setPixelColor(i, color);
  }

  strip.show();
}

void clearStrip() {
  strip.clear();
  strip.show();
  digitalWrite(NEOPIXEL_MOSFET_PIN, LOW);
}

void updateChopCountDisplay(int chopCount) {
  if (chopCount < 0) chopCount = 0;
  if (chopCount > MAX_CHOP_COUNT) chopCount = MAX_CHOP_COUNT;

  if (chopCount == MAX_CHOP_COUNT) {
    setStripColor(strip.Color(0, 255, 0));
    return;
  }

  int ledsPerChop = NUM_PIXELS / MAX_CHOP_COUNT;
  int ledsToLight = chopCount * ledsPerChop;

  if (chopCount > 0) {
    digitalWrite(NEOPIXEL_MOSFET_PIN, HIGH);
  } else {
    digitalWrite(NEOPIXEL_MOSFET_PIN, LOW);
  }

  strip.clear();

  for (int i = 0; i < ledsToLight; i++) {
    strip.setPixelColor(i, strip.Color(255, 255, 0));
  }

  strip.show();
}

void updateCookLedSection(int cookCount) {
  int ledsPerSection = NUM_PIXELS / MAX_COOK_COUNT;
  int totalLedsToLight = cookCount * ledsPerSection;

  if (totalLedsToLight > NUM_PIXELS) {
    totalLedsToLight = NUM_PIXELS;
  }

  uint32_t color;

  if (cookCount >= 1 && cookCount <= 6) {
    color = strip.Color(255, 255, 0);
  } else if (cookCount >= 7 && cookCount <= 9) {
    color = strip.Color(0, 255, 0);
  } else if (cookCount >= MAX_COOK_COUNT) {
    color = strip.Color(255, 0, 0);
  } else {
    color = strip.Color(0, 0, 0);
  }

  digitalWrite(NEOPIXEL_MOSFET_PIN, HIGH);

  for (int i = 0; i < totalLedsToLight; i++) {
    strip.setPixelColor(i, color);
  }

  for (int i = totalLedsToLight; i < NUM_PIXELS; i++) {
    strip.setPixelColor(i, strip.Color(0, 0, 0));
  }

  strip.show();
}

void graduallyLightUpSection(int startLED, int endLED) {
  digitalWrite(NEOPIXEL_MOSFET_PIN, HIGH);

  for (int i = startLED; i <= endLED; i++) {
    strip.setPixelColor(i, strip.Color(255, 255, 0));
    strip.show();
    delay(100);
  }
}

void updateMixStationLEDs() {
  if (!currentRecipeValid || currentRecipe.numIngredients <= 0) {
    clearStrip();
    return;
  }

  strip.clear();

  int ledsPerIngredient = NUM_PIXELS / currentRecipe.numIngredients;

  for (int i = 0; i < collectedIngredientCount; i++) {
    int startLED = ledsPerIngredient * i;
    int endLED = startLED + ledsPerIngredient - 1;

    if (endLED >= NUM_PIXELS) {
      endLED = NUM_PIXELS - 1;
    }

    for (int j = startLED; j <= endLED; j++) {
      strip.setPixelColor(j, strip.Color(255, 255, 0));
    }
  }

  strip.show();
}

void runFireEffect() {
  digitalWrite(NEOPIXEL_MOSFET_PIN, HIGH);

  for (int i = 0; i < NUM_PIXELS; i++) {
    int flickerR = random(220, 256);
    int flickerG = random(0, 100);
    int flickerB = 0;

    strip.setPixelColor(i, strip.Color(flickerR, flickerG, flickerB));
  }

  strip.show();
}

/*************************************************************
  RECIPE HELPERS
*************************************************************/

void requestAndUpdateCurrentRecipe() {
  resetPacket(outgoingPacket, LEC_PKT_CURRENT_RECIPE_REQUEST);

  memset(&currentRecipeData, 0, sizeof(currentRecipeData));
  dataReceived = false;

  sendDataToServer(&outgoingPacket);

  if (!waitForServerResponse(CLIENT_RESPONSE_TIMEOUT_MS)) {
    Serial.println("Timeout waiting for recipe state.");
    return;
  }

  if (serverData.packetType != LEC_PKT_RECIPE_STATE) {
    Serial.print("Unexpected recipe response packet type: ");
    Serial.println(static_cast<int>(serverData.packetType));
    return;
  }

  memcpy(&currentRecipeData, &serverData, sizeof(LecPacket));

  if (lecFindRecipeByName(currentRecipeData.recipeName, currentRecipe)) {
    currentRecipeValid = true;
    Serial.print("Current recipe updated to: ");
    Serial.println(currentRecipe.name);
  } else {
    currentRecipeValid = false;
    Serial.println("Unknown recipe received.");
  }
}

bool recipeRequirementMet(const LecIngredientRequirement& req, const LecPacket& plateData) {
  if (strcmp(plateData.ingredient, req.name) != 0) {
    return false;
  }

  if (req.requiresChop && plateData.chopCount < req.requiredChopCount) {
    return false;
  }

  if (req.requiresCook) {
    if (plateData.cookCount < req.requiredCookCountMin ||
        plateData.cookCount > req.requiredCookCountMax) {
      return false;
    }
  }

  return true;
}

/*************************************************************
  CHOP ROLE
*************************************************************/

void executeChopStationClient() {
  currentTime = millis();

  if (chopState == CLIENT_IDLE) {
    if (isButtonPressed(stationButtonState)) {
      Serial.println("Chop Station: button pressed.");
      blinkButtonLED();

      if (readRFID(localRFID)) {
        Serial.print("RFID detected: ");
        Serial.println(localRFID);

        if (!requestPlateState(localRFID, CLIENT_RESPONSE_TIMEOUT_MS)) {
          return;
        }

        strncpy(localIngredient, serverData.ingredient, INGREDIENT_LENGTH - 1);
        localIngredient[INGREDIENT_LENGTH - 1] = '\0';

        localChopCount = serverData.chopCount;

        Serial.print("Ingredient: ");
        Serial.println(localIngredient);
        Serial.print("Chop count: ");
        Serial.println(localChopCount);

        if (strcmp(localIngredient, "none") == 0 || localIngredient[0] == '\0') {
          Serial.println("No ingredient on plate.");
          setStripColor(strip.Color(255, 0, 0));
          delay(1000);
          clearStrip();
          return;
        }

        if (localChopCount >= MAX_CHOP_COUNT) {
          Serial.println("Plate already max chopped.");
          setStripColor(strip.Color(0, 255, 0));
          delay(1000);
          clearStrip();
          return;
        }
        chopState = CLIENT_PROCESSING;
        localChopCount++;
        updateChopCountDisplay(localChopCount);
        //audioModule.stop();
        audioModule.playSpecified(2);
      } else {
        Serial.println("No RFID card detected.");
      }
    }
  } else if (chopState == CLIENT_PROCESSING) {
    if (isButtonPressed(stationButtonState)) {
      Serial.println("Chop Station: increment chop.");
      blinkButtonLED();

      if (localChopCount < MAX_CHOP_COUNT) {
        localChopCount++;
        updateChopCountDisplay(localChopCount);
        //audioModule.stop();
        audioModule.playSpecified(2);
      } else {
        setStripColor(strip.Color(0, 255, 0));
      }
    }

    if (currentTime - lastCheckTime >= CHECK_INTERVAL_MS) {
      lastCheckTime = currentTime;

      char currentRfid[RFID_LENGTH] = {0};

      if (readRFID(currentRfid)) {
        if (strncmp(currentRfid, localRFID, RFID_LENGTH) == 0) {
          return;
        }
      }

      Serial.println("Plate removed. Sending final chop count.");

      sendPlateUpdate(
        localRFID,
        localIngredient,
        localChopCount,
        INVALID_COUNT,
        INVALID_COUNT,
        false,
        0,
        CLIENT_SHORT_ACK_TIMEOUT_MS
      );

      clearStrip();

      localChopCount = 0;
      memset(localRFID, 0, sizeof(localRFID));
      memset(localIngredient, 0, sizeof(localIngredient));

      chopState = CLIENT_IDLE;
    }
  }
}

/*************************************************************
  COOK ROLE
*************************************************************/

bool canCookIngredientForCurrentRecipe(const char* ingredient, const LecPacket& plateData) {
  if (!ingredient || !currentRecipeValid) {
    return false;
  }

  for (int i = 0; i < currentRecipe.numIngredients; i++) {
    const LecIngredientRequirement& req = currentRecipe.ingredients[i];

    if (strcmp(req.name, ingredient) != 0) {
      continue;
    }

    // Ingredient is part of this recipe, but this recipe does not require cooking it.
    if (!req.requiresCook) {
      return false;
    }

    // If this recipe requires chopping before cooking, enforce the chop requirement.
    if (req.requiresChop && plateData.chopCount < req.requiredChopCount) {
      return false;
    }

    return true;
  }

  return false;
}

void executeCookStationClient() {
  currentTime = millis();

  if (!currentRecipeValid) {
    requestAndUpdateCurrentRecipe();

    if (!currentRecipeValid) {
      delay(100);
      return;
    }
  }

  if (cookState == CLIENT_IDLE) {
    if (isButtonPressed(stationButtonState)) {
      Serial.println("Cook Station: button pressed.");
      blinkButtonLED();

      if (readRFID(localRFID)) {
        Serial.print("RFID detected: ");
        Serial.println(localRFID);

        if (!requestPlateState(localRFID, CLIENT_RESPONSE_TIMEOUT_MS)) {
          return;
        }

        strncpy(localIngredient, serverData.ingredient, INGREDIENT_LENGTH - 1);
        localIngredient[INGREDIENT_LENGTH - 1] = '\0';

        localCookCount = serverData.cookCount;

        Serial.print("Ingredient: ");
        Serial.println(localIngredient);
        Serial.print("Chop count: ");
        Serial.println(serverData.chopCount);
        Serial.print("Cook count: ");
        Serial.println(localCookCount);
        Serial.print("Current recipe: ");
        Serial.println(currentRecipe.name);

        if (localCookCount >= MAX_COOK_COUNT) {
          Serial.println("Plate already burnt.");
          burntPlate = true;
          cooking = false;
          cookState = CLIENT_PROCESSING;

          setStripColor(strip.Color(255, 0, 0));
          audioModule.stop();
          audioModule.playSpecified(4);
          lastRFIDReadTime = millis();
          return;
        }

        bool canCook = canCookIngredientForCurrentRecipe(localIngredient, serverData);

        if (canCook) {
          Serial.println("Cooking started.");

          char debugMsg[LEC_PAYLOAD_LENGTH];
          snprintf(
            debugMsg,
            sizeof(debugMsg),
            "start ingredient=%s chop=%d cook=%d recipe=%s",
            localIngredient,
            serverData.chopCount,
            serverData.cookCount,
            currentRecipe.name
          );
          sendDebugLogToServer("cook-start", debugMsg);

          cooking = true;
          burntPlate = false;
          isPlayingCookingAudio = false;
          hasPlayedCookingCompleteAudio = false;

          cookState = CLIENT_PROCESSING;

          updateCookLedSection(localCookCount);

          audioModule.stop();
          audioModule.playSpecified(3);
          isPlayingCookingAudio = true;

          digitalWrite(NEOPIXEL_MOSFET_PIN, HIGH);
          lastCookTime = millis();
          lastRFIDReadTime = millis();
        } else {
          char debugMsg[LEC_PAYLOAD_LENGTH];

          snprintf(
            debugMsg,
            sizeof(debugMsg),
            "reject ingredient=%s chop=%d cook=%d recipe=%s",
            localIngredient,
            serverData.chopCount,
            serverData.cookCount,
            currentRecipeValid ? currentRecipe.name : "none"
          );

          sendDebugLogToServer("cook-reject", debugMsg);

          Serial.println("Ingredient is not allowed to cook for this recipe.");
          setStripColor(strip.Color(255, 0, 0));
          delay(1000);
          clearStrip();
        }
      } else {
        Serial.println("No RFID card detected.");
      }
    }
  } else if (cookState == CLIENT_PROCESSING) {
    if (!burntPlate && cooking) {
      if (currentTime - lastCookTime >= 1000) {
        lastCookTime = currentTime;

        char currentRfid[RFID_LENGTH] = {0};

        if (readRFID(currentRfid)) {
          if (strncmp(currentRfid, localRFID, RFID_LENGTH) == 0) {
            lastRFIDReadTime = millis();

            if (localCookCount < MAX_COOK_COUNT) {
              localCookCount++;
              updateCookLedSection(localCookCount);

              char debugMsg[LEC_PAYLOAD_LENGTH];
              snprintf(
                debugMsg,
                sizeof(debugMsg),
                "tick rfid=%s count=%d",
                localRFID,
                localCookCount
              );
              sendDebugLogToServer("cook-tick", debugMsg);

              if (localCookCount >= MAX_COOK_COUNT && !hasPlayedCookingCompleteAudio) {
                Serial.println("Plate burnt.");

                audioModule.stop();
                audioModule.playSpecified(4);

                hasPlayedCookingCompleteAudio = true;
                isPlayingCookingAudio = false;
                burntPlate = true;

                resetPacket(outgoingPacket, LEC_PKT_ON_FIRE);
                lecSetRfid(outgoingPacket, localRFID);
                sendDataToServer(&outgoingPacket);
              }
            }
          } else {
            Serial.println("Different RFID detected. Finalizing old plate.");

            sendPlateUpdate(
              localRFID,
              localIngredient,
              INVALID_COUNT,
              localCookCount,
              INVALID_COUNT,
              false,
              0,
              CLIENT_SHORT_ACK_TIMEOUT_MS
            );

            cooking = false;
            isPlayingCookingAudio = false;
            audioModule.stop();
            clearStrip();

            localCookCount = 0;
            memset(localRFID, 0, sizeof(localRFID));
            memset(localIngredient, 0, sizeof(localIngredient));

            cookState = CLIENT_IDLE;
          }
        } else if (PICC_IsAnyCardPresent()) {
          // Card is still physically present, but UID read was weak this cycle.
          // Keep cooking instead of stopping instantly.
          lastRFIDReadTime = millis();

          if (localCookCount < MAX_COOK_COUNT) {
            localCookCount++;
            updateCookLedSection(localCookCount);

            char debugMsg[LEC_PAYLOAD_LENGTH];
            snprintf(
              debugMsg,
              sizeof(debugMsg),
              "weak-read count=%d",
              localCookCount
            );
            sendDebugLogToServer("cook-tick", debugMsg);
          }
        } else if (millis() - lastRFIDReadTime > CLIENT_RFID_TIMEOUT_MS) {
          Serial.println("RFID removed. Finalizing cook count.");

          sendPlateUpdate(
            localRFID,
            localIngredient,
            INVALID_COUNT,
            localCookCount,
            INVALID_COUNT,
            false,
            0,
            CLIENT_SHORT_ACK_TIMEOUT_MS
          );

          cooking = false;
          isPlayingCookingAudio = false;
          audioModule.stop();
          clearStrip();

          localCookCount = 0;
          memset(localRFID, 0, sizeof(localRFID));
          memset(localIngredient, 0, sizeof(localIngredient));

          cookState = CLIENT_IDLE;
        }

        delay(50);
      }
    } else if (burntPlate) {
      char currentRfid[RFID_LENGTH] = {0};

      if (readRFID(currentRfid)) {
        if (strncmp(currentRfid, localRFID, RFID_LENGTH) == 0) {
          lastRFIDReadTime = millis();
        } else {
          Serial.println("Different RFID. Ending burnt scenario.");
          burntPlate = false;
          audioModule.stop();
          clearStrip();

          localCookCount = 0;
          memset(localRFID, 0, sizeof(localRFID));
          memset(localIngredient, 0, sizeof(localIngredient));

          cookState = CLIENT_IDLE;
        }
      } else if (millis() - lastRFIDReadTime > CLIENT_RFID_TIMEOUT_MS) {
        Serial.println("Burnt plate removed.");

        burntPlate = false;
        audioModule.stop();
        clearStrip();

        localCookCount = 0;
        memset(localRFID, 0, sizeof(localRFID));
        memset(localIngredient, 0, sizeof(localIngredient));

        cookState = CLIENT_IDLE;
      }

      delay(50);
    }
  }
}

/*************************************************************
  MIX ROLE
*************************************************************/

void executeMixStationClient() {
  currentTime = millis();

  if (!currentRecipeValid) {
    requestAndUpdateCurrentRecipe();

    if (!currentRecipeValid) {
      delay(100);
      return;
    }
  }

  if (isButtonPressed(stationButtonState)) {
    Serial.print("Mix Station button pressed. State: ");
    Serial.println(mixState == MIX_IDLE ? "IDLE" : "MIXING");

    blinkButtonLED();

    char scannedRfid[RFID_LENGTH] = {0};

    if (!readRFID(scannedRfid)) {
      Serial.println("No RFID card detected.");
      return;
    }

    Serial.print("RFID detected: ");
    Serial.println(scannedRfid);

    if (!requestPlateState(scannedRfid, CLIENT_RESPONSE_TIMEOUT_MS)) {
      return;
    }

    LecPacket plateData = serverData;

    char debugMsg[LEC_PAYLOAD_LENGTH];

    snprintf(
      debugMsg,
      sizeof(debugMsg),
      "recipe=%s plate=%s chop=%d cook=%d",
      currentRecipe.name,
      plateData.ingredient,
      plateData.chopCount,
      plateData.cookCount
    );

    sendDebugLogToServer("mix-plate", debugMsg);

    bool ingredientValid = false;

    for (int i = 0; i < currentRecipe.numIngredients; i++) {
      bool alreadyCollected = false;

      for (int j = 0; j < collectedIngredientCount; j++) {
        if (strcmp(collectedIngredients[j], currentRecipe.ingredients[i].name) == 0) {
          alreadyCollected = true;
          break;
        }
      }

      if (alreadyCollected) {
        continue;
      }

      snprintf(
        debugMsg,
        sizeof(debugMsg),
        "needs=%s reqChop=%d reqCook=%d",
        currentRecipe.ingredients[i].name,
        currentRecipe.ingredients[i].requiresChop,
        currentRecipe.ingredients[i].requiresCook
      );

      sendDebugLogToServer("mix-need", debugMsg);

      if (recipeRequirementMet(currentRecipe.ingredients[i], plateData)) {
        ingredientValid = true;

        strncpy(
          collectedIngredients[collectedIngredientCount],
          plateData.ingredient,
          INGREDIENT_LENGTH - 1
        );

        collectedIngredients[collectedIngredientCount][INGREDIENT_LENGTH - 1] = '\0';

        collectedIngredientCount++;

        if (collectedIngredientCount == currentRecipe.numIngredients) {
          audioModule.stop();
          audioModule.playSpecified(6);
        } else {
          audioModule.stop();
          audioModule.playSpecified(5);
        }

        int ledsPerIngredient = NUM_PIXELS / currentRecipe.numIngredients;
        int startLED = ledsPerIngredient * (collectedIngredientCount - 1);
        int endLED = startLED + ledsPerIngredient - 1;

        if (endLED >= NUM_PIXELS) {
          endLED = NUM_PIXELS - 1;
        }

        graduallyLightUpSection(startLED, endLED);

        sendPlateUpdate(
          plateData.rfid,
          "none",
          0,
          INVALID_COUNT,
          INVALID_COUNT,
          true,
          0,
          CLIENT_SHORT_ACK_TIMEOUT_MS
        );

        break;
      }
    }

    if (!ingredientValid) {
      sendDebugLogToServer("mix-rejected", "ingredient invalid or already collected");

      Serial.println("Ingredient invalid or already collected.");
      setStripColor(strip.Color(255, 0, 0));
      delay(1000);
      updateMixStationLEDs();
      return;
    }

    if (collectedIngredientCount == currentRecipe.numIngredients) {
      Serial.println("Mix complete.");

      sendDebugLogToServer("mix-accepted", plateData.ingredient);

      sendPlateUpdate(
        plateData.rfid,
        currentRecipe.name,
        0,
        INVALID_COUNT,
        INVALID_COUNT,
        false,
        0,
        CLIENT_SHORT_ACK_TIMEOUT_MS
      );

      setStripColor(strip.Color(0, 255, 0));
      delay(2000);
      clearStrip();

      mixState = MIX_IDLE;
      collectedIngredientCount = 0;
      memset(collectedIngredients, 0, sizeof(collectedIngredients));
    } else {
      mixState = MIX_MIXING;
    }
  }
}

/*************************************************************
  SERVE ROLE
*************************************************************/

void executeServeStationClient() {
  if (!currentRecipeValid) {
    requestAndUpdateCurrentRecipe();

    if (!currentRecipeValid) {
      delay(100);
      return;
    }
  }

  if (isButtonPressed(stationButtonState)) {
    Serial.println("Serve Station button pressed.");
    blinkButtonLED();

    char scannedRfid[RFID_LENGTH] = {0};

    if (!readRFID(scannedRfid)) {
      Serial.println("No RFID card detected.");
      return;
    }

    Serial.print("RFID detected: ");
    Serial.println(scannedRfid);

    if (!sendServeAttempt(scannedRfid, currentRecipe.name, CLIENT_RESPONSE_TIMEOUT_MS)) {
      return;
    }

    bool success = serverData.success || serverData.playerScoreDelta > 0;

    if (success) {
      bool serveCompletedRoundOrGame =
        strcmp(serverData.status, "serve-complete") == 0;

      Serial.println(
        serveCompletedRoundOrGame
          ? "Serve success completed round/game."
          : "Serve success."
      );

      setStripColor(strip.Color(0, 255, 0));
      audioModule.stop();

      if (serveCompletedRoundOrGame) {
        // Do not play +time video when this serve ends the game/round.
        // Show check-your-score / completion screen instead.
        sendSprite(0x0F);
      } else {
        audioModule.playSpecified(7);
        sendSprite(0x14);
      }

      delay(2000);
      clearStrip();
    }
    else {
      Serial.println("Serve failed.");

      setStripColor(strip.Color(255, 0, 0));
      audioModule.stop();
      audioModule.playSpecified(8);
      sendSprite(0x15);
      delay(2000);/*************************************************************
    Let Em Cook - Generic Client Station

    Handles 4 reusable client roles:
    - Chop
    - Cook
    - Mix
    - Serve
*************************************************************/

#define LEC_DEBUG 1

#include <LetEmCook.h>

#include <esp_now.h>
#include <WiFi.h>
#include <MFRC522.h>
#include <SPI.h>
#include <Adafruit_NeoPixel.h>
#include <DYPlayerArduino.h>

extern "C" {
  #include "esp_wifi.h"
}

/*************************************************************
  PIN DEFINITIONS
*************************************************************/

#define SS_PIN 21
#define RST_PIN 22

#define BUTTON_PIN 13
#define BUTTON_LIGHT_PIN 27

#define NEOPIXEL_PIN 4
#define NEOPIXEL_MOSFET_PIN 26
#define NUM_PIXELS 20

#define AUDIO_TX 16
#define AUDIO_RX 17

#define SPRITE_TX 32
#define SPRITE_RX 33

/*************************************************************
  LOCAL CONSTANTS
*************************************************************/

#define CLIENT_BUTTON_DEBOUNCE_MS 0
#define CLIENT_RESPONSE_TIMEOUT_MS 5000
#define CLIENT_SHORT_ACK_TIMEOUT_MS 2000
#define CLIENT_RFID_TIMEOUT_MS 800
#define ROLE_DELAY_MS 4500
#define CHECK_INTERVAL_MS 500
#define HELLO_RETRY_MS 3000

#define EXTINGUISH_VIDEO_MS 2500UL


/*************************************************************
  HARDWARE
*************************************************************/

MFRC522 rfid(SS_PIN, RST_PIN);
Adafruit_NeoPixel strip(NUM_PIXELS, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

HardwareSerial audioSerial(2);
DY::Player audioModule(&audioSerial);

HardwareSerial spriteSerial(1);

/*************************************************************
  NETWORK / PACKETS
*************************************************************/

uint8_t serverAddress[6];

LecPacket outgoingPacket;
LecPacket serverData;
LecPacket currentRecipeData;

volatile bool dataReceived = false;

unsigned long lastHelloSentAt = 0;

/*************************************************************
  ROLE / GAME STATE
*************************************************************/

LecRole currentRole = LEC_ROLE_NONE;
LecRole previousRole = LEC_ROLE_NONE;

LecRound currentRound = LEC_ROUND_NONE;

bool onFire = false;

bool roleTransitionPending = false;
LecRole roleTransitionTarget = LEC_ROLE_NONE;
unsigned long roleTransitionStart = 0;

volatile uint8_t pendingSpriteCmd = 0;

/*************************************************************
  LOCAL RFID / PLATE STATE
*************************************************************/

char localRFID[RFID_LENGTH] = {0};
char localIngredient[INGREDIENT_LENGTH] = {0};

unsigned long lastCheckTime = 0;
unsigned long currentTime = 0;
unsigned long lastRFIDReadTime = 0;

/*************************************************************
  BUTTON STATE
*************************************************************/

struct ButtonState {
  unsigned long lastButtonPressTime;
  bool lastButtonState;
};

ButtonState stationButtonState = {0, HIGH};

/*************************************************************
  SYSTEM STATES
*************************************************************/

enum SystemState {
  CLIENT_IDLE,
  CLIENT_PROCESSING
};

SystemState chopState = CLIENT_IDLE;
SystemState cookState = CLIENT_IDLE;

enum MixState {
  MIX_IDLE,
  MIX_MIXING
};

MixState mixState = MIX_IDLE;

/*************************************************************
  CHOP STATE
*************************************************************/

int localChopCount = 0;

/*************************************************************
  COOK STATE
*************************************************************/

bool cooking = false;
bool burntPlate = false;

unsigned long lastCookTime = 0;
int localCookCount = 0;

bool isPlayingCookingAudio = false;
bool hasPlayedCookingCompleteAudio = false;

bool resumeScreenAfterExtinguish = false;
unsigned long resumeScreenAt = 0;

/*************************************************************
  MIX / SERVE STATE
*************************************************************/

LecRecipe currentRecipe;
bool currentRecipeValid = false;

char collectedIngredients[MAX_RECIPE_INGREDIENTS][INGREDIENT_LENGTH] = {0};
int collectedIngredientCount = 0;

/*************************************************************
  FUNCTION PROTOTYPES
*************************************************************/

void initializeEspNow();
void initializeHardware();

void sendHelloToServer();
void sendDataToServer(LecPacket* packet);
bool waitForServerResponse(unsigned long timeout);
void processHelloHeartbeat();
void sendDebugLogToServer(const char* status, const char* payload);

void resetPacket(LecPacket& packet, LecPacketType type);
void resetSharedData();
void resetAllRoleData();
void performReinitialization();

void handleRoleAssignment(LecRole newRole);
void applyPendingRoleTransition();

void executeChopStationClient();
void executeCookStationClient();
void executeMixStationClient();
void executeServeStationClient();
bool canCookIngredientForCurrentRecipe(const char* ingredient, const LecPacket& plateData);

bool requestPlateState(const char* rfidUID, unsigned long timeoutMs);

bool sendPlateUpdate(
  const char* rfidUID,
  const char* ingredient,
  int16_t chopCount,
  int16_t cookCount,
  int16_t bakeCount,
  bool resetPlate,
  int16_t scoreDelta,
  unsigned long ackTimeoutMs
);

bool sendServeAttempt(const char* rfidUID, const char* recipeName, unsigned long timeoutMs);
void requestAndUpdateCurrentRecipe();

bool isButtonPressed(ButtonState& buttonState);
bool readRFID(char* rfidUID);
bool PICC_IsAnyCardPresent();
void resetRFIDReader();

void updateChopCountDisplay(int chopCount);
void updateCookLedSection(int cookCount);
void updateMixStationLEDs();

void setStripColor(uint32_t color);
void clearStrip();
void forceStripOffOnBoot();
void blinkButtonLED();

void graduallyLightUpSection(int startLED, int endLED);
void runFireEffect();

uint8_t getVideoID(const char* recipeName);
void sendSprite(uint8_t cmd);

bool recipeRequirementMet(const LecIngredientRequirement& req, const LecPacket& plateData);

void processResumeScreenAfterExtinguish();
void playCurrentRoleScreen();

void sendMaintenanceResultToServer(
  LecPacketType packetType,
  LecPacketResult result,
  const char* status,
  const char* payload
);

void handleMaintenanceStatus(const char* status);
void handleMaintenanceProgress(uint8_t percent);
void showMaintenanceProgress(uint8_t percent);
void showMaintenanceError();
void showMaintenanceSuccess();

/*************************************************************
  ESP-NOW CALLBACKS
*************************************************************/

void onDataSent(const wifi_tx_info_t* tx_info, esp_now_send_status_t status) {
  Serial.print("Send status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAILED");
}

void onDataRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (len != sizeof(LecPacket)) {
    Serial.println("Received packet size mismatch.");
    return;
  }

  LecPacket incoming;
  memcpy(&incoming, data, sizeof(incoming));

  if (!lecIsValidPacket(incoming, len)) {
    Serial.println("Invalid Let Em Cook packet received.");
    return;
  }

  Serial.println("=== Packet Received From Server ===");
  Serial.print("Packet Type: ");
  Serial.println(static_cast<int>(incoming.packetType));
  Serial.print("Role: ");
  Serial.println(static_cast<int>(incoming.role));
  Serial.print("Round: ");
  Serial.println(static_cast<int>(incoming.round));
  Serial.print("Ingredient: ");
  Serial.println(incoming.ingredient);
  Serial.print("Recipe: ");
  Serial.println(incoming.recipeName);
  Serial.println("===================================");

  if (LecMaintenance::handlePacket(incoming)) {
    return;
  }

  if (roleTransitionPending) {
    if (incoming.packetType == LEC_PKT_REINITIALIZE ||
        incoming.packetType == LEC_PKT_END_GAME) {
      roleTransitionPending = false;
      roleTransitionTarget = LEC_ROLE_NONE; // CHANGED: fully clear pending role target.
    }
  }

  switch (incoming.packetType) {
    case LEC_PKT_ASSIGN_ROLE:
      handleRoleAssignment(incoming.role);
      break;

    case LEC_PKT_START_ROUND:
      // CHANGED: More defensive duplicate check.
      // If we are already in this round OR transitioning into this round, do not restart visuals.

      resumeScreenAfterExtinguish = false;
      if (currentRound == incoming.round &&
          (currentRole != LEC_ROLE_NONE || roleTransitionPending)) {
        Serial.println("Duplicate START_ROUND ignored.");
        break;
      }

      currentRound = incoming.round;

      // CHANGED: clear recipe for the new round so serve station does not use stale round recipe.
      currentRecipeValid = false;
      memset(&currentRecipe, 0, sizeof(currentRecipe));
      memset(&currentRecipeData, 0, sizeof(currentRecipeData));

      // CHANGED: clear old gameplay/LED/RFID state when a new round starts.
      resetAllRoleData();

      // CHANGED: cancel any old role transition from previous round.
      roleTransitionPending = false;
      roleTransitionTarget = LEC_ROLE_NONE;
      currentRole = LEC_ROLE_NONE;

      if (incoming.round == LEC_ROUND_1) {
        // Round 1 has the countdown video.
        pendingSpriteCmd = 0x13;
        Serial.println("Received Start Round 1. Playing countdown 0x13.");
      } 
      else if (incoming.round == LEC_ROUND_2) {
        // Do NOT play tornado here.
        // Tornado 0x16 should only happen from LEC_PKT_ASSIGN_ROLE.
        pendingSpriteCmd = 0x00;
        Serial.println("Received Start Round 2. Waiting for role assignment tornado.");
      }

      break;

    case LEC_PKT_END_ROUND:
      resetAllRoleData();
      currentRole = LEC_ROLE_NONE;         
      previousRole = LEC_ROLE_NONE;        
      roleTransitionPending = false;       
      roleTransitionTarget = LEC_ROLE_NONE;
      currentRound = LEC_ROUND_NONE;
      currentRecipeValid = false;           
      resumeScreenAfterExtinguish = false;
      break;

    case LEC_PKT_END_GAME:
      Serial.println("Received End Game.");
      audioModule.stop();
      clearStrip();
      onFire = false;
      currentRole = LEC_ROLE_NONE;
      previousRole = LEC_ROLE_NONE;
      currentRound = LEC_ROUND_NONE;
      roleTransitionPending = false;        
      roleTransitionTarget = LEC_ROLE_NONE; 
      currentRecipeValid = false;          
      pendingSpriteCmd = 0x0F;
      resumeScreenAfterExtinguish = false;
      resetAllRoleData();
      break;

    case LEC_PKT_REINITIALIZE:
      performReinitialization();
      break;

    case LEC_PKT_PLATE_STATE:
    case LEC_PKT_SERVE_RESULT:
    case LEC_PKT_RECIPE_STATE:
      memcpy(&serverData, &incoming, sizeof(LecPacket));
      dataReceived = true;
      break;

    case LEC_PKT_NEW_RECIPE:
      Serial.println("Received New Recipe.");

      // Ignore recipe packets from another round.
      if (incoming.round != currentRound) {
        Serial.println("Ignoring recipe from different round.");
        break;
      }

      if (currentRecipeValid && strcmp(currentRecipe.name, incoming.recipeName) == 0) {
        // Same recipe can still be a new iteration and should refresh the screen.
        Serial.println("Same recipe received again; refreshing screen/state.");
      }

      if (lecFindRecipeByName(incoming.recipeName, currentRecipe)) {
        currentRecipeValid = true;
        memcpy(&currentRecipeData, &incoming, sizeof(LecPacket));

        Serial.print("Current recipe set to: ");
        Serial.println(currentRecipe.name);

        if (currentRole == LEC_ROLE_MIX_STATION) {
          mixState = MIX_IDLE;
          collectedIngredientCount = 0;
          memset(collectedIngredients, 0, sizeof(collectedIngredients));
          clearStrip();
        }

        if (currentRole == LEC_ROLE_SERVE_STATION) {
          uint8_t videoID = getVideoID(currentRecipe.name);
          if (videoID != 0x00) {
            pendingSpriteCmd = videoID;
          }
        } else {
          // This is expected during tornado transition.
          // The recipe is stored and applyPendingRoleTransition() will use it if this client becomes serve.
          Serial.println("Recipe stored; client is not currently serve station.");
        }
      } else {
        Serial.println("Unknown recipe received.");
      }
      break;

    case LEC_PKT_ON_FIRE:
      Serial.println("Station is now on fire.");
      pendingSpriteCmd = 0x0A;
      audioModule.stop();
      audioModule.playSpecified(4);
      onFire = true;
      break;

    case LEC_PKT_EXTINGUISH_FIRE:
      Serial.println("Station fire extinguished.");

      pendingSpriteCmd = 0x0B;

      audioModule.stop();
      audioModule.playSpecified(9);

      resetAllRoleData();

      onFire = false;

      // After extinguish video, resume current station/recipe screen.
      resumeScreenAfterExtinguish = true;
      resumeScreenAt = millis() + EXTINGUISH_VIDEO_MS;

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
  Serial.println("Booting Let Em Cook Generic Client...");

  memcpy(serverAddress, LEC_DEFAULT_SERVER_MAC, 6);

  WiFi.mode(WIFI_STA);
  delay(100);

  Serial.print("Client MAC Address: ");
  Serial.println(WiFi.macAddress());

  SPI.begin();
  esp_wifi_set_channel(LEC_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

  Serial.print("ESP-NOW Channel: ");
  Serial.println(LEC_ESPNOW_CHANNEL);

  initializeHardware();
  initializeEspNow();

  resetAllRoleData();
  sendHelloToServer();

  LecMaintenance::begin(
    LEC_DEVICE_GENERIC_CLIENT,
    handleMaintenanceStatus,
    handleMaintenanceProgress,
    sendMaintenanceResultToServer
  );

  Serial.print("Size of LecPacket: ");
  Serial.println(sizeof(LecPacket));

  Serial.println("Generic Client ready.");
}

/*************************************************************
  LOOP
*************************************************************/

void loop() {
  LecMaintenance::tick();

  if (LecMaintenance::isMaintenanceMode()) {
    delay(25);
    return;
  }

  processHelloHeartbeat();
  
  if (pendingSpriteCmd) {
    sendSprite(pendingSpriteCmd);
    pendingSpriteCmd = 0;
  }

  processResumeScreenAfterExtinguish();

  if (roleTransitionPending) {
    applyPendingRoleTransition();
    return;
  }

  if (onFire) {
    runFireEffect();
    delay(50);
    return;
  }

  switch (currentRole) {
    case LEC_ROLE_CHOP_STATION:
      executeChopStationClient();
      break;

    case LEC_ROLE_COOK_STATION:
      executeCookStationClient();
      break;

    case LEC_ROLE_MIX_STATION:
      executeMixStationClient();
      break;

    case LEC_ROLE_SERVE_STATION:
      executeServeStationClient();
      break;

    default:
      delay(100);
      break;
  }

  delay(10);
}

/*************************************************************
  INITIALIZATION
*************************************************************/

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

void initializeHardware() {
  rfid.PCD_Init();
  delay(50);
  Serial.println("RFID reader initialized.");

  pinMode(NEOPIXEL_MOSFET_PIN, OUTPUT);
  digitalWrite(NEOPIXEL_MOSFET_PIN, LOW);

  strip.begin();
  forceStripOffOnBoot(); // Prevents strip from staying random colors on boot
  delay(50);

  Serial.println("Neopixel strip initialized.");

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  pinMode(BUTTON_LIGHT_PIN, OUTPUT);
  digitalWrite(BUTTON_LIGHT_PIN, HIGH);

  audioSerial.begin(9600, SERIAL_8N1, AUDIO_RX, AUDIO_TX);
  audioModule.begin();
  audioModule.setCycleMode(DY::PlayMode::OneOff);
  audioModule.setVolume(24);
  audioModule.stop();

  spriteSerial.begin(9600, SERIAL_8N1, SPRITE_RX, SPRITE_TX);
  sendSprite(0x00);

  Serial.println("Hardware initialization complete.");
}

/*************************************************************
  PACKET HELPERS
*************************************************************/

void resetPacket(LecPacket& packet, LecPacketType type) {
  lecInitPacket(
    packet,
    type,
    LEC_DEVICE_GENERIC_CLIENT,
    currentRole
  );

  packet.round = currentRound;
}

void sendDebugLogToServer(const char* status, const char* payload) {
  LecPacket debugPacket;

  lecInitPacket(
    debugPacket,
    LEC_PKT_DEBUG_LOG,
    LEC_DEVICE_GENERIC_CLIENT,
    currentRole
  );

  debugPacket.round = currentRound;

  if (status != nullptr) {
    lecSetStatus(debugPacket, status);
  }

  if (payload != nullptr) {
    lecSetPayload(debugPacket, payload);
  }

  sendDataToServer(&debugPacket);
}

void processHelloHeartbeat() {
  if (roleTransitionPending) {
    return;
  }

  if (currentRound != LEC_ROUND_NONE || currentRole != LEC_ROLE_NONE) {
    return;
  }

  if (millis() - lastHelloSentAt < HELLO_RETRY_MS) {
    return;
  }

  lastHelloSentAt = millis();

  Serial.println("Sending HELLO heartbeat to server...");
  sendHelloToServer();
}

void sendHelloToServer() {
  LecPacket hello;

  lecInitPacket(
    hello,
    LEC_PKT_HELLO,
    LEC_DEVICE_GENERIC_CLIENT,
    currentRole
  );

  hello.round = currentRound;
  lecSetStatus(hello, "generic-client-online");

  lastHelloSentAt = millis();

  sendDataToServer(&hello);
}

void sendDataToServer(LecPacket* packet) {
  if (packet == nullptr) return;

  packet->protocolVersion = LEC_PROTOCOL_VERSION;
  packet->packetSize = sizeof(LecPacket);
  packet->uptimeMs = millis();
  packet->deviceClass = LEC_DEVICE_GENERIC_CLIENT;
  packet->role = currentRole;
  packet->round = currentRound;
  packet->runMode = LecMaintenance::getRunMode();

  Serial.println("=== Sending Packet To Server ===");
  Serial.print("Packet Type: ");
  Serial.println(static_cast<int>(packet->packetType));
  Serial.print("Role: ");
  Serial.println(static_cast<int>(packet->role));
  Serial.print("RFID: ");
  Serial.println(packet->rfid);
  Serial.print("Ingredient: ");
  Serial.println(packet->ingredient);
  Serial.println("================================");

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
    LEC_DEVICE_GENERIC_CLIENT,
    currentRole
  );

  response.result = result;
  response.round = currentRound;
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

  if (strcmp(status, "ota-starting") == 0 ||
      strcmp(status, "wifi-connecting") == 0 ||
      strcmp(status, "ota-http-begin") == 0 ||
      strcmp(status, "ota-update-begin") == 0) {
    // Indicate update/maintenance activity.
    digitalWrite(BUTTON_LIGHT_PIN, LOW);
    setStripColor(strip.Color(0, 0, 255));
  }
  else if (strcmp(status, "ota-success") == 0) {
    showMaintenanceSuccess();
  }
  else if (strcmp(status, "ota-failed") == 0 ||
           strcmp(status, "wifi-failed") == 0) {
    showMaintenanceError();
  }
  else if (strcmp(status, "maintenance-exit") == 0) {
    clearStrip();
    digitalWrite(BUTTON_LIGHT_PIN, HIGH);
  }
}

void handleMaintenanceProgress(uint8_t percent) {
  showMaintenanceProgress(percent);
}

void showMaintenanceProgress(uint8_t percent) {
  if (percent > 100) {
    percent = 100;
  }

  digitalWrite(NEOPIXEL_MOSFET_PIN, HIGH);
  strip.clear();

  int litPixels = (NUM_PIXELS * percent) / 100;

  for (int i = 0; i < litPixels; i++) {
    strip.setPixelColor(i, strip.Color(0, 0, 255));
  }

  strip.show();

  // CHANGED: Blink button light during update.
  digitalWrite(BUTTON_LIGHT_PIN, (millis() / 250) % 2 == 0 ? HIGH : LOW);
}

void showMaintenanceError() {
  digitalWrite(NEOPIXEL_MOSFET_PIN, HIGH);

  for (int flash = 0; flash < 3; flash++) {
    setStripColor(strip.Color(255, 0, 0));
    digitalWrite(BUTTON_LIGHT_PIN, LOW);
    delay(150);

    clearStrip();
    digitalWrite(BUTTON_LIGHT_PIN, HIGH);
    delay(150);
  }
}

void showMaintenanceSuccess() {
  digitalWrite(NEOPIXEL_MOSFET_PIN, HIGH);

  for (int flash = 0; flash < 3; flash++) {
    setStripColor(strip.Color(0, 255, 0));
    digitalWrite(BUTTON_LIGHT_PIN, HIGH);
    delay(150);

    clearStrip();
    digitalWrite(BUTTON_LIGHT_PIN, LOW);
    delay(150);
  }
}

bool waitForServerResponse(unsigned long timeout) {
  unsigned long startTime = millis();
  dataReceived = false;

  while (!dataReceived && (millis() - startTime) < timeout) {
    delay(10);
  }

  return dataReceived;
}

bool requestPlateState(const char* rfidUID, unsigned long timeoutMs) {
  if (!rfidUID) return false;

  resetPacket(outgoingPacket, LEC_PKT_PLATE_LOOKUP);
  lecSetRfid(outgoingPacket, rfidUID);

  memset(&serverData, 0, sizeof(serverData));
  dataReceived = false;

  sendDataToServer(&outgoingPacket);

  if (!waitForServerResponse(timeoutMs)) {
    Serial.println("Timeout waiting for plate state.");
    return false;
  }

  if (serverData.packetType != LEC_PKT_PLATE_STATE) {
    Serial.print("Unexpected packet type: ");
    Serial.println(static_cast<int>(serverData.packetType));
    return false;
  }

  return true;
}

bool sendPlateUpdate(
  const char* rfidUID,
  const char* ingredient,
  int16_t chopCount,
  int16_t cookCount,
  int16_t bakeCount,
  bool resetPlate,
  int16_t scoreDelta,
  unsigned long ackTimeoutMs
) {
  resetPacket(outgoingPacket, LEC_PKT_PLATE_UPDATE);

  lecSetRfid(outgoingPacket, rfidUID);
  lecSetIngredient(outgoingPacket, ingredient);

  outgoingPacket.chopCount = chopCount;
  outgoingPacket.cookCount = cookCount;
  outgoingPacket.bakeCount = bakeCount;
  outgoingPacket.resetPlate = resetPlate;
  outgoingPacket.playerScoreDelta = scoreDelta;

  dataReceived = false;
  sendDataToServer(&outgoingPacket);

  if (ackTimeoutMs == 0) {
    return true;
  }

  if (!waitForServerResponse(ackTimeoutMs)) {
    Serial.println("No ACK from server on plate update.");
    return false;
  }

  return true;
}

bool sendServeAttempt(const char* rfidUID, const char* recipeName, unsigned long timeoutMs) {
  resetPacket(outgoingPacket, LEC_PKT_SERVE_ATTEMPT);

  lecSetRfid(outgoingPacket, rfidUID);
  lecSetRecipeName(outgoingPacket, recipeName);

  memset(&serverData, 0, sizeof(serverData));
  dataReceived = false;

  sendDataToServer(&outgoingPacket);

  if (!waitForServerResponse(timeoutMs)) {
    Serial.println("Timeout waiting for serve result.");
    return false;
  }

  if (serverData.packetType != LEC_PKT_SERVE_RESULT) {
    Serial.print("Unexpected serve result packet type: ");
    Serial.println(static_cast<int>(serverData.packetType));
    return false;
  }

  return true;
}

/*************************************************************
  RESET / ROLE HANDLING
*************************************************************/

void resetSharedData() {
  memset(&outgoingPacket, 0, sizeof(outgoingPacket));
  memset(&serverData, 0, sizeof(serverData));
  dataReceived = false;
}

void resetAllRoleData() {
  resetSharedData();

  chopState = CLIENT_IDLE;
  cookState = CLIENT_IDLE;
  mixState = MIX_IDLE;

  memset(localRFID, 0, sizeof(localRFID));
  memset(localIngredient, 0, sizeof(localIngredient));

  localChopCount = 0;
  localCookCount = 0;

  cooking = false;
  burntPlate = false;
  isPlayingCookingAudio = false;
  hasPlayedCookingCompleteAudio = false;

  collectedIngredientCount = 0;
  memset(collectedIngredients, 0, sizeof(collectedIngredients));

  clearStrip();

  Serial.println("All role-specific data reset.");
}

void performReinitialization() {
  Serial.println("Performing re-initialization...");

  audioModule.stop();

  resetAllRoleData();

  onFire = false;
  currentRole = LEC_ROLE_NONE;
  previousRole = LEC_ROLE_NONE;
  currentRound = LEC_ROUND_NONE;
  roleTransitionPending = false;
  resumeScreenAfterExtinguish = false;

  pendingSpriteCmd = 0x00;

  Serial.println("Re-initialization complete.");
}

void playCurrentRoleScreen() {
  uint8_t playCommand = 0x00;

  switch (currentRole) {
    case LEC_ROLE_CHOP_STATION:
      playCommand = 0x02;
      break;

    case LEC_ROLE_COOK_STATION:
      playCommand = 0x03;
      break;

    case LEC_ROLE_MIX_STATION:
      playCommand = 0x04;
      break;

    case LEC_ROLE_SERVE_STATION:
      if (!currentRecipeValid) {
        Serial.println("Serve resume requested but recipe missing. Requesting recipe.");
        requestAndUpdateCurrentRecipe();
      }

      if (currentRecipeValid) {
        playCommand = getVideoID(currentRecipe.name);
      }
      break;

    default:
      playCommand = 0x00;
      break;
  }

  if (playCommand != 0x00) {
    sendSprite(playCommand);
  }
}

void processResumeScreenAfterExtinguish() {
  if (!resumeScreenAfterExtinguish) return;

  if (millis() >= resumeScreenAt) {
    resumeScreenAfterExtinguish = false;

    Serial.println("Resuming role screen after extinguish video.");

    // Return each client to its current station/recipe screen.
    playCurrentRoleScreen();
  }
}

void handleRoleAssignment(LecRole newRole) {
  Serial.print("Received role assignment: ");
  Serial.println(static_cast<int>(newRole));

  resumeScreenAfterExtinguish = false;

  if (newRole != LEC_ROLE_CHOP_STATION &&
      newRole != LEC_ROLE_COOK_STATION &&
      newRole != LEC_ROLE_MIX_STATION &&
      newRole != LEC_ROLE_SERVE_STATION) {
    Serial.println("WARNING: Invalid gameplay role received by generic client.");
    return;
  }

  // Ignore duplicate role packet if already fully on this role.
  if (!roleTransitionPending && currentRole == newRole) {
    Serial.println("Duplicate role assignment ignored.");
    return;
  }

  // Ignore duplicate role packet if already transitioning to this role.
  if (roleTransitionPending && roleTransitionTarget == newRole) {
    Serial.println("Duplicate pending role assignment ignored.");
    return;
  }

  audioModule.stop();
  audioModule.playSpecified(11);

  // Save old role, but mark client as neutral during transition.
  // This prevents recipe packets or button logic from acting like the old station.
  previousRole = currentRole;
  currentRole = LEC_ROLE_NONE;

  // Reset station state before tornado.
  resetAllRoleData();

  // Tornado should only be triggered here, not by StartRound2.
  pendingSpriteCmd = 0x16;

  roleTransitionPending = true;
  roleTransitionTarget = newRole;
  roleTransitionStart = millis();

  Serial.println("Role transition started. Playing tornado 0x16.");
}

void applyPendingRoleTransition() {
  if (millis() - roleTransitionStart < ROLE_DELAY_MS) {
    return;
  }

  roleTransitionPending = false;
  currentRole = roleTransitionTarget;
  previousRole = roleTransitionTarget;
  roleTransitionTarget = LEC_ROLE_NONE; // clear target after applying.

  uint8_t playCommand = 0x00;

  switch (currentRole) {
    case LEC_ROLE_CHOP_STATION:
      playCommand = 0x02;
      break;

    case LEC_ROLE_COOK_STATION:
      playCommand = 0x03;
      break;

    case LEC_ROLE_MIX_STATION:
      playCommand = 0x04;
      break;

    case LEC_ROLE_SERVE_STATION:
      // If recipe arrived during tornado, this will already be true.
      // If it did not arrive yet, ask server once so station video is less likely to be missed.
      if (!currentRecipeValid) {
        Serial.println("Serve role applied but recipe missing. Requesting recipe.");
        requestAndUpdateCurrentRecipe();
      }

      if (currentRecipeValid) {
        playCommand = getVideoID(currentRecipe.name);
      } else {
        playCommand = 0x00;
        Serial.println("Serve station has no recipe video yet.");
      }
      break;

    default:
      playCommand = 0x00;
      break;
  }

  if (playCommand != 0x00) {
    sendSprite(playCommand);
  }

  Serial.print("Role transition applied. Current role: ");
  Serial.println(static_cast<int>(currentRole));
}

/*************************************************************
  RFID HELPERS
*************************************************************/

bool PICC_IsAnyCardPresent() {
  byte bufferATQA[2];
  byte bufferSize = sizeof(bufferATQA);

  rfid.PCD_WriteRegister(rfid.TxModeReg, 0x00);
  rfid.PCD_WriteRegister(rfid.RxModeReg, 0x00);
  rfid.PCD_WriteRegister(rfid.ModWidthReg, 0x26);

  MFRC522::StatusCode result = rfid.PICC_WakeupA(bufferATQA, &bufferSize);

  return result == MFRC522::STATUS_OK || result == MFRC522::STATUS_COLLISION;
}

bool readRFID(char* rfidUID) {
  if (!rfidUID) return false;

  if (PICC_IsAnyCardPresent() && rfid.PICC_ReadCardSerial()) {
    snprintf(
      rfidUID,
      RFID_LENGTH,
      "%02X%02X%02X%02X",
      rfid.uid.uidByte[0],
      rfid.uid.uidByte[1],
      rfid.uid.uidByte[2],
      rfid.uid.uidByte[3]
    );

    rfidUID[RFID_LENGTH - 1] = '\0';

    resetRFIDReader();
    return true;
  }

  return false;
}

void resetRFIDReader() {
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}

/*************************************************************
  BUTTON / LED HELPERS
*************************************************************/

bool isButtonPressed(ButtonState& buttonState) {
  bool currentButtonState = digitalRead(BUTTON_PIN);
  unsigned long now = millis();

  if (buttonState.lastButtonState == HIGH && currentButtonState == LOW) {
    if (now - buttonState.lastButtonPressTime > CLIENT_BUTTON_DEBOUNCE_MS) {
      buttonState.lastButtonPressTime = now;
      buttonState.lastButtonState = currentButtonState;
      return true;
    }
  }

  buttonState.lastButtonState = currentButtonState;
  return false;
}

void blinkButtonLED() {
  digitalWrite(BUTTON_LIGHT_PIN, LOW);
  delay(100);
  digitalWrite(BUTTON_LIGHT_PIN, HIGH);
}

void setStripColor(uint32_t color) {
  digitalWrite(NEOPIXEL_MOSFET_PIN, HIGH);
  strip.clear();

  for (int i = 0; i < NUM_PIXELS; i++) {
    strip.setPixelColor(i, color);
  }

  strip.show();
}

void clearStrip() {
  strip.clear();
  strip.show();
  digitalWrite(NEOPIXEL_MOSFET_PIN, LOW);
}

void updateChopCountDisplay(int chopCount) {
  if (chopCount < 0) chopCount = 0;
  if (chopCount > MAX_CHOP_COUNT) chopCount = MAX_CHOP_COUNT;

  if (chopCount == MAX_CHOP_COUNT) {
    setStripColor(strip.Color(0, 255, 0));
    return;
  }

  int ledsPerChop = NUM_PIXELS / MAX_CHOP_COUNT;
  int ledsToLight = chopCount * ledsPerChop;

  if (chopCount > 0) {
    digitalWrite(NEOPIXEL_MOSFET_PIN, HIGH);
  } else {
    digitalWrite(NEOPIXEL_MOSFET_PIN, LOW);
  }

  strip.clear();

  for (int i = 0; i < ledsToLight; i++) {
    strip.setPixelColor(i, strip.Color(255, 255, 0));
  }

  strip.show();
}

void updateCookLedSection(int cookCount) {
  int ledsPerSection = NUM_PIXELS / MAX_COOK_COUNT;
  int totalLedsToLight = cookCount * ledsPerSection;

  if (totalLedsToLight > NUM_PIXELS) {
    totalLedsToLight = NUM_PIXELS;
  }

  uint32_t color;

  if (cookCount >= 1 && cookCount <= 6) {
    color = strip.Color(255, 255, 0);
  } else if (cookCount >= 7 && cookCount <= 9) {
    color = strip.Color(0, 255, 0);
  } else if (cookCount >= MAX_COOK_COUNT) {
    color = strip.Color(255, 0, 0);
  } else {
    color = strip.Color(0, 0, 0);
  }

  digitalWrite(NEOPIXEL_MOSFET_PIN, HIGH);

  for (int i = 0; i < totalLedsToLight; i++) {
    strip.setPixelColor(i, color);
  }

  for (int i = totalLedsToLight; i < NUM_PIXELS; i++) {
    strip.setPixelColor(i, strip.Color(0, 0, 0));
  }

  strip.show();
}

void graduallyLightUpSection(int startLED, int endLED) {
  digitalWrite(NEOPIXEL_MOSFET_PIN, HIGH);

  for (int i = startLED; i <= endLED; i++) {
    strip.setPixelColor(i, strip.Color(255, 255, 0));
    strip.show();
    delay(100);
  }
}

void updateMixStationLEDs() {
  if (!currentRecipeValid || currentRecipe.numIngredients <= 0) {
    clearStrip();
    return;
  }

  strip.clear();

  int ledsPerIngredient = NUM_PIXELS / currentRecipe.numIngredients;

  for (int i = 0; i < collectedIngredientCount; i++) {
    int startLED = ledsPerIngredient * i;
    int endLED = startLED + ledsPerIngredient - 1;

    if (endLED >= NUM_PIXELS) {
      endLED = NUM_PIXELS - 1;
    }

    for (int j = startLED; j <= endLED; j++) {
      strip.setPixelColor(j, strip.Color(255, 255, 0));
    }
  }

  strip.show();
}

void runFireEffect() {
  digitalWrite(NEOPIXEL_MOSFET_PIN, HIGH);

  for (int i = 0; i < NUM_PIXELS; i++) {
    int flickerR = random(220, 256);
    int flickerG = random(0, 100);
    int flickerB = 0;

    strip.setPixelColor(i, strip.Color(flickerR, flickerG, flickerB));
  }

  strip.show();
}

/*************************************************************
  RECIPE HELPERS
*************************************************************/

void requestAndUpdateCurrentRecipe() {
  resetPacket(outgoingPacket, LEC_PKT_CURRENT_RECIPE_REQUEST);

  memset(&currentRecipeData, 0, sizeof(currentRecipeData));
  dataReceived = false;

  sendDataToServer(&outgoingPacket);

  if (!waitForServerResponse(CLIENT_RESPONSE_TIMEOUT_MS)) {
    Serial.println("Timeout waiting for recipe state.");
    return;
  }

  if (serverData.packetType != LEC_PKT_RECIPE_STATE) {
    Serial.print("Unexpected recipe response packet type: ");
    Serial.println(static_cast<int>(serverData.packetType));
    return;
  }

  memcpy(&currentRecipeData, &serverData, sizeof(LecPacket));

  if (lecFindRecipeByName(currentRecipeData.recipeName, currentRecipe)) {
    currentRecipeValid = true;
    Serial.print("Current recipe updated to: ");
    Serial.println(currentRecipe.name);
  } else {
    currentRecipeValid = false;
    Serial.println("Unknown recipe received.");
  }
}

bool recipeRequirementMet(const LecIngredientRequirement& req, const LecPacket& plateData) {
  if (strcmp(plateData.ingredient, req.name) != 0) {
    return false;
  }

  if (req.requiresChop && plateData.chopCount < req.requiredChopCount) {
    return false;
  }

  if (req.requiresCook) {
    if (plateData.cookCount < req.requiredCookCountMin ||
        plateData.cookCount > req.requiredCookCountMax) {
      return false;
    }
  }

  return true;
}

/*************************************************************
  CHOP ROLE
*************************************************************/

void executeChopStationClient() {
  currentTime = millis();

  if (chopState == CLIENT_IDLE) {
    if (isButtonPressed(stationButtonState)) {
      Serial.println("Chop Station: button pressed.");
      blinkButtonLED();

      if (readRFID(localRFID)) {
        Serial.print("RFID detected: ");
        Serial.println(localRFID);

        if (!requestPlateState(localRFID, CLIENT_RESPONSE_TIMEOUT_MS)) {
          return;
        }

        strncpy(localIngredient, serverData.ingredient, INGREDIENT_LENGTH - 1);
        localIngredient[INGREDIENT_LENGTH - 1] = '\0';

        localChopCount = serverData.chopCount;

        Serial.print("Ingredient: ");
        Serial.println(localIngredient);
        Serial.print("Chop count: ");
        Serial.println(localChopCount);

        if (strcmp(localIngredient, "none") == 0 || localIngredient[0] == '\0') {
          Serial.println("No ingredient on plate.");
          setStripColor(strip.Color(255, 0, 0));
          delay(1000);
          clearStrip();
          return;
        }

        if (localChopCount >= MAX_CHOP_COUNT) {
          Serial.println("Plate already max chopped.");
          setStripColor(strip.Color(0, 255, 0));
          delay(1000);
          clearStrip();
          return;
        }
        chopState = CLIENT_PROCESSING;
        localChopCount++;
        updateChopCountDisplay(localChopCount);
        //audioModule.stop();
        audioModule.playSpecified(2);
      } else {
        Serial.println("No RFID card detected.");
      }
    }
  } else if (chopState == CLIENT_PROCESSING) {
    if (isButtonPressed(stationButtonState)) {
      Serial.println("Chop Station: increment chop.");
      blinkButtonLED();

      if (localChopCount < MAX_CHOP_COUNT) {
        localChopCount++;
        updateChopCountDisplay(localChopCount);
        //audioModule.stop();
        audioModule.playSpecified(2);
      } else {
        setStripColor(strip.Color(0, 255, 0));
      }
    }

    if (currentTime - lastCheckTime >= CHECK_INTERVAL_MS) {
      lastCheckTime = currentTime;

      char currentRfid[RFID_LENGTH] = {0};

      if (readRFID(currentRfid)) {
        if (strncmp(currentRfid, localRFID, RFID_LENGTH) == 0) {
          return;
        }
      }

      Serial.println("Plate removed. Sending final chop count.");

      sendPlateUpdate(
        localRFID,
        localIngredient,
        localChopCount,
        INVALID_COUNT,
        INVALID_COUNT,
        false,
        0,
        CLIENT_SHORT_ACK_TIMEOUT_MS
      );

      clearStrip();

      localChopCount = 0;
      memset(localRFID, 0, sizeof(localRFID));
      memset(localIngredient, 0, sizeof(localIngredient));

      chopState = CLIENT_IDLE;
    }
  }
}

/*************************************************************
  COOK ROLE
*************************************************************/

bool canCookIngredientForCurrentRecipe(const char* ingredient, const LecPacket& plateData) {
  if (!ingredient || !currentRecipeValid) {
    return false;
  }

  for (int i = 0; i < currentRecipe.numIngredients; i++) {
    const LecIngredientRequirement& req = currentRecipe.ingredients[i];

    if (strcmp(req.name, ingredient) != 0) {
      continue;
    }

    // Ingredient is part of this recipe, but this recipe does not require cooking it.
    if (!req.requiresCook) {
      return false;
    }

    // If this recipe requires chopping before cooking, enforce the chop requirement.
    if (req.requiresChop && plateData.chopCount < req.requiredChopCount) {
      return false;
    }

    return true;
  }

  return false;
}

void executeCookStationClient() {
  currentTime = millis();

  if (!currentRecipeValid) {
    requestAndUpdateCurrentRecipe();

    if (!currentRecipeValid) {
      delay(100);
      return;
    }
  }

  if (cookState == CLIENT_IDLE) {
    if (isButtonPressed(stationButtonState)) {
      Serial.println("Cook Station: button pressed.");
      blinkButtonLED();

      if (readRFID(localRFID)) {
        Serial.print("RFID detected: ");
        Serial.println(localRFID);

        if (!requestPlateState(localRFID, CLIENT_RESPONSE_TIMEOUT_MS)) {
          return;
        }

        strncpy(localIngredient, serverData.ingredient, INGREDIENT_LENGTH - 1);
        localIngredient[INGREDIENT_LENGTH - 1] = '\0';

        localCookCount = serverData.cookCount;

        Serial.print("Ingredient: ");
        Serial.println(localIngredient);
        Serial.print("Chop count: ");
        Serial.println(serverData.chopCount);
        Serial.print("Cook count: ");
        Serial.println(localCookCount);
        Serial.print("Current recipe: ");
        Serial.println(currentRecipe.name);

        if (localCookCount >= MAX_COOK_COUNT) {
          Serial.println("Plate already burnt.");
          burntPlate = true;
          cooking = false;
          cookState = CLIENT_PROCESSING;

          setStripColor(strip.Color(255, 0, 0));
          audioModule.stop();
          audioModule.playSpecified(4);
          lastRFIDReadTime = millis();
          return;
        }

        bool canCook = canCookIngredientForCurrentRecipe(localIngredient, serverData);

        if (canCook) {
          Serial.println("Cooking started.");

          char debugMsg[LEC_PAYLOAD_LENGTH];
          snprintf(
            debugMsg,
            sizeof(debugMsg),
            "start ingredient=%s chop=%d cook=%d recipe=%s",
            localIngredient,
            serverData.chopCount,
            serverData.cookCount,
            currentRecipe.name
          );
          sendDebugLogToServer("cook-start", debugMsg);

          cooking = true;
          burntPlate = false;
          isPlayingCookingAudio = false;
          hasPlayedCookingCompleteAudio = false;

          cookState = CLIENT_PROCESSING;

          updateCookLedSection(localCookCount);

          audioModule.stop();
          audioModule.playSpecified(3);
          isPlayingCookingAudio = true;

          digitalWrite(NEOPIXEL_MOSFET_PIN, HIGH);
          lastCookTime = millis();
          lastRFIDReadTime = millis();
        } else {
          char debugMsg[LEC_PAYLOAD_LENGTH];

          snprintf(
            debugMsg,
            sizeof(debugMsg),
            "reject ingredient=%s chop=%d cook=%d recipe=%s",
            localIngredient,
            serverData.chopCount,
            serverData.cookCount,
            currentRecipeValid ? currentRecipe.name : "none"
          );

          sendDebugLogToServer("cook-reject", debugMsg);

          Serial.println("Ingredient is not allowed to cook for this recipe.");
          setStripColor(strip.Color(255, 0, 0));
          delay(1000);
          clearStrip();
        }
      } else {
        Serial.println("No RFID card detected.");
      }
    }
  } else if (cookState == CLIENT_PROCESSING) {
    if (!burntPlate && cooking) {
      if (currentTime - lastCookTime >= 1000) {
        lastCookTime = currentTime;

        char currentRfid[RFID_LENGTH] = {0};

        if (readRFID(currentRfid)) {
          if (strncmp(currentRfid, localRFID, RFID_LENGTH) == 0) {
            lastRFIDReadTime = millis();

            if (localCookCount < MAX_COOK_COUNT) {
              localCookCount++;
              updateCookLedSection(localCookCount);

              char debugMsg[LEC_PAYLOAD_LENGTH];
              snprintf(
                debugMsg,
                sizeof(debugMsg),
                "tick rfid=%s count=%d",
                localRFID,
                localCookCount
              );
              sendDebugLogToServer("cook-tick", debugMsg);

              if (localCookCount >= MAX_COOK_COUNT && !hasPlayedCookingCompleteAudio) {
                Serial.println("Plate burnt.");

                audioModule.stop();
                audioModule.playSpecified(4);

                hasPlayedCookingCompleteAudio = true;
                isPlayingCookingAudio = false;
                burntPlate = true;

                resetPacket(outgoingPacket, LEC_PKT_ON_FIRE);
                lecSetRfid(outgoingPacket, localRFID);
                sendDataToServer(&outgoingPacket);
              }
            }
          } else {
            Serial.println("Different RFID detected. Finalizing old plate.");

            sendPlateUpdate(
              localRFID,
              localIngredient,
              INVALID_COUNT,
              localCookCount,
              INVALID_COUNT,
              false,
              0,
              CLIENT_SHORT_ACK_TIMEOUT_MS
            );

            cooking = false;
            isPlayingCookingAudio = false;
            audioModule.stop();
            clearStrip();

            localCookCount = 0;
            memset(localRFID, 0, sizeof(localRFID));
            memset(localIngredient, 0, sizeof(localIngredient));

            cookState = CLIENT_IDLE;
          }
        } else if (PICC_IsAnyCardPresent()) {
          // Card is still physically present, but UID read was weak this cycle.
          // Keep cooking instead of stopping instantly.
          lastRFIDReadTime = millis();

          if (localCookCount < MAX_COOK_COUNT) {
            localCookCount++;
            updateCookLedSection(localCookCount);

            char debugMsg[LEC_PAYLOAD_LENGTH];
            snprintf(
              debugMsg,
              sizeof(debugMsg),
              "weak-read count=%d",
              localCookCount
            );
            sendDebugLogToServer("cook-tick", debugMsg);
          }
        } else if (millis() - lastRFIDReadTime > CLIENT_RFID_TIMEOUT_MS) {
          Serial.println("RFID removed. Finalizing cook count.");

          sendPlateUpdate(
            localRFID,
            localIngredient,
            INVALID_COUNT,
            localCookCount,
            INVALID_COUNT,
            false,
            0,
            CLIENT_SHORT_ACK_TIMEOUT_MS
          );

          cooking = false;
          isPlayingCookingAudio = false;
          audioModule.stop();
          clearStrip();

          localCookCount = 0;
          memset(localRFID, 0, sizeof(localRFID));
          memset(localIngredient, 0, sizeof(localIngredient));

          cookState = CLIENT_IDLE;
        }

        delay(50);
      }
    } else if (burntPlate) {
      char currentRfid[RFID_LENGTH] = {0};

      if (readRFID(currentRfid)) {
        if (strncmp(currentRfid, localRFID, RFID_LENGTH) == 0) {
          lastRFIDReadTime = millis();
        } else {
          Serial.println("Different RFID. Ending burnt scenario.");
          burntPlate = false;
          audioModule.stop();
          clearStrip();

          localCookCount = 0;
          memset(localRFID, 0, sizeof(localRFID));
          memset(localIngredient, 0, sizeof(localIngredient));

          cookState = CLIENT_IDLE;
        }
      } else if (millis() - lastRFIDReadTime > CLIENT_RFID_TIMEOUT_MS) {
        Serial.println("Burnt plate removed.");

        burntPlate = false;
        audioModule.stop();
        clearStrip();

        localCookCount = 0;
        memset(localRFID, 0, sizeof(localRFID));
        memset(localIngredient, 0, sizeof(localIngredient));

        cookState = CLIENT_IDLE;
      }

      delay(50);
    }
  }
}

/*************************************************************
  MIX ROLE
*************************************************************/

void executeMixStationClient() {
  currentTime = millis();

  if (!currentRecipeValid) {
    requestAndUpdateCurrentRecipe();

    if (!currentRecipeValid) {
      delay(100);
      return;
    }
  }

  if (isButtonPressed(stationButtonState)) {
    Serial.print("Mix Station button pressed. State: ");
    Serial.println(mixState == MIX_IDLE ? "IDLE" : "MIXING");

    blinkButtonLED();

    char scannedRfid[RFID_LENGTH] = {0};

    if (!readRFID(scannedRfid)) {
      Serial.println("No RFID card detected.");
      return;
    }

    Serial.print("RFID detected: ");
    Serial.println(scannedRfid);

    if (!requestPlateState(scannedRfid, CLIENT_RESPONSE_TIMEOUT_MS)) {
      return;
    }

    LecPacket plateData = serverData;

    char debugMsg[LEC_PAYLOAD_LENGTH];

    snprintf(
      debugMsg,
      sizeof(debugMsg),
      "recipe=%s plate=%s chop=%d cook=%d",
      currentRecipe.name,
      plateData.ingredient,
      plateData.chopCount,
      plateData.cookCount
    );

    sendDebugLogToServer("mix-plate", debugMsg);

    bool ingredientValid = false;

    for (int i = 0; i < currentRecipe.numIngredients; i++) {
      bool alreadyCollected = false;

      for (int j = 0; j < collectedIngredientCount; j++) {
        if (strcmp(collectedIngredients[j], currentRecipe.ingredients[i].name) == 0) {
          alreadyCollected = true;
          break;
        }
      }

      if (alreadyCollected) {
        continue;
      }

      snprintf(
        debugMsg,
        sizeof(debugMsg),
        "needs=%s reqChop=%d reqCook=%d",
        currentRecipe.ingredients[i].name,
        currentRecipe.ingredients[i].requiresChop,
        currentRecipe.ingredients[i].requiresCook
      );

      sendDebugLogToServer("mix-need", debugMsg);

      if (recipeRequirementMet(currentRecipe.ingredients[i], plateData)) {
        ingredientValid = true;

        strncpy(
          collectedIngredients[collectedIngredientCount],
          plateData.ingredient,
          INGREDIENT_LENGTH - 1
        );

        collectedIngredients[collectedIngredientCount][INGREDIENT_LENGTH - 1] = '\0';

        collectedIngredientCount++;

        if (collectedIngredientCount == currentRecipe.numIngredients) {
          audioModule.stop();
          audioModule.playSpecified(6);
        } else {
          audioModule.stop();
          audioModule.playSpecified(5);
        }

        int ledsPerIngredient = NUM_PIXELS / currentRecipe.numIngredients;
        int startLED = ledsPerIngredient * (collectedIngredientCount - 1);
        int endLED = startLED + ledsPerIngredient - 1;

        if (endLED >= NUM_PIXELS) {
          endLED = NUM_PIXELS - 1;
        }

        graduallyLightUpSection(startLED, endLED);

        sendPlateUpdate(
          plateData.rfid,
          "none",
          0,
          INVALID_COUNT,
          INVALID_COUNT,
          true,
          0,
          CLIENT_SHORT_ACK_TIMEOUT_MS
        );

        break;
      }
    }

    if (!ingredientValid) {
      sendDebugLogToServer("mix-rejected", "ingredient invalid or already collected");

      Serial.println("Ingredient invalid or already collected.");
      setStripColor(strip.Color(255, 0, 0));
      delay(1000);
      updateMixStationLEDs();
      return;
    }

    if (collectedIngredientCount == currentRecipe.numIngredients) {
      Serial.println("Mix complete.");

      sendDebugLogToServer("mix-accepted", plateData.ingredient);

      sendPlateUpdate(
        plateData.rfid,
        currentRecipe.name,
        0,
        INVALID_COUNT,
        INVALID_COUNT,
        false,
        0,
        CLIENT_SHORT_ACK_TIMEOUT_MS
      );

      setStripColor(strip.Color(0, 255, 0));
      delay(2000);
      clearStrip();

      mixState = MIX_IDLE;
      collectedIngredientCount = 0;
      memset(collectedIngredients, 0, sizeof(collectedIngredients));
    } else {
      mixState = MIX_MIXING;
    }
  }
}

/*************************************************************
  SERVE ROLE
*************************************************************/

void executeServeStationClient() {
  if (!currentRecipeValid) {
    requestAndUpdateCurrentRecipe();

    if (!currentRecipeValid) {
      delay(100);
      return;
    }
  }

  if (isButtonPressed(stationButtonState)) {
    Serial.println("Serve Station button pressed.");
    blinkButtonLED();

    char scannedRfid[RFID_LENGTH] = {0};

    if (!readRFID(scannedRfid)) {
      Serial.println("No RFID card detected.");
      return;
    }

    Serial.print("RFID detected: ");
    Serial.println(scannedRfid);

    if (!sendServeAttempt(scannedRfid, currentRecipe.name, CLIENT_RESPONSE_TIMEOUT_MS)) {
      return;
    }

    bool success = serverData.success || serverData.playerScoreDelta > 0;

    if (success) {
      bool serveCompletedRoundOrGame =
        strcmp(serverData.status, "serve-complete") == 0;

      Serial.println(
        serveCompletedRoundOrGame
          ? "Serve success completed round/game."
          : "Serve success."
      );

      setStripColor(strip.Color(0, 255, 0));
      audioModule.stop();

      if (serveCompletedRoundOrGame) {
        // Do not play +time video when this serve ends the game/round.
        // Show check-your-score / completion screen instead.
        sendSprite(0x0F);
      } else {
        audioModule.playSpecified(7);
        sendSprite(0x14);
      }

      delay(2000);
      clearStrip();
    }
    else {
      Serial.println("Serve failed.");

      setStripColor(strip.Color(255, 0, 0));
      audioModule.stop();
      audioModule.playSpecified(8);
      sendSprite(0x15);
      delay(2000);

      if (currentRecipeValid) {
        sendSprite(getVideoID(currentRecipe.name));
      }

      clearStrip();
    }
  }
}

/*************************************************************
  SPRITE / VIDEO
*************************************************************/

uint8_t getVideoID(const char* recipeName) {
  if (strcmp(recipeName, "garden salad") == 0) return 0x05;
  else if (strcmp(recipeName, "apple salad") == 0) return 0x06;
  else if (strcmp(recipeName, "cheese salad") == 0) return 0x07;
  else if (strcmp(recipeName, "tomatoes and cheese") == 0) return 0x0C;
  else if (strcmp(recipeName, "apples and cheese") == 0) return 0x08;
  else if (strcmp(recipeName, "tomato pasta with cheese") == 0) return 0x09;
  else if (strcmp(recipeName, "taco with cheese") == 0) return 0x11;
  else if (strcmp(recipeName, "taco with lettuce") == 0) return 0x0D;
  else if (strcmp(recipeName, "taco with tomato") == 0) return 0x0E;
  else if (strcmp(recipeName, "beef stew with cheese") == 0) return 0x17;
  else if (strcmp(recipeName, "beef patty") == 0) return 0x18;
  else if (strcmp(recipeName, "tomato pizza") == 0) return 0x19;
  else if (strcmp(recipeName, "baked mac and cheese") == 0) return 0x1A;
  else if (strcmp(recipeName, "apple pie") == 0) return 0x10;

  return 0x00;
}

/*************************************************************
  SEND SPRITES TWICE
*************************************************************/

void sendSprite(uint8_t cmd) {
  for (int i = 0; i < 2; i++) {
    spriteSerial.write(cmd);
    delay(40);
  }

  spriteSerial.flush();
}

      if (currentRecipeValid) {
        sendSprite(getVideoID(currentRecipe.name));
      }

      clearStrip();
    }
  }
}

/*************************************************************
  SPRITE / VIDEO
*************************************************************/

uint8_t getVideoID(const char* recipeName) {
  if (strcmp(recipeName, "garden salad") == 0) return 0x05;
  else if (strcmp(recipeName, "apple salad") == 0) return 0x06;
  else if (strcmp(recipeName, "cheese salad") == 0) return 0x07;
  else if (strcmp(recipeName, "tomatoes and cheese") == 0) return 0x0C;
  else if (strcmp(recipeName, "apples and cheese") == 0) return 0x08;
  else if (strcmp(recipeName, "tomato pasta with cheese") == 0) return 0x09;
  else if (strcmp(recipeName, "taco with cheese") == 0) return 0x11;
  else if (strcmp(recipeName, "taco with lettuce") == 0) return 0x0D;
  else if (strcmp(recipeName, "taco with tomato") == 0) return 0x0E;
  else if (strcmp(recipeName, "beef stew with cheese") == 0) return 0x17;
  else if (strcmp(recipeName, "beef patty") == 0) return 0x18;
  else if (strcmp(recipeName, "tomato pizza") == 0) return 0x19;
  else if (strcmp(recipeName, "baked mac and cheese") == 0) return 0x1A;
  else if (strcmp(recipeName, "apple pie") == 0) return 0x10;

  return 0x00;
}

/*************************************************************
  SEND SPRITES TWICE
*************************************************************/

void sendSprite(uint8_t cmd) {
  for (int i = 0; i < 2; i++) {
    spriteSerial.write(cmd);
    delay(40);
  }

  spriteSerial.flush();
}