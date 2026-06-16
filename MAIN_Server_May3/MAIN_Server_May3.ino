/*************************************************************
  Let Em Cook - Server Code Refactored

  Architecture:
  - Uses LetEmCookShared
  - Uses LecPacket packetType protocol
  - No legacy requestType strings
  - No garbage station
  - No oven/bake station
  - 4 generic clients + 1 ingredient station
  - Round 1: Chop, Chop, Mix, Serve
  - Round 2: Chop, Cook, Mix, Serve
  - Ingredient station owns overwriting/resetting plate ingredient
  - Raspberry Pi display remains separate through HTTP
*************************************************************/

#define LEC_DEBUG 0

#include <LetEmCook.h>

#include <esp_now.h>
#include <WiFi.h>
#include <MFRC522.h>
#include <SPI.h>
#include <DYPlayerArduino.h>
#include <HTTPClient.h>

extern "C" {
  #include "esp_wifi.h"
}

/*************************************************************
  WIFI / SCOREBOARD CONFIG
*************************************************************/

const char* scoreboardSSID = "MyScoreboardAP";
const char* scoreboardPassword = "MySecretPassword";

const char* SCOREBOARD_START_URL  = "http://10.42.0.1:5000/start_game";
const char* SCOREBOARD_END_URL    = "http://10.42.0.1:5000/end_game";
const char* SCOREBOARD_UPDATE_URL = "http://10.42.0.1:5000/update_score";

const char* STATS_RESULT_URL = "http://10.42.0.1:5001/api/game-result";

/*************************************************************
  ASYNC PI SCOREBOARD QUEUE
*************************************************************/

enum PiJobType : uint8_t {
  PI_JOB_START,
  PI_JOB_UPDATE,
  PI_JOB_END,
  PI_JOB_STATS
};

struct PiJob {
  PiJobType type;

  int score;
  int timeLeftSec;

  unsigned long durationSec;
  int finalScore;

  char reason[80];
  char currentRecipe[MAX_RECIPE_NAME_LENGTH];
  char nextRecipe[MAX_RECIPE_NAME_LENGTH];

  char statsJson[1600];
};

QueueHandle_t piQueue = nullptr;
TaskHandle_t piTaskHandle = nullptr;

unsigned long lastPiUpdateQueuedAt = 0;
const unsigned long PI_UPDATE_QUEUE_INTERVAL_MS = 750;
const uint16_t PI_HTTP_TIMEOUT_MS = 500;

bool connectedToScoreboard = false;

// Scoreboard reconnect settings.
// Keep attempts short so the game can run even if the Pi is off/unplugged
unsigned long lastScoreboardReconnectAttemptMs = 0;
const unsigned long SCOREBOARD_RECONNECT_INTERVAL_MS = 5000UL;

const unsigned long SCOREBOARD_CONNECT_TIMEOUT_IDLE_MS = 3000UL;
const unsigned long SCOREBOARD_CONNECT_TIMEOUT_GAME_MS = 300UL;

bool piStartSentForCurrentGame = false;

bool pendingPiStartGame = false;
unsigned long pendingPiStartDurationSec = 0;


/*************************************************************
  ASYNC INCOMING ESP-NOW PACKET QUEUE
*************************************************************/

struct IncomingPacketJob {
  uint8_t mac[6];
  LecPacket packet;
};

QueueHandle_t incomingPacketQueue = nullptr;

/*************************************************************
  PIN DEFINITIONS
*************************************************************/

#define SS_PIN 21
#define RST_PIN 22

#define GREEN_BUTTON_PIN 13
#define RED_BUTTON_PIN   4
#define FIRE_BUTTON_PIN  35

#define GREEN_BUTTON_LED_PIN 27
#define RED_BUTTON_LED_PIN   26
#define FIRE_LED_PIN         25

#define AUDIO_TX 16
#define AUDIO_RX 17

/*************************************************************
  HARDWARE
*************************************************************/

MFRC522 rfid(SS_PIN, RST_PIN);

HardwareSerial audioSerial(2);
DY::Player audioModule(&audioSerial);

/*************************************************************
  SERVER CONFIG
*************************************************************/

#define SERVER_MAX_NODES 12
#define ROUND_TRANSITION_DELAY_MS 2500
#define ROLE_RECIPE_DELAY_MS 2000

#define ROUND_1_DURATION_MS 120000UL
#define ROUND_2_DURATION_MS 120000UL

const int starThresholds[3] = {2, 4, 6};
const int starMaxScore = 6;

const int SCORE_PER_SUCCESSFUL_SERVE = 1;
const int ROUND_1_COMPLETE_BONUS = 1;
const int GAME_COMPLETE_BONUS = 2;

#define ROUND_1_COUNTDOWN_DELAY_MS 6500UL

#define ROLE_ASSIGNMENT_SEND_COUNT 3
#define ROLE_ASSIGNMENT_RETRY_INTERVAL_MS 700UL
#define SERVER_ROLE_TRANSITION_DELAY_MS 4700UL

int roleAssignmentSendsRemaining = 0;

bool pendingRoleAssignmentBroadcast = false;
unsigned long roleAssignmentBroadcastAt = 0;

/*************************************************************
  NODE ROSTER
*************************************************************/

struct ServerNode {
  bool used;
  bool online;

  uint8_t mac[6];

  LecDeviceClass deviceClass;
  LecRole assignedRole;
  LecRunMode runMode;

  char claimedId[LEC_CLAIMED_ID_LENGTH];
  char firmwareVersion[LEC_FW_VERSION_LENGTH];
  char status[LEC_STATUS_LENGTH];

  unsigned long lastSeenMs;
  uint32_t lastSequence;
};

ServerNode nodes[SERVER_MAX_NODES];

/*************************************************************
  PLATES
*************************************************************/

LecPlateState plates[MAX_PLATES];
int plateCount = 0;

/*************************************************************
  GAME STATE
*************************************************************/

SemaphoreHandle_t xMutex;

bool gameRunning = false;
bool onFire = false;

LecRound currentRound = LEC_ROUND_NONE;

int playerScore = 0;
int correctServesInRound = 0;

unsigned long roundStartTime = 0;
unsigned long roundDuration = 0;

bool fifteenSecondWarningPlayed = false;

LecRecipe currentRecipe;
LecRecipe nextRecipe;
bool currentRecipeValid = false;
bool nextRecipeValid = false;

bool pendingRoundTransition = false;
LecRound pendingNextRound = LEC_ROUND_NONE;
unsigned long roundTransitionAt = 0;

bool pendingRecipeBroadcast = false;
unsigned long recipeBroadcastAt = 0;

/*************************************************************
  GAME STATS TRACKING
*************************************************************/

struct LecRoundStats {
  bool played;
  unsigned long startedAtMs;
  unsigned long endedAtMs;
  unsigned long durationMs;
  int targetServes;
  int completedServes;
  bool completed;
};

unsigned long statsGameStartedAtMs = 0;
unsigned long statsGameEndedAtMs = 0;

LecRoundStats statsRound1;
LecRoundStats statsRound2;

int statsFireCount = 0;

/*************************************************************
  BUTTON STATE
*************************************************************/

unsigned long greenButtonPressStartTime = 0;
bool greenButtonLongPressHandled = false;

unsigned long redButtonPressStartTime = 0;
bool redButtonLongPressHandled = false;

unsigned long fireButtonPressStartTime = 0;

unsigned long lastFireBlinkTime = 0;
bool fireLedState = LOW;

const unsigned long START_BUTTON_BLINK_INTERVAL_MS = 500;
unsigned long lastStartButtonBlinkTime = 0;
bool startButtonLedState = LOW;

/*************************************************************
  FUNCTION PROTOTYPES
*************************************************************/

void connectToScoreboardWithTimeout();
bool tryConnectToScoreboardOnce(unsigned long timeoutMs);
void processScoreboardReconnect();
void processPendingPiStartGame();

void initializeHardware();
void initializeEspNow();

void initializeAsyncQueues();

void processIncomingPackets();

void startPiScoreboardTask();
void piScoreboardTask(void* parameter);

void queuePiStartGame(unsigned long durationSec);
void queuePiEndGame(const String& reason, int finalScore);
void queuePiUpdate(int score, int timeLeftSec);

void performPiStartGame(const PiJob& job);
void performPiEndGame(const PiJob& job);
void performPiUpdate(const PiJob& job);

LecRoundStats* getStatsForRound(LecRound round);
void resetStatsTracking();
void beginStatsGame();
void beginStatsRound(LecRound round);
void finalizeStatsRound(LecRound round, bool completed);
void finalizeStatsGame();

String jsonEscape(const String& value);
String buildStatsPayload(const String& reason, int finalScore);
void queuePiStatsPost(const String& payload);
void performPiStatsPost(const PiJob& job);

void handleSerialCommands();
void handleButtons();
void handleGreenButton();
void handleRedButton();
void handleFireButton();
void processStartButtonBlink();

void startGame();
void startRound(LecRound round);
void endCurrentRound();
void endGame(const String& reason);
void resetGameState();

void assignRolesForCurrentRound();
void sendRoleAssignmentToNode(ServerNode& node);
void broadcastStartRound(LecRound round);
void broadcastEndGame();
void broadcastReinitialize();
void broadcastFireState(bool fireActive);
void broadcastCurrentRecipe();
void sendRecipeToNode(ServerNode& node);
void scheduleRoleAssignmentBroadcast(unsigned long delayMs);

void pickRecipeForRound(LecRound round, LecRecipe& outRecipe, bool& valid);
bool isRecipeAllowedForRound(const LecRecipe& recipe, LecRound round);
int getServeTargetForRound(LecRound round);

void processRoundTransition();
void processPendingRoleAssignmentBroadcast();
void broadcastRoleAssignments();
void processPendingRecipeBroadcast();
void processCountdownAndScoreboard();

void handleIncomingPacket(const uint8_t* mac, const LecPacket& packet);
void handleHelloPacket(const uint8_t* mac, const LecPacket& packet);
void handlePlateLookup(const uint8_t* mac, const LecPacket& packet);
void handlePlateUpdate(const uint8_t* mac, const LecPacket& packet);
void handleRecipeRequest(const uint8_t* mac, const LecPacket& packet);
void handleServeAttempt(const uint8_t* mac, const LecPacket& packet);
void handleFireRequest(const uint8_t* mac, const LecPacket& packet);
void handleDebugLog(const uint8_t* mac, const LecPacket& packet);

void sendPacketToMac(const uint8_t* mac, LecPacket& packet);
void sendPlateStateToMac(const uint8_t* mac, const LecPlateState& plate, LecPacketResult result);
void sendAckToMac(const uint8_t* mac, LecPacketType type, LecPacketResult result, const char* status);

int findNodeByMac(const uint8_t* mac);
int upsertNode(const uint8_t* mac, const LecPacket& packet);
bool addPeerIfNeeded(const uint8_t* mac);
int countGenericClients();
void collectGenericClientIndexes(int* indexes, int& count);
void shuffleIndexes(int* indexes, int count);

