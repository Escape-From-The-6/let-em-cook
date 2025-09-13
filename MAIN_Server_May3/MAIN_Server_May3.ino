/*************************************************************
 MAIN_Server_Final_Updated_With_RoleAssignment_On_GameStart.ino

 This sketch serves as the main "server" for an ESP-NOW-based
 escape room game, with the following responsibilities:

   • Handling round-based game logic (Round 1, 2, 3).
   • Managing RFID data (adding/removing plates).
   • Receiving data from client stations (chop, cook, etc.).
   • Tracking and updating player score.
   • Connecting to Raspberry Pi's AP for scoreboard updates
     (start_game, end_game, score/time updates) via HTTP POST.
   • Sending role assignments & controlling the flow of each round.

 It uses:
   – Arduino core for ESP32
   – ESP-NOW for wireless communication with stations
   – HTTPClient for scoreboard POST requests
   – MFRC522 library for RFID
   – DYPlayerArduino for audio feedback
   – etc.

*************************************************************/

#include <esp_now.h>
#include <WiFi.h>
#include <map>
#include <string>
#include <MFRC522.h>
#include <SPI.h>
#include <DYPlayerArduino.h>
#include <HTTPClient.h> // Needed for HTTP POST to Pi
#include <ArduinoOTA.h>  // Added OTA support

/*************************************************************
 *  ADD THESE LINES **ONCE**, right after your last #include
 *  (before any code that calls Serial).
 *************************************************************/

//#define DEBUG   // <-- uncomment when you DO want Serial output

#ifndef DEBUG
class NullSerial {
public:
  template<typename... Args> void begin(Args...)   {}
  template<typename... Args> void print(Args...)   {}
  template<typename... Args> void println(Args...) {}
  template<typename... Args> void printf(const char*, ...) {}
};
static NullSerial  __nullSerial;
#define Serial     __nullSerial     // transparently replaces every Serial call
#endif

/*************************************************************
    WIFI CREDENTIALS (to connect to Pi's AP)
*************************************************************/
const char* scoreboardSSID = "MyScoreboardAP";     // Adjust to match your Pi's AP SSID
const char* scoreboardPassword = "MySecretPassword"; // Adjust to match your Pi's AP password

// Flag to track if we're connected to Pi's AP
bool connectedToScoreboard = false;

/*************************************************************
    GAME CONFIGURATIONS AND LIMITS
*************************************************************/
// Maximum number of RFID "plates"
#define MAX_PLATES 10

// Maximum lengths for fixed-size strings
#define RFID_LENGTH 20
#define INGREDIENT_LENGTH 30
#define REQUEST_TYPE_LENGTH 25
#define MAX_RECIPE_NAME_LENGTH 30

// Sentinel for invalid chop/cook counts
#define INVALID_COUNT -1

// Long press duration (for adding/removing RFID)
#define LONG_PRESS_DURATION 3000 // 3 seconds

// Debounce delay (for button presses)
#define DEBOUNCE_DELAY 30 // 30 ms

// Max chop/cook
#define MAX_CHOP_COUNT 5
#define MAX_COOK_COUNT 10

/*************************************************************
    STAR LOGIC CONFIGURATION
*************************************************************/
// For example, 1 star if score >=2, 2 stars if >=5, 3 stars if >=7
int starThresholds[3] = {2, 4, 6};
int starMaxScore = 6;

// Helper to build "2,5,7"
String getStarThresholdString() {
  return String(starThresholds[0]) + "," + String(starThresholds[1]) + "," + String(starThresholds[2]);
}


/*************************************************************
    STRUCTS AND ENUMS
*************************************************************/
// The message structure used for ESP-NOW
typedef struct struct_message {
  char rfid[RFID_LENGTH];                 // RFID tag ID
  char ingredient[INGREDIENT_LENGTH];     // Ingredient name
  int chopCount;                          // Chop count
  int cookCount;                          // Cook count
  int playerScoreDelta;                   // Score changes
  bool reset;                             // Reset flag
  char requestType[REQUEST_TYPE_LENGTH];  // e.g. "DataRequest"
  int role;                               // ClientRole
  char recipeName[MAX_RECIPE_NAME_LENGTH];// For recipes
} struct_message;

// Enum for client roles
enum ClientRole {
  ROLE_NONE = 0,
  ROLE_GARBAGE = 1,
  ROLE_CHOP_STATION = 2,
  ROLE_COOK_STATION = 3,
  ROLE_MIX_STATION = 4,
  ROLE_ORDER_SERVE_STATION = 5,
  ROLE_INGREDIENT_STATION = 6,
  ROLE_OVEN_STATION = 7
};

// For request types
enum RequestType {
  DATA_REQUEST,
  DATA_UPDATE,
  ROLE_REQUEST,
  CURRENT_RECIPE_REQUEST,
  CURRENT_RECIPE_RESPONSE,
  SELECT_NEW_RECIPE,
  REINITIALIZE,
  NEW_RECIPE,
  ROLE_ASSIGNMENT,
  BURNT,
  ON_FIRE,
  EXTINGUISH_FIRE,

  UNKNOWN_REQUEST
};

/*************************************************************
    NEW ROUND LOGIC – State, Timers, Goals
*************************************************************/
// Round states
enum RoundState {
  ROUND_NONE = 0,  // no active round
  ROUND_1,
  ROUND_2,
  ROUND_3,
  ROUND_DONE
};

RoundState currentRound = ROUND_NONE;

// For each round, we track how many correct serves have occurred
int correctServesInRound = 0;

// how many dishes must be served to finish each round
const int serveTargets[] = {
  0,  // ROUND_NONE (unused)
  2,  // ROUND_1: need 2 serves
  2,  // ROUND_2: need 2 serves
  99  // ROUND_3: potential for bonus round
};

// Instead of a single gameRunning, we use it to indicate if *any* round is active
bool gameRunning = false;

// Add near your other globals:
bool fifteenSecondWarningPlayed = false;

// We'll keep track of the time each round started + how long it should last
unsigned long roundStartTime = 0;
unsigned long roundDuration = 0;

// round transitions
bool   pendingRoundTransition = false;
int    nextRoundNumber        = 0;
unsigned long transitionRequestTime = 0;
const unsigned long TRANSITION_DELAY = 2000;  // wait 2 s for video

// non-blocking “finish start round” helpers
bool     roundStartPending     = false;
unsigned long roundStartQueuedAt = 0;
const unsigned long ROLE_ASSIGN_DELAY_MS = 4800;

// ── place near the other “pending/queued” flags ──
bool     fireRecipePending      = false;
unsigned long fireRecipeQueuedAt = 0;

// ── one‑shot re‑broadcast of NewRecipe ─────────────────────
bool           recipeRebroadcastPending = false;
unsigned long  recipeRebroadcastAt      = 0;   // millis target
// ───────────────────────────────────────────────────────────

// — delayed broadcast after a correct serve —
bool           serveSuccessRecipePending = false;
unsigned long  serveSuccessRecipeAt      = 0;
const unsigned long SUCCESS_DELAY_MS     = 2500;   // 2.5 s

/*************************************************************
    PIN DEFINITIONS
*************************************************************/
// RFID reader pins
#define SS_PIN 21
#define RST_PIN 22

// Buttons (assume active LOW)
#define GREEN_BUTTON_PIN 13
#define RED_BUTTON_PIN   4
#define FIRE_BUTTON_PIN  35

// Button LEDs
#define GREEN_BUTTON_LED_PIN 27
#define RED_BUTTON_LED_PIN   26
#define FIRE_LED_PIN         25

// Audio module pins (DY-HV20T)
#define AUDIO_TX 16 // TX2
#define AUDIO_RX 17 // RX2

// Sprite media player pins
#define SPRITE_TX 32 // TX1
#define SPRITE_RX 33 // RX1

/*************************************************************
    GLOBAL HARDWARE INSTANCES
*************************************************************/
// RFID reader
MFRC522 rfid(SS_PIN, RST_PIN);

// Audio module
HardwareSerial audioSerial(2);
DY::Player audioModule(&audioSerial);

// Sprite media player
HardwareSerial spriteSerial(1);

/*************************************************************
    GAME VARIABLES
*************************************************************/
// This array holds all RFID data for "plates"
struct_message rfidDataArray[MAX_PLATES];
int rfidDataCount = 0; // how many in use

// The player score
int playerScore = 0;

// the players star score
int starScore = 0;

// For concurrency control
SemaphoreHandle_t xMutex;

// Maps from MAC -> assigned role
std::map<std::string, ClientRole> clientRoles;
std::map<std::string, ClientRole> macToRoleMap = {
  {"C4:DE:E2:5B:81:58", ROLE_NONE},        // Server
  {"E4:65:B8:DA:17:8C", ROLE_INGREDIENT_STATION},        // Ingredient
  {"D0:EF:76:31:63:F8", ROLE_NONE},
  {"D0:EF:76:33:59:74", ROLE_NONE},
  {"D0:EF:76:30:58:EC", ROLE_NONE},
  {"D0:EF:76:34:04:1C", ROLE_NONE},
  {"08:A6:F7:B1:16:20", ROLE_NONE},
  {"C4:DE:E2:9C:8E:94", ROLE_NONE}
};

/*************************************************************
    RECIPE DATA
*************************************************************/
// Each ingredient requirement in a recipe
struct IngredientRequirement {
  char name[INGREDIENT_LENGTH];
  bool requiresChop;
  int requiredChopCount;
  bool requiresCook;
  int requiredCookCountMin;
  int requiredCookCountMax;
};

