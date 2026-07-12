/**
 * P4_DARKROOM // ESP32-P4 Microcontroller Firmware
 * 
 * High-performance Wi-Fi camera spool server designed for ESP32-P4.
 * Mounts an SD Card via SDMMC, spins up a local web server, and exposes
 * REST endpoints for batch color-grading and film development on the client.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SD_MMC.h>
#include <FS.h>
#include <DNSServer.h>

// Hardware configuration
const bool SDMMC_1BIT_MODE = false; // Set to true if custom board wiring requires 1-bit mode

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
    
    File root = SD_MMC.open("/");
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
    File file = SD_MMC.open(filePath, FILE_READ);
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
    Serial.println("\n=== P4_DARKROOM Microcontroller Init ===");

    // 1. Mount SD Card via SDMMC interface (1-bit mode configuration supported)
    Serial.printf("Mounting SD Card (1-Bit Mode: %s)...\n", SDMMC_1BIT_MODE ? "ON" : "OFF");
    if (!SD_MMC.begin("/sdcard", SDMMC_1BIT_MODE)) {
        Serial.println("Error: SD Card mount failed! Check wiring/card formatting.");
    } else {
        uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
        Serial.printf("SD Card Mounted. Size: %llu MB\n", cardSize);
    }

    // 2. Configure WiFi mode
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

    // 3. Register HTTP routing endpoints
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