int findPlateIndex(const char* rfid);
int findOrCreatePlate(const char* rfid);
void initializePlates();
void resetAllPlates();

bool readRFID(char* rfidUID);
bool PICC_IsAnyCardPresent();
void resetRFIDReader();

int getTimeLeftInSeconds();
void notifyPiStartGame(unsigned long durationSec);
void notifyPiEndGame(const String& reason, int finalScore);
void updateScoreAndTimeOnPi(int score, int timeLeftSec);
String getStarThresholdString();

void blinkButtonLED(int ledPin);

void pickDifferentRecipeForRound(
  LecRound round,
  const char* avoidRecipeName,
  LecRecipe& outRecipe,
  bool& valid
);

void printOperatorHelp();

void handleMaintenanceResultPacket(const uint8_t* mac, const LecPacket& packet);

void sendMaintenancePacketToNode(
  ServerNode& node,
  LecPacketType packetType,
  const char* status,
  const char* payload
);

void broadcastMaintenancePacket(
  LecPacketType packetType,
  const char* status,
  const char* payload,
  LecDeviceClass deviceClassFilter = LEC_DEVICE_UNKNOWN
);

ServerNode* findNodeByClaimedId(const char* claimedId);

void sendMaintenancePacketToTarget(
  const char* target,
  LecPacketType packetType,
  const char* status,
  const char* payload
);

String getArgToken(const String& text, int index);
void handleNetSetCommand(const String& rawCommand);
void handleNetPushCommand(const String& rawCommand);
void handleUpdateCommand(const String& rawCommand);
void handleMaintCommand(const String& rawCommand);
void handleRebootCommand(const String& rawCommand);

/*************************************************************
  ESP-NOW CALLBACKS
*************************************************************/

void onDataSent(const wifi_tx_info_t* tx_info, esp_now_send_status_t status) {
#if LEC_DEBUG
  Serial.print("Send status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAILED");
#endif
}

void onDataRecv(const esp_now_recv_info_t* info, const uint8_t* incomingData, int len) {
  if (len != sizeof(LecPacket)) {
#if LEC_DEBUG
    Serial.println("Received packet size mismatch.");
#endif
    return;
  }

  if (incomingPacketQueue == nullptr) {
    return;
  }

  IncomingPacketJob job;
  memcpy(job.mac, info->src_addr, 6);
  memcpy(&job.packet, incomingData, sizeof(LecPacket));

  BaseType_t result = xQueueSend(incomingPacketQueue, &job, 0);

#if LEC_DEBUG
  if (result != pdTRUE) {
    Serial.println("Incoming packet queue full. Packet dropped.");
  }
#endif
}

/*************************************************************
  SETUP
*************************************************************/

void setup() {
  Serial.begin(115200);
  delay(200);

  Serial.println();
  Serial.println("Booting Let Em Cook Server...");

  WiFi.mode(WIFI_STA);
  delay(100);

  Serial.print("Server MAC Address: ");
  Serial.println(WiFi.macAddress());

  connectToScoreboardWithTimeout();

  initializeAsyncQueues();
  startPiScoreboardTask();

  SPI.begin();
  rfid.PCD_Init();

  esp_wifi_set_channel(LEC_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

  xMutex = xSemaphoreCreateMutex();

  if (xMutex == NULL) {
    Serial.println("Failed to create mutex.");
    return;
  }

  initializeHardware();
  initializeEspNow();
  initializePlates();

  currentRound = LEC_ROUND_NONE;

  Serial.println("Server setup complete.");
  Serial.println("Serial commands: start, end, reset, list, roles, recipe, fire, ext");
}

/*************************************************************
  LOOP
*************************************************************/

void loop() {
  processIncomingPackets();

  processScoreboardReconnect();
  processPendingPiStartGame();

  handleSerialCommands();
  handleButtons();
  processStartButtonBlink();

  processRoundTransition();

  processPendingRoleAssignmentBroadcast();

  processPendingRecipeBroadcast();
  processCountdownAndScoreboard();

  if (gameRunning && roundDuration > 0) {
    if (millis() - roundStartTime >= roundDuration) {
      endCurrentRound();
    }
  }

  delay(5);
}

/*************************************************************
  INIT
*************************************************************/

bool tryConnectToScoreboardOnce(unsigned long timeoutMs) {
  WiFi.mode(WIFI_STA);
  delay(50);

  WiFi.begin(scoreboardSSID, scoreboardPassword, LEC_ESPNOW_CHANNEL);

  unsigned long start = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(100);
  }

  if (WiFi.status() == WL_CONNECTED) {
    connectedToScoreboard = true;

    Serial.println("Connected to scoreboard AP.");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
    Serial.print("WiFi Channel: ");
    Serial.println(WiFi.channel());

    if (WiFi.channel() != LEC_ESPNOW_CHANNEL) {
      Serial.println("WARNING: Scoreboard AP channel does not match LEC_ESPNOW_CHANNEL.");
      Serial.println("ESP-NOW clients may not communicate correctly.");
    }

    return true;
  }

  connectedToScoreboard = false;

  // Important: return to STA mode and force ESP-NOW channel after failed WiFi attempt.
  WiFi.disconnect(false);
  WiFi.mode(WIFI_STA);
  delay(50);
  esp_wifi_set_channel(LEC_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

  return false;
}

void connectToScoreboardWithTimeout() {
  Serial.println("Connecting to scoreboard AP...");

  if (!tryConnectToScoreboardOnce(45000UL)) {
    Serial.println("Scoreboard AP unavailable at boot. Continuing with ESP-NOW only.");
    Serial.println("Will keep retrying scoreboard connection in background.");
  }
}

void processScoreboardReconnect() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!connectedToScoreboard) {
      connectedToScoreboard = true;

      Serial.println("Scoreboard reconnected.");

      if (gameRunning) {
        Serial.println("Resyncing scoreboard after reconnect.");

        pendingPiStartDurationSec = getTimeLeftInSeconds();
        pendingPiStartGame = true;
        lastPiUpdateQueuedAt = 0;

        processPendingPiStartGame();
      }
    }

    return;
  }

  if (connectedToScoreboard) {
    connectedToScoreboard = false;
    Serial.println("Scoreboard WiFi lost. Will retry.");
  }

  // Avoid reconnect attempts during sensitive role/recipe/round transitions.
  if (pendingRoleAssignmentBroadcast || pendingRecipeBroadcast || pendingRoundTransition) {
    return;
  }

  unsigned long now = millis();

  if (now - lastScoreboardReconnectAttemptMs < SCOREBOARD_RECONNECT_INTERVAL_MS) {
    return;
  }

  lastScoreboardReconnectAttemptMs = now;

  Serial.println("Attempting scoreboard reconnect...");

  unsigned long timeoutMs = gameRunning
    ? SCOREBOARD_CONNECT_TIMEOUT_GAME_MS
    : SCOREBOARD_CONNECT_TIMEOUT_IDLE_MS;

  bool reconnected = tryConnectToScoreboardOnce(timeoutMs);
  
  if (reconnected && gameRunning) {
    Serial.println("Scoreboard reconnected during active game. Sending state.");

    pendingPiStartDurationSec = getTimeLeftInSeconds();
    pendingPiStartGame = true;
    lastPiUpdateQueuedAt = 0;

    processPendingPiStartGame();
  }
}

void processPendingPiStartGame() {
  if (!pendingPiStartGame) {
    return;
  }

  if (!connectedToScoreboard || WiFi.status() != WL_CONNECTED || piQueue == nullptr) {
    return;
  }

  notifyPiStartGame(pendingPiStartDurationSec);
  piStartSentForCurrentGame = true;
  pendingPiStartGame = false;

  lastPiUpdateQueuedAt = 0;
  updateScoreAndTimeOnPi(playerScore, getTimeLeftInSeconds());

  Serial.println("Pending scoreboard start_game sent.");
}

void initializeHardware() {
  pinMode(GREEN_BUTTON_PIN, INPUT_PULLUP);
  pinMode(RED_BUTTON_PIN, INPUT_PULLUP);

  // GPIO35 is input-only and has no internal pullup/pulldown.
  // Use external resistor wiring for this button.
  pinMode(FIRE_BUTTON_PIN, INPUT);

  pinMode(GREEN_BUTTON_LED_PIN, OUTPUT);
  pinMode(RED_BUTTON_LED_PIN, OUTPUT);
  pinMode(FIRE_LED_PIN, OUTPUT);

  digitalWrite(GREEN_BUTTON_LED_PIN, LOW);
  digitalWrite(RED_BUTTON_LED_PIN, LOW);
  digitalWrite(FIRE_LED_PIN, LOW);

  audioSerial.begin(9600, SERIAL_8N1, AUDIO_RX, AUDIO_TX);
  audioModule.begin();
  audioModule.setCycleMode(DY::PlayMode::OneOff);
  audioModule.setVolume(24);
  audioModule.stop();

  Serial.println("Hardware initialized.");
}

void initializeEspNow() {
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW.");
    return;
  }

  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataRecv);

  Serial.println("ESP-NOW initialized.");
}

void initializeAsyncQueues() {
  incomingPacketQueue = xQueueCreate(20, sizeof(IncomingPacketJob));

  if (incomingPacketQueue == nullptr) {
    Serial.println("Failed to create incoming ESP-NOW packet queue.");
  } else {
    Serial.println("Incoming ESP-NOW packet queue created.");
  }
}

void processIncomingPackets() {
  if (incomingPacketQueue == nullptr) {
    return;
  }

  IncomingPacketJob job;

  while (xQueueReceive(incomingPacketQueue, &job, 0) == pdTRUE) {
    if (!addPeerIfNeeded(job.mac)) {
#if LEC_DEBUG
      Serial.println("Could not add sender as peer.");
#endif
      continue;
    }

    if (!lecIsValidPacket(job.packet, sizeof(LecPacket))) {
#if LEC_DEBUG
      Serial.println("Invalid Let Em Cook packet.");
#endif
      continue;
    }

#if LEC_DEBUG
    if (job.packet.packetType != LEC_PKT_HELLO &&
        job.packet.packetType != LEC_PKT_NODE_STATUS) {
      Serial.println("=== Server Received Packet ===");
      Serial.print("From: ");
      Serial.println(lecMacToString(job.mac));
      Serial.print("Packet Type: ");
      Serial.println(static_cast<int>(job.packet.packetType));
      Serial.print("Device Class: ");
      Serial.println(static_cast<int>(job.packet.deviceClass));
      Serial.print("Role: ");
      Serial.println(static_cast<int>(job.packet.role));
      Serial.print("RFID: ");
      Serial.println(job.packet.rfid);
      Serial.print("Ingredient: ");
      Serial.println(job.packet.ingredient);
      Serial.print("Recipe: ");
      Serial.println(job.packet.recipeName);
      Serial.println("==============================");
    }
#endif

    handleIncomingPacket(job.mac, job.packet);
  }
}

/*************************************************************
  SERIAL / BUTTONS
*************************************************************/