// A recipe is a collection of IngredientRequirements
#define MAX_RECIPE_INGREDIENTS 3

struct Recipe {
  char name[MAX_RECIPE_NAME_LENGTH];
  int numIngredients;
  IngredientRequirement ingredients[MAX_RECIPE_INGREDIENTS];
  bool bakeable; 
  int requiredBakeCountMin;
  int requiredBakeCountMax;
};

// Our recipes
Recipe recipes[] = {
    {
        "garden salad",
        2,
        {
            {"lettuce", true, MAX_CHOP_COUNT, false, 0, 0},
            {"tomato", true, MAX_CHOP_COUNT, false, 0, 0}
        },
        false, 0, 0
    },
    {
        "apple salad",
        2,
        {
            {"apple", true, MAX_CHOP_COUNT, false, 0, 0},
            {"lettuce", true, MAX_CHOP_COUNT, false, 0, 0}
        },
        false, 0, 0
    },
    {
        "tomatoes and cheese",
        2,
        {
            {"tomato", true, MAX_CHOP_COUNT, false, 0, 0},
            {"cheese", true, MAX_CHOP_COUNT, false, 0, 0}
        },
        false, 0, 0
    },
    {
        "apples and cheese",
        2,
        {
            {"apple", true, MAX_CHOP_COUNT, false, 0, 0},
            {"cheese", true, MAX_CHOP_COUNT, false, 0, 0}
        },
        false, 0, 0
    },
    {
        "cheese salad",
        2,
        {
            {"lettuce", true, MAX_CHOP_COUNT, false, 0, 0},
            {"cheese", true, MAX_CHOP_COUNT, false, 0, 0}
        },
        false, 0, 0
    },
    {
        "tomato pasta with cheese",
        3,
        {
            {"tomato", false, 0, true, 7, 9},
            {"dough", false, 0, true, 7, 9},
            {"cheese", true, MAX_CHOP_COUNT, false, 0, 0}
        },
        false, 0, 0
    },
    {
        "taco with cheese",
        3,
        {
            {"meat", false, 0, true, 7, 9},
            {"dough", false, 0, true, 7, 9},
            {"cheese", true, MAX_CHOP_COUNT, false, 0, 0}
        },
        false, 0, 0
    },
    {
        "taco with lettuce",
        3,
        {
            {"meat", false, 0, true, 7, 9},
            {"dough", false, 0, true, 7, 9},
            {"lettuce", true, MAX_CHOP_COUNT, false, 0, 0}
        },
        false, 0, 0
    },
    {
        "taco with tomato",
        3,
        {
            {"meat", false, 0, true, 7, 9},
            {"dough", false, 0, true, 7, 9},
            {"tomato", true, MAX_CHOP_COUNT, false, 0, 0}
        },
        false, 0, 0
    },
    {
        "beef stew with cheese",
        3,
        {
            {"meat", false, 0, true, 7, 9},
            {"tomato", false, 0, true, 7, 9},
            {"cheese", true, MAX_CHOP_COUNT, false, 0, 0}
        },
        false, 0, 0
    },
    {
        "beef patty",
        2,
        {
            {"meat", false, MAX_CHOP_COUNT, true, 7, 9},
            {"dough", true, MAX_CHOP_COUNT, false, 0, 0},
        },
        true, 4, 4
    },
    {
        "tomato pizza",
        2,
        {
            {"tomato", false, MAX_CHOP_COUNT, true, 7, 9},
            {"dough", true, MAX_CHOP_COUNT, false, 0, 0},
        },
        true, 4, 4
    },
    {
        "baked mac and cheese",
        2,
        {
            {"dough", false, MAX_CHOP_COUNT, true, 7, 9},
            {"cheese", true, MAX_CHOP_COUNT, false, 0, 0},
        },
        true, 4, 4
    },
    {
        "apple pie",
        2,
        {
            {"apple", false, MAX_CHOP_COUNT, true, 7, 9},
            {"dough", true, MAX_CHOP_COUNT, false, 0, 0},
        },
        true, 4, 4
    }
};

const int totalRecipes = sizeof(recipes) / sizeof(recipes[0]);

// We'll store a pointer or index for the "current recipe" in each round
Recipe currentRecipe;

// holds the recipe we’ll use immediately after currentRecipe
Recipe nextRecipe;

/*************************************************************
    BUTTON STATE VARIABLES
*************************************************************/
// For green button
unsigned long greenButtonPressStartTime = 0;
bool greenButtonLongPressHandled = false;

// For red button
unsigned long redButtonPressStartTime = 0;
bool redButtonLongPressHandled = false;

// for fire button
unsigned long fireButtonPressStartTime = 0;

/*************************************************************
    DEFERRED SCORE UPDATE TO PI
*************************************************************/
volatile bool scoreUpdatePending = false;
int pendingScore = 0;
int pendingTimeLeft = 0;

/*************************************************************
    NEW FIRE LOGIC - PIN DEFINITIONS & GLOBALS
*************************************************************/
bool onFire = false;
unsigned long lastFireButtonBlinkTime = 0;
bool fireLedState = LOW;

/*************************************************************
    FUNCTION PROTOTYPES
*************************************************************/
void initializeHardware();
bool readRFID(char* rfidUID);
void blinkButtonLED(int ledPin);
int findRFIDIndex(const char* rfid);
bool addRFIDData(const struct_message* data);
bool addRFID(const char* rfidUID);
bool removeRFIDFromArray(const char* rfidUID);
void sendDataBackToClient(const uint8_t* mac_addr, struct_message* message);
String macToString(const uint8_t* mac_addr);
bool stringToMAC(const std::string& macStr, uint8_t* macBytes);
RequestType getRequestType(const char* requestTypeStr);
void sendNewRecipeToRelevantStations(bool scheduleRepeat = true);
void initializeRFIDData();
void reinitializeRFIDData();
void sendReinitializeToAllClients();
void sendRoleAssignmentToAllClients();
void addAllPeers();
int getTimeLeftInSeconds();
void notifyPiStartGame(unsigned long durationSec);
void notifyPiEndGame(String reason, int finalScore);
void updateScoreAndTimeOnPi(int newScore, int timeLeftSec);

// NEW round logic
void startRound1();
void startRound2();
void startRound3();
void endCurrentRound();

// Role assignment for each round
void assignRolesForRound1();
void assignRolesForRound2();
void assignRolesForRound3();

// Random recipe picks
void pickRandomRecipeForRound1();
void pickRandomRecipeForRound2();
void pickRandomRecipeForRound3();
void pickNextRecipeForRound1();
void pickNextRecipeForRound2();
void pickNextRecipeForRound3();

