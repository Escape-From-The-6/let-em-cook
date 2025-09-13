/*************************************************************
    GENERIC CLIENT CODE
    (Client_Code.ino)
    
    - Handles multiple station roles: garbage, chop, cook, mix,
      order/serve, and now oven.
    - Uses ESP-NOW to communicate with a server and an optional
      ingredient station.
    - Includes logic for round-based updates from the server
      ("StartRound1", "StartRound2", "StartRound3").
    - Updated to reset local variables and play a Sprite video
      on each round change, *without* playing audio at the same time.
*************************************************************/

#include <esp_now.h>
#include <WiFi.h>
#include <MFRC522.h>
#include <SPI.h>
#include <Adafruit_NeoPixel.h>
#include <DYPlayerArduino.h> // Include the DY-HV20T library
#include <ArduinoOTA.h>      // Added OTA support

/*****************************************************************
   ─── OPTIONAL DEBUG SWITCH ────────────────────────────────────
   Comment-out the line   #define DEBUG
   or re-define it in PlatformIO / Arduino “Build Flags”
   to silence every Serial.print / Serial.println in the file.
*****************************************************************/
// #define DEBUG            //  ← keep or remove as you like
#ifdef DEBUG
  #define DPRINT(x)  Serial.print(x)
  #define DPRINTLN(x)  Serial.println(x)
#else
  // -- Replace global 'Serial' with a do-nothing instance --
  struct NullSerial_t {
    template<typename... T> void begin(T...)            {}
    template<typename... T> void print (T...)           {}
    template<typename... T> void println(T...)          {}
    template<typename... T> void printf (T...)          {}
    template<typename... T> void write (T...)           {}
    template<typename... T> void flush (T...)           {}
  } __nullSerial;
  #define Serial  __nullSerial
  #define DPRINT(x)
  #define DPRINTLN(x)
#endif

// Shared constants and definitions
#define RFID_LENGTH 20
#define INGREDIENT_LENGTH 30  
#define REQUEST_TYPE_LENGTH 25
#define MAX_RECIPE_NAME_LENGTH 30

#define MAX_CHOP_COUNT 5   // Maximum allowed chop count
#define MAX_COOK_COUNT 10  // Maximum allowed cook count
#define MAX_BAKE_COUNT 5   // Maximum allowed bake count

// Maximum number of ingredients in a recipe
#define MAX_RECIPE_INGREDIENTS 3

extern "C" {
  #include "esp_wifi.h"
}

// Define roles
enum ClientRole {
    ROLE_NONE = 0,
    ROLE_GARBAGE = 1,
    ROLE_CHOP_STATION = 2,
    ROLE_COOK_STATION = 3,
    ROLE_MIX_STATION = 4,
    ROLE_ORDER_SERVE_STATION = 5,
    ROLE_OVEN_STATION = 7
};

typedef struct struct_message {
    char rfid[RFID_LENGTH];
    char ingredient[INGREDIENT_LENGTH];
    int chopCount;
    int cookCount;
    int playerScoreDelta;
    bool reset;
    char requestType[REQUEST_TYPE_LENGTH];
    int role;
    char recipeName[MAX_RECIPE_NAME_LENGTH];
} struct_message;


// Global variables
ClientRole currentRole = ROLE_NONE;
ClientRole previousRole = ROLE_NONE;

// Update with your server's MAC address
uint8_t serverAddress[] = {0xC4, 0xDE, 0xE2, 0x5B, 0x81, 0x58};  // Replace with your server's MAC address

// Pin definitions (same for all clients)
#define SS_PIN 21
#define RST_PIN 22
#define BUTTON_PIN 13         
#define DEBOUNCE_DELAY 0      

#define NEOPIXEL_PIN 4             
#define NEOPIXEL_MOSFET_PIN 26     
#define NUM_PIXELS 20              

#define BUTTON_LIGHT_PIN 27  

// Audio module pins (DY-HV20T)
#define AUDIO_TX 16              
#define AUDIO_RX 17              

// Sprite media player pins
#define SPRITE_TX 32   
#define SPRITE_RX 33   

// Hardware instances
MFRC522 rfid(SS_PIN, RST_PIN);  // Initialize RFID reader
Adafruit_NeoPixel strip(NUM_PIXELS, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

// Initialize DYPlayer
HardwareSerial audioSerial(2);        
DY::Player audioModule(&audioSerial); 

// Initialize Sprite media player
HardwareSerial spriteSerial(1);  

// Data structures
struct_message myData;            // Data to send to the server
struct_message serverData;        // Data received from the server (DataResponse)
struct_message currentRecipeData; // Data received from the server (CurrentRecipeResponse)

volatile bool dataReceived = false; // Flag to indicate data reception

// — globals for non-blocking role transitions —
bool         roleTransitionPending   = false;
ClientRole   roleTransitionTarget    = ROLE_NONE;
unsigned long roleTransitionStart    = 0;
const unsigned long ROLE_DELAY_MS    = 4500; 

// Global onFire flag
bool onFire = false;

// for sending sprite command in loop vs OnDataRecv
volatile uint8_t pendingSpriteCmd = 0;   // 0 = none

// System states
enum SystemState {
    IDLE,
    PROCESSING
};

unsigned long lastCheckTime = 0;
unsigned long currentTime = millis();
char localRFID[RFID_LENGTH] = {0};
char localIngredient[INGREDIENT_LENGTH] = {0};

// Variables for Chop Station
SystemState chopState = IDLE;
int localChopCount = 0;

// Variables for Cook Station
bool cooking = false;                  
unsigned long lastCookTime = 0;        
char storedRFID[RFID_LENGTH];          
int ledSectionCook = 0;
SystemState cookState = IDLE;
int localCookCount = 0;
bool burntPlate = false;

// Variables for Oven Station
bool baking = false;
unsigned long lastBakeTime = 0;
SystemState ovenState = IDLE;
bool isPlayingBakingAudio = false;
bool hasPlayedBakingCompleteAudio = false;
int localBakeCount = 0;

// Timing variables for RFID presence
unsigned long lastRFIDReadTime = 0;
const unsigned long RFID_TIMEOUT = 800; // 800ms timeout for RFID absence

// Variables for audio playback in cook station
bool isPlayingCookingAudio = false;        
bool hasPlayedCookingCompleteAudio = false; 

// Variables for Mix Station
enum MixState {
    MIX_IDLE,
    MIX_MIXING
};
MixState mixState = MIX_IDLE;
char collectedIngredients[MAX_RECIPE_INGREDIENTS][INGREDIENT_LENGTH] = {0};
int collectedIngredientCount = 0;

// Variables for Order/Serve Station
struct IngredientRequirement {
    char name[INGREDIENT_LENGTH];
    bool requiresChop;
    int requiredChopCount;
    bool requiresCook;
    int requiredCookCountMin;
    int requiredCookCountMax;
};

// Structure to define a recipe
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
Recipe currentRecipe;

// Timing variables
const unsigned long CHECK_INTERVAL = 500; 

struct ButtonState {
    unsigned long lastButtonPressTime;
    bool lastButtonState;
};

ButtonState garbageButtonState = {0, HIGH};
ButtonState chopButtonState = {0, HIGH};
ButtonState cookButtonState = {0, HIGH};
ButtonState mixButtonState = {0, HIGH};
ButtonState orderServeButtonState = {0, HIGH};
ButtonState ovenButtonState = {0, HIGH};

// Function prototypes
void initializeHardware();
void executeGarbageClient();
void executeChopStationClient();
void executeCookStationClient();
void executeMixStationClient();
void executeOrderServeStationClient();
void executeOvenStationClient();
void onDataRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len);
void onDataSent(const uint8_t* mac_addr, esp_now_send_status_t status);
bool PICC_IsAnyCardPresent();
void resetRFIDData();
void sendDataToServer(struct_message* data);
void updateChopCountDisplay(int chopCount);
void updateCookLedSection(int cookCount);
void updateBakeLedSection(int bakeCount);
void setStripColor(uint32_t color);
void clearStrip();
void blinkButtonLED();
bool isButtonPressed(ButtonState& buttonState);
bool readRFID(char* rfidUID);
bool waitForServerResponse(unsigned long timeout);
void resetRFIDReader();
void graduallyLightUpHalf(int startLED, int endLED);
void updateMixStationLEDs();
void requestAndUpdateCurrentRecipe();
void performReinitialization();
uint8_t getVideoID(const char* recipeName);
void resetSharedData();
void resetAllRolesData();
void handleRoleAssignment(ClientRole newRole);
void runFireEffect();
void sendSprite(uint8_t cmd);