void handleSerialCommands() {
  if (!Serial.available()) {
    return;
  }

  String rawCmd = Serial.readStringUntil('\n');
  rawCmd.trim();

  if (rawCmd.length() == 0) {
    return;
  }

  // CHANGED: Keep rawCmd untouched so SSID/password/URL case is preserved.
  // Only use lowerCmd for command detection.
  String lowerCmd = rawCmd;
  lowerCmd.toLowerCase();

  if (lowerCmd == "help") {
    printOperatorHelp();

  } else if (lowerCmd == "start") {
    startGame();

  } else if (lowerCmd == "end") {
    endGame("Serial Command");

  } else if (lowerCmd == "reset") {
    resetGameState();
    broadcastReinitialize();

  } else if (lowerCmd == "list") {
    Serial.println("=== Nodes ===");

    for (int i = 0; i < SERVER_MAX_NODES; i++) {
      if (!nodes[i].used) continue;

      Serial.print(i);
      Serial.print(" | ");
      Serial.print(lecMacToString(nodes[i].mac));
      Serial.print(" | claimed=");
      Serial.print(nodes[i].claimedId);
      Serial.print(" | class=");
      Serial.print(static_cast<int>(nodes[i].deviceClass));
      Serial.print(" | role=");
      Serial.print(static_cast<int>(nodes[i].assignedRole));
      Serial.print(" | mode=");
      Serial.print(static_cast<int>(nodes[i].runMode));
      Serial.print(" | fw=");
      Serial.print(nodes[i].firmwareVersion);
      Serial.print(" | status=");
      Serial.print(nodes[i].status);
      Serial.print(" | lastSeen=");
      Serial.println(nodes[i].lastSeenMs);
    }

  } else if (lowerCmd == "roles") {
    assignRolesForCurrentRound();

    for (int i = 0; i < SERVER_MAX_NODES; i++) {
      if (nodes[i].used) {
        sendRoleAssignmentToNode(nodes[i]);
      }
    }

  } else if (lowerCmd == "recipe") {
    pickRecipeForRound(currentRound, currentRecipe, currentRecipeValid);

    pickDifferentRecipeForRound(
      currentRound,
      currentRecipeValid ? currentRecipe.name : "",
      nextRecipe,
      nextRecipeValid
    );

    broadcastCurrentRecipe();
  } else if (lowerCmd == "fire") {

    if (gameRunning && !onFire) {
      statsFireCount++;
    }

    onFire = true;
    digitalWrite(FIRE_LED_PIN, HIGH);
    broadcastFireState(true);

  } else if (lowerCmd == "ext") {
    onFire = false;
    digitalWrite(FIRE_LED_PIN, LOW);

    broadcastFireState(false);

    pendingRecipeBroadcast = false;

  } else if (lowerCmd.startsWith("net set ")) {
    handleNetSetCommand(rawCmd);

  } else if (lowerCmd.startsWith("net push")) {
    handleNetPushCommand(rawCmd);

  } else if (lowerCmd.startsWith("update ")) {
    handleUpdateCommand(rawCmd);

  } else if (lowerCmd.startsWith("maint ")) {
    handleMaintCommand(rawCmd);

  } else if (lowerCmd.startsWith("reboot ")) {
    handleRebootCommand(rawCmd);

  } else {
    Serial.println("Unknown command. Type: help");
  }
}

void handleButtons() {
  handleGreenButton();
  handleRedButton();
  handleFireButton();
}

void processStartButtonBlink() {
  if (gameRunning) {
    startButtonLedState = LOW;
    digitalWrite(GREEN_BUTTON_LED_PIN, LOW);
    return;
  }

  unsigned long now = millis();

  if (now - lastStartButtonBlinkTime >= START_BUTTON_BLINK_INTERVAL_MS) {
    lastStartButtonBlinkTime = now;
    startButtonLedState = !startButtonLedState;
    digitalWrite(GREEN_BUTTON_LED_PIN, startButtonLedState);
  }
}

void handleGreenButton() {
  if (digitalRead(GREEN_BUTTON_PIN) == LOW) {
    if (greenButtonPressStartTime == 0) {
      greenButtonPressStartTime = millis();
      greenButtonLongPressHandled = false;
    }
  } else {
    if (greenButtonPressStartTime != 0) {
      unsigned long pressDuration = millis() - greenButtonPressStartTime;
  
      if (!greenButtonLongPressHandled && pressDuration < LEC_LONG_PRESS_DURATION_MS) {
        blinkButtonLED(GREEN_BUTTON_LED_PIN);

        if (!gameRunning) {
          startGame();
        } 

      greenButtonPressStartTime = 0;
      greenButtonLongPressHandled = false;
    }
  }
}
}

void handleRedButton() {
  if (digitalRead(RED_BUTTON_PIN) == LOW) {
    if (redButtonPressStartTime == 0) {
      redButtonPressStartTime = millis();
      redButtonLongPressHandled = false;
    }
  } else {
    if (redButtonPressStartTime != 0) {
      unsigned long pressDuration = millis() - redButtonPressStartTime;

      if (!redButtonLongPressHandled && pressDuration < LEC_LONG_PRESS_DURATION_MS) {
        blinkButtonLED(RED_BUTTON_LED_PIN);

        if (gameRunning) {
          endGame("Red Button Press");
        }
      }

      redButtonPressStartTime = 0;
      redButtonLongPressHandled = false;
    }
  }
}

void handleFireButton() {
  // Fire extinguisher button should do absolutely nothing
  // unless the game is currently running and there is an active fire.
  if (!gameRunning || !onFire) {
    fireButtonPressStartTime = 0;
    digitalWrite(FIRE_LED_PIN, LOW);
    return;
  }

  if (millis() - lastFireBlinkTime >= 500) {
    lastFireBlinkTime = millis();
    fireLedState = !fireLedState;
    digitalWrite(FIRE_LED_PIN, fireLedState);
  }

  if (digitalRead(FIRE_BUTTON_PIN) == LOW) {
    if (fireButtonPressStartTime == 0) {
      fireButtonPressStartTime = millis();
    }
  } else {
    if (fireButtonPressStartTime != 0) {
      unsigned long duration = millis() - fireButtonPressStartTime;

      if (duration >= LEC_DEFAULT_DEBOUNCE_MS) {
        Serial.println("Fire extinguished by button.");

        onFire = false;
        fireButtonPressStartTime = 0;
        digitalWrite(FIRE_LED_PIN, LOW);

        broadcastFireState(false);

        // Do not refresh recipe here.
        pendingRecipeBroadcast = false;
      }

      fireButtonPressStartTime = 0;
    }
  }
}

void handleDebugLog(const uint8_t* mac, const LecPacket& packet) {
#if LEC_DEBUG
  Serial.println("=== CLIENT DEBUG LOG ===");
  Serial.print("From: ");
  Serial.println(lecMacToString(mac));
  Serial.print("Role: ");
  Serial.println(static_cast<int>(packet.role));
  Serial.print("Round: ");
  Serial.println(static_cast<int>(packet.round));
  Serial.print("Status: ");
  Serial.println(packet.status);
  Serial.print("Payload: ");
  Serial.println(packet.payload);
  Serial.print("RFID: ");
  Serial.println(packet.rfid);
  Serial.print("Ingredient: ");
  Serial.println(packet.ingredient);
  Serial.print("Recipe: ");
  Serial.println(packet.recipeName);
  Serial.print("Chop: ");
  Serial.println(packet.chopCount);
  Serial.print("Cook: ");
  Serial.println(packet.cookCount);
  Serial.println("========================");
#endif
}

LecRoundStats* getStatsForRound(LecRound round) {
  if (round == LEC_ROUND_1) {
    return &statsRound1;
  }

  if (round == LEC_ROUND_2) {
    return &statsRound2;
  }

  return nullptr;
}

void resetStatsTracking() {
  statsGameStartedAtMs = 0;
  statsGameEndedAtMs = 0;
  statsFireCount = 0;

  statsRound1 = {
    false,
    0,
    0,
    0,
    LEC_ROUND_1_SERVE_TARGET,
    0,
    false
  };

  statsRound2 = {
    false,
    0,
    0,
    0,
    LEC_ROUND_2_SERVE_TARGET,
    0,
    false
  };
}

void beginStatsGame() {
  resetStatsTracking();
  statsGameStartedAtMs = millis();

  Serial.println("Stats tracking started for new game.");
}

void beginStatsRound(LecRound round) {
  LecRoundStats* stats = getStatsForRound(round);

  if (stats == nullptr) {
    return;
  }

  stats->played = true;
  stats->startedAtMs = millis();
  stats->endedAtMs = 0;
  stats->durationMs = 0;
  stats->completedServes = 0;
  stats->completed = false;

  if (round == LEC_ROUND_1) {
    stats->targetServes = LEC_ROUND_1_SERVE_TARGET;
  } else if (round == LEC_ROUND_2) {
    stats->targetServes = LEC_ROUND_2_SERVE_TARGET;
  }

  Serial.print("Stats tracking started for round ");
  Serial.println(static_cast<int>(round));
}

void finalizeStatsRound(LecRound round, bool completed) {
  LecRoundStats* stats = getStatsForRound(round);

  if (stats == nullptr || !stats->played) {
    return;
  }

  if (stats->endedAtMs > 0) {
    return;
  }

  stats->endedAtMs = millis();

  if (stats->startedAtMs > 0) {
    stats->durationMs = stats->endedAtMs - stats->startedAtMs;
  }

  stats->completedServes = correctServesInRound;
  stats->completed = completed;

  Serial.print("Stats finalized for round ");
  Serial.print(static_cast<int>(round));
  Serial.print(" | completed: ");
  Serial.print(completed ? "true" : "false");
  Serial.print(" | duration ms: ");
  Serial.println(stats->durationMs);
}

void finalizeStatsGame() {
  statsGameEndedAtMs = millis();

  Serial.print("Stats finalized for game. Duration ms: ");

  if (statsGameStartedAtMs > 0) {
    Serial.println(statsGameEndedAtMs - statsGameStartedAtMs);
  } else {
    Serial.println(0);
  }
}

/*************************************************************
  GAME FLOW
*************************************************************/

void startGame() {
  if (gameRunning) {
    Serial.println("Game already running.");
    return;
  }

  startButtonLedState = LOW;
  digitalWrite(GREEN_BUTTON_LED_PIN, LOW);

  resetAllPlates();

  playerScore = 0;
  correctServesInRound = 0;
  fifteenSecondWarningPlayed = false;
  pendingRoundTransition = false;
  pendingRecipeBroadcast = false;

  pendingRoleAssignmentBroadcast = false;

  roleAssignmentSendsRemaining = 0;

  piStartSentForCurrentGame = false;
  pendingPiStartGame = false;
  pendingPiStartDurationSec = 0;

  beginStatsGame();

  startRound(LEC_ROUND_1);
}