/*************************************************************
    SETUP FUNCTION
*************************************************************/
void setup() {
  // Start serial
  Serial.begin(115200);

  /*************************************************
   * Quick scan for debugging (optional)
   *************************************************/
  Serial.println("Scanning for WiFi networks...");
  int n = WiFi.scanNetworks();
  Serial.println("Scan done.");
  if (n == 0) {
    Serial.println("No networks found");
  } else {
    for (int i = 0; i < n; i++) {
      Serial.print(i + 1);
      Serial.print(": ");
      Serial.print(WiFi.SSID(i));
      Serial.print(" (");
      Serial.print(WiFi.RSSI(i));
      Serial.print(") ");
      if (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) {
        Serial.println("Open");
      } else {
        Serial.println("Encrypted");
      }
      delay(10);
    }
  }

  /*************************************************
   * Connect to Pi's AP
   *************************************************/
  Serial.println("\nConnecting to scoreboard AP...");
  WiFi.mode(WIFI_STA);
  delay(100);

  // Optional: pass channel (6) if you know Pi AP is on 6
  WiFi.begin(scoreboardSSID, scoreboardPassword, 6);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConnected to scoreboard AP!");
  connectedToScoreboard = true;

  /*************************************************
   * Initialize OTA
   *************************************************/
  ArduinoOTA.onStart([]() {
    Serial.println("OTA Update Start");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nOTA Update End");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("OTA Progress: %u%%\n", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  ArduinoOTA.begin();
  Serial.println("OTA Initialized");

  /*************************************************
   * Initialize mutex
   *************************************************/
  xMutex = xSemaphoreCreateMutex();
  if (xMutex == NULL) {
    Serial.println("Failed to create mutex.");
    return;
  }

  /*************************************************
   * Initialize ESP-NOW
   *************************************************/
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }

  // Register receive callback
  esp_now_register_recv_cb(onDataRecv);

  // Add peers
  addAllPeers();

  // Initialize hardware
  initializeHardware();

  // Initialize RFID data to default
  initializeRFIDData();

  // Set currentRound to none
  currentRound = ROUND_NONE;

  // We won't automatically start a round here.
  // We'll wait for the green button short press to do `startRound1()`.

  Serial.println("Server setup complete.");
}

/*************************************************************
    LOOP FUNCTION
*************************************************************/
void loop() {
  // Handle OTA updates
  ArduinoOTA.handle();

  /*************************************************
   * Handle GREEN BUTTON
   *************************************************/
  if (digitalRead(GREEN_BUTTON_PIN) == LOW) {
    // Button pressed
    if (greenButtonPressStartTime == 0) {
      // Start of press
      greenButtonPressStartTime = millis();
      greenButtonLongPressHandled = false;
    } else {
      // Check how long
      unsigned long pressDuration = millis() - greenButtonPressStartTime;
      if (pressDuration >= LONG_PRESS_DURATION && !greenButtonLongPressHandled) {
        // Long press detected => Add RFID
        greenButtonLongPressHandled = true;
        Serial.println("Long press detected on Green button. Adding RFID...");

        char newRFID[RFID_LENGTH];
        if (readRFID(newRFID)) {
          Serial.print("Read RFID: ");
          Serial.println(newRFID);
          if (addRFID(newRFID)) {
            blinkButtonLED(GREEN_BUTTON_LED_PIN);
          } else {
            blinkButtonLED(GREEN_BUTTON_LED_PIN);
          }
        } else {
          Serial.println("Failed to read RFID during Green button long press.");
          blinkButtonLED(GREEN_BUTTON_LED_PIN);
        }
      }
    }
  } else {
    // Button released
    if (greenButtonPressStartTime != 0) {
      unsigned long pressDuration = millis() - greenButtonPressStartTime;
      if (!greenButtonLongPressHandled && pressDuration < LONG_PRESS_DURATION) {
        // Blink Button LED
        blinkButtonLED(GREEN_BUTTON_LED_PIN);
        // Short press => start Round 1 if not in a round
        if (!gameRunning && currentRound == ROUND_NONE) {
          startRound1();
        } else {
          // --- NEW LOGIC: If a round is active, pick new recipe:
          if (gameRunning && currentRound != ROUND_NONE) {
             Serial.println("Green button short press => selecting a NEW recipe for this round...");

             // Acquire mutex if needed
             if (xSemaphoreTake(xMutex, portMAX_DELAY)) {
                // Choose from correct subset:
                switch (currentRound) {
                  case ROUND_1:
                    pickRandomRecipeForRound1();
                    break;
                  case ROUND_2:
                    pickRandomRecipeForRound2();
                    break;
                  case ROUND_3:
                    pickRandomRecipeForRound3();
                    break;
                  default:
                    // or pick from all recipes if not strictly in a round
                    // pickRandomRecipeFromAll();
                    break;
                }
                // Now broadcast to relevant stations
                sendNewRecipeToRelevantStations();

                xSemaphoreGive(xMutex);
             } else {
                Serial.println("Failed to acquire mutex for new recipe selection.");
             }
         }
         // else if (!gameRunning) { // Optionally do something else }

      
          // If we are in the middle of a round, let's do the old "select new recipe" approach?
          // Or you can decide it does nothing if a round is running.
          // For demonstration, let's keep old logic:
          Serial.println("Short press on Green while round in progress => picked new recipe.");
          // You could do something else here if you want.
        }
      }
      // Reset
      greenButtonPressStartTime = 0;
      greenButtonLongPressHandled = false;
    }
  }

  /*************************************************
   * Handle RED BUTTON
   *************************************************/
  if (digitalRead(RED_BUTTON_PIN) == LOW) {
    // Button pressed
    if (redButtonPressStartTime == 0) {
      redButtonPressStartTime = millis();
      redButtonLongPressHandled = false;
    } else {
      unsigned long pressDuration = millis() - redButtonPressStartTime;
      if (pressDuration >= LONG_PRESS_DURATION && !redButtonLongPressHandled) {
        // Long press => remove RFID
        redButtonLongPressHandled = true;
        Serial.println("Long press detected on Red button. Removing RFID...");

        char rfidToRemove[RFID_LENGTH];
        if (readRFID(rfidToRemove)) {
          Serial.print("Read RFID: ");
          Serial.println(rfidToRemove);
          if (removeRFIDFromArray(rfidToRemove)) {
            blinkButtonLED(RED_BUTTON_LED_PIN);
          } else {
            blinkButtonLED(RED_BUTTON_LED_PIN);
          }
        } else {
          Serial.println("Failed to read RFID during Red button long press.");
          blinkButtonLED(RED_BUTTON_LED_PIN);
        }
      }
    }
  } else {
    // Button released
    if (redButtonPressStartTime != 0) {
      unsigned long pressDuration = millis() - redButtonPressStartTime;
      if (!redButtonLongPressHandled && pressDuration < LONG_PRESS_DURATION) {
        // Short press => end round/game if a round is running
        if (gameRunning) {
          Serial.println("Short press on Red => forcibly ending the game in the middle of a round.");
          endGame("Red Button Press");
        } else {
          Serial.println("Short press on Red, no round running => do nothing special.");
          blinkButtonLED(RED_BUTTON_LED_PIN);
        }
      }
      // Reset
      redButtonPressStartTime = 0;
      redButtonLongPressHandled = false;
    }
  }

  /*************************************************
   * Update score/timer
   *************************************************/
  static unsigned long lastCountdownUpdate = 0;
  if (gameRunning) {
      unsigned long now = millis();
      if (now - lastCountdownUpdate >= 1000) {
          lastCountdownUpdate = now;
          int timeLeft = getTimeLeftInSeconds();
          
          // Here, call updateScoreAndTimeOnPi:
          updateScoreAndTimeOnPi(playerScore, timeLeft);

          //  ─── play track 2 at 15 seconds left ───
          if (timeLeft == 15 && !fifteenSecondWarningPlayed) {
            audioModule.stop();
            audioModule.playSpecified(2);
            fifteenSecondWarningPlayed = true;
          }
      }
  }

  // ── finish any pending startRound work ──
  if (roundStartPending && millis() - roundStartQueuedAt >= ROLE_ASSIGN_DELAY_MS) {
    roundStartPending = false;

    // now it’s been 4.8 s since assignRoles()
    // do exactly what you used to do at the end of startRoundX():

    // 1) pick the first recipe
    switch (currentRound) {
      case ROUND_1:
        pickRandomRecipeForRound1();
        if (serveTargets[currentRound] > 1) pickNextRecipeForRound1();
        gameRunning = true;
        roundStartTime = millis();
        notifyPiStartGame(roundDuration / 1000);
        break;
      case ROUND_2:
        gameRunning = true;
        break;
      case ROUND_3:
        gameRunning = true;
        break;
      default:
        break;
    }

    // 2) send the NewRecipe out
    sendNewRecipeToRelevantStations();

    Serial.println("Finished startRound sequence after 4.8 s delay.");
  }

  // if a round transition is pending and its delay has elapsed…
  if (pendingRoundTransition && (millis() - transitionRequestTime >= TRANSITION_DELAY)) {
    pendingRoundTransition = false;
    if (nextRoundNumber == 2) startRound2();
    else if (nextRoundNumber == 3) startRound3();
  }

  // — first NewRecipe after a successful serve —
  if (serveSuccessRecipePending && millis() >= serveSuccessRecipeAt) {
      serveSuccessRecipePending = false;
      sendNewRecipeToRelevantStations();   // goes out once
  }

  // ── timed re‑broadcast of NewRecipe ─────────────────────────
  if (recipeRebroadcastPending && millis() >= recipeRebroadcastAt) {
      recipeRebroadcastPending = false;
      sendNewRecipeToRelevantStations(false);   // ← no further repeats
  }

  // ── after role‑change on fire extinguish ──
  if (fireRecipePending && millis() - fireRecipeQueuedAt >= ROLE_ASSIGN_DELAY_MS) {
      fireRecipePending = false;
      sendNewRecipeToRelevantStations();   // pushes *currentRecipe*
  }

  /*************************************************
   * Check round timer
   *************************************************/
  if (gameRunning && (millis() - roundStartTime >= roundDuration)) {
    // Round time expired => check pass/fail
    endCurrentRound();
  }

  /*************************************************
   * Handle Deferred Score Update
   *************************************************/
  if (scoreUpdatePending) {
    int localScore = pendingScore;
    int localTimeLeft = pendingTimeLeft;
    scoreUpdatePending = false;
    updateScoreAndTimeOnPi(localScore, localTimeLeft);
  }

  /************************************************************
  * NEW FIRE LOGIC - Handle Fire Button
  ************************************************************/
  if (onFire) {
    // Blink the FIRE_LED_PIN
    if (millis() - lastFireButtonBlinkTime >= 500) {
      lastFireButtonBlinkTime = millis();
      fireLedState = !fireLedState;
      digitalWrite(FIRE_LED_PIN, fireLedState);
    }

    // Debounced short press to extinguish
    if (digitalRead(FIRE_BUTTON_PIN) == LOW) {
      if (fireButtonPressStartTime == 0) {
        // Button press started
        fireButtonPressStartTime = millis();
      }
    } else {
      if (fireButtonPressStartTime != 0) {
        unsigned long pressDuration = millis() - fireButtonPressStartTime;
        if (pressDuration >= DEBOUNCE_DELAY) {
          onFire = false;
          digitalWrite(FIRE_LED_PIN, LOW);

          // Broadcast "ExtinguishFire" to all
          struct_message extMsg;
          memset(&extMsg, 0, sizeof(extMsg));
          strncpy(extMsg.requestType, "ExtinguishFire", REQUEST_TYPE_LENGTH - 1);

          for (const auto& entry : macToRoleMap) {
            uint8_t macBytes[6];
            if (stringToMAC(entry.first, macBytes)) {
              sendDataBackToClient(macBytes, &extMsg);
            }
          }
          // Reinit RFID data
          reinitializeRFIDData();

          delay(2500);

          // 1) Keep the SAME recipe but reshuffle roles
          if      (currentRound == ROUND_1) assignRolesForRound1();
          else if (currentRound == ROUND_2) assignRolesForRound2();
          else if (currentRound == ROUND_3) assignRolesForRound3();

          // 2) queue a delayed broadcast of whatever ‘currentRecipe’ already is
          fireRecipePending   = true;
          fireRecipeQueuedAt  = millis();
          
          Serial.println("Fire extinguished. All stations set to normal mode.");
        }
        fireButtonPressStartTime = 0;
      }
    }
  }

  delay(10); // small delay to prevent watchdog resets
}

/*************************************************************
    NEW ROUND LOGIC IMPLEMENTATION
*************************************************************/

// --------------------------- Round 1 ------------------------
void startRound1() {
  currentRound = ROUND_1;
  correctServesInRound = 0;
  playerScore = 0;
  roundDuration = 120000UL;
  roundStartTime = 0;
  

  updateScoreAndTimeOnPi(playerScore, roundDuration / 1000);

  // Optional: Play "start round 1" audio on server
  Serial.println("Starting Round 1 => playing audio track for Round 1...");
  audioModule.stop();
  audioModule.playSpecified(1); // track #10 as an example

  // Broadcast "StartRound1" so clients can do a Round 1 video
  struct_message msg;
  memset(&msg, 0, sizeof(msg));
  strncpy(msg.requestType, "StartRound1", REQUEST_TYPE_LENGTH - 1);
  // Send to all
  for (auto& entry : macToRoleMap) {
    uint8_t macBytes[6];
    if (stringToMAC(entry.first, macBytes)) {
      sendDataBackToClient(macBytes, &msg);
    }
  }

  // Wait 10s for them to show some animation
  delay(6000);

  // Now randomize role assignments (3 chop, 1 mix, 1 order, 1 garbage, 1 ingredient, 0 cook):
  assignRolesForRound1();

  roundStartPending    = true;
  roundStartQueuedAt   = millis();
}

void assignRolesForRound1() {
  // We have 8 clients in macToRoleMap (excluding server).
  // Round 1: 3 chop, 0 cook, 1 mix, 1 order, 1 garbage, 1 ingredient, etc.
  // This is just a demonstration; you can adapt how you shuffle them.

  // Collect MAC addresses (excluding server itself).
  std::vector<std::string> macList;
  for (auto& kv : macToRoleMap) {
    // skip the server if it's in the map
    if (kv.second == ROLE_NONE && kv.first == WiFi.macAddress().c_str()) {
      continue;
    }
    // Also skip the fridge’s MAC so we don’t randomly reassign it
    if (kv.first == "E4:65:B8:DA:17:8C") {
      // Force it to remain ingredient station
      macToRoleMap[kv.first] = ROLE_INGREDIENT_STATION;
      continue;
    }

    // Everyone else can be randomized
    macList.push_back(kv.first);
  }

  // Shuffle
  for (int i = macList.size() - 1; i > 0; i--) {
    int j = random(0, i + 1);
    std::swap(macList[i], macList[j]);
  }

  // We'll assign in order:
  // 0 -> ingredient
  // 1 -> garbage
  // Next 3 -> chop
  // next -> mix
  // next -> order
  // remainder -> none (if any)
  // (Adapt to however you want the distribution.)

  if (macList.size() < 7) {
    // fallback if we don't have enough
    Serial.println("Warning: Not enough stations to fully assign Round 1 roles!");
  }

  int index = 0;

  // 1 -> garbage
  if (index < (int)macList.size()) {
    macToRoleMap[macList[index]] = ROLE_GARBAGE;
    index++;
  }
  // next 3 -> chop
  for (int c = 0; c < 3; c++) {
    if (index < (int)macList.size()) {
      macToRoleMap[macList[index]] = ROLE_CHOP_STATION;
      index++;
    }
  }
  // next -> mix
  if (index < (int)macList.size()) {
    macToRoleMap[macList[index]] = ROLE_MIX_STATION;
    index++;
  }
  // next -> order
  if (index < (int)macList.size()) {
    macToRoleMap[macList[index]] = ROLE_ORDER_SERVE_STATION;
    index++;
  }
  // remainder -> none
  while (index < (int)macList.size()) {
    macToRoleMap[macList[index]] = ROLE_NONE;
    index++;
  }

  // Now broadcast role assignments
  sendRoleAssignmentToAllClients();
}

void pickRandomRecipeForRound1() {
  // Round 1: recipes[0]..recipes[4]
  static int lastIdx = -1;
  int idx;
  do {
    idx = random(0, 5);     // 0..4
  } while (idx == lastIdx);
  lastIdx = idx;

  currentRecipe = recipes[idx];
  Serial.print("Round 1 => Chosen recipe: ");
  Serial.println(currentRecipe.name);
}

void pickNextRecipeForRound1() {
  // Round 1 next: recipes[0]..recipes[4]
  static int lastNextIdx = -1;
  int idx;
  do {
    idx = random(0, 5);
  } while (
    idx == lastNextIdx
    || strncmp(recipes[idx].name, currentRecipe.name, MAX_RECIPE_NAME_LENGTH) == 0
  );
  lastNextIdx = idx;

  nextRecipe = recipes[idx];
  Serial.print("→ Queued next (Round 1): ");
  Serial.println(nextRecipe.name);
}

// --------------------------- Round 2 ------------------------
void startRound2() {
  currentRound = ROUND_2;
  correctServesInRound = 0;

  // Reinit RFID data for the new round
  //reinitializeRFIDData();
  
  // Now randomize station locations with 2 cook stations, 1 chop, etc.
  assignRolesForRound2();

  roundStartPending    = true;
  roundStartQueuedAt   = millis();
}

void assignRolesForRound2() {
  // Round 2 => 2 cook stations, 1 chop, 1 mix, 1 order, 1 garbage, 1 ingredient, etc.
  std::vector<std::string> macList;
  for (auto& kv : macToRoleMap) {
    if (kv.first == WiFi.macAddress().c_str()) {
      continue;
    }
    // Also skip the fridge’s MAC so we don’t randomly reassign it
    if (kv.first == "E4:65:B8:DA:17:8C") {
      // Force it to remain ingredient station
      macToRoleMap[kv.first] = ROLE_INGREDIENT_STATION;
      continue;
    }

    // Everyone else can be randomized
    macList.push_back(kv.first);
  }
  for (int i = macList.size() - 1; i > 0; i--) {
    int j = random(0, i + 1);
    std::swap(macList[i], macList[j]);
  }

  int index = 0;
  // 1 -> garbage
  if (index < (int)macList.size()) {
    macToRoleMap[macList[index]] = ROLE_GARBAGE;
    index++;
  }
  // 2 cooks
  for (int c = 0; c < 2; c++) {
    if (index < (int)macList.size()) {
      macToRoleMap[macList[index]] = ROLE_COOK_STATION;
      index++;
    }
  }
  // 1 chop
  if (index < (int)macList.size()) {
    macToRoleMap[macList[index]] = ROLE_CHOP_STATION;
    index++;
  }
  // 1 mix
  if (index < (int)macList.size()) {
    macToRoleMap[macList[index]] = ROLE_MIX_STATION;
    index++;
  }
  // 1 order
  if (index < (int)macList.size()) {
    macToRoleMap[macList[index]] = ROLE_ORDER_SERVE_STATION;
    index++;
  }
  // remainder -> none
  while (index < (int)macList.size()) {
    macToRoleMap[macList[index]] = ROLE_NONE;
    index++;
  }

  // Send role assignments
  sendRoleAssignmentToAllClients();
}

void pickRandomRecipeForRound2() {
  // Round 2: recipes[5]..recipes[9]
  static int lastIdx = -1;
  int idx;
  do {
    idx = random(5, 10);    // 5..9
  } while (idx == lastIdx);
  lastIdx = idx;

  currentRecipe = recipes[idx];
  Serial.print("Round 2 => Chosen recipe: ");
  Serial.println(currentRecipe.name);
}

void pickNextRecipeForRound2() {
  // Round 2 next: recipes[5]..recipes[9]
  static int lastNextIdx = -1;
  int idx;
  do {
    idx = random(5, 10);
  } while (
    idx == lastNextIdx
    || strncmp(recipes[idx].name, currentRecipe.name, MAX_RECIPE_NAME_LENGTH) == 0
  );
  lastNextIdx = idx;

  nextRecipe = recipes[idx];
  Serial.print("→ Queued next (Round 2): ");
  Serial.println(nextRecipe.name);
}

// --------------------------- Round 3 ------------------------
void startRound3() {
  currentRound = ROUND_3;
  correctServesInRound = 0;

  // Reinit plates
  // reinitializeRFIDData();

  // Round 3 => 1 oven station, 1 cook station, 1 chop, 1 garbage, 1 mix, 1 order, 1 ingredient
  assignRolesForRound3();

  roundStartPending    = true;
  roundStartQueuedAt   = millis();
}

void assignRolesForRound3() {
  // We'll just demonstrate 1 oven, 1 cook, 1 chop, 1 mix, 1 order, 1 garbage, 1 ingredient
  std::vector<std::string> macList;
  for (auto& kv : macToRoleMap) {
    if (kv.first == WiFi.macAddress().c_str()) {
      continue;
    }
    // Also skip the fridge’s MAC so we don’t randomly reassign it
    if (kv.first == "E4:65:B8:DA:17:8C") {
      // Force it to remain ingredient station
      macToRoleMap[kv.first] = ROLE_INGREDIENT_STATION;
      continue;
    }

    // Everyone else can be randomized
    macList.push_back(kv.first);
  }
  for (int i = macList.size() - 1; i > 0; i--) {
    int j = random(0, i + 1);
    std::swap(macList[i], macList[j]);
  }

  int index = 0;

  if (index < (int)macList.size()) {
    macToRoleMap[macList[index]] = ROLE_OVEN_STATION; // The new oven station
    index++;
  }

  // 2 cooks
  for (int c = 0; c < 2; c++) {
    if (index < (int)macList.size()) {
      macToRoleMap[macList[index]] = ROLE_COOK_STATION;
      index++;
    }
  }
  if (index < (int)macList.size()) {
    macToRoleMap[macList[index]] = ROLE_CHOP_STATION;
    index++;
  }
  if (index < (int)macList.size()) {
    macToRoleMap[macList[index]] = ROLE_MIX_STATION;
    index++;
  }
  if (index < (int)macList.size()) {
    macToRoleMap[macList[index]] = ROLE_ORDER_SERVE_STATION;
    index++;
  }
  // remainder -> none
  while (index < (int)macList.size()) {
    macToRoleMap[macList[index]] = ROLE_NONE;
    index++;
  }

  sendRoleAssignmentToAllClients();
}

void pickRandomRecipeForRound3() {
  // Round 3: recipes[10]..recipes[13]
  static int lastIdx = -1;
  int idx;
  do {
    idx = random(10, 14);   // 10..13
  } while (idx == lastIdx);
  lastIdx = idx;

  currentRecipe = recipes[idx];
  Serial.print("Round 3 => Chosen recipe: ");
  Serial.println(currentRecipe.name);
}

void pickNextRecipeForRound3() {
  // Round 3 next: recipes[10]..recipes[13]
  static int lastNextIdx = -1;
  int idx;
  do {
    idx = random(10, 14);
  } while (
    idx == lastNextIdx
    || strncmp(recipes[idx].name, currentRecipe.name, MAX_RECIPE_NAME_LENGTH) == 0
  );
  lastNextIdx = idx;

  nextRecipe = recipes[idx];
  Serial.print("→ Queued next (Round 3): ");
  Serial.println(nextRecipe.name);
}

// Called when a round's time runs out:
void endCurrentRound() {
  gameRunning = false; // Temporarily stop

  switch (currentRound) {
    case ROUND_1:
      if (correctServesInRound >= serveTargets[currentRound]) {
        // pass => move to round 2
        Serial.println("Round 1 success => starting Round 2...");
        startRound2();
      } else {
        // fail => end game with final score = 0
        starScore = 0;
        endGame("not enough appetizers served");
      }
      break;

    case ROUND_2:
      if (correctServesInRound >= serveTargets[currentRound]) {
        // pass => move to round 3
        Serial.println("Round 2 success => starting Round 3...");
        startRound3();
      } else {
        // fail => end game with final score = 1
        starScore = 1;
        endGame("not enough entrees served");
      }
      break;

    case ROUND_3:
      // final awarding
      if (correctServesInRound >= serveTargets[currentRound]) {
        starScore = 3; // if success
      } else {
        starScore = 2; // if not enough
      }
      endGame("Round 3 complete");
      break;

    default:
      Serial.println("No active round to end. Doing nothing...");
      break;
  }
}

/*************************************************************
    ESP-NOW RECEIVE CALLBACK
*************************************************************/
void onDataRecv(const esp_now_recv_info_t* info, const uint8_t* incomingData, int len) {
  if (len != sizeof(struct_message)) {
    Serial.println("Received data size mismatch!");
    return;
  }

  // If peer not known, add it
  if (!esp_now_is_peer_exist(info->src_addr)) {
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, info->src_addr, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;
    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
      Serial.println("Failed to add client as peer");
      return;
    }
  }

  // Copy data into a struct_message
  struct_message incomingMessage;
  memcpy(&incomingMessage, incomingData, sizeof(incomingMessage));

  // Ensure strings are null-terminated
  incomingMessage.rfid[RFID_LENGTH - 1] = '\0';
  incomingMessage.ingredient[INGREDIENT_LENGTH - 1] = '\0';
  incomingMessage.requestType[REQUEST_TYPE_LENGTH - 1] = '\0';
  incomingMessage.recipeName[MAX_RECIPE_NAME_LENGTH - 1] = '\0';

  // MAC as string
  String macStr = macToString(info->src_addr);

  // Print debug
  Serial.println("=== Server Received Data ===");
  Serial.print("From MAC: ");  Serial.println(macStr);
  Serial.print("RFID: ");      Serial.println(incomingMessage.rfid);
  Serial.print("Ingredient: ");Serial.println(incomingMessage.ingredient);
  Serial.print("Chop Count: ");Serial.println(incomingMessage.chopCount);
  Serial.print("Cook Count: ");Serial.println(incomingMessage.cookCount);
  Serial.print("Score Delta: ");Serial.println(incomingMessage.playerScoreDelta);
  Serial.print("Reset: ");     Serial.println(incomingMessage.reset ? "true" : "false");
  Serial.print("Request: ");   Serial.println(incomingMessage.requestType);
  Serial.print("Role: ");      Serial.println(incomingMessage.role);
  Serial.print("Recipe: ");    Serial.println(incomingMessage.recipeName);
  Serial.println("=============================");

  // Parse request
  RequestType reqType = getRequestType(incomingMessage.requestType);

  // ROLE_REQUEST
  if (reqType == ROLE_REQUEST) {
    ClientRole assignedRole = ROLE_NONE;
    auto it = macToRoleMap.find(macStr.c_str());
    if (it != macToRoleMap.end()) {
      assignedRole = it->second;
    } else {
      Serial.println("Client MAC not recognized, assigning ROLE_NONE");
    }

    // Store role
    clientRoles[macStr.c_str()] = assignedRole;

    // Send back RoleAssignment
    struct_message roleMsg;
    memset(&roleMsg, 0, sizeof(roleMsg));
    strncpy(roleMsg.requestType, "RoleAssignment", REQUEST_TYPE_LENGTH - 1);
    roleMsg.role = assignedRole;

    sendDataBackToClient(info->src_addr, &roleMsg);

    Serial.print("Assigned role ");
    Serial.print(assignedRole);
    Serial.print(" to MAC ");
    Serial.println(macStr);
    return;
  }

  // SELECT_NEW_RECIPE – now we pick from the round's valid subset
  if (reqType == SELECT_NEW_RECIPE) {
    Serial.println("Processing SelectNewRecipe on server...");

    // only if we’re inside a round
    if (!gameRunning || currentRound == ROUND_NONE) {
      Serial.println("No active round—ignoring SelectNewRecipe.");
      return;
    }

    // if we’ve already hit the serve target, just drop it
    if (correctServesInRound >= serveTargets[currentRound]) {
      Serial.println("Ignoring client SelectNewRecipe—round is ending.");
      return;
    }

    // lock our recipe state
    if (xSemaphoreTake(xMutex, portMAX_DELAY)) {
      // 1) promote the queued recipe to current
      currentRecipe = nextRecipe;
      Serial.print("Promoted next → current: ");
      Serial.println(currentRecipe.name);

    if (correctServesInRound < serveTargets[currentRound]-1) {
      // still within this round
      switch (currentRound) {
        case ROUND_1: pickNextRecipeForRound1(); break;
        case ROUND_2: pickNextRecipeForRound2(); break;
        case ROUND_3: pickNextRecipeForRound3(); break;
      }
    } else {
      // last serve of this round → prepare next round’s first recipe
      switch (currentRound) {
        case ROUND_1:
          pickNextRecipeForRound2();
          break;
        case ROUND_2:
          pickNextRecipeForRound3();
          break;
        default:
          // Round 3 has no Round 4 → clear
          memset(&nextRecipe, 0, sizeof(nextRecipe));
          break;
      }
    }

      // 3) broadcast the new “current” out to stations
      sendNewRecipeToRelevantStations();

      xSemaphoreGive(xMutex);
    } else {
      Serial.println("Failed to acquire mutex in SELECT_NEW_RECIPE.");
    }

    return;
  }


  // CURRENT_RECIPE_REQUEST
  if (reqType == CURRENT_RECIPE_REQUEST) {
    Serial.println("Processing CurrentRecipeRequest...");

    struct_message recipeMsg;
    memset(&recipeMsg, 0, sizeof(recipeMsg));
    strncpy(recipeMsg.requestType, "CurrentRecipeResponse", REQUEST_TYPE_LENGTH - 1);
    strncpy(recipeMsg.recipeName, currentRecipe.name, MAX_RECIPE_NAME_LENGTH - 1);

    sendDataBackToClient(info->src_addr, &recipeMsg);
    Serial.println("CurrentRecipeResponse sent.");
    return;
  }

  // REINITIALIZE
  if (reqType == REINITIALIZE) {
    Serial.println("Received Reinitialize request from client (unexpected).");
    // Typically only the server triggers that. We can ignore or handle.
    return;
  }

  // --- NEW FIRE LOGIC: BURNT
  if (reqType == BURNT) {
    Serial.println("Received BURNT request from cooking station => onFire = true...");
    if (!onFire) {
      onFire = true;
      struct_message fireMsg;
      memset(&fireMsg, 0, sizeof(fireMsg));
      strncpy(fireMsg.requestType, "OnFire", REQUEST_TYPE_LENGTH - 1);

      for (const auto& entry : macToRoleMap) {
        uint8_t macBytes[6];
        if (stringToMAC(entry.first, macBytes)) {
          sendDataBackToClient(macBytes, &fireMsg);
        }
      }
      Serial.println("All stations set to OnFire mode. Waiting for Fire button press to extinguish.");
    }
    return;
  }

  // If the round isn't running, ignore normal DataRequests
  if (!gameRunning) {
    Serial.println("Game not running (no active round), ignoring data.");
    return;
  }

  // Otherwise handle data request/update
  if (xSemaphoreTake(xMutex, portMAX_DELAY)) {
    int rfidIndex = findRFIDIndex(incomingMessage.rfid);
    if (rfidIndex == -1) {
      Serial.print("Unknown RFID: ");
      Serial.println(incomingMessage.rfid);
      xSemaphoreGive(xMutex);
      return;
    }

    struct_message* currentData = &rfidDataArray[rfidIndex];

    switch (reqType) {
      case DATA_REQUEST: {
        Serial.println("Processing DataRequest...");
        strncpy(currentData->requestType, "DataResponse", REQUEST_TYPE_LENGTH - 1);
        sendDataBackToClient(info->src_addr, currentData);
        break;
      }

      case DATA_UPDATE: {
        Serial.println("Processing DataUpdate...");
        // reset if requested
        if (incomingMessage.reset) {
          strncpy(currentData->ingredient, "none", INGREDIENT_LENGTH - 1);
          currentData->chopCount = 0;
          currentData->cookCount = 0;
          currentData->reset = false;
          Serial.println("Reset plate data.");
        }

        // update ingredient
        if (strncmp(incomingMessage.ingredient, "", INGREDIENT_LENGTH) != 0) {
          strncpy(currentData->ingredient, incomingMessage.ingredient, INGREDIENT_LENGTH - 1);
          Serial.print("Updated ingredient to: ");
          Serial.println(currentData->ingredient);
        }

        // update chopCount
        if (incomingMessage.chopCount != INVALID_COUNT) {
          currentData->chopCount = incomingMessage.chopCount;
          Serial.print("Updated chop count to: ");
          Serial.println(currentData->chopCount);
        }

        // update cookCount
        if (incomingMessage.cookCount != INVALID_COUNT) {
          currentData->cookCount = incomingMessage.cookCount;
          Serial.print("Updated cook count to: ");
          Serial.println(currentData->cookCount);
        }

        // update player score
        // handle correct vs. wrong serves
        if (incomingMessage.playerScoreDelta > 0) {
          // ─── correct serve ───
          playerScore += incomingMessage.playerScoreDelta;
          correctServesInRound += incomingMessage.playerScoreDelta;

          // add 15 seconds
          roundDuration += 15000UL;
          pendingScore    = playerScore;
          pendingTimeLeft = getTimeLeftInSeconds();
          scoreUpdatePending = true;

          currentRecipe = nextRecipe; 

          if (correctServesInRound < serveTargets[currentRound]-1) {
            // still within this round
            switch (currentRound) {
              case ROUND_1: pickNextRecipeForRound1(); break;
              case ROUND_2: pickNextRecipeForRound2(); break;
              case ROUND_3: pickNextRecipeForRound3(); break;
            }
          } else {
            // last serve of this round → prepare next round’s first recipe
            switch (currentRound) {
              case ROUND_1:
                pickNextRecipeForRound2();
                break;
              case ROUND_2:
                pickNextRecipeForRound3();
                break;
              default:
                // Round 3 has no Round 4 → clear
                memset(&nextRecipe, 0, sizeof(nextRecipe));
                break;
            }
          }

          // schedule the broadcast after the success animation finishes
          serveSuccessRecipePending = true;
          serveSuccessRecipeAt      = millis() + SUCCESS_DELAY_MS;

          // schedule the next round rather than calling it right away
          if (currentRound == ROUND_1 && correctServesInRound >= serveTargets[currentRound]) {
            Serial.println("Round 1 complete → scheduling Round 2 in 2 s");
            pendingRoundTransition = true;
            nextRoundNumber        = 2;
            transitionRequestTime  = millis();
          }
          else if (currentRound == ROUND_2 && correctServesInRound >= serveTargets[currentRound]) {
            Serial.println("Round 2 complete → scheduling Round 3 in 2 s");
            pendingRoundTransition = true;
            nextRoundNumber        = 3;
            transitionRequestTime  = millis();
          }
        }
        else if (incomingMessage.playerScoreDelta == 0
              && incomingMessage.role    == ROLE_ORDER_SERVE_STATION) {
          // ─── wrong serve ───
          // subtract 10 seconds (no underflow)
          if (roundDuration > 10000UL) roundDuration -= 10000UL;
          else                         roundDuration = 0;

          // push the new time to the Pi
          pendingScore    = playerScore;           // score unchanged
          pendingTimeLeft = getTimeLeftInSeconds();
          scoreUpdatePending = true;
        }
        // otherwise (zero-deltas from other stations) do nothing


        strncpy(currentData->requestType, "DataResponse", REQUEST_TYPE_LENGTH - 1);
        sendDataBackToClient(info->src_addr, currentData);
        break;
      }

      default:
        Serial.println("Unknown request type received.");
        break;
    }

    xSemaphoreGive(xMutex);
  } else {
    Serial.println("Failed to acquire mutex.");
  }
}