void setup() {
    Serial.begin(115200);  // Default Serial (USB for debugging)

    // Force channel 6 so it matches the server's channel
    WiFi.mode(WIFI_STA);
    delay(100);
    // Connect to WiFi (required for OTA)
    WiFi.begin("MyScoreboardAP", "MySecretPassword"); 
    while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.print(".");
    }
    Serial.println("\nWiFi connected, IP address: ");
    Serial.println(WiFi.localIP());
    
    String macAddress = WiFi.macAddress();  // Retrieve MAC address
    Serial.print("MAC Address: ");
    Serial.println(macAddress);  // Print the MAC address
    
    // Set channel for ESP-NOW
    SPI.begin();
    esp_wifi_set_channel(6, WIFI_SECOND_CHAN_NONE);
    Serial.print("Home channel: ");
    Serial.println(WiFi.channel());  // Should print 6

    // Initialize OTA
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

    // Initialize ESP-NOW
    if (esp_now_init() != ESP_OK) {
        Serial.println("Error initializing ESP-NOW");
        return;
    }
    esp_now_register_send_cb(onDataSent);
    esp_now_register_recv_cb(onDataRecv);

    // Add server as a peer
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, serverAddress, 6);
    peerInfo.channel = 6;  
    peerInfo.encrypt = false;
    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
        Serial.println("Failed to add server peer");
        return;
    }

    // Initialize all hardware components
    initializeHardware();
}

void loop() {
    // Handle OTA updates
    ArduinoOTA.handle();

    // send spritecmd in loop instead of OnDataRecv
    if (pendingSpriteCmd) {
        sendSprite(pendingSpriteCmd);
        pendingSpriteCmd = 0;    // mark as done
    }
    
    // 1) If we’re waiting to finish a role change…
    if (roleTransitionPending) {
        if (millis() - roleTransitionStart >= ROLE_DELAY_MS) {
        // time’s up: apply it
        roleTransitionPending = false;
        currentRole = roleTransitionTarget;
        previousRole = roleTransitionTarget;

        // play the “you are now X station” video
        byte playCommand = 0x00;
        switch (currentRole) {
            case ROLE_GARBAGE:          playCommand = 0x01; break;
            case ROLE_CHOP_STATION:     playCommand = 0x02; break;
            case ROLE_COOK_STATION:     playCommand = 0x03; break;
            case ROLE_MIX_STATION:      playCommand = 0x04; break;
            case ROLE_ORDER_SERVE_STATION: playCommand = getVideoID(currentRecipe.name); break;
            case ROLE_OVEN_STATION:     playCommand = 0x12; break;
            default:                    playCommand = 0x00; break;
        }
        sendSprite(playCommand);
        }
        // while pending, skip processing any station code so we stay “in transition”
        return;
    }

    // If station is on fire, do nothing except loop
    if (onFire) {
      runFireEffect();
      delay(50);
      return; 
    }

    switch (currentRole) {
        case ROLE_GARBAGE:
            executeGarbageClient();
            break;
        case ROLE_CHOP_STATION:
            executeChopStationClient();
            break;
        case ROLE_COOK_STATION:
            executeCookStationClient();
            break;
        case ROLE_MIX_STATION:
            executeMixStationClient();
            break;
        case ROLE_ORDER_SERVE_STATION:
            executeOrderServeStationClient();
            break;
        case ROLE_OVEN_STATION:
            executeOvenStationClient();
            break;
        default:
            // Idle or perform default actions
            delay(100); 
            break;
    }
    delay(10);
}

/*************************************************************
   Function to initialize all hardware components
*************************************************************/
void initializeHardware() {
    // Initialize RFID reader
    rfid.PCD_Init();
    delay(50);
    Serial.println("RFID reader initialized.");

    // Initialize MOSFET pin for Neopixel power control
    pinMode(NEOPIXEL_MOSFET_PIN, OUTPUT);
    digitalWrite(NEOPIXEL_MOSFET_PIN, LOW);
    delay(50);

    // Initialize Neopixel strip
    strip.begin();
    strip.show();
    delay(50);
    Serial.println("Neopixel strip initialized."); 

    // Initialize button
    pinMode(BUTTON_PIN, INPUT_PULLUP);

    // Initialize button light pin
    pinMode(BUTTON_LIGHT_PIN, OUTPUT);
    digitalWrite(BUTTON_LIGHT_PIN, LOW); // Turn off button light

    // Turn on the button light to indicate readiness
    digitalWrite(BUTTON_LIGHT_PIN, HIGH);

    // Initialize DY-HV20T audio module
    audioSerial.begin(9600, SERIAL_8N1, AUDIO_RX, AUDIO_TX);
    audioModule.begin();
    audioModule.setCycleMode(DY::PlayMode::OneOff);
    audioModule.setVolume(24);
    audioModule.stop();

    // Initialize spriteSerial for media player
    spriteSerial.begin(9600, SERIAL_8N1, SPRITE_RX, SPRITE_TX);
    byte playCommand = 0x00;
    sendSprite(playCommand);  

    Serial.println("Hardware initialization complete.");
}