void startRound(LecRound round) {
  currentRound = round;
  correctServesInRound = 0;
  gameRunning = true;
  onFire = false;
  digitalWrite(FIRE_LED_PIN, LOW);

  if (round == LEC_ROUND_1) {
    roundDuration = ROUND_1_DURATION_MS;
  } else if (round == LEC_ROUND_2) {
    roundDuration = ROUND_2_DURATION_MS;
  } else {
    endGame("Invalid Round");
    return;
  }

  roundStartTime = millis();

  beginStatsRound(round);

  Serial.print("Starting round: ");
  Serial.println(static_cast<int>(currentRound));

  broadcastStartRound(currentRound);

  assignRolesForCurrentRound();

  pickRecipeForRound(currentRound, currentRecipe, currentRecipeValid);

  pickDifferentRecipeForRound(
    currentRound,
    currentRecipeValid ? currentRecipe.name : "",
    nextRecipe,
    nextRecipeValid
  );

  if (currentRound == LEC_ROUND_1) {
    scheduleRoleAssignmentBroadcast(ROUND_1_COUNTDOWN_DELAY_MS);

    pendingRecipeBroadcast = false;

    pendingPiStartDurationSec = roundDuration / 1000;
    pendingPiStartGame = true;

    processPendingPiStartGame();
  } else {
    scheduleRoleAssignmentBroadcast(0);

    pendingRecipeBroadcast = false;
  }

  updateScoreAndTimeOnPi(playerScore, getTimeLeftInSeconds());

  audioModule.stop();
  audioModule.playSpecified(currentRound == LEC_ROUND_1 ? 1 : 2);
}

void endCurrentRound() {
  Serial.print("Ending round: ");
  Serial.println(static_cast<int>(currentRound));

  if (correctServesInRound < getServeTargetForRound(currentRound)) {
    finalizeStatsRound(currentRound, false);

    if (currentRound == LEC_ROUND_1) {
      endGame("not enough round 1 orders served");
    } else if (currentRound == LEC_ROUND_2) {
      endGame("not enough round 2 orders served");
    }

    return;
  }

  finalizeStatsRound(currentRound, true);

  if (currentRound == LEC_ROUND_1) {
    pendingRoundTransition = true;
    pendingNextRound = LEC_ROUND_2;
    roundTransitionAt = millis() + ROUND_TRANSITION_DELAY_MS;
  } else {
    endGame("Round 2 complete");
  }
}

void endGame(const String& reason) {
  Serial.println("Game ended.");
  Serial.print("Reason: ");
  Serial.println(reason);
  Serial.print("Final score: ");
  Serial.println(playerScore);

  if (currentRound != LEC_ROUND_NONE) {
    finalizeStatsRound(currentRound, false);
  }

  finalizeStatsGame();

  String statsPayload = buildStatsPayload(reason, playerScore);
  queuePiStatsPost(statsPayload);

  gameRunning = false;
  currentRound = LEC_ROUND_NONE;
  correctServesInRound = 0;
  fifteenSecondWarningPlayed = false;
  roleAssignmentSendsRemaining = 0;

  pendingRoundTransition = false;
  pendingRecipeBroadcast = false;

  pendingRoleAssignmentBroadcast = false;

  if (onFire) {
    onFire = false;
    digitalWrite(FIRE_LED_PIN, LOW);
    broadcastFireState(false);
  }

  broadcastEndGame();

  audioModule.stop();
  audioModule.playSpecified(3);

  notifyPiEndGame(reason, playerScore);

  resetAllPlates();

  delay(2000);
  broadcastReinitialize();
}

void resetGameState() {
  Serial.println("Resetting server game state.");

  gameRunning = false;
  onFire = false;
  currentRound = LEC_ROUND_NONE;

  playerScore = 0;
  correctServesInRound = 0;

  roundStartTime = 0;
  roundDuration = 0;

  roleAssignmentSendsRemaining = 0;

  currentRecipeValid = false;
  nextRecipeValid = false;

  pendingRoundTransition = false;
  pendingRecipeBroadcast = false;

  pendingRoleAssignmentBroadcast = false;

  resetStatsTracking();

  digitalWrite(FIRE_LED_PIN, LOW);

  resetAllPlates();

  audioModule.stop();
}

/*************************************************************
  ROLE ASSIGNMENT
*************************************************************/

void assignRolesForCurrentRound() {
  int indexes[SERVER_MAX_NODES];
  int count = 0;

  collectGenericClientIndexes(indexes, count);
  shuffleIndexes(indexes, count);

  if (count < 4) {
    Serial.print("Warning: expected 4 generic clients, found ");
    Serial.println(count);
  }

  for (int i = 0; i < SERVER_MAX_NODES; i++) {
    if (!nodes[i].used) continue;

    if (nodes[i].deviceClass == LEC_DEVICE_INGREDIENT_STATION) {
      nodes[i].assignedRole = LEC_ROLE_INGREDIENT_STATION;
    } else if (nodes[i].deviceClass == LEC_DEVICE_GENERIC_CLIENT) {
      nodes[i].assignedRole = LEC_ROLE_NONE;
    }
  }

  LecRole roundRoles[4];

  if (currentRound == LEC_ROUND_1) {
    roundRoles[0] = LEC_ROLE_CHOP_STATION;
    roundRoles[1] = LEC_ROLE_CHOP_STATION;
    roundRoles[2] = LEC_ROLE_MIX_STATION;
    roundRoles[3] = LEC_ROLE_SERVE_STATION;
  } else {
    roundRoles[0] = LEC_ROLE_CHOP_STATION;
    roundRoles[1] = LEC_ROLE_COOK_STATION;
    roundRoles[2] = LEC_ROLE_MIX_STATION;
    roundRoles[3] = LEC_ROLE_SERVE_STATION;
  }

  for (int i = 0; i < count && i < 4; i++) {
    nodes[indexes[i]].assignedRole = roundRoles[i];
  }

  Serial.println("Assigned roles for current round:");

  for (int i = 0; i < SERVER_MAX_NODES; i++) {
    if (!nodes[i].used) continue;

    Serial.print(lecMacToString(nodes[i].mac));
    Serial.print(" -> ");
    Serial.println(static_cast<int>(nodes[i].assignedRole));
  }
}

void sendRoleAssignmentToNode(ServerNode& node) {
  if (!node.used) return;

  LecPacket msg;
  lecInitPacket(msg, LEC_PKT_ASSIGN_ROLE, LEC_DEVICE_SERVER, LEC_ROLE_SERVER);
  msg.role = node.assignedRole;
  msg.round = currentRound;
  lecSetStatus(msg, "role-assignment");

  sendPacketToMac(node.mac, msg);
}



void broadcastRoleAssignments() {
  // Centralized role assignment broadcast so startRound()
  // and delayed Round 1 flow use the same logic.
  for (int i = 0; i < SERVER_MAX_NODES; i++) {
    if (nodes[i].used) {
      sendRoleAssignmentToNode(nodes[i]);
    }
  }
}

void scheduleRoleAssignmentBroadcast(unsigned long delayMs) {
  // Schedules multiple role assignment sends.
  // This makes round-start more reliable if one ESP-NOW packet is missed.
  pendingRoleAssignmentBroadcast = true;
  roleAssignmentBroadcastAt = millis() + delayMs;
  roleAssignmentSendsRemaining = ROLE_ASSIGNMENT_SEND_COUNT;
}

/*************************************************************
  BROADCASTS
*************************************************************/

void broadcastStartRound(LecRound round) {
  LecPacket msg;
  lecInitPacket(msg, LEC_PKT_START_ROUND, LEC_DEVICE_SERVER, LEC_ROLE_SERVER);
  msg.round = round;
  lecSetStatus(msg, "start-round");

  for (int i = 0; i < SERVER_MAX_NODES; i++) {
    if (nodes[i].used) {
      sendPacketToMac(nodes[i].mac, msg);
    }
  }
}

void broadcastEndGame() {
  LecPacket msg;
  lecInitPacket(msg, LEC_PKT_END_GAME, LEC_DEVICE_SERVER, LEC_ROLE_SERVER);
  lecSetStatus(msg, "end-game");

  for (int i = 0; i < SERVER_MAX_NODES; i++) {
    if (nodes[i].used) {
      sendPacketToMac(nodes[i].mac, msg);
    }
  }
}

void broadcastReinitialize() {
  LecPacket msg;
  lecInitPacket(msg, LEC_PKT_REINITIALIZE, LEC_DEVICE_SERVER, LEC_ROLE_SERVER);
  lecSetStatus(msg, "reinitialize");

  for (int i = 0; i < SERVER_MAX_NODES; i++) {
    if (nodes[i].used) {
      sendPacketToMac(nodes[i].mac, msg);
    }
  }
}

void broadcastFireState(bool fireActive) {
  LecPacket msg;
  lecInitPacket(
    msg,
    fireActive ? LEC_PKT_ON_FIRE : LEC_PKT_EXTINGUISH_FIRE,
    LEC_DEVICE_SERVER,
    LEC_ROLE_SERVER
  );

  msg.success = true;
  lecSetStatus(msg, fireActive ? "on-fire" : "fire-extinguished");

  for (int i = 0; i < SERVER_MAX_NODES; i++) {
    if (nodes[i].used) {
      sendPacketToMac(nodes[i].mac, msg);
    }
  }
}

void broadcastCurrentRecipe() {
  if (!currentRecipeValid) {
    Serial.println("No current recipe to broadcast.");
    return;
  }

  for (int i = 0; i < SERVER_MAX_NODES; i++) {
    if (!nodes[i].used) continue;

    if (nodes[i].assignedRole == LEC_ROLE_MIX_STATION ||
        nodes[i].assignedRole == LEC_ROLE_SERVE_STATION ||
        nodes[i].assignedRole == LEC_ROLE_COOK_STATION) {
      sendRecipeToNode(nodes[i]);
    }
  }

  //updateScoreAndTimeOnPi(playerScore, getTimeLeftInSeconds());
}

void sendRecipeToNode(ServerNode& node) {
  LecPacket msg;
  lecInitPacket(msg, LEC_PKT_NEW_RECIPE, LEC_DEVICE_SERVER, LEC_ROLE_SERVER);
  msg.round = currentRound;
  lecSetRecipeName(msg, currentRecipe.name);
  lecSetStatus(msg, "new-recipe");

  sendPacketToMac(node.mac, msg);
}

/*************************************************************
  RECIPES
*************************************************************/

void pickDifferentRecipeForRound(
  LecRound round,
  const char* avoidRecipeName,
  LecRecipe& outRecipe,
  bool& valid
) {
  int allowed[LEC_TOTAL_RECIPES];
  int count = 0;

  for (int i = 0; i < LEC_TOTAL_RECIPES; i++) {
    if (!isRecipeAllowedForRound(LEC_RECIPES[i], round)) {
      continue;
    }

    // Prefer a recipe different from the current visible recipe.
    if (avoidRecipeName != nullptr &&
        avoidRecipeName[0] != '\0' &&
        strcmp(LEC_RECIPES[i].name, avoidRecipeName) == 0) {
      continue;
    }

    allowed[count++] = i;
  }

  // Fallback if there is only one valid recipe for this round.
  if (count == 0) {
    pickRecipeForRound(round, outRecipe, valid);
    return;
  }

  int selected = allowed[random(0, count)];

  outRecipe = LEC_RECIPES[selected];
  valid = true;

  Serial.print("Picked different recipe: ");
  Serial.println(outRecipe.name);
}