/*************************************************************
    END GAME
*************************************************************/
void endGame(String reason) {
  // We'll treat this as the absolute end (fail or finish).
  currentRound = ROUND_NONE;
  gameRunning = false; // no active round
  fifteenSecondWarningPlayed = false;
  // cancel any in-flight transitions
  pendingRoundTransition = false;
  nextRoundNumber        = 0;
  roundStartPending      = false;
  roundStartTime = 0;


  Serial.println("Game ended.");
  Serial.print("Reason: ");
  Serial.println(reason);
  Serial.print("Player score: ");
  Serial.println(playerScore);

  // Exit onFire mode if active
  if (onFire) {
    onFire = false;
    digitalWrite(FIRE_LED_PIN, LOW);

    // Only send "ExtinguishFire" to the ingredient station(s)
    struct_message extMsg;
    memset(&extMsg, 0, sizeof(extMsg));
    strncpy(extMsg.requestType, "ExtinguishFire", REQUEST_TYPE_LENGTH - 1);

    for (const auto& entry : macToRoleMap) {
      if (entry.second == ROLE_INGREDIENT_STATION) {
        // Convert MAC string to bytes
        uint8_t macBytes[6];
        if (stringToMAC(entry.first, macBytes)) {
          sendDataBackToClient(macBytes, &extMsg);
        }
      }
    }
    Serial.println("Exiting onFire mode.");
  }

  // Now send "endgame" to all stations EXCEPT the ingredient station
  struct_message endgameMsg;
  memset(&endgameMsg, 0, sizeof(endgameMsg));
  strncpy(endgameMsg.requestType, "endgame", REQUEST_TYPE_LENGTH - 1);

  for (const auto& entry : macToRoleMap) {
    uint8_t macBytes[6];
    if (stringToMAC(entry.first, macBytes)) {
      sendDataBackToClient(macBytes, &endgameMsg);
    }
  }
  
  //stop music
  audioModule.stop();

  // Reinit RFID data
  reinitializeRFIDData();

  // Notify Pi
  notifyPiEndGame(reason, playerScore);

  // Provide feedback
  if (reason == "Red Button Press") {
    blinkButtonLED(RED_BUTTON_LED_PIN);
  } else if (reason == "not enough appetizers served" || reason == "not enough entrees served") {
    blinkButtonLED(RED_BUTTON_LED_PIN);
  } else if (reason == "Round 3 complete") {
    blinkButtonLED(GREEN_BUTTON_LED_PIN);
  } else {
    blinkButtonLED(GREEN_BUTTON_LED_PIN);
  }

  audioModule.stop();
  audioModule.playSpecified(3);

  // Wait 15 seconds so stations can finish playing the "endgame" video
  delay(15000);

  // Send Reinitialize to clients
  sendReinitializeToAllClients();
}

