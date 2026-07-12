/**
 * P4_DARKROOM // LILYGO T-Embed testing firmware
 * 
 * High-performance Wi-Fi camera spool server designed for the LilyGO T-Embed (ESP32-S3).
 * Mounts the SD Card via SPI, drives GPIO 46 HIGH to enable board power, starts a web server
 * and a DNS Captive Portal to serve spools to the PWA client without connection drops.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SD.h>
#include <SPI.h>
#include <FS.h>
#include <DNSServer.h>

// LilyGO T-Embed Pinouts
#define PIN_POWER_ON 46
#define SD_CS 39
#define SD_SCK 40
#define SD_MOSI 41
#define SD_MISO 38

// Wi-Fi Config
const char* AP_SSID = "P4-Cam-Spool";
const char* AP_PASS = "antigravity";
const IPAddress AP_IP(192, 168, 4, 1);
const IPAddress AP_GATEWAY(192, 168, 4, 1);
const IPAddress AP_SUBNET(255, 255, 255, 0);

// Optionally connect to local router (STA mode) if credentials provided
const char* STA_SSID = "";
const char* STA_PASS = "";

WebServer server(80);
DNSServer dnsServer;

// Set CORS Headers for PWA cross-origin compatibility
void sendCORSHeaders() {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    server.sendHeader("Access-Control-Allow-Headers", "*");
}

// Handle preflight OPTIONS requests
void handleOptions() {
    sendCORSHeaders();
    server.send(200, "text/plain", "OK");
}

// GET /api/images - Streams photos stored on the SD Card using Chunked Transfer Encoding
// This avoids dynamic String concatenation and prevents memory heap fragmentation.
void handleListImages() {
    sendCORSHeaders();
    
    File root = SD.open("/");
    if (!root) {
        server.send(500, "application/json", "{\"error\":\"Failed to open SD Card root\"}");
        return;
    }
    if (!root.isDirectory()) {
        root.close();
        server.send(500, "application/json", "{\"error\":\"Root is not a directory\"}");
        return;
    }

    // Set chunked encoding header
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "application/json", "");
    server.sendContent("[");

    File file = root.openNextFile();
    bool first = true;
    char buffer[192];

    while (file) {
        if (!file.isDirectory()) {
            String name = String(file.name());
            String nameLower = name;
            nameLower.toLowerCase();

            // Filter out system files and process JPEG/PNG camera images
            if (!name.startsWith(".") && 
                (nameLower.endsWith(".jpg") || nameLower.endsWith(".jpeg") || nameLower.endsWith(".png"))) {
                
                if (!first) {
                    server.sendContent(",");
                }
                first = false;

                uint32_t size = file.size();
                time_t writeTime = file.getLastWrite();
                uint64_t timestamp = (writeTime > 0) ? (uint64_t)writeTime * 1000 : millis();

                // Format JSON structure inside a fixed char buffer to prevent heap allocations
                snprintf(buffer, sizeof(buffer), 
                         "{\"filename\":\"%s\",\"size\":%u,\"timestamp\":%llu}", 
                         name.c_str(), size, timestamp);
                
                server.sendContent(buffer);
            }
        }
        file.close(); // Release file resource handle
        file = root.openNextFile();
    }
    root.close();
    
    server.sendContent("]");
    server.sendContent(""); // Empty chunk closes transmission
}

// GET /api/images/{filename} - Streams a raw JPEG from the SD Card
void handleGetImage() {
    sendCORSHeaders();
    
    // Extract filename from URI path: e.g. "/api/images/IMG_0001.JPG"
    String uri = server.uri();
    String prefix = "/api/images/";
    if (!uri.startsWith(prefix)) {
        server.send(400, "text/plain", "Bad Request");
        return;
    }
    
    String filename = uri.substring(prefix.length());
    if (filename.length() == 0) {
        server.send(400, "text/plain", "Filename missing");
        return;
    }

    // Security check: prevent directory traversal
    if (filename.indexOf('/') != -1 || filename.indexOf('\\') != -1 || filename.indexOf("..") != -1) {
        server.send(403, "text/plain", "Access Denied");
        return;
    }

    Serial.printf("File: %s requested by client.\n", filename.c_str());

    String filePath = "/" + filename;
    File file = SD.open(filePath, FILE_READ);
    if (!file || file.isDirectory()) {
        if (file) file.close();
        Serial.printf("Error: File %s not found on SD card.\n", filename.c_str());
        server.send(404, "text/plain", "File Not Found: " + filename);
        return;
    }

    // Send content type based on extension
    String nameLower = filename;
    nameLower.toLowerCase();
    String contentType = "image/jpeg";
    if (nameLower.endsWith(".png")) {
        contentType = "image/png";
    }

    server.streamFile(file, contentType);
    file.close();
    
    Serial.printf("File: %s successfully sent to client.\n", filename.c_str());
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n=== P4_DARKROOM LilyGO T-Embed Init ===");

    // 1. Turn on Board Power for Display and SD Card Slot Peripherals (GPIO 46)
    Serial.println("Enabling LilyGO T-Embed board power (GPIO 46 HIGH)...");
    pinMode(PIN_POWER_ON, OUTPUT);
    digitalWrite(PIN_POWER_ON, HIGH);
    
    // CRITICAL: Give the SD Card's internal microcontroller time to boot up!
    Serial.println("Waiting for SD card microcontroller boot delay (800ms)...");
    delay(800);

    // CRITICAL: Disable all CS pins on the shared SPI bus to prevent collisions!
    Serial.println("Disabling all peripheral CS pins (TFT & SD) to prevent SPI collisions...");
    pinMode(41, OUTPUT); digitalWrite(41, HIGH); // CC1101 TFT CS
    pinMode(13, OUTPUT); digitalWrite(13, HIGH); // CC1101 SD CS
    pinMode(10, OUTPUT); digitalWrite(10, HIGH); // Standard TFT CS
    pinMode(39, OUTPUT); digitalWrite(39, HIGH); // Standard SD CS
    delay(50); // Settle bus lines

    // 2. Initialize SPI Bus and Mount SD Card
    // LilyGO T-Embed has two hardware revisions with different SPI pinouts:
    // Variant 1 (CC1101 version): CS:13, SCK:11, MOSI:9, MISO:10 (Shares bus with TFT on CS:41)
    // Variant 2 (Standard version): CS:39, SCK:40, MOSI:41, MISO:38
    
    bool sdMounted = false;
    
    // Enable pull-ups on target pins
    pinMode(10, INPUT_PULLUP); // MISO
    pinMode(9, INPUT_PULLUP);  // MOSI
    
    Serial.println("Attempting SD mount: T-Embed-CC1101 Pinout (CS:13, SCK:11, MOSI:9, MISO:10)...");
    SPI.begin(11, 10, 9, 13); // SCK, MISO, MOSI, SS
    
    if (SD.begin(13, SPI, 1000000)) { // Run at 1MHz for maximum signal integrity
        sdMounted = true;
        Serial.println("SD Card Mounted successfully using CC1101 pinout!");
    } else {
        Serial.println("CC1101 pinout failed. Re-configuring SPI for Standard pinout...");
        SPI.end();
        delay(200); // Settle bus lines
        
        // Enable pull-ups on Standard target pins
        pinMode(38, INPUT_PULLUP); // MISO
        pinMode(41, INPUT_PULLUP); // MOSI
        
        Serial.println("Attempting SD mount: T-Embed Standard Pinout (CS:39, SCK:40, MOSI:41, MISO:38)...");
        SPI.begin(40, 38, 41, 39); // SCK, MISO, MOSI, SS
        
        if (SD.begin(39, SPI, 1000000)) { // Run at 1MHz
            sdMounted = true;
            Serial.println("SD Card Mounted successfully using Standard pinout!");
        } else {
            SPI.end();
        }
    }

    if (!sdMounted) {
        Serial.println("Error: SD Card mount failed on both T-Embed pinout configurations!");
        Serial.println("-> CRITICAL: Ensure your SD card is formatted as FAT32. exFAT is NOT supported!");
        Serial.println("-> Check if the SD card is pushed all the way into the spring slot.");
    } else {
        uint64_t cardSize = SD.cardSize() / (1024 * 1024);
        Serial.printf("SD Card Mounted. Size: %llu MB\n", cardSize);
    }

    // 3. Configure WiFi mode
    bool isSTA = (strlen(STA_SSID) > 0);
    if (isSTA) {
        Serial.printf("Connecting to WiFi Station SSID: %s...\n", STA_SSID);
        WiFi.begin(STA_SSID, STA_PASS);
        int retries = 0;
        while (WiFi.status() != WL_CONNECTED && retries < 20) {
            delay(500);
            Serial.print(".");
            retries++;
        }
        if (WiFi.status() == WL_CONNECTED) {
            Serial.print("\nConnected! Station IP: ");
            Serial.println(WiFi.localIP());
        } else {
            Serial.println("\nWiFi connection failed. Falling back to AP Mode...");
            isSTA = false;
        }
    }

    if (!isSTA) {
        Serial.printf("Starting Access Point: %s...\n", AP_SSID);
        WiFi.mode(WIFI_AP);
        WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET);
        WiFi.softAP(AP_SSID, AP_PASS);
        Serial.print("Soft AP Active. IP Address: ");
        Serial.println(WiFi.softAPIP());
        
        // Start Captive Portal DNS Interceptor on default port 53.
        // Forces all domain queries to resolve to local AP IP.
        dnsServer.start(53, "*", AP_IP);
        Serial.println("Captive Portal DNS Interceptor active.");
    }

    // 4. Register HTTP routing endpoints
    server.on("/", []() {
        sendCORSHeaders();
        String html = "<HTML><HEAD><TITLE>P4 Cam Spool Server</TITLE>";
        html += "<style>body{background:#0d0d0d;color:#fff;font-family:monospace;padding:30px;text-align:center;}";
        html += "h1{color:#ff5500;} a{color:#ff5500;text-decoration:none;border:1px solid #ff5500;padding:8px 15px;margin-top:20px;display:inline-block;}</style></HEAD>";
        html += "<BODY><h1>P4_DARKROOM</h1><p>CAMERA Wi-Fi SPOOL ACTIVE</p>";
        html += "<p>IP: 192.168.4.1</p>";
        html += "<a href=\"/api/images\">VIEW API SPOOL</a></BODY></HTML>";
        server.send(200, "text/html", html);
    });
    server.on("/api/images", HTTP_GET, handleListImages);
    server.on("/api/images", HTTP_OPTIONS, handleOptions);
    
    // Explicit API Handshake Ping Endpoint
    server.on("/api/ping", HTTP_GET, []() {
        sendCORSHeaders();
        server.send(200, "application/json", "{\"status\":\"ok\"}");
    });
    server.on("/api/ping", HTTP_OPTIONS, handleOptions);
    
    // Apple Captive Portal Checks
    server.on("/hotspot-detect.html", []() {
        sendCORSHeaders();
        server.send(200, "text/html", "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
    });
    server.on("/library/test/success.html", []() {
        sendCORSHeaders();
        server.send(200, "text/html", "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
    });
    
    // Android Captive Portal Checks
    server.on("/generate_204", []() {
        sendCORSHeaders();
        server.send(204, "text/plain", "");
    });
    
    // Register catch-all routing for streaming specific filenames dynamically
    server.onNotFound([]() {
        if (server.method() == HTTP_OPTIONS) {
            handleOptions();
        } else if (server.uri().startsWith("/api/images/")) {
            handleGetImage();
        } else {
            // Redirect unrecognized HTTP requests to AP IP root to assist Captive redirection
            sendCORSHeaders();
            server.sendHeader("Location", "http://192.168.4.1/");
            server.send(302, "text/plain", "Redirecting...");
        }
    });

    server.begin();
    Serial.println("HTTP REST server successfully started on port 80.");
}

void loop() {
    dnsServer.processNextRequest();
    server.handleClient();
}