void pickRecipeForRound(LecRound round, LecRecipe& outRecipe, bool& valid) {
  int allowed[LEC_TOTAL_RECIPES];
  int count = 0;

  for (int i = 0; i < LEC_TOTAL_RECIPES; i++) {
    if (isRecipeAllowedForRound(LEC_RECIPES[i], round)) {
      allowed[count++] = i;
    }
  }

  if (count == 0) {
    valid = false;
    memset(&outRecipe, 0, sizeof(outRecipe));
    Serial.println("No valid recipes for round.");
    return;
  }

  int selected = allowed[random(0, count)];

  outRecipe = LEC_RECIPES[selected];
  valid = true;

  Serial.print("Picked recipe: ");
  Serial.println(outRecipe.name);
}

bool isRecipeAllowedForRound(const LecRecipe& recipe, LecRound round) {
  bool requiresCook = false;

  for (int i = 0; i < recipe.numIngredients; i++) {
    if (recipe.ingredients[i].requiresCook) {
      requiresCook = true;
      break;
    }
  }

  if (round == LEC_ROUND_1) {
    return !requiresCook;
  }

  if (round == LEC_ROUND_2) {
    return requiresCook;
  }

  return false;
}

int getServeTargetForRound(LecRound round) {
  if (round == LEC_ROUND_1) return LEC_ROUND_1_SERVE_TARGET;
  if (round == LEC_ROUND_2) return LEC_ROUND_2_SERVE_TARGET;
  return 0;
}

/*************************************************************
  LOOP PROCESSORS
*************************************************************/

void processRoundTransition() {
  if (!pendingRoundTransition) return;

  if (millis() >= roundTransitionAt) {
    LecRound next = pendingNextRound;

    pendingRoundTransition = false;
    pendingNextRound = LEC_ROUND_NONE;

    startRound(next);
  }
}

void processPendingRecipeBroadcast() {
  if (!pendingRecipeBroadcast) return;

  if (millis() >= recipeBroadcastAt) {
    pendingRecipeBroadcast = false;
    broadcastCurrentRecipe();
  }
}

void processPendingRoleAssignmentBroadcast() {
  if (!pendingRoleAssignmentBroadcast) return;

  if (millis() >= roleAssignmentBroadcastAt) {
    // Send role assignment, but keep retrying a couple times.
    broadcastRoleAssignments();

    roleAssignmentSendsRemaining--;

    if (roleAssignmentSendsRemaining > 0) {
      // Schedule another role assignment retry.
      roleAssignmentBroadcastAt = millis() + ROLE_ASSIGNMENT_RETRY_INTERVAL_MS;
      return;
    }

    // Finished all role assignment sends.
    pendingRoleAssignmentBroadcast = false;

    // Schedule recipe after clients have had time to finish tornado/role transition.
    // This is safer than only waiting ROLE_RECIPE_DELAY_MS, because the client role delay is 4500ms.
    pendingRecipeBroadcast = true;
    recipeBroadcastAt = millis() + SERVER_ROLE_TRANSITION_DELAY_MS;
  }
}

void processCountdownAndScoreboard() {
  static unsigned long lastCountdownUpdate = 0;

  if (!gameRunning) return;

  if (millis() - lastCountdownUpdate >= 1000) {
    lastCountdownUpdate = millis();

    int timeLeft = getTimeLeftInSeconds();

    updateScoreAndTimeOnPi(playerScore, timeLeft);

    if (timeLeft == 15 && !fifteenSecondWarningPlayed) {
      audioModule.stop();
      audioModule.playSpecified(2);
      fifteenSecondWarningPlayed = true;
    }
  }
}

/*************************************************************
  INCOMING PACKETS
*************************************************************/

void handleIncomingPacket(const uint8_t* mac, const LecPacket& packet) {
  upsertNode(mac, packet);

  switch (packet.packetType) {
    case LEC_PKT_HELLO:
    case LEC_PKT_NODE_STATUS:
      handleHelloPacket(mac, packet);
      break;

    case LEC_PKT_PLATE_LOOKUP:
      handlePlateLookup(mac, packet);
      break;

    case LEC_PKT_PLATE_UPDATE:
      handlePlateUpdate(mac, packet);
      break;

    case LEC_PKT_CURRENT_RECIPE_REQUEST:
      handleRecipeRequest(mac, packet);
      break;

    case LEC_PKT_SERVE_ATTEMPT:
      handleServeAttempt(mac, packet);
      break;

    case LEC_PKT_ON_FIRE:
      handleFireRequest(mac, packet);
      break;
    
    case LEC_PKT_MAINT_ACK:
    case LEC_PKT_NET_CONFIG:
    case LEC_PKT_BULK_UPDATE_RESULT:
    case LEC_PKT_REBOOT_REQUEST:
      handleMaintenanceResultPacket(mac, packet);
      break;

    case LEC_PKT_DEBUG_LOG:
      handleDebugLog(mac, packet);
      break;

    default:
      Serial.println("Unhandled packet type.");
      break;
  }
}

void handleMaintenanceResultPacket(const uint8_t* mac, const LecPacket& packet) {
  int index = findNodeByMac(mac);

  Serial.println("=== Maintenance Result ===");
  Serial.print("From: ");
  Serial.println(lecMacToString(mac));

  if (index != -1) {
    Serial.print("Claimed ID: ");
    Serial.println(nodes[index].claimedId);
  }

  Serial.print("Packet Type: ");
  Serial.println(static_cast<int>(packet.packetType));
  Serial.print("Result: ");
  Serial.println(static_cast<int>(packet.result));
  Serial.print("Mode: ");
  Serial.println(static_cast<int>(packet.runMode));
  Serial.print("Status: ");
  Serial.println(packet.status);
  Serial.print("Payload: ");
  Serial.println(packet.payload);
  Serial.println("==========================");
}

void handleHelloPacket(const uint8_t* mac, const LecPacket& packet) {
  int index = upsertNode(mac, packet);

  if (index == -1) {
    return;
  }

  if (nodes[index].deviceClass == LEC_DEVICE_INGREDIENT_STATION) {
    nodes[index].assignedRole = LEC_ROLE_INGREDIENT_STATION;
  }

  LecPacket ack;
  lecInitPacket(ack, LEC_PKT_CLAIM_ACK, LEC_DEVICE_SERVER, LEC_ROLE_SERVER);
  ack.result = LEC_RESULT_OK;
  ack.round = currentRound;
  lecSetStatus(ack, "hello-ack");

  sendPacketToMac(mac, ack);

  if (gameRunning) {
    // Start-round should be sent before role assignment.
    LecPacket startMsg;
    lecInitPacket(startMsg, LEC_PKT_START_ROUND, LEC_DEVICE_SERVER, LEC_ROLE_SERVER);
    startMsg.round = currentRound;
    lecSetStatus(startMsg, "start-round-sync");
    sendPacketToMac(mac, startMsg);

    // If Round 1 countdown is still pending, do not send role yet.
    // The normal delayed broadcast will send it.
    if (pendingRoleAssignmentBroadcast) {
      Serial.println("HELLO during Round 1 countdown; sent start sync only.");
      return;
    }

    // Now send role after start-round sync.
    sendRoleAssignmentToNode(nodes[index]);

    if (currentRecipeValid &&
        (nodes[index].assignedRole == LEC_ROLE_MIX_STATION ||
         nodes[index].assignedRole == LEC_ROLE_SERVE_STATION ||
         nodes[index].assignedRole == LEC_ROLE_COOK_STATION)) {
      sendRecipeToNode(nodes[index]);
    }
  }
}

void handlePlateLookup(const uint8_t* mac, const LecPacket& packet) {
  if (packet.rfid[0] == '\0') {
    sendAckToMac(mac, LEC_PKT_PLATE_STATE, LEC_RESULT_INVALID, "missing-rfid");
    return;
  }

  if (!gameRunning) {
    sendAckToMac(mac, LEC_PKT_PLATE_STATE, LEC_RESULT_BUSY, "game-not-running");
    return;
  }

  if (xSemaphoreTake(xMutex, portMAX_DELAY)) {
    int index = findOrCreatePlate(packet.rfid);

    if (index == -1) {
      xSemaphoreGive(xMutex);
      sendAckToMac(mac, LEC_PKT_PLATE_STATE, LEC_RESULT_FAIL, "plate-store-full");
      return;
    }

    LecPlateState plate = plates[index];

    xSemaphoreGive(xMutex);

    sendPlateStateToMac(mac, plate, LEC_RESULT_OK);
  }
}

void handlePlateUpdate(const uint8_t* mac, const LecPacket& packet) {
  if (packet.rfid[0] == '\0') {
    sendAckToMac(mac, LEC_PKT_PLATE_STATE, LEC_RESULT_INVALID, "missing-rfid");
    return;
  }

  if (!gameRunning) {
    sendAckToMac(mac, LEC_PKT_PLATE_STATE, LEC_RESULT_BUSY, "game-not-running");
    return;
  }

  if (xSemaphoreTake(xMutex, portMAX_DELAY)) {
    int index = findOrCreatePlate(packet.rfid);

    if (index == -1) {
      xSemaphoreGive(xMutex);
      sendAckToMac(mac, LEC_PKT_PLATE_STATE, LEC_RESULT_FAIL, "plate-store-full");
      return;
    }

    LecPlateState& plate = plates[index];

    if (packet.resetPlate) {
      strncpy(plate.ingredient, "none", INGREDIENT_LENGTH - 1);
      plate.ingredient[INGREDIENT_LENGTH - 1] = '\0';

      plate.chopCount = 0;
      plate.cookCount = 0;
      plate.status = LEC_PLATE_ACTIVE;
    }

    if (packet.ingredient[0] != '\0') {
      strncpy(plate.ingredient, packet.ingredient, INGREDIENT_LENGTH - 1);
      plate.ingredient[INGREDIENT_LENGTH - 1] = '\0';
      plate.status = LEC_PLATE_ACTIVE;
    }

    if (packet.chopCount != INVALID_COUNT) {
      plate.chopCount = packet.chopCount;
    }

    if (packet.cookCount != INVALID_COUNT) {
      plate.cookCount = packet.cookCount;
    }

    plate.updatedAtMs = millis();

    LecPlateState responsePlate = plate;

    xSemaphoreGive(xMutex);

    sendPlateStateToMac(mac, responsePlate, LEC_RESULT_OK);
  }
}

void handleRecipeRequest(const uint8_t* mac, const LecPacket& packet) {
  if (!currentRecipeValid) {
    sendAckToMac(mac, LEC_PKT_RECIPE_STATE, LEC_RESULT_FAIL, "no-current-recipe");
    return;
  }

  LecPacket response;
  lecInitPacket(response, LEC_PKT_RECIPE_STATE, LEC_DEVICE_SERVER, LEC_ROLE_SERVER);
  response.result = LEC_RESULT_OK;
  response.round = currentRound;
  lecSetRecipeName(response, currentRecipe.name);
  lecSetStatus(response, "recipe-state");

  sendPacketToMac(mac, response);
}