/*************************************************************
    PI NOTIFICATION FUNCTIONS
*************************************************************/
// Return time left in seconds in the current round
int getTimeLeftInSeconds() {
  if (!gameRunning) {
    return 0;
  }
  unsigned long elapsed = millis() - roundStartTime;
  if (elapsed >= roundDuration) {
    return 0;
  }
  unsigned long remaining = roundDuration - elapsed;
  return (int)(remaining / 1000);
}

// Notify Pi that game started
// (We won't call this for each round. If you want, you can do it in startRound1())
void notifyPiStartGame(unsigned long durationSec) {
  if (!connectedToScoreboard) {
    Serial.println("Not connected to scoreboard AP, cannot POST /start_game.");
    return;
  }

  // Build the star_threshold string => "2,5,7"
  String thresholdsStr = getStarThresholdString();

  // Convert starMaxScore to a String
  String maxScoreStr = String(starMaxScore);

  HTTPClient http;
  http.begin("http://10.42.0.1:5000/start_game");
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  // e.g. "duration=180&star_thresholds=2,5,7&max_score=7"
  String postData = "duration=" + String(durationSec)
                  + "&star_thresholds=" + thresholdsStr
                  + "&max_score=" + maxScoreStr;

  int httpCode = http.POST(postData);
  http.end();

  Serial.print("notifyPiStartGame => HTTP code: ");
  Serial.println(httpCode);
  Serial.println("POST data: " + postData);
}