/*************************************************************
   ESP-NOW Callback: Data Received
*************************************************************/
void onDataRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    if (len == sizeof(struct_message)) {
        struct_message incomingMessage;
        memcpy(&incomingMessage, data, sizeof(struct_message));

        incomingMessage.requestType[REQUEST_TYPE_LENGTH - 1] = '\0';
        incomingMessage.recipeName[MAX_RECIPE_NAME_LENGTH - 1] = '\0';

        Serial.println("=== Data Received from Server ===");
        Serial.print("Request Type: ");
        Serial.println(incomingMessage.requestType);
        Serial.println("==================================");

        // 0) Pre-empt any pending role change if it’s a reinit or endgame
        if (roleTransitionPending) {
            if (strcmp(incomingMessage.requestType, "Reinitialize")==0) {
            roleTransitionPending = false;
            performReinitialization();  
            return;
            }
            if (strcmp(incomingMessage.requestType, "endgame")==0) {
            roleTransitionPending = false;
            Serial.println("Received 'endgame' message => playing sprite video #15...");
            audioModule.stop();
            clearStrip();
            onFire = false;
            currentRole = ROLE_NONE; 
            byte playCommand = 0x0F;
            pendingSpriteCmd = playCommand; 
            Serial.println("Re-initialization complete.");
            return;
            }
        }

        // RoleAssignment
        if (strcmp(incomingMessage.requestType, "RoleAssignment") == 0) {
            ClientRole newRole = static_cast<ClientRole>(incomingMessage.role);
            Serial.print("Received RoleAssignment, assigned role: ");
            Serial.println(newRole);
            handleRoleAssignment(newRole);
            return;
        }
        // CurrentRecipeResponse
        else if (strcmp(incomingMessage.requestType, "CurrentRecipeResponse") == 0) {
            memcpy(&currentRecipeData, &incomingMessage, sizeof(struct_message));
            dataReceived = true;
        }
        // DataResponse
        else if (strcmp(incomingMessage.requestType, "DataResponse") == 0) {
            memcpy(&serverData, &incomingMessage, sizeof(struct_message));
            dataReceived = true;
        }
        // NewRecipe
        else if (strcmp(incomingMessage.requestType, "NewRecipe") == 0) {
            Serial.println("Received NewRecipe from server.");

            // Reset if we are the mix station
            if (currentRole == ROLE_MIX_STATION) {
                resetRFIDData();
                mixState = MIX_IDLE;
                collectedIngredientCount = 0;
                memset(collectedIngredients, 0, sizeof(collectedIngredients));
                clearStrip();
                Serial.println("Mix Station reinitialized.");
            }
            // Update current recipe if we are the order/serve
            if (currentRole == ROLE_ORDER_SERVE_STATION) {
                memcpy(&currentRecipeData, &incomingMessage, sizeof(struct_message));

                bool recipeFound = false;
                for (int i = 0; i < totalRecipes; i++) {
                    if (strcmp(recipes[i].name, currentRecipeData.recipeName) == 0) {
                        currentRecipe = recipes[i];
                        recipeFound = true;
                        Serial.print("New recipe selected: ");
                        Serial.println(currentRecipe.name);

                        // Map recipe name to video ID
                        uint8_t videoID = getVideoID(currentRecipe.name);
                        if (videoID != 0x00) {
                            pendingSpriteCmd = videoID;
                            Serial.print("Playing video ID: ");
                            Serial.println(videoID, HEX); 
                        } else {
                            Serial.println("No video ID mapped for this recipe.");
                        }
                        break;
                    }
                }
                if (!recipeFound) {
                    Serial.println("Received unknown recipe.");
                }
            }
        }
        // Reinitialize
        else if (strcmp(incomingMessage.requestType, "Reinitialize") == 0) {
            Serial.println("Received Reinitialize command from server.");
            performReinitialization();
            return;
        }
        // OnFire / ExtinguishFire
        else if (strcmp(incomingMessage.requestType, "OnFire") == 0) {
            Serial.println("Station is now on FIRE mode!");
            // Possibly play a sprite video for fire
            pendingSpriteCmd = 0x0A;
            // Possibly also do audio, but you may not want to
            audioModule.stop();
            audioModule.playSpecified(4); // if you do want audio
            onFire = true;
        }
        else if (strcmp(incomingMessage.requestType, "ExtinguishFire") == 0) {
            Serial.println("Station Fire Extinguished. Returning to normal operation.");
            pendingSpriteCmd = 0x0B; 
            audioModule.stop();
            audioModule.playSpecified(9);

            //Reset Data
            resetAllRolesData();

            onFire = false;
        }
        // --- NEW: Handle StartRound1, StartRound2, StartRound3 WITHOUT audio ---
        else if (strcmp(incomingMessage.requestType, "StartRound1") == 0) {
            Serial.println("Received StartRound1 => resetting local data + playing round 1 video on sprite.");
            resetAllRolesData();

            // Send spriteSerial command for Round 1 (example 0x0A)
            pendingSpriteCmd = 0x13; 
        }
        else if (strcmp(incomingMessage.requestType, "StartRound2") == 0) {
            Serial.println("Received StartRound2 => resetting local data + playing round 2 video on sprite.");
            resetAllRolesData();
            pendingSpriteCmd = 0x0D; 
        }
        else if (strcmp(incomingMessage.requestType, "StartRound3") == 0) {
            Serial.println("Received StartRound3 => resetting local data + playing round 3 video on sprite.");
            resetAllRolesData();
            pendingSpriteCmd = 0x0E;
        }
        else if (strcmp(incomingMessage.requestType, "endgame") == 0) {
            Serial.println("Received 'endgame' message => playing sprite video #15...");
            audioModule.stop();
            clearStrip();
            onFire = false;
            currentRole = ROLE_NONE; 
            byte playCommand = 0x0F;
            pendingSpriteCmd = playCommand;
            Serial.println("Re-initialization complete.");

            // Optionally: set some LEDs, or do any other "game end" visuals
            // For example:
            // setStripColor(strip.Color(255, 128, 0)); 
            // delay(1000);
            // clearStrip();
        }
        else {
            // Other messages
        }
    } 
    else {
        Serial.println("Received data size mismatch!");
    }
}

/*************************************************************
   ESP-NOW Callback: Data Sent
*************************************************************/
void onDataSent(const uint8_t* mac_addr, esp_now_send_status_t status) {
    Serial.print("Last Packet Send Status: ");
    Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Delivery Success" : "Delivery Fail");
}

/*************************************************************
   Helper: Check if any RFID card is present
*************************************************************/
bool PICC_IsAnyCardPresent() {
    byte bufferATQA[2];
    byte bufferSize = sizeof(bufferATQA);
    rfid.PCD_WriteRegister(rfid.TxModeReg, 0x00);
    rfid.PCD_WriteRegister(rfid.RxModeReg, 0x00);
    rfid.PCD_WriteRegister(rfid.ModWidthReg, 0x26);
    MFRC522::StatusCode result = rfid.PICC_WakeupA(bufferATQA, &bufferSize);
    return (result == MFRC522::STATUS_OK || result == MFRC522::STATUS_COLLISION);
}

/*************************************************************
   Helper: Reset RFID Data
*************************************************************/
void resetRFIDData() {
    memset(&myData, 0, sizeof(myData));
    myData.chopCount = -1;   
    myData.cookCount = -1;   
    myData.reset = false;
    myData.playerScoreDelta = 0;
    Serial.println("RFID data reset.");
}

/*************************************************************
   Helper: Reset Shared Data
*************************************************************/
void resetSharedData() {
    memset(&myData, 0, sizeof(myData));
    memset(&serverData, 0, sizeof(serverData));
    memset(&currentRecipeData, 0, sizeof(currentRecipeData));
    dataReceived = false;
}

/*************************************************************
   Helper: Reset All Roles Data
*************************************************************/
void resetAllRolesData() {
    resetSharedData();

    chopState = IDLE;
    memset(localRFID, 0, sizeof(localRFID));       
    memset(localIngredient, 0, sizeof(localIngredient)); 
    localChopCount = 0;

    cookState = IDLE;
    cooking = false;
    isPlayingCookingAudio = false;
    hasPlayedCookingCompleteAudio = false;
    localCookCount = 0;

    mixState = MIX_IDLE;
    collectedIngredientCount = 0;
    memset(collectedIngredients, 0, sizeof(collectedIngredients));

    ovenState = IDLE;
    baking = false;
    isPlayingBakingAudio = false;
    hasPlayedBakingCompleteAudio = false;
    localBakeCount = 0; 
    
    burntPlate = false;

    clearStrip();
    Serial.println("All role-specific data reset.");
}

/*************************************************************
   Handle new RoleAssignment from server
*************************************************************/
void handleRoleAssignment(ClientRole newRole) {
    // play the “incoming” jingle / slide-in video immediately
    audioModule.stop();
    audioModule.playSpecified(11);
    pendingSpriteCmd = 0x16;

    // reset data right away so client UI is clean
    resetAllRolesData();

    // schedule the *actual* role assignment 4.5 s from now
    roleTransitionPending   = true;
    roleTransitionTarget    = newRole;
    roleTransitionStart     = millis();
}

/*************************************************************
   Send Data to Server
*************************************************************/
void sendDataToServer(struct_message* data) {
    Serial.println("=== Sending Data to Server ===");
    Serial.print("Request Type: ");
    Serial.println(data->requestType);
    Serial.println("==============================");

    esp_err_t result = esp_now_send(serverAddress, (uint8_t*)data, sizeof(struct_message));
    if (result == ESP_OK) {
        Serial.println("Data sent to server.");
    } else {
        Serial.print("Error sending data: ");
        Serial.println(result);
    }
}