void handleServeAttempt(const uint8_t* mac, const LecPacket& packet) {
  if (!gameRunning || !currentRecipeValid) {
    sendAckToMac(mac, LEC_PKT_SERVE_RESULT, LEC_RESULT_BUSY, "game-not-ready");
    return;
  }

  if (packet.rfid[0] == '\0') {
    sendAckToMac(mac, LEC_PKT_SERVE_RESULT, LEC_RESULT_INVALID, "missing-rfid");
    return;
  }

  bool success = false;

  if (xSemaphoreTake(xMutex, portMAX_DELAY)) {
    int index = findPlateIndex(packet.rfid);

    if (index != -1) {
      LecPlateState& plate = plates[index];

      success = strcmp(plate.ingredient, currentRecipe.name) == 0;

      if (success) {
        Serial.println("Serve success.");

        playerScore += SCORE_PER_SUCCESSFUL_SERVE;
        correctServesInRound += 1;

        roundDuration += 15000UL;

        strncpy(plate.ingredient, "none", INGREDIENT_LENGTH - 1);
        plate.ingredient[INGREDIENT_LENGTH - 1] = '\0';

        plate.chopCount = 0;
        plate.cookCount = 0;
        plate.updatedAtMs = millis();
      } else {
        Serial.println("Serve failed.");

        if (roundDuration > 10000UL) {
          roundDuration -= 10000UL;
        } else {
          roundDuration = 0;
        }
      }
    } else {
      Serial.println("Serve failed: RFID not found.");
      success = false;

      if (roundDuration > 10000UL) {
        roundDuration -= 10000UL;
      } else {
        roundDuration = 0;
      }
    }

    xSemaphoreGive(xMutex);
  }

  LecPacket response;
  lecInitPacket(response, LEC_PKT_SERVE_RESULT, LEC_DEVICE_SERVER, LEC_ROLE_SERVER);

  // Round complete can be Round 1 or Round 2.
  bool roundComplete =
    success && correctServesInRound >= getServeTargetForRound(currentRound);

  // Game complete only happens when Round 2 is completed.
  bool gameComplete =
    roundComplete && currentRound == LEC_ROUND_2;

  response.result = success ? LEC_RESULT_OK : LEC_RESULT_FAIL;
  response.success = success;
  response.playerScoreDelta = success ? 1 : 0;
  response.round = currentRound;

  // Only the final game-winning serve gets "serve-complete".
  // Round 1 completion still behaves like normal serve success.
  if (gameComplete) {
    lecSetStatus(response, "serve-complete");
  } else {
    lecSetStatus(response, success ? "serve-success" : "serve-fail");
  }

  sendPacketToMac(mac, response);

  updateScoreAndTimeOnPi(playerScore, getTimeLeftInSeconds());

  if (success) {
    if (roundComplete) {
      if (currentRound == LEC_ROUND_1) {
        playerScore += ROUND_1_COMPLETE_BONUS;
        Serial.println("Round 1 complete bonus awarded.");
      } else if (currentRound == LEC_ROUND_2) {
        playerScore += GAME_COMPLETE_BONUS;
        Serial.println("Game complete bonus awarded.");
      }

      updateScoreAndTimeOnPi(playerScore, getTimeLeftInSeconds());

      endCurrentRound();
    } else {
      currentRecipe = nextRecipe;
      currentRecipeValid = nextRecipeValid;

      pickDifferentRecipeForRound(
        currentRound,
        currentRecipeValid ? currentRecipe.name : "",
        nextRecipe,
        nextRecipeValid
      );

      pendingRecipeBroadcast = true;
      recipeBroadcastAt = millis() + ROLE_RECIPE_DELAY_MS;
    }
  }
}

void handleFireRequest(const uint8_t* mac, const LecPacket& packet) {
  if (!gameRunning) {
    Serial.println("Ignored fire request because game is not running.");
    return;
  }

  if (onFire) {
    return;
  }

  Serial.println("Fire triggered by client.");

  statsFireCount++;

  onFire = true;
  digitalWrite(FIRE_LED_PIN, HIGH);

  broadcastFireState(true);
}

/*************************************************************
  SEND HELPERS
*************************************************************/

void sendPacketToMac(const uint8_t* mac, LecPacket& packet) {
  if (!mac) return;

  packet.protocolVersion = LEC_PROTOCOL_VERSION;
  packet.packetSize = sizeof(LecPacket);
  packet.uptimeMs = millis();
  packet.deviceClass = LEC_DEVICE_SERVER;

  if (packet.packetType != LEC_PKT_MAINT_REQUEST &&
      packet.packetType != LEC_PKT_MAINT_EXIT &&
      packet.packetType != LEC_PKT_NET_CONFIG &&
      packet.packetType != LEC_PKT_BULK_UPDATE_REQUEST &&
      packet.packetType != LEC_PKT_REBOOT_REQUEST) {
    packet.runMode = LEC_MODE_GAME;
  }
  
  // For role assignment packets, packet.role is the ASSIGNED client role.
  // Do not overwrite it with LEC_ROLE_SERVER.
  if (packet.packetType != LEC_PKT_ASSIGN_ROLE) {
    packet.role = LEC_ROLE_SERVER;
  }

  #if LEC_DEBUG
    Serial.println("=== Sending Packet ===");
    Serial.print("To: ");
    Serial.println(lecMacToString(mac));
    Serial.print("Type: ");
    Serial.println(static_cast<int>(packet.packetType));
    Serial.print("Role Field: ");
    Serial.println(static_cast<int>(packet.role));
    Serial.print("Result: ");
    Serial.println(static_cast<int>(packet.result));
    Serial.print("Recipe: ");
    Serial.println(packet.recipeName);
    Serial.println("======================");
  #endif

  esp_err_t result = esp_now_send(
    mac,
    reinterpret_cast<uint8_t*>(&packet),
    sizeof(LecPacket)
  );

  if (result != ESP_OK) {
    Serial.print("ESP-NOW send error: ");
    Serial.println(result);
  }
}

void sendPlateStateToMac(const uint8_t* mac, const LecPlateState& plate, LecPacketResult result) {
  LecPacket response;
  lecInitPacket(response, LEC_PKT_PLATE_STATE, LEC_DEVICE_SERVER, LEC_ROLE_SERVER);

  response.result = result;
  response.round = currentRound;

  lecSetRfid(response, plate.rfid);
  lecSetIngredient(response, plate.ingredient);

  response.chopCount = plate.chopCount;
  response.cookCount = plate.cookCount;

  lecSetStatus(response, result == LEC_RESULT_OK ? "plate-state" : "plate-error");

  sendPacketToMac(mac, response);
}

void sendAckToMac(const uint8_t* mac, LecPacketType type, LecPacketResult result, const char* status) {
  LecPacket response;
  lecInitPacket(response, type, LEC_DEVICE_SERVER, LEC_ROLE_SERVER);

  response.result = result;
  response.round = currentRound;
  response.success = result == LEC_RESULT_OK;

  lecSetStatus(response, status);

  sendPacketToMac(mac, response);
}

void printOperatorHelp() {
  Serial.println();
  Serial.println("=== Let Em Cook Operator Console ===");
  Serial.println("Gameplay:");
  Serial.println("  start");
  Serial.println("  end");
  Serial.println("  reset");
  Serial.println("  list");
  Serial.println("  roles");
  Serial.println("  recipe");
  Serial.println("  fire");
  Serial.println("  ext");
  Serial.println();
  Serial.println("Maintenance / OTA:");
  Serial.println("  net set <ssid> <password> <baseUrl>");
  Serial.println("  net push all");
  Serial.println("  net push <claimedId>");
  Serial.println("  maint all");
  Serial.println("  maint <claimedId>");
  Serial.println("  update all <binFile>");
  Serial.println("  update generic <binFile>");
  Serial.println("  update pantry <binFile>");
  Serial.println("  update <claimedId> <binFile>");
  Serial.println("  reboot all");
  Serial.println("  reboot <claimedId>");
  Serial.println();
  Serial.println("Examples:");
  Serial.println("  net set MyScoreboardAP MySecretPassword http://192.168.4.2:8080/LetEmCook");
  Serial.println("  net push all");
  Serial.println("  update generic generic-client.bin");
  Serial.println("  update pantry ingredient-station.bin");
  Serial.println("====================================");
  Serial.println();
}

String getArgToken(const String& text, int index) {
  int currentIndex = 0;
  int tokenStart = -1;

  for (int i = 0; i <= text.length(); i++) {
    bool atEnd = i == text.length();
    bool isSpace = !atEnd && isspace(text.charAt(i));

    if (!atEnd && !isSpace && tokenStart == -1) {
      tokenStart = i;
    }

    if ((atEnd || isSpace) && tokenStart != -1) {
      if (currentIndex == index) {
        return text.substring(tokenStart, i);
      }

      currentIndex++;
      tokenStart = -1;
    }
  }

  return "";
}

ServerNode* findNodeByClaimedId(const char* claimedId) {
  if (claimedId == nullptr || claimedId[0] == '\0') {
    return nullptr;
  }

  for (int i = 0; i < SERVER_MAX_NODES; i++) {
    if (!nodes[i].used) continue;

    if (strcmp(nodes[i].claimedId, claimedId) == 0) {
      return &nodes[i];
    }
  }

  return nullptr;
}

void sendMaintenancePacketToNode(
  ServerNode& node,
  LecPacketType packetType,
  const char* status,
  const char* payload
) {
  if (!node.used) {
    return;
  }

  LecPacket msg;
  lecInitPacket(msg, packetType, LEC_DEVICE_SERVER, LEC_ROLE_SERVER);

  msg.round = currentRound;
  msg.runMode = LEC_MODE_MAINT_REQUESTED;

  if (status != nullptr) {
    lecSetStatus(msg, status);
  }

  if (payload != nullptr) {
    lecSetPayload(msg, payload);
  }

  sendPacketToMac(node.mac, msg);
}

void broadcastMaintenancePacket(
  LecPacketType packetType,
  const char* status,
  const char* payload,
  LecDeviceClass deviceClassFilter
) {
  for (int i = 0; i < SERVER_MAX_NODES; i++) {
    if (!nodes[i].used) continue;

    if (deviceClassFilter != LEC_DEVICE_UNKNOWN &&
        nodes[i].deviceClass != deviceClassFilter) {
      continue;
    }

    sendMaintenancePacketToNode(nodes[i], packetType, status, payload);
  }
}