// Notify Pi that game ended
void notifyPiEndGame(String reason, int finalScore) {
  if (!connectedToScoreboard) {
    Serial.println("Not connected to scoreboard, cannot POST /end_game.");
    return;
  }

  HTTPClient http;
  http.begin("http://10.42.0.1:5000/end_game");
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  // e.g. "score=500&reason=TimerExpired"
  String postData = "score=" + String(finalScore) + "&reason=" + reason;
  int httpCode = http.POST(postData);
  http.end();

  Serial.print("notifyPiEndGame => HTTP code: ");
  Serial.println(httpCode);
}

// Send updated score/time to Pi
void updateScoreAndTimeOnPi(int newScore, int timeLeftSec) {
  if (!connectedToScoreboard) {
    Serial.println("Not connected to scoreboard AP, cannot POST /update_score.");
    return;
  }

  // Build the star_threshold string => "2,5,7"
  String thresholdsStr = getStarThresholdString();
  String maxScoreStr   = String(starMaxScore);

  /* // Blank recipes during any transition (round‐end delay or round‐start delay)
  bool inTransition = pendingRoundTransition || roundStartPending;
  String currRec = inTransition
                     ? ""
                     : (strlen(currentRecipe.name) > 0
                         ? String(currentRecipe.name)
                         : "");
  String nextRec = inTransition
                     ? ""
                     : (strlen(nextRecipe.name) > 0
                         ? String(nextRecipe.name)
                         : ""); */

  String currRec = String(currentRecipe.name);
  String nextRec = String(nextRecipe.name);

  HTTPClient http;
  http.begin("http://10.42.0.1:5000/update_score");
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  /*
    We now include:
      score = newScore
      time_left = timeLeftSec
      star_thresholds = "2,5,7"
      max_score = "7"
  */
  String postData = "score=" + String(newScore)
                  + "&time_left=" + String(timeLeftSec)
                  + "&star_thresholds=" + thresholdsStr
                  + "&max_score=" + maxScoreStr
                  + "&current_recipe=" + currRec
                  + "&next_recipe="    + nextRec;

  int httpResponseCode = http.POST(postData);
  http.end();

  Serial.print("Posted score/time => Score: ");
  Serial.print(newScore);
  Serial.print(", Time left: ");
  Serial.print(timeLeftSec);
  Serial.print(", HTTP code: ");
  Serial.println(httpResponseCode);

  Serial.println("POST data: " + postData);
}