/*************************************************************
   Update Chop Station LED Display
*************************************************************/
void updateChopCountDisplay(int chopCount) {
    if (chopCount < 0) chopCount = 0;
    if (chopCount > MAX_CHOP_COUNT) chopCount = MAX_CHOP_COUNT;

    if (chopCount == MAX_CHOP_COUNT) {
        digitalWrite(NEOPIXEL_MOSFET_PIN, HIGH);
        strip.clear();
        for (int i = 0; i < NUM_PIXELS; i++) {
            strip.setPixelColor(i, strip.Color(0, 255, 0)); // Green
        }
        strip.show();
    }
    else {
        int ledsPerChop = NUM_PIXELS / MAX_CHOP_COUNT;
        int ledsToLight = chopCount * ledsPerChop;

        if (chopCount > 0) {
            digitalWrite(NEOPIXEL_MOSFET_PIN, HIGH);
        } else {
            digitalWrite(NEOPIXEL_MOSFET_PIN, LOW);
        }

        strip.clear();
        for (int i = 0; i < ledsToLight; i++) {
            strip.setPixelColor(i, strip.Color(255, 255, 0));  // Yellow
        }
        strip.show();
    }
}

/*************************************************************
   Update Cook Station LED Display
*************************************************************/
void updateCookLedSection(int cookCount) {
    int ledsPerSection = NUM_PIXELS / MAX_COOK_COUNT;
    int totalLedsToLight = cookCount * ledsPerSection;
    if (totalLedsToLight > NUM_PIXELS) totalLedsToLight = NUM_PIXELS;

    uint32_t color;
    if (cookCount >= 1 && cookCount <= 6) {
        color = strip.Color(255, 255, 0); // Yellow
    } else if (cookCount >= 7 && cookCount <= 9) {
        color = strip.Color(0, 255, 0);   // Green
    } else if (cookCount >= MAX_COOK_COUNT) {
        color = strip.Color(255, 0, 0);   // Red
    } else {
        color = strip.Color(0, 0, 0);     // Off
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

/*************************************************************
   NEW/CHANGED FOR OVEN STATION: Update Oven (Bake) LED Display
*************************************************************/
void updateBakeLedSection(int bakeCount) {
    // We'll do a partial fill, just like cook station,
    // except the max is 8. 0..5 => Yellow, 6..7 => Green, 8 => Red (burnt).
    int ledsPerSection = NUM_PIXELS / MAX_BAKE_COUNT;
    int totalLedsToLight = bakeCount * ledsPerSection;
    if (totalLedsToLight > NUM_PIXELS) totalLedsToLight = NUM_PIXELS;

    uint32_t color;
    if (bakeCount >= 0 && bakeCount <= 3) {
        color = strip.Color(255, 255, 0); // Yellow
    } else if (bakeCount == 4) {
        color = strip.Color(0, 255, 0);   // Green
    } else {
        // Once it hits 8 or above => burnt
        color = strip.Color(255, 0, 0);   // Red
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

/*************************************************************
   Helper: Set entire Strip to a color
*************************************************************/
void setStripColor(uint32_t color) {
    digitalWrite(NEOPIXEL_MOSFET_PIN, HIGH);
    strip.clear();
    for(int i = 0; i < NUM_PIXELS; i++) {
        strip.setPixelColor(i, color);
    }
    strip.show();
}

/*************************************************************
   Helper: Clear Strip
*************************************************************/
void clearStrip() {
    strip.clear();
    strip.show();
    digitalWrite(NEOPIXEL_MOSFET_PIN, LOW);
}

/*************************************************************
   Helper: Blink Button LED
*************************************************************/
void blinkButtonLED() {
    digitalWrite(BUTTON_LIGHT_PIN, LOW);  
    delay(100);
    digitalWrite(BUTTON_LIGHT_PIN, HIGH); 
}

/*************************************************************
   Check if Button Pressed (with minimal/no debounce)
*************************************************************/
bool isButtonPressed(ButtonState& buttonState) {
    bool currentButtonState = digitalRead(BUTTON_PIN);
    unsigned long currentTime = millis();
    if (buttonState.lastButtonState == HIGH && currentButtonState == LOW) {
        if (currentTime - buttonState.lastButtonPressTime > DEBOUNCE_DELAY) {
            buttonState.lastButtonPressTime = currentTime;
            buttonState.lastButtonState = currentButtonState;
            return true;
        }
    }
    buttonState.lastButtonState = currentButtonState;
    return false;
}

/*************************************************************
   Read RFID
*************************************************************/
bool readRFID(char* rfidUID) {
    if (PICC_IsAnyCardPresent() && rfid.PICC_ReadCardSerial()) {
        snprintf(rfidUID, RFID_LENGTH, "%02X%02X%02X%02X",
                 rfid.uid.uidByte[0], rfid.uid.uidByte[1],
                 rfid.uid.uidByte[2], rfid.uid.uidByte[3]);
        rfidUID[RFID_LENGTH - 1] = '\0';
        resetRFIDReader();
        return true;
    } 
    return false;
}

/*************************************************************
   Wait for Server Response
*************************************************************/
bool waitForServerResponse(unsigned long timeout) {
    unsigned long startTime = millis();
    dataReceived = false;
    while (!dataReceived && (millis() - startTime) < timeout) {
        delay(10);
    }
    return dataReceived;
}

/*************************************************************
   Reset RFID Reader
*************************************************************/
void resetRFIDReader() {
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
}

/*************************************************************
   Gradually Light Up a Section of LEDs
*************************************************************/
void graduallyLightUpHalf(int startLED, int endLED) {
    digitalWrite(NEOPIXEL_MOSFET_PIN, HIGH);
    for (int i = startLED; i <= endLED; i++) {
        strip.setPixelColor(i, strip.Color(255, 255, 0));
        strip.show();
        delay(100);
    }
}

/*************************************************************
   Mix Station LED Updates
*************************************************************/
void updateMixStationLEDs() {
    strip.clear();
    int ledsPerIngredient = NUM_PIXELS / currentRecipe.numIngredients;
    for (int i = 0; i < collectedIngredientCount; i++) {
        int startLED = ledsPerIngredient * i;
        int endLED = startLED + ledsPerIngredient - 1;
        if (endLED >= NUM_PIXELS) endLED = NUM_PIXELS - 1;
        for (int j = startLED; j <= endLED; j++) {
            strip.setPixelColor(j, strip.Color(255, 255, 0));
        }
    }
    strip.show();
}

/*************************************************************
   Request & Update Current Recipe
*************************************************************/
void requestAndUpdateCurrentRecipe() {
    struct_message recipeRequestMsg;
    memset(&recipeRequestMsg, 0, sizeof(recipeRequestMsg));
    strncpy(recipeRequestMsg.requestType, "CurrentRecipeRequest", REQUEST_TYPE_LENGTH);
    recipeRequestMsg.requestType[REQUEST_TYPE_LENGTH - 1] = '\0';

    sendDataToServer(&recipeRequestMsg);
    if (!waitForServerResponse(5000)) {
        Serial.println("Timeout waiting for CurrentRecipeResponse.");
    } else {
        dataReceived = false;
        if (strcmp(currentRecipeData.requestType, "CurrentRecipeResponse") == 0) {
            bool recipeFound = false;
            for (int i = 0; i < totalRecipes; i++) {
                if (strcmp(recipes[i].name, currentRecipeData.recipeName) == 0) {
                    currentRecipe = recipes[i];
                    recipeFound = true;
                    Serial.print("Current recipe updated to: ");
                    Serial.println(currentRecipe.name);
                    break;
                }
            }
            if (!recipeFound) {
                Serial.println("Received unknown recipe.");
            }
        } else {
            Serial.print("Unexpected response type received: ");
            Serial.println(currentRecipeData.requestType);
        }
    }
}

/*************************************************************
   Perform Reinitialization
*************************************************************/
void performReinitialization() {
    Serial.println("Performing re-initialization...");
    audioModule.stop();
    resetAllRolesData();
    onFire = false;
    currentRole = ROLE_NONE; 
    roleTransitionPending = false;
    byte playCommand = 0x00;
    pendingSpriteCmd = playCommand; 
    Serial.println("Re-initialization complete.");
}

/*************************************************************
   Map Recipe Name to Video ID
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
    else return 0x00;
}

/*************************************************************
   Fire Effect Function
*************************************************************/
void runFireEffect() {
    // Just an example of random flicker in red/yellow range
    digitalWrite(NEOPIXEL_MOSFET_PIN, HIGH);

    for(int i = 0; i<NUM_PIXELS; i++) {
        // We'll pick random shades of orange/red
        int flickerR = random(220, 256);
        int flickerG = random(0, 100);
        int flickerB = 0;
        strip.setPixelColor(i, strip.Color(flickerR, flickerG, flickerB));
    }
    strip.show();
}

/*************************************************************
   send sprites twice
*************************************************************/
void sendSprite(uint8_t cmd) {
    for (int i = 0; i < 2; ++i) {       // two identical bytes
        spriteSerial.write(cmd);
        delay(40);                      // > one frame at 9600 baud
    }
    spriteSerial.flush();
}


/*************************************************************
   GARBAGE STATION EXECUTION
*************************************************************/
void executeGarbageClient() {
    if (isButtonPressed(garbageButtonState)) {
        Serial.println("Garbage Station: Button pressed, checking for RFID...");
        blinkButtonLED();
        resetRFIDData();

        if (readRFID(myData.rfid)) {
            Serial.print("RFID detected: ");
            Serial.println(myData.rfid);

            strncpy(myData.requestType, "DataUpdate", REQUEST_TYPE_LENGTH);
            myData.requestType[REQUEST_TYPE_LENGTH - 1] = '\0';

            myData.reset = true;
            myData.chopCount = -1; 
            myData.cookCount = -1; 
            strncpy(myData.ingredient, "", INGREDIENT_LENGTH);

            Serial.println("Sending reset request to server...");
            sendDataToServer(&myData);

            if (!waitForServerResponse(5000)) {
                Serial.println("Timeout waiting for server response.");
            } else {
                dataReceived = false;
                if (strcmp(serverData.requestType, "DataResponse") == 0) {
                    Serial.println("Garbage Station: RFID data reset successfully.");
                    setStripColor(strip.Color(0, 255, 0)); // Green
                    audioModule.stop();
                    audioModule.playSpecified(1); // success track
                    delay(2000);
                    clearStrip();
                } else {
                    Serial.print("Unexpected response type received: ");
                    Serial.println(serverData.requestType);
                }
            }
        } else {
            Serial.println("No RFID card detected.");
        }
    }
}

/*************************************************************
   CHOP STATION EXECUTION
*************************************************************/
void executeChopStationClient() {
    currentTime = millis();
    
    if (chopState == IDLE) {
        if (isButtonPressed(chopButtonState)) {
            Serial.println("Chop Station (IDLE): Button pressed, checking for RFID...");
            blinkButtonLED();
            resetRFIDData();

            if (readRFID(myData.rfid)) {
                Serial.print("RFID detected: ");
                Serial.println(myData.rfid);

                strncpy(myData.requestType, "DataRequest", REQUEST_TYPE_LENGTH);
                myData.requestType[REQUEST_TYPE_LENGTH - 1] = '\0';

                Serial.println("Requesting initial chop data from server...");
                sendDataToServer(&myData);

                if (!waitForServerResponse(5000)) {
                    Serial.println("Timeout waiting for server response.");
                } else {
                    dataReceived = false;
                    if (strcmp(serverData.requestType, "DataResponse") == 0) {
                        Serial.print("Got initial chop data. Ingredient: ");
                        Serial.print(serverData.ingredient);
                        Serial.print(", chopCount = ");
                        Serial.println(serverData.chopCount);

                        strncpy(localRFID, serverData.rfid, RFID_LENGTH);
                        strncpy(localIngredient, serverData.ingredient, INGREDIENT_LENGTH);
                        localChopCount = serverData.chopCount;

                        if (strncmp(localIngredient, "none", INGREDIENT_LENGTH) != 0 &&
                            localChopCount >= MAX_CHOP_COUNT) {
                            Serial.println("Plate is already at max chop => flash green.");
                            setStripColor(strip.Color(0, 255, 0));
                            delay(1000);
                            clearStrip();
                        }
                        else if (strncmp(localIngredient, "none", INGREDIENT_LENGTH) != 0) {
                            chopState = PROCESSING;
                            localChopCount++;
                            updateChopCountDisplay(localChopCount);
                            audioModule.playSpecified(2);
                        }
                        else {
                            Serial.println("Ingredient is 'none' => show red.");
                            setStripColor(strip.Color(255, 0, 0));
                            delay(1000);
                            clearStrip();
                        }
                    } else {
                        Serial.print("Unexpected response type: ");
                        Serial.println(serverData.requestType);
                    }
                }
            } else {
                Serial.println("No RFID card detected.");
            }
        }
    }
    else if (chopState == PROCESSING) {
        if (isButtonPressed(chopButtonState)) {
            Serial.println("Chop Station (PROCESSING): Button pressed => local chop increment.");
            blinkButtonLED();

            if (localChopCount < MAX_CHOP_COUNT) {
                localChopCount++;
                updateChopCountDisplay(localChopCount);
                audioModule.playSpecified(2);
            } else {
                Serial.println("Already at MAX chop => green flash");
                setStripColor(strip.Color(0, 255, 0));
            }
        }

        if (currentTime - lastCheckTime >= CHECK_INTERVAL) {
            lastCheckTime = currentTime;
            if (PICC_IsAnyCardPresent() && rfid.PICC_ReadCardSerial()) {
                resetRFIDReader();
            } else {
                Serial.println("Plate removed => sending final chop count to server.");
                memset(&myData, 0, sizeof(myData));
                strncpy(myData.rfid, localRFID, RFID_LENGTH);
                strncpy(myData.ingredient, localIngredient, INGREDIENT_LENGTH);
                myData.chopCount = localChopCount;
                myData.cookCount = -1;
                myData.reset = false;
                strncpy(myData.requestType, "DataUpdate", REQUEST_TYPE_LENGTH);

                sendDataToServer(&myData);
                if (!waitForServerResponse(2000)) {
                    Serial.println("No ACK from server on final chop update.");
                } else {
                    dataReceived = false;
                    Serial.println("Server ACK final chop update.");
                }

                clearStrip();
                resetRFIDData();
                resetRFIDReader();
                localChopCount = 0;
                memset(localRFID, 0, sizeof(localRFID));
                memset(localIngredient, 0, sizeof(localIngredient));

                chopState = IDLE;
            }
        }
    }
}

/*************************************************************
   COOK STATION EXECUTION
*************************************************************/
void executeCookStationClient() {
    currentTime = millis();

    if (cookState == IDLE) {
        if (isButtonPressed(cookButtonState)) {
            Serial.println("Cook Station (IDLE): Button pressed, checking for RFID...");
            blinkButtonLED();
            resetRFIDData();

            if (readRFID(myData.rfid)) {
                Serial.print("RFID detected: ");
                Serial.println(myData.rfid);

                strncpy(myData.requestType, "DataRequest", REQUEST_TYPE_LENGTH);
                myData.requestType[REQUEST_TYPE_LENGTH - 1] = '\0';
                Serial.println("Requesting data from server for cooking...");
                sendDataToServer(&myData);

                if (!waitForServerResponse(5000)) {
                    Serial.println("Timeout waiting for server response.");
                } else {
                    dataReceived = false;
                    if (strcmp(serverData.requestType, "DataResponse") == 0) {
                        Serial.print("Server response received. Ingredient: ");
                        Serial.print(serverData.ingredient);
                        Serial.print(", chopCount = ");
                        Serial.print(serverData.chopCount);
                        Serial.print(", cookCount = ");
                        Serial.println(serverData.cookCount);

                        strncpy(localRFID, serverData.rfid, RFID_LENGTH);
                        strncpy(localIngredient, serverData.ingredient, INGREDIENT_LENGTH);
                        localCookCount = serverData.cookCount;

                        if (serverData.cookCount == MAX_COOK_COUNT) {
                            Serial.println("Plate is already at MAX COOK -> BURNT scenario.");
                            burntPlate = true;
                            cooking = false;
                            isPlayingCookingAudio = false;
                            hasPlayedCookingCompleteAudio = false;
                            cookState = PROCESSING;

                            setStripColor(strip.Color(255, 0, 0)); // Red
                            audioModule.stop();
                            audioModule.playSpecified(4); // Burnt audio?
                        }
                        else if ((strncmp(localIngredient, "dough", INGREDIENT_LENGTH) == 0 ||
                                  strncmp(localIngredient, "meat", INGREDIENT_LENGTH) == 0 ||
                                  strncmp(localIngredient, "apple", INGREDIENT_LENGTH) == 0 ||
                                  strncmp(localIngredient, "tomato", INGREDIENT_LENGTH) == 0) &&
                                 serverData.chopCount >= MAX_CHOP_COUNT) {

                            Serial.println("Conditions met. Transition to PROCESSING for cooking.");
                            cooking = true;
                            isPlayingCookingAudio = false;
                            hasPlayedCookingCompleteAudio = false;
                            cookState = PROCESSING;

                            updateCookLedSection(localCookCount);
                            audioModule.stop();
                            audioModule.playSpecified(3); // cooking track
                            isPlayingCookingAudio = true;

                            digitalWrite(NEOPIXEL_MOSFET_PIN, HIGH);
                            lastCookTime = millis();
                            lastRFIDReadTime = millis();
                        } else {
                            Serial.println("Conditions not met. No cooking -> show red.");
                            setStripColor(strip.Color(255, 0, 0));
                            delay(1000);
                            clearStrip();
                        }
                    } else {
                        Serial.print("Unexpected response type: ");
                        Serial.println(serverData.requestType);
                    }
                }
            } else {
                Serial.println("No RFID card detected.");
            }
        }
    }
    else if (cookState == PROCESSING) {
        if (!burntPlate && cooking) {
            if ((currentTime - lastCookTime) >= 1000) {
                lastCookTime = currentTime;
                if (readRFID(myData.rfid)) {
                    if (strncmp(myData.rfid, localRFID, RFID_LENGTH) == 0) {
                        lastRFIDReadTime = millis();
                        if (localCookCount < MAX_COOK_COUNT) {
                            localCookCount++;
                            updateCookLedSection(localCookCount);
                            if (localCookCount == MAX_COOK_COUNT && !hasPlayedCookingCompleteAudio) {
                                Serial.println("plate burnt");
                                audioModule.stop();
                                audioModule.playSpecified(4);
                                hasPlayedCookingCompleteAudio = true;
                                isPlayingCookingAudio = false;
                            
                                // NEW FIRE LOGIC: send "Burnt" to server
                                struct_message burntMsg;
                                memset(&burntMsg, 0, sizeof(burntMsg));
                                strncpy(burntMsg.requestType, "Burnt", REQUEST_TYPE_LENGTH - 1);
                                burntMsg.requestType[REQUEST_TYPE_LENGTH - 1] = '\0';
                                strncpy(burntMsg.rfid, localRFID, RFID_LENGTH - 1);
                                sendDataToServer(&burntMsg);

                                burntPlate = true;
                            }
                        }
                    } else {
                        Serial.println("Different RFID card => stopping cooking process");

                        // 1) Finalize the old plate’s cookCount
                        memset(&myData, 0, sizeof(myData));
                        strncpy(myData.rfid,        localRFID,           RFID_LENGTH);
                        strncpy(myData.ingredient,  localIngredient,     INGREDIENT_LENGTH);
                        myData.chopCount = -1;  
                        myData.cookCount = localCookCount;  
                        myData.reset = false;
                        strncpy(myData.requestType, "DataUpdate", REQUEST_TYPE_LENGTH - 1);

                        sendDataToServer(&myData);
                        if (!waitForServerResponse(2000)) {
                            Serial.println("No ACK from server on final cook update.");
                        } else {
                            dataReceived = false;
                            Serial.println("Server ACK final cook update.");
                        }

                        // 2) Cleanup
                        cooking = false;
                        isPlayingCookingAudio = false;
                        audioModule.stop();
                        clearStrip();
                        digitalWrite(NEOPIXEL_MOSFET_PIN, LOW);

                        // 3) Reset local cooking variables
                        localCookCount = 0;
                        memset(localRFID, 0, sizeof(localRFID));
                        memset(localIngredient, 0, sizeof(localIngredient));

                        // 4) Return to IDLE
                        cookState = IDLE;
                    }
                } else {
                    if (millis() - lastRFIDReadTime > RFID_TIMEOUT) {
                        Serial.println("RFID card removed => finalize cooking updates");

                        // 1) Finalize the old plate’s cookCount
                        memset(&myData, 0, sizeof(myData));
                        strncpy(myData.rfid,        localRFID,           RFID_LENGTH);
                        strncpy(myData.ingredient,  localIngredient,     INGREDIENT_LENGTH);
                        myData.chopCount = -1;  
                        myData.cookCount = localCookCount;  
                        myData.reset = false;
                        strncpy(myData.requestType, "DataUpdate", REQUEST_TYPE_LENGTH - 1);

                        sendDataToServer(&myData);
                        if (!waitForServerResponse(2000)) {
                            Serial.println("No ACK from server on final cook update.");
                        } else {
                            dataReceived = false;
                            Serial.println("Server ACK final cook update.");
                        }

                        // 2) Cleanup
                        cooking = false;
                        isPlayingCookingAudio = false;
                        audioModule.stop();
                        clearStrip();
                        digitalWrite(NEOPIXEL_MOSFET_PIN, LOW);

                        // 3) Reset local cooking variables
                        localCookCount = 0;
                        memset(localRFID, 0, sizeof(localRFID));
                        memset(localIngredient, 0, sizeof(localIngredient));

                        // 4) Return to IDLE
                        cookState = IDLE;
                    }
                }
                delay(50);
            }
        }
        else if (burntPlate) {
            // If burnt, keep red LED on, wait for plate removal
            if (readRFID(myData.rfid)) {
                if (strncmp(myData.rfid, localRFID, RFID_LENGTH) == 0) {
                    lastRFIDReadTime = millis();
                } else {
                    Serial.println("Different RFID card => stopping burnt scenario");
                    burntPlate = false;
                    audioModule.stop();
                    clearStrip();
                    digitalWrite(NEOPIXEL_MOSFET_PIN, LOW);

                    localCookCount = 0;
                    memset(localRFID, 0, sizeof(localRFID));
                    memset(localIngredient, 0, sizeof(localIngredient));
                    cookState = IDLE;
                }
            } else {
                if (millis() - lastRFIDReadTime > RFID_TIMEOUT) {
                    Serial.println("RFID card removed => stopping burnt scenario");
                    burntPlate = false;
                    audioModule.stop();
                    clearStrip();
                    digitalWrite(NEOPIXEL_MOSFET_PIN, LOW);
                    localCookCount = 0;
                    memset(localRFID, 0, sizeof(localRFID));
                    memset(localIngredient, 0, sizeof(localIngredient));
                    cookState = IDLE;
                }
            }
            delay(50);
        }
    }
}

/*************************************************************
   MIX STATION EXECUTION
*************************************************************/
void executeMixStationClient() {
    currentTime = millis();

    if (isButtonPressed(mixButtonState)) {
        Serial.print("Mix Station: Button pressed. Current State: ");
        Serial.println(mixState == MIX_IDLE ? "IDLE" : "MIXING");

        blinkButtonLED();
        resetRFIDData();

        if (readRFID(myData.rfid)) {
            Serial.print("RFID detected: ");
            Serial.println(myData.rfid);

            strncpy(myData.requestType, "DataRequest", REQUEST_TYPE_LENGTH);
            myData.requestType[REQUEST_TYPE_LENGTH - 1] = '\0';

            Serial.println("Requesting data from server...");
            sendDataToServer(&myData);

            if (!waitForServerResponse(5000)) {
                Serial.println("Timeout waiting for server response.");
            } else {
                dataReceived = false;
                if (strcmp(serverData.requestType, "DataResponse") == 0) {
                    struct_message plateData = serverData;
                    requestAndUpdateCurrentRecipe();

                    bool ingredientValid = false;
                    for (int i = 0; i < currentRecipe.numIngredients; i++) {
                        bool alreadyCollected = false;
                        for (int j = 0; j < collectedIngredientCount; j++) {
                            if (strcmp(collectedIngredients[j], currentRecipe.ingredients[i].name) == 0) {
                                alreadyCollected = true;
                                break;
                            }
                        }
                        if (alreadyCollected) continue;

                        if (strcmp(plateData.ingredient, currentRecipe.ingredients[i].name) == 0) {
                            bool requirementsMet = true;

                            if (currentRecipe.ingredients[i].requiresChop) {
                                if (plateData.chopCount < currentRecipe.ingredients[i].requiredChopCount) {
                                    requirementsMet = false;
                                }
                            }
                            if (currentRecipe.ingredients[i].requiresCook) {
                                if (plateData.cookCount < currentRecipe.ingredients[i].requiredCookCountMin ||
                                    plateData.cookCount > currentRecipe.ingredients[i].requiredCookCountMax) {
                                    requirementsMet = false;
                                }
                            }
                            if (requirementsMet) {
                                ingredientValid = true;
                                strncpy(collectedIngredients[collectedIngredientCount], plateData.ingredient, INGREDIENT_LENGTH - 1);
                                collectedIngredientCount++;

                                if (collectedIngredientCount == currentRecipe.numIngredients) {
                                    // All ingredients collected
                                    audioModule.stop();
                                    audioModule.playSpecified(6); 
                                } else {
                                    audioModule.stop();
                                    audioModule.playSpecified(5); 
                                }

                                int ledsPerIngredient = NUM_PIXELS / currentRecipe.numIngredients;
                                int startLED = ledsPerIngredient * (collectedIngredientCount - 1);
                                int endLED = startLED + ledsPerIngredient - 1;
                                if (endLED >= NUM_PIXELS) endLED = NUM_PIXELS - 1;
                                graduallyLightUpHalf(startLED, endLED);

                                memset(&myData, 0, sizeof(myData));
                                strncpy(myData.rfid, plateData.rfid, RFID_LENGTH);
                                strncpy(myData.requestType, "DataUpdate", REQUEST_TYPE_LENGTH);
                                myData.chopCount = 0;
                                myData.cookCount = -1;
                                myData.reset = true;
                                strncpy(myData.ingredient, "none", INGREDIENT_LENGTH);
                                myData.playerScoreDelta = 0;

                                sendDataToServer(&myData);
                                break;
                            }
                        }
                    }

                    if (!ingredientValid) {
                        Serial.println("Ingredient does not meet recipe requirements or already collected.");
                        setStripColor(strip.Color(255, 0, 0));
                        delay(1000);
                        updateMixStationLEDs();
                    }

                    if (collectedIngredientCount == currentRecipe.numIngredients) {
                        Serial.println("All ingredients collected. Mixing complete.");
                        memset(&myData, 0, sizeof(myData));
                        strncpy(myData.rfid, plateData.rfid, RFID_LENGTH);
                        strncpy(myData.requestType, "DataUpdate", REQUEST_TYPE_LENGTH);
                        strncpy(myData.ingredient, currentRecipe.name, INGREDIENT_LENGTH);
                        myData.chopCount = 0; 
                        myData.cookCount = -1; 
                        myData.reset = false;
                        myData.playerScoreDelta = 0;

                        sendDataToServer(&myData);

                        setStripColor(strip.Color(0, 255, 0));
                        delay(2000);
                        clearStrip();

                        mixState = MIX_IDLE;
                        collectedIngredientCount = 0;
                        memset(collectedIngredients, 0, sizeof(collectedIngredients));
                        Serial.println("Mix Station has been reset to MIX_IDLE.");
                    } else {
                        mixState = MIX_MIXING;
                    }
                } else {
                    Serial.print("Unexpected response type received: ");
                    Serial.println(serverData.requestType);
                }
                dataReceived = false;
            }
        } else {
            Serial.println("No RFID card detected or failed to read.");
        }
    }
}

/*************************************************************
   ORDER/SERVE STATION EXECUTION
*************************************************************/
void executeOrderServeStationClient() {
    if (isButtonPressed(orderServeButtonState)) {
        Serial.println("Order/Serve Station: Button pressed, checking for RFID...");
        blinkButtonLED();
        resetRFIDData();

        if (readRFID(myData.rfid)) {
            Serial.print("RFID detected: ");
            Serial.println(myData.rfid);

            strncpy(myData.requestType, "DataRequest", REQUEST_TYPE_LENGTH);
            myData.requestType[REQUEST_TYPE_LENGTH - 1] = '\0';
            myData.chopCount = -1;  
            myData.cookCount = -1;  
            myData.reset = false;
            myData.playerScoreDelta = 0;

            sendDataToServer(&myData);
            if (!waitForServerResponse(5000)) {
                Serial.println("Timeout waiting for server response.");
            } else {
                dataReceived = false;
                struct_message updateMsg;
                memset(&updateMsg, 0, sizeof(updateMsg));
                strncpy(updateMsg.rfid, myData.rfid, RFID_LENGTH - 1);
                strncpy(updateMsg.requestType, "DataUpdate", REQUEST_TYPE_LENGTH - 1);
                updateMsg.chopCount = -1;  
                updateMsg.cookCount = -1;  
                updateMsg.reset = true;  
                strncpy(updateMsg.ingredient, "none", INGREDIENT_LENGTH - 1);
                updateMsg.playerScoreDelta = 0;
                
                // Compare final item to the local recipe
                bool itemMatchesRecipe = (strcmp(serverData.ingredient, currentRecipe.name) == 0);

                if (itemMatchesRecipe) {
                    // if it's bakeable => check final cookCount 
                    if (currentRecipe.bakeable) {
                        if (serverData.cookCount >= currentRecipe.requiredBakeCountMin 
                            && serverData.cookCount <= currentRecipe.requiredBakeCountMax) 
                        {
                            updateMsg.playerScoreDelta = 1; // success
                            Serial.println("Order/Serve: Plate is properly baked => awarding +1");
                        } else {
                            updateMsg.playerScoreDelta = 0; // fails
                            Serial.println("Order/Serve: Plate not in bake range => no score.");
                        }
                    } else {
                        // Non-bakeable => just accept
                        updateMsg.playerScoreDelta = 1;
                    }
                } else {
                    updateMsg.playerScoreDelta = 0;
                    Serial.println("Order/Serve: Ingredient mismatch => no score.");
                }
                
                updateMsg.role = ROLE_ORDER_SERVE_STATION;
                
                if (updateMsg.playerScoreDelta == 1){
                    sendDataToServer(&updateMsg);

                    /* // Request new recipe from server
                    struct_message selectRecipeMsg;
                    memset(&selectRecipeMsg, 0, sizeof(selectRecipeMsg));
                    strncpy(selectRecipeMsg.requestType, "SelectNewRecipe", REQUEST_TYPE_LENGTH - 1);
                    selectRecipeMsg.requestType[REQUEST_TYPE_LENGTH - 1] = '\0'; */

                    setStripColor(strip.Color(0, 255, 0));
                    audioModule.stop();
                    audioModule.playSpecified(7);
                    sendSprite(0x14);
                    delay(2000);
                    // sendDataToServer(&selectRecipeMsg);
                    clearStrip();
                } else {
                    Serial.println("Incorrect recipe");
                    sendDataToServer(&updateMsg);
                    setStripColor(strip.Color(255, 0, 0));
                    audioModule.stop();
                    audioModule.playSpecified(8);
                    sendSprite(0x15);
                    delay(2000);
                    sendSprite(getVideoID(currentRecipe.name));
                    clearStrip();
                }
                resetRFIDData();
            }
        } else {
            Serial.println("No RFID card detected.");
        }
    }
}

/*************************************************************
   NEW/CHANGED FOR OVEN STATION:
   OVEN STATION EXECUTION
*************************************************************/
void executeOvenStationClient() {
    currentTime = millis();
    
    if (ovenState == IDLE) {
        if (isButtonPressed(ovenButtonState)) {
            Serial.println("Oven Station (IDLE): Button pressed, checking for RFID...");
            blinkButtonLED();
            resetRFIDData();

            if (readRFID(myData.rfid)) {
                Serial.print("RFID detected: ");
                Serial.println(myData.rfid);

                strncpy(myData.requestType, "DataRequest", REQUEST_TYPE_LENGTH);
                myData.requestType[REQUEST_TYPE_LENGTH - 1] = '\0';
                Serial.println("Requesting data from server for baking...");
                sendDataToServer(&myData);

                if (!waitForServerResponse(5000)) {
                    Serial.println("Timeout waiting for server response.");
                } else {
                    dataReceived = false;
                    if (strcmp(serverData.requestType, "DataResponse") == 0) {
                        Serial.print("Server response received. Ingredient: ");
                        Serial.print(serverData.ingredient);
                        Serial.print(", chopCount = ");
                        Serial.print(serverData.chopCount);
                        Serial.print(", cookCount (used as bakeCount) = ");
                        Serial.println(serverData.cookCount);

                        strncpy(localRFID, serverData.rfid, RFID_LENGTH);
                        strncpy(localIngredient, serverData.ingredient, INGREDIENT_LENGTH);
                        localBakeCount = serverData.cookCount;  // We treat cookCount as bakeCount here

                        // Check if the ingredient (recipe name) is bakeable
                        bool bakeableFound = false;
                        for (int i = 0; i < totalRecipes; i++) {
                            if (strcmp(recipes[i].name, localIngredient) == 0 && recipes[i].bakeable) {
                                bakeableFound = true;
                                break;
                            }
                        }

                        // Check if it's bakeable
                        if (bakeableFound) {
                          //Check if it's burnt
                          if (localBakeCount >= MAX_BAKE_COUNT) {                            
                            Serial.println("Plate is already at MAX BAKE -> BURNT scenario.");
                            burntPlate = true;
                            baking = false;
                            isPlayingBakingAudio = false;
                            hasPlayedBakingCompleteAudio = false;
                            ovenState = PROCESSING;

                            setStripColor(strip.Color(255, 0, 0)); // Red
                            audioModule.stop();
                            audioModule.playSpecified(4); // Burnt audio}
                            Serial.println("Plate is already at MAX BAKE -> BURNT scenario.");
                          } else {
                              Serial.println("Correct ingredient found: apple pie. Begin baking...");
                              baking = true;
                              isPlayingBakingAudio = false;
                              hasPlayedBakingCompleteAudio = false;
                              ovenState = PROCESSING;

                              updateBakeLedSection(localBakeCount);
                              audioModule.stop();
                              // You can use a new track if you have a "baking track"
                              audioModule.playSpecified(3); 
                              isPlayingBakingAudio = true;

                              digitalWrite(NEOPIXEL_MOSFET_PIN, HIGH);
                              lastBakeTime = millis();
                              lastRFIDReadTime = millis();
                          }
                        }
                        else {
                            Serial.println("This plate is not 'apple pie' => show red.");
                            setStripColor(strip.Color(255, 0, 0));
                            delay(1000);
                            clearStrip();
                        }
                    } else {
                        Serial.print("Unexpected response type: ");
                        Serial.println(serverData.requestType);
                    }
                }
            } else {
                Serial.println("No RFID card detected.");
            }
        }
    }
    else if (ovenState == PROCESSING) {
        if (!burntPlate && baking) {
            // Increment bakeCount if the plate remains in place
            if ((currentTime - lastBakeTime) >= 1000) {
                lastBakeTime = currentTime;
                if (readRFID(myData.rfid)) {
                    // Same plate?
                    if (strncmp(myData.rfid, localRFID, RFID_LENGTH) == 0) {
                        lastRFIDReadTime = millis();
                        if (localBakeCount < MAX_BAKE_COUNT) {
                            localBakeCount++;
                            updateBakeLedSection(localBakeCount);

                            // If we just hit 8 => burnt
                            if (localBakeCount >= MAX_BAKE_COUNT && !hasPlayedBakingCompleteAudio) {
                                Serial.println("Baking complete but effectively burnt => track 4");
                                audioModule.stop();
                                audioModule.playSpecified(4);
                                hasPlayedBakingCompleteAudio = true;
                                isPlayingBakingAudio = false;
                            
                                // Send "Burnt" to server
                                struct_message burntMsg;
                                memset(&burntMsg, 0, sizeof(burntMsg));
                                strncpy(burntMsg.requestType, "Burnt", REQUEST_TYPE_LENGTH - 1);
                                burntMsg.requestType[REQUEST_TYPE_LENGTH - 1] = '\0';
                                strncpy(burntMsg.rfid, localRFID, RFID_LENGTH - 1);
                                sendDataToServer(&burntMsg);

                                burntPlate = true;
                            }
                        }
                    } else {
                        // Different plate => stop baking
                        Serial.println("Different RFID card => stopping baking process");
                        baking = false;
                        isPlayingBakingAudio = false;
                        audioModule.stop();
                        clearStrip();
                        digitalWrite(NEOPIXEL_MOSFET_PIN, LOW);
                        ovenState = IDLE;
                    }
                } else {
                    // Card removed
                    if (millis() - lastRFIDReadTime > RFID_TIMEOUT) {
                        Serial.println("RFID card removed => finalize baking updates");
                        baking = false;
                        isPlayingBakingAudio = false;
                        audioModule.stop();

                        clearStrip();
                        digitalWrite(NEOPIXEL_MOSFET_PIN, LOW);

                        memset(&myData, 0, sizeof(myData));
                        strncpy(myData.rfid, localRFID, RFID_LENGTH);
                        strncpy(myData.ingredient, localIngredient, INGREDIENT_LENGTH);
                        myData.chopCount = -1;  
                        myData.cookCount = localBakeCount;  // reuse cookCount for bakeCount
                        myData.reset = false;
                        strncpy(myData.requestType, "DataUpdate", REQUEST_TYPE_LENGTH);

                        sendDataToServer(&myData);
                        if (!waitForServerResponse(2000)) {
                            Serial.println("No ACK from server on final bake update.");
                        } else {
                            dataReceived = false;
                            Serial.println("Server ACK final bake update.");
                        }

                        localBakeCount = 0;
                        memset(localRFID, 0, sizeof(localRFID));
                        memset(localIngredient, 0, sizeof(localIngredient));
                        ovenState = IDLE;
                    }
                }
                delay(50);
            }
        }
        else if (burntPlate) {
            // If burnt, keep red LED on, wait for plate removal
            if (readRFID(myData.rfid)) {
                if (strncmp(myData.rfid, localRFID, RFID_LENGTH) == 0) {
                    lastRFIDReadTime = millis();
                } else {
                    Serial.println("Different RFID card => stopping burnt scenario (oven).");
                    burntPlate = false;
                    audioModule.stop();
                    clearStrip();
                    digitalWrite(NEOPIXEL_MOSFET_PIN, LOW);
                    localBakeCount = 0;
                    memset(localRFID, 0, sizeof(localRFID));
                    memset(localIngredient, 0, sizeof(localIngredient));
                    ovenState = IDLE;
                }
            } else {
                if (millis() - lastRFIDReadTime > RFID_TIMEOUT) {
                    Serial.println("RFID card removed => stopping burnt scenario (oven).");
                    burntPlate = false;
                    audioModule.stop();
                    clearStrip();
                    digitalWrite(NEOPIXEL_MOSFET_PIN, LOW);
                    localBakeCount = 0;
                    memset(localRFID, 0, sizeof(localRFID));
                    memset(localIngredient, 0, sizeof(localIngredient));
                    ovenState = IDLE;
                }
            }
            delay(50);
        }
    }
}