void sendMaintenancePacketToTarget(
  const char* target,
  LecPacketType packetType,
  const char* status,
  const char* payload
) {
  if (target == nullptr || target[0] == '\0') {
    Serial.println("Missing target.");
    return;
  }

  if (strcmp(target, "all") == 0) {
    broadcastMaintenancePacket(packetType, status, payload);
    return;
  }

  if (strcmp(target, "generic") == 0) {
    broadcastMaintenancePacket(
      packetType,
      status,
      payload,
      LEC_DEVICE_GENERIC_CLIENT
    );
    return;
  }

  if (strcmp(target, "pantry") == 0 ||
      strcmp(target, "ingredient") == 0 ||
      strcmp(target, "ingredients") == 0) {
    broadcastMaintenancePacket(
      packetType,
      status,
      payload,
      LEC_DEVICE_INGREDIENT_STATION
    );
    return;
  }

  ServerNode* node = findNodeByClaimedId(target);

  if (node == nullptr) {
    Serial.print("No node found with claimedId: ");
    Serial.println(target);
    return;
  }

  sendMaintenancePacketToNode(*node, packetType, status, payload);
}

void handleNetSetCommand(const String& rawCommand) {
  String ssid = getArgToken(rawCommand, 2);
  String password = getArgToken(rawCommand, 3);
  String baseUrl = getArgToken(rawCommand, 4);

  if (ssid.length() == 0 || baseUrl.length() == 0) {
    Serial.println("Usage: net set <ssid> <password> <baseUrl>");
    return;
  }

  bool saved = LecStorage::saveNetConfig(
    ssid.c_str(),
    password.c_str(),
    baseUrl.c_str()
  );

  if (!saved) {
    Serial.println("Failed to save server net config.");
    return;
  }

  Serial.println("Server net config saved.");
  Serial.print("SSID: ");
  Serial.println(ssid);
  Serial.print("OTA Base URL: ");
  Serial.println(baseUrl);
}

void handleNetPushCommand(const String& rawCommand) {
  String target = getArgToken(rawCommand, 2);

  if (target.length() == 0) {
    target = "all";
  }

  LecStorage::NetConfig config;

  if (!LecStorage::loadNetConfig(config)) {
    Serial.println("No server net config saved. Use:");
    Serial.println("  net set <ssid> <password> <baseUrl>");
    return;
  }

  String payload =
    String(config.ssid) + "|" +
    String(config.password) + "|" +
    String(config.otaBaseUrl);

  if (payload.length() >= LEC_PAYLOAD_LENGTH) {
    Serial.println("Net config payload too long for ESP-NOW packet.");
    return;
  }

  String targetLower = target;
  targetLower.toLowerCase();

  Serial.print("Pushing net config to: ");
  Serial.println(targetLower);

  sendMaintenancePacketToTarget(
    targetLower.c_str(),
    LEC_PKT_NET_CONFIG,
    "net-config",
    payload.c_str()
  );
}

void handleMaintCommand(const String& rawCommand) {
  String target = getArgToken(rawCommand, 1);

  if (target.length() == 0) {
    Serial.println("Usage: maint <all|generic|pantry|claimedId>");
    return;
  }

  target.toLowerCase();

  Serial.print("Sending maintenance request to: ");
  Serial.println(target);

  sendMaintenancePacketToTarget(
    target.c_str(),
    LEC_PKT_MAINT_REQUEST,
    "maint-request",
    nullptr
  );
}

void handleUpdateCommand(const String& rawCommand) {
  String target = getArgToken(rawCommand, 1);
  String binFile = getArgToken(rawCommand, 2);

  if (target.length() == 0 || binFile.length() == 0) {
    Serial.println("Usage: update <all|generic|pantry|claimedId> <binFile>");
    return;
  }

  if (binFile.length() >= LEC_PAYLOAD_LENGTH) {
    Serial.println("Bin filename/path too long for payload.");
    return;
  }

  String targetLower = target;
  targetLower.toLowerCase();

  Serial.print("Sending update request to: ");
  Serial.print(targetLower);
  Serial.print(" | bin=");
  Serial.println(binFile);

  sendMaintenancePacketToTarget(
    targetLower.c_str(),
    LEC_PKT_BULK_UPDATE_REQUEST,
    "bulk-update",
    binFile.c_str()
  );
}

void handleRebootCommand(const String& rawCommand) {
  String target = getArgToken(rawCommand, 1);

  if (target.length() == 0) {
    Serial.println("Usage: reboot <all|generic|pantry|claimedId>");
    return;
  }

  target.toLowerCase();

  Serial.print("Sending reboot request to: ");
  Serial.println(target);

  sendMaintenancePacketToTarget(
    target.c_str(),
    LEC_PKT_REBOOT_REQUEST,
    "reboot",
    nullptr
  );
}

/*************************************************************
  NODE HELPERS
*************************************************************/

int findNodeByMac(const uint8_t* mac) {
  for (int i = 0; i < SERVER_MAX_NODES; i++) {
    if (!nodes[i].used) continue;

    if (lecMacEquals(nodes[i].mac, mac)) {
      return i;
    }
  }

  return -1;
}

int upsertNode(const uint8_t* mac, const LecPacket& packet) {
  int existing = findNodeByMac(mac);

  if (existing != -1) {
    nodes[existing].online = true;
    nodes[existing].lastSeenMs = millis();
    nodes[existing].lastSequence = packet.sequence;
    nodes[existing].deviceClass = packet.deviceClass;
    nodes[existing].runMode = packet.runMode;

    strncpy(nodes[existing].firmwareVersion, packet.firmwareVersion, LEC_FW_VERSION_LENGTH - 1);
    nodes[existing].firmwareVersion[LEC_FW_VERSION_LENGTH - 1] = '\0';

    strncpy(nodes[existing].status, packet.status, LEC_STATUS_LENGTH - 1);
    nodes[existing].status[LEC_STATUS_LENGTH - 1] = '\0';

    return existing;
  }

  for (int i = 0; i < SERVER_MAX_NODES; i++) {
    if (nodes[i].used) continue;

    memset(&nodes[i], 0, sizeof(ServerNode));

    nodes[i].used = true;
    nodes[i].online = true;

    memcpy(nodes[i].mac, mac, 6);

    nodes[i].deviceClass = packet.deviceClass;
    nodes[i].assignedRole = LEC_ROLE_NONE;
    nodes[i].runMode = packet.runMode;

    strncpy(nodes[i].claimedId, packet.claimedId, LEC_CLAIMED_ID_LENGTH - 1);
    nodes[i].claimedId[LEC_CLAIMED_ID_LENGTH - 1] = '\0';

    strncpy(nodes[i].firmwareVersion, packet.firmwareVersion, LEC_FW_VERSION_LENGTH - 1);
    nodes[i].firmwareVersion[LEC_FW_VERSION_LENGTH - 1] = '\0';

    strncpy(nodes[i].status, packet.status, LEC_STATUS_LENGTH - 1);
    nodes[i].status[LEC_STATUS_LENGTH - 1] = '\0';

    nodes[i].lastSeenMs = millis();
    nodes[i].lastSequence = packet.sequence;

    if (nodes[i].deviceClass == LEC_DEVICE_INGREDIENT_STATION) {
      nodes[i].assignedRole = LEC_ROLE_INGREDIENT_STATION;
    }

    Serial.print("Registered new node: ");
    Serial.println(lecMacToString(mac));

    return i;
  }

  Serial.println("Node roster full.");
  return -1;
}

bool addPeerIfNeeded(const uint8_t* mac) {
  if (esp_now_is_peer_exist(mac)) {
    return true;
  }

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, mac, 6);
  peerInfo.channel = LEC_ESPNOW_CHANNEL;
  peerInfo.encrypt = false;

  esp_err_t result = esp_now_add_peer(&peerInfo);

  if (result == ESP_OK) {
    Serial.print("Added peer: ");
    Serial.println(lecMacToString(mac));
    return true;
  }

  Serial.print("Failed to add peer. Code: ");
  Serial.println(result);
  return false;
}

int countGenericClients() {
  int count = 0;

  for (int i = 0; i < SERVER_MAX_NODES; i++) {
    if (nodes[i].used && nodes[i].deviceClass == LEC_DEVICE_GENERIC_CLIENT) {
      count++;
    }
  }

  return count;
}

void collectGenericClientIndexes(int* indexes, int& count) {
  count = 0;

  for (int i = 0; i < SERVER_MAX_NODES; i++) {
    if (nodes[i].used && nodes[i].deviceClass == LEC_DEVICE_GENERIC_CLIENT) {
      indexes[count++] = i;
    }
  }
}

void shuffleIndexes(int* indexes, int count) {
  for (int i = count - 1; i > 0; i--) {
    int j = random(0, i + 1);

    int temp = indexes[i];
    indexes[i] = indexes[j];
    indexes[j] = temp;
  }
}

/*************************************************************
  PLATE HELPERS
*************************************************************/

void initializePlates() {
  const char* knownRFIDs[] = {
    "30ED1279",
    "E0EC1279",
    "F0EC1279",
    "40ED1279",
    "BCB27E01",
    "D0EC1279"
  };

  plateCount = 0;
  memset(plates, 0, sizeof(plates));

  for (int i = 0; i < 6; i++) {
    int index = findOrCreatePlate(knownRFIDs[i]);

    if (index != -1) {
      strncpy(plates[index].ingredient, "none", INGREDIENT_LENGTH - 1);
      plates[index].ingredient[INGREDIENT_LENGTH - 1] = '\0';

      plates[index].chopCount = 0;
      plates[index].cookCount = 0;
      plates[index].status = LEC_PLATE_ACTIVE;
    }
  }

  Serial.println("Plate data initialized.");
}

void resetAllPlates() {
  if (xSemaphoreTake(xMutex, portMAX_DELAY)) {
    for (int i = 0; i < plateCount; i++) {
      strncpy(plates[i].ingredient, "none", INGREDIENT_LENGTH - 1);
      plates[i].ingredient[INGREDIENT_LENGTH - 1] = '\0';

      plates[i].chopCount = 0;
      plates[i].cookCount = 0;
      plates[i].status = LEC_PLATE_ACTIVE;
      plates[i].updatedAtMs = millis();
    }

    xSemaphoreGive(xMutex);
  }

  Serial.println("All plates reset.");
}

int findPlateIndex(const char* rfid) {
  if (!rfid || rfid[0] == '\0') return -1;

  for (int i = 0; i < plateCount; i++) {
    if (strncmp(plates[i].rfid, rfid, RFID_LENGTH) == 0) {
      return i;
    }
  }

  return -1;
}

int findOrCreatePlate(const char* rfid) {
  int existing = findPlateIndex(rfid);

  if (existing != -1) {
    return existing;
  }

  if (plateCount >= MAX_PLATES) {
    return -1;
  }

  int index = plateCount++;

  lecInitPlate(plates[index]);
  lecSetPlateRfid(plates[index], rfid);
  lecSetPlateIngredient(plates[index], "none");

  plates[index].chopCount = 0;
  plates[index].cookCount = 0;
  plates[index].status = LEC_PLATE_ACTIVE;

  return index;
}

/*************************************************************
  RFID
*************************************************************/

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