/*************************************************************
    RECIPE SELECTION + SENDING
*************************************************************/
void sendNewRecipeToRelevantStations(bool scheduleRepeat) {
  // Send "NewRecipe" to stations that care (order/serve, mix, etc.)
  Serial.println("Sending NewRecipe to relevant stations...");
  for (const auto& entry : macToRoleMap) {
    if (   entry.second == ROLE_ORDER_SERVE_STATION
        || entry.second == ROLE_MIX_STATION
        || entry.second == ROLE_COOK_STATION
        || entry.second == ROLE_OVEN_STATION // if needed
       ) 
    {
      uint8_t macBytes[6];
      if (stringToMAC(entry.first, macBytes)) {
        struct_message newRecipeMsg;
        memset(&newRecipeMsg, 0, sizeof(newRecipeMsg));
        strncpy(newRecipeMsg.requestType, "NewRecipe", REQUEST_TYPE_LENGTH - 1);
        strncpy(newRecipeMsg.recipeName, currentRecipe.name, MAX_RECIPE_NAME_LENGTH - 1);

        sendDataBackToClient(macBytes, &newRecipeMsg);
        Serial.print("Sent NewRecipe to ");
        Serial.println(entry.first.c_str());
      } else {
        Serial.print("Failed to convert MAC: ");
        Serial.println(entry.first.c_str());
      }
    }
  }

  if (scheduleRepeat) {
      recipeRebroadcastPending = true;
      recipeRebroadcastAt      = millis() + 1000;
  }

  Serial.println("Completed sending NewRecipe messages.");
}

/*************************************************************
    RFID DATA INITIALIZATION/REINIT
*************************************************************/
void initializeRFIDData() {
  // Known RFID values for gameplay (6 plates)
  const int initialRFIDCount = 6;
  const char* knownRFIDs[initialRFIDCount] = {
    "30ED1279",
    "E0EC1279",
    "F0EC1279",
    "40ED1279",
    "BCB27E01",
    "D0EC1279"
  };

  // Clear array
  memset(rfidDataArray, 0, sizeof(rfidDataArray));
  rfidDataCount = 0;

  // Add each known RFID plate with default values.
  for (int i = 0; i < initialRFIDCount; i++) {
    struct_message newRFIDData;
    memset(&newRFIDData, 0, sizeof(newRFIDData));

    strncpy(newRFIDData.rfid, knownRFIDs[i], RFID_LENGTH - 1);
    strncpy(newRFIDData.ingredient, "none", INGREDIENT_LENGTH - 1);
    newRFIDData.chopCount = 0;
    newRFIDData.cookCount = 0;
    newRFIDData.playerScoreDelta = 0;
    newRFIDData.reset = false;
    strncpy(newRFIDData.requestType, "DataUpdate", REQUEST_TYPE_LENGTH - 1);
    newRFIDData.role = ROLE_NONE;
    strncpy(newRFIDData.recipeName, "none", MAX_RECIPE_NAME_LENGTH - 1);

    addRFIDData(&newRFIDData);
  }

  Serial.println("RFID data initialized with known plates (default values).");
}

void reinitializeRFIDData() {
  if (xSemaphoreTake(xMutex, portMAX_DELAY)) {
    for (int i = 0; i < rfidDataCount; i++) {
      strncpy(rfidDataArray[i].ingredient, "none", INGREDIENT_LENGTH - 1);
      rfidDataArray[i].chopCount = 0;
      rfidDataArray[i].cookCount = 0;
      rfidDataArray[i].playerScoreDelta = 0;
      rfidDataArray[i].reset = false;
      strncpy(rfidDataArray[i].requestType, "DataUpdate", REQUEST_TYPE_LENGTH - 1);
      rfidDataArray[i].role = ROLE_NONE;
      strncpy(rfidDataArray[i].recipeName, "none", MAX_RECIPE_NAME_LENGTH - 1);
    }
    Serial.println("RFID data reinitialized to default.");
    xSemaphoreGive(xMutex);
  } else {
    Serial.println("Failed to acquire mutex for reinitRFIDData.");
  }
}

/*************************************************************
    ADD/REMOVE RFID
*************************************************************/
bool addRFIDData(const struct_message* data) {
  if (rfidDataCount >= MAX_PLATES) {
    Serial.println("RFID data array is full!");
    return false;
  }
  memcpy(&rfidDataArray[rfidDataCount], data, sizeof(struct_message));
  rfidDataCount++;
  return true;
}

bool addRFID(const char* rfidUID) {
  if (rfidDataCount >= MAX_PLATES) {
    Serial.println("Cannot add RFID: Array is full.");
    return false;
  }
  if (findRFIDIndex(rfidUID) != -1) {
    Serial.println("Cannot add RFID: Already exists.");
    return false;
  }

  struct_message newRFIDData;
  memset(&newRFIDData, 0, sizeof(newRFIDData));
  strncpy(newRFIDData.rfid, rfidUID, RFID_LENGTH - 1);
  strncpy(newRFIDData.ingredient, "none", INGREDIENT_LENGTH - 1);
  newRFIDData.chopCount = 0;
  newRFIDData.cookCount = 0;
  newRFIDData.playerScoreDelta = 0;
  newRFIDData.reset = false;
  strncpy(newRFIDData.requestType, "DataUpdate", REQUEST_TYPE_LENGTH - 1);
  newRFIDData.role = ROLE_NONE;
  strncpy(newRFIDData.recipeName, "none", MAX_RECIPE_NAME_LENGTH - 1);

  if (addRFIDData(&newRFIDData)) {
    Serial.println("Successfully added new RFID to array.");
    return true;
  } else {
    Serial.println("Failed to add RFID to array.");
    return false;
  }
}

bool removeRFIDFromArray(const char* rfidUID) {
  int index = findRFIDIndex(rfidUID);
  if (index == -1) {
    Serial.println("Cannot remove RFID: Not found.");
    return false;
  }

  for (int i = index; i < rfidDataCount - 1; i++) {
    rfidDataArray[i] = rfidDataArray[i + 1];
  }
  rfidDataCount--;

  Serial.println("Removed RFID from array.");
  return true;
}

int findRFIDIndex(const char* rfid) {
  for (int i = 0; i < rfidDataCount; i++) {
    if (strncmp(rfidDataArray[i].rfid, rfid, RFID_LENGTH) == 0) {
      return i;
    }
  }
  return -1;
}

/*************************************************************
    SENDING DATA BACK TO CLIENT
*************************************************************/
void sendDataBackToClient(const uint8_t* mac_addr, struct_message* message) {
  Serial.println("=== Sending Data to Client ===");
  Serial.print("To MAC: ");
  for (int i = 0; i < 6; i++) {
    Serial.printf("%02X", mac_addr[i]);
    if (i < 5) Serial.print(":");
  }
  Serial.println();
  Serial.print("RFID: ");      Serial.println(message->rfid);
  Serial.print("Ingredient: ");Serial.println(message->ingredient);
  Serial.print("ChopCount: "); Serial.println(message->chopCount);
  Serial.print("CookCount: "); Serial.println(message->cookCount);
  Serial.print("ScoreDelta: ");Serial.println(message->playerScoreDelta);
  Serial.print("Reset: ");     Serial.println(message->reset ? "true" : "false");
  Serial.print("ReqType: ");   Serial.println(message->requestType);
  Serial.print("Role: ");      Serial.println(message->role);
  Serial.print("Recipe: ");    Serial.println(message->recipeName);
  Serial.println("===============================");

  esp_err_t result = esp_now_send(mac_addr, (uint8_t*)message, sizeof(struct_message));
  if (result != ESP_OK) {
    Serial.print("Error sending data to client: ");
    Serial.println(result);
  } else {
    Serial.println("Data sent to client successfully.");
  }
}

/*************************************************************
    ADD ALL PEERS
*************************************************************/
void addAllPeers() {
  Serial.println("Adding peers from macToRoleMap...");

  // Get server's own MAC
  String serverMacStr = WiFi.macAddress();
  Serial.print("Server's own MAC: ");
  Serial.println(serverMacStr);

  for (const auto& entry : macToRoleMap) {
    uint8_t macBytes[6];
    if (stringToMAC(entry.first, macBytes)) {
      String clientMacStr = String(entry.first.c_str());
      if (clientMacStr.equalsIgnoreCase(serverMacStr)) {
        Serial.print("Skipping server's own MAC: ");
        Serial.println(entry.first.c_str());
        continue;
      }

      if (!esp_now_is_peer_exist(macBytes)) {
        esp_now_peer_info_t peerInfo = {};
        memcpy(peerInfo.peer_addr, macBytes, 6);
        peerInfo.channel = 0;
        peerInfo.encrypt = false;

        esp_err_t addPeerStatus = esp_now_add_peer(&peerInfo);
        if (addPeerStatus == ESP_OK) {
          Serial.print("Successfully added peer: ");
          Serial.println(entry.first.c_str());
        } else {
          Serial.print("Failed to add peer (code ");
          Serial.print(addPeerStatus);
          Serial.println("):");
          Serial.println(entry.first.c_str());
        }
      } else {
        Serial.print("Peer already exists: ");
        Serial.println(entry.first.c_str());
      }
    } else {
      Serial.print("Invalid MAC address format: ");
      Serial.println(entry.first.c_str());
    }
  }

  Serial.println("Completed adding peers.");
}

/*************************************************************
    SENDING REINITIALIZE / ROLE
*************************************************************/
void sendReinitializeToClient(const uint8_t* clientAddress) {
  struct_message reinitMsg;
  memset(&reinitMsg, 0, sizeof(reinitMsg));
  strncpy(reinitMsg.requestType, "Reinitialize", REQUEST_TYPE_LENGTH - 1);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, clientAddress, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  if (!esp_now_is_peer_exist(clientAddress)) {
    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
      Serial.println("Failed to add client as peer for Reinitialize");
      return;
    }
  }

  esp_err_t result = esp_now_send(clientAddress, (uint8_t*)&reinitMsg, sizeof(struct_message));
  if (result == ESP_OK) {
    Serial.println("Reinitialize message sent to client.");
  } else {
    Serial.print("Error sending Reinitialize: ");
    Serial.println(result);
  }
}

void sendReinitializeToAllClients() {
  Serial.println("Sending Reinitialize command to all clients...");
  for (const auto& entry : macToRoleMap) {
    uint8_t macBytes[6];
    if (stringToMAC(entry.first, macBytes)) {
      sendReinitializeToClient(macBytes);
    } else {
      Serial.print("Failed to convert MAC string: ");
      Serial.println(entry.first.c_str());
    }
  }
  Serial.println("Completed sending Reinitialize to all.");
}

void sendRoleAssignmentToAllClients() {
  Serial.println("Sending RoleAssignment messages to all clients...");
  for (const auto& entry : macToRoleMap) {
    uint8_t macBytes[6];
    if (stringToMAC(entry.first, macBytes)) {
      struct_message roleMsg;
      memset(&roleMsg, 0, sizeof(roleMsg));
      strncpy(roleMsg.requestType, "RoleAssignment", REQUEST_TYPE_LENGTH - 1);
      roleMsg.role = entry.second;

      sendDataBackToClient(macBytes, &roleMsg);
      Serial.print("Sent RoleAssignment to MAC: ");
      Serial.println(entry.first.c_str());
    } else {
      Serial.print("Failed to convert MAC string: ");
      Serial.println(entry.first.c_str());
    }
  }
  Serial.println("Done sending RoleAssignment.");
}

/*************************************************************
    HELPER FUNCTIONS
*************************************************************/
bool readRFID(char* rfidUID) {
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) {
    return false;
  }

  String uidStr = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    uidStr += String(rfid.uid.uidByte[i] < 0x10 ? "0" : "");
    uidStr += String(rfid.uid.uidByte[i], HEX);
  }
  uidStr.toUpperCase();

  strncpy(rfidUID, uidStr.c_str(), RFID_LENGTH - 1);

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  return true;
}

void blinkButtonLED(int ledPin) {
  digitalWrite(ledPin, LOW);
  delay(100);
  digitalWrite(ledPin, HIGH);
}

// Convert MAC to String
String macToString(const uint8_t* mac_addr) {
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac_addr[0], mac_addr[1], mac_addr[2],
           mac_addr[3], mac_addr[4], mac_addr[5]);
  return String(macStr);
}

// Convert string "AA:BB:CC:DD:EE:FF" to byte array
bool stringToMAC(const std::string& macStr, uint8_t* macBytes) {
  if (macStr.length() != 17) {
    Serial.print("Invalid MAC string length: ");
    Serial.println(macStr.c_str());
    return false;
  }
  unsigned int bytes[6];
  int scanned = sscanf(macStr.c_str(), "%02X:%02X:%02X:%02X:%02X:%02X",
                       &bytes[0], &bytes[1], &bytes[2],
                       &bytes[3], &bytes[4], &bytes[5]);
  if (scanned != 6) {
    Serial.print("Failed to parse MAC string: ");
    Serial.println(macStr.c_str());
    return false;
  }
  for (int i = 0; i < 6; i++) {
    macBytes[i] = static_cast<uint8_t>(bytes[i]);
  }
  return true;
}

// Parse requestType string into enum
RequestType getRequestType(const char* requestTypeStr) {
  if (strncmp(requestTypeStr, "DataRequest", REQUEST_TYPE_LENGTH) == 0) {
    return DATA_REQUEST;
  } else if (strncmp(requestTypeStr, "DataUpdate", REQUEST_TYPE_LENGTH) == 0) {
    return DATA_UPDATE;
  } else if (strncmp(requestTypeStr, "RoleRequest", REQUEST_TYPE_LENGTH) == 0) {
    return ROLE_REQUEST;
  } else if (strncmp(requestTypeStr, "CurrentRecipeRequest", REQUEST_TYPE_LENGTH) == 0) {
    return CURRENT_RECIPE_REQUEST;
  } else if (strncmp(requestTypeStr, "CurrentRecipeResponse", REQUEST_TYPE_LENGTH) == 0) {
    return CURRENT_RECIPE_RESPONSE;
  } else if (strncmp(requestTypeStr, "SelectNewRecipe", REQUEST_TYPE_LENGTH) == 0) {
    return SELECT_NEW_RECIPE;
  } else if (strncmp(requestTypeStr, "Reinitialize", REQUEST_TYPE_LENGTH) == 0) {
    return REINITIALIZE;
  } else if (strncmp(requestTypeStr, "NewRecipe", REQUEST_TYPE_LENGTH) == 0) {
    return NEW_RECIPE;
  } else if (strncmp(requestTypeStr, "RoleAssignment", REQUEST_TYPE_LENGTH) == 0) {
    return ROLE_ASSIGNMENT;
  }
  // --- NEW FIRE LOGIC: 3 new comparisons ---
  else if (strncmp(requestTypeStr, "Burnt", REQUEST_TYPE_LENGTH) == 0) {
    return BURNT;
  } else if (strncmp(requestTypeStr, "OnFire", REQUEST_TYPE_LENGTH) == 0) {
    return ON_FIRE;
  } else if (strncmp(requestTypeStr, "ExtinguishFire", REQUEST_TYPE_LENGTH) == 0) {
    return EXTINGUISH_FIRE;
  }

  return UNKNOWN_REQUEST;
}

/*************************************************************
    INITIALIZE HARDWARE
*************************************************************/
void initializeHardware() {
  // RFID
  SPI.begin();
  rfid.PCD_Init();
  Serial.println("RFID reader initialized.");

  // Buttons
  pinMode(GREEN_BUTTON_PIN, INPUT_PULLUP);
  pinMode(RED_BUTTON_PIN, INPUT_PULLUP);

  // Button LEDs
  pinMode(GREEN_BUTTON_LED_PIN, OUTPUT);
  digitalWrite(GREEN_BUTTON_LED_PIN, HIGH); // turn on
  pinMode(RED_BUTTON_LED_PIN, OUTPUT);
  digitalWrite(RED_BUTTON_LED_PIN, HIGH); // turn on

  // Audio module
  audioSerial.begin(9600, SERIAL_8N1, AUDIO_RX, AUDIO_TX);
  audioModule.begin();
  audioModule.setCycleMode(DY::PlayMode::OneOff);
  audioModule.setVolume(15);
  audioModule.stop();

  // Sprite media player
  spriteSerial.begin(9600, SERIAL_8N1, SPRITE_RX, SPRITE_TX);
  byte playCommand = 0x00;
  spriteSerial.write(playCommand);

  // Fire button & LED
  pinMode(FIRE_BUTTON_PIN, INPUT_PULLUP);
  pinMode(FIRE_LED_PIN, OUTPUT);
  digitalWrite(FIRE_LED_PIN, LOW);

  Serial.println("Hardware initialization complete.");
}