bool PICC_IsAnyCardPresent() {
  byte bufferATQA[2];
  byte bufferSize = sizeof(bufferATQA);

  rfid.PCD_WriteRegister(rfid.TxModeReg, 0x00);
  rfid.PCD_WriteRegister(rfid.RxModeReg, 0x00);
  rfid.PCD_WriteRegister(rfid.ModWidthReg, 0x26);

  MFRC522::StatusCode result = rfid.PICC_WakeupA(bufferATQA, &bufferSize);

  return result == MFRC522::STATUS_OK || result == MFRC522::STATUS_COLLISION;
}

void resetRFIDReader() {
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}

/*************************************************************
  PI SCOREBOARD
*************************************************************/

int getTimeLeftInSeconds() {
  if (!gameRunning || roundStartTime == 0) {
    return 0;
  }

  unsigned long elapsed = millis() - roundStartTime;

  if (elapsed >= roundDuration) {
    return 0;
  }

  return static_cast<int>((roundDuration - elapsed) / 1000);
}

String getStarThresholdString() {
  return String(starThresholds[0]) + "," +
         String(starThresholds[1]) + "," +
         String(starThresholds[2]);
}

void startPiScoreboardTask() {
  piQueue = xQueueCreate(8, sizeof(PiJob));

  if (piQueue == nullptr) {
    Serial.println("Failed to create Pi scoreboard queue.");
    return;
  }

  xTaskCreatePinnedToCore(
    piScoreboardTask,
    "PiScoreboardTask",
    8192,
    nullptr,
    1,
    &piTaskHandle,
    0
  );

  Serial.println("Pi scoreboard async task started.");
}

void piScoreboardTask(void* parameter) {
  PiJob job;

  while (true) {
    if (xQueueReceive(piQueue, &job, portMAX_DELAY) == pdTRUE) {
      if (!connectedToScoreboard || WiFi.status() != WL_CONNECTED) {
        continue;
      }

      switch (job.type) {
        case PI_JOB_START:
          performPiStartGame(job);
          break;

        case PI_JOB_UPDATE:
          performPiUpdate(job);
          break;

        case PI_JOB_END:
          performPiEndGame(job);
          break;

        case PI_JOB_STATS:
          performPiStatsPost(job);
          break;
      }
    }
  }
}

void queuePiStartGame(unsigned long durationSec) {
  if (!connectedToScoreboard || piQueue == nullptr) {
    return;
  }

  PiJob job = {};
  job.type = PI_JOB_START;
  job.durationSec = durationSec;

  xQueueSend(piQueue, &job, 0);
}

void queuePiEndGame(const String& reason, int finalScore) {
  if (!connectedToScoreboard || piQueue == nullptr) {
    return;
  }

  PiJob job = {};
  job.type = PI_JOB_END;
  job.finalScore = finalScore;

  strncpy(job.reason, reason.c_str(), sizeof(job.reason) - 1);
  job.reason[sizeof(job.reason) - 1] = '\0';

  xQueueSend(piQueue, &job, 0);
}

void queuePiUpdate(int score, int timeLeftSec) {
  if (!connectedToScoreboard || piQueue == nullptr) {
    return;
  }

  unsigned long now = millis();

  if (now - lastPiUpdateQueuedAt < PI_UPDATE_QUEUE_INTERVAL_MS) {
    return;
  }

  lastPiUpdateQueuedAt = now;

  PiJob job = {};
  job.type = PI_JOB_UPDATE;
  job.score = score;
  job.timeLeftSec = timeLeftSec;

  if (currentRecipeValid) {
    strncpy(job.currentRecipe, currentRecipe.name, sizeof(job.currentRecipe) - 1);
    job.currentRecipe[sizeof(job.currentRecipe) - 1] = '\0';
  }

  if (nextRecipeValid) {
    strncpy(job.nextRecipe, nextRecipe.name, sizeof(job.nextRecipe) - 1);
    job.nextRecipe[sizeof(job.nextRecipe) - 1] = '\0';
  }

  xQueueSend(piQueue, &job, 0);
}

String jsonEscape(const String& value) {
  String escaped = "";

  for (int i = 0; i < value.length(); i++) {
    char c = value.charAt(i);

    if (c == '"') {
      escaped += "\\\"";
    } else if (c == '\\') {
      escaped += "\\\\";
    } else if (c == '\n') {
      escaped += "\\n";
    } else if (c == '\r') {
      escaped += "\\r";
    } else if (c == '\t') {
      escaped += "\\t";
    } else {
      escaped += c;
    }
  }

  return escaped;
}

String buildStatsPayload(const String& reason, int finalScore) {
  unsigned long gameDurationMs = 0;

  if (statsGameStartedAtMs > 0 && statsGameEndedAtMs > statsGameStartedAtMs) {
    gameDurationMs = statsGameEndedAtMs - statsGameStartedAtMs;
  }

  bool completed = statsRound2.completed;

  int recipesCompleted =
    statsRound1.completedServes +
    statsRound2.completedServes;

  int stars = 0;

  if (finalScore >= starThresholds[0]) stars = 1;
  if (finalScore >= starThresholds[1]) stars = 2;
  if (finalScore >= starThresholds[2]) stars = 3;

  String escapedReason = jsonEscape(reason);

  String json = "{";

  json += "\"startedAtMs\":";
  json += String(statsGameStartedAtMs);
  json += ",";

  json += "\"endedAtMs\":";
  json += String(statsGameEndedAtMs);
  json += ",";

  json += "\"gameDurationMs\":";
  json += String(gameDurationMs);
  json += ",";

  json += "\"completed\":";
  json += completed ? "true" : "false";
  json += ",";

  json += "\"finalScore\":";
  json += String(finalScore);
  json += ",";

  json += "\"stars\":";
  json += String(stars);
  json += ",";

  json += "\"recipesCompleted\":";
  json += String(recipesCompleted);
  json += ",";

  json += "\"fireCount\":";
  json += String(statsFireCount);
  json += ",";

  json += "\"failureReason\":";

  if (completed) {
    json += "null";
  } else {
    json += "\"";
    json += escapedReason;
    json += "\"";
  }

  json += ",";

  json += "\"rounds\":[";

  if (statsRound1.played) {
    json += "{";
    json += "\"roundNumber\":1,";
    json += "\"startedAtMs\":";
    json += String(statsRound1.startedAtMs);
    json += ",";
    json += "\"endedAtMs\":";
    json += String(statsRound1.endedAtMs);
    json += ",";
    json += "\"durationMs\":";
    json += String(statsRound1.durationMs);
    json += ",";
    json += "\"targetServes\":";
    json += String(statsRound1.targetServes);
    json += ",";
    json += "\"completedServes\":";
    json += String(statsRound1.completedServes);
    json += ",";
    json += "\"completed\":";
    json += statsRound1.completed ? "true" : "false";
    json += "}";
  }

  if (statsRound1.played && statsRound2.played) {
    json += ",";
  }

  if (statsRound2.played) {
    json += "{";
    json += "\"roundNumber\":2,";
    json += "\"startedAtMs\":";
    json += String(statsRound2.startedAtMs);
    json += ",";
    json += "\"endedAtMs\":";
    json += String(statsRound2.endedAtMs);
    json += ",";
    json += "\"durationMs\":";
    json += String(statsRound2.durationMs);
    json += ",";
    json += "\"targetServes\":";
    json += String(statsRound2.targetServes);
    json += ",";
    json += "\"completedServes\":";
    json += String(statsRound2.completedServes);
    json += ",";
    json += "\"completed\":";
    json += statsRound2.completed ? "true" : "false";
    json += "}";
  }

  json += "]";
  json += "}";

  return json;
}

void queuePiStatsPost(const String& payload) {
  if (!connectedToScoreboard || piQueue == nullptr) {
    Serial.println("Stats post skipped: scoreboard/Pi not connected.");
    return;
  }

  PiJob job = {};
  job.type = PI_JOB_STATS;

  strncpy(job.statsJson, payload.c_str(), sizeof(job.statsJson) - 1);
  job.statsJson[sizeof(job.statsJson) - 1] = '\0';

  BaseType_t queued = xQueueSend(piQueue, &job, 0);

  if (queued != pdTRUE) {
    Serial.println("Stats post skipped: Pi queue full.");
    return;
  }

  Serial.println("Stats post queued.");
}

void performPiStatsPost(const PiJob& job) {
  HTTPClient http;
  http.setTimeout(PI_HTTP_TIMEOUT_MS);

  bool began = http.begin(STATS_RESULT_URL);

  if (!began) {
#if LEC_DEBUG
    Serial.println("Stats HTTP begin failed.");
#endif
    return;
  }

  http.addHeader("Content-Type", "application/json");

  int httpCode = http.POST(String(job.statsJson));

  http.end();

#if LEC_DEBUG
  Serial.print("Async stats HTTP: ");
  Serial.println(httpCode);
#endif
}

void performPiStartGame(const PiJob& job) {
  HTTPClient http;
  http.setTimeout(PI_HTTP_TIMEOUT_MS);

  http.begin(SCOREBOARD_START_URL);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String postData =
    "duration=" + String(job.durationSec) +
    "&star_thresholds=" + getStarThresholdString() +
    "&max_score=" + String(starMaxScore);

  int httpCode = http.POST(postData);
  http.end();

#if LEC_DEBUG
  Serial.print("Async start_game HTTP: ");
  Serial.println(httpCode);
#endif
}

void performPiEndGame(const PiJob& job) {
  HTTPClient http;
  http.setTimeout(PI_HTTP_TIMEOUT_MS);

  http.begin(SCOREBOARD_END_URL);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String postData =
    "score=" + String(job.finalScore) +
    "&reason=" + String(job.reason);

  int httpCode = http.POST(postData);
  http.end();

#if LEC_DEBUG
  Serial.print("Async end_game HTTP: ");
  Serial.println(httpCode);
#endif
}

void performPiUpdate(const PiJob& job) {
  HTTPClient http;
  http.setTimeout(PI_HTTP_TIMEOUT_MS);

  http.begin(SCOREBOARD_UPDATE_URL);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String postData =
    "score=" + String(job.score) +
    "&time_left=" + String(job.timeLeftSec) +
    "&star_thresholds=" + getStarThresholdString() +
    "&max_score=" + String(starMaxScore) +
    "&current_recipe=" + String(job.currentRecipe) +
    "&next_recipe=" + String(job.nextRecipe);

  int httpCode = http.POST(postData);
  http.end();

#if LEC_DEBUG
  Serial.print("Async scoreboard update HTTP: ");
  Serial.println(httpCode);
#endif
}

void notifyPiStartGame(unsigned long durationSec) {
  queuePiStartGame(durationSec);
}

void notifyPiEndGame(const String& reason, int finalScore) {
  queuePiEndGame(reason, finalScore);
}

void updateScoreAndTimeOnPi(int score, int timeLeftSec) {
  queuePiUpdate(score, timeLeftSec);
}

/*************************************************************
  LED FEEDBACK
*************************************************************/

void blinkButtonLED(int ledPin) {
  digitalWrite(ledPin, HIGH);
  delay(100);
  digitalWrite(ledPin, LOW);
}