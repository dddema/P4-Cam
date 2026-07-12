# P4_DARKROOM // WebGL Batch Film Darkroom

`P4_DARKROOM` is an industry-standard, hardware-integrated batch film developer project. It contains a high-performance offline Progressive Web App (PWA) client and an ESP32-P4 microcontroller server firmware designed to mount an SD card and stream raw camera exposures over Wi-Fi.

---

## 📁 Repository Structure

The project is split into two main sections:
* **/web**: Progressive Web Application (PWA) client. Implements batch GPU color-grading (WebGL 3D LUTs), procedural Hoskins grain, Black Mist diffusion highlight bloom, and swipe comparison previews.
* **/firmware**: ESP32-P4 server firmware. Mounts the SD card (supports 1-bit/4-bit mode configuration), handles Soft AP Wi-Fi broadcasts with an integrated Captive Portal DNS interceptor (stabilizes iOS connections), and streams JSON spools using chunked transfer encoding to prevent heap fragmentation.

---

## 🚀 Key Features

* **WebGL Batch Emulsion Processor**: Parallel color-grading of multi-frame rolls directly on the GPU.
* **Emulation Profiles**: Built-in emulations mapped to iconic film stocks:
  * **Portra 400**: Soft warm pastel tones.
  * **Gold 200**: Rich warm golden sunset amber tones.
  * **Provia 100**: Vibrant daylight balance and saturation.
  * **Fuji FP-100C**: Faded vintage instant-film contrast.
  * **Superia 400**: Punchy print tones with rich autumn greens and reds.
  * **Superia XPRO**: Cool cross-processed teal shadow casts.
  * **Polaroid 690**: Faded retro instant look with muddy shadows.
* **Custom LUT Ingestion**: Upload custom `.cube` 3D LUT matrices or Hald CLUT image maps (`.png`, `.jpg`) at runtime.
* **Large Screen Preset Comparison Swiper**: A scroll-snapped fullscreen swipe compare gallery displaying the user's first frame graded in all available styles in real-time.
* **Cinematic Effects Suite**:
  * **Black Mist Diffusion**: Dilative highlight bloom filter that creates soft halation and light leaks without washing out shadow details.
  * **Sine-Free Isotropic Film Grain**: Procedural random grain utilizing Hoskins sine-free GPU hash functions to resolve diagonal aliasing artifacts on Apple GPUs.
  * **Lens Aberrations**: Corner chromatic color separation and radial vignette falloff.
* **PWA & Offline Capability**: Full service worker caching with versioning, manifest metadata, and responsive layouts designed with bezel-chin bounds for iPhone home indicator safety.

---

## 🛠️ API & Connection Protocol

The web application communicates directly with local camera spools (typically an ESP32-CAM Access Point or server) via simple HTTP endpoints:
1. **Fetch Exposures**: `GET /api/images`
   * Returns a JSON array of raw exposures:
     ```json
     [
       { "filename": "IMG_0001.JPG", "size": 1024500, "timestamp": 1690000000000 },
       { "filename": "IMG_0002.JPG", "size": 980120, "timestamp": 1690000000450 }
     ]
     ```
2. **Download Exposures**: `GET /api/images/{filename}`
   * Serves the raw JPEG payload for grading.

---

## 💻 Getting Started

### 1. Build and Flash the Firmware (`/firmware`)
The firmware is built using PlatformIO.

#### Prerequisites
* Install [PlatformIO Core](https://platformio.org/) or the PlatformIO IDE extension inside VS Code.
* Connect your ESP32-P4 Dev Board via USB.

#### Compilation & Upload
Run the following commands inside the `/firmware` directory:
```bash
# Compile firmware binaries
pio run

# Flash the firmware to your ESP32-P4
pio run --target upload

# Launch the Serial Monitor for diagnostics
pio device monitor
```

*Note: On boot, the ESP32-P4 mounts the SD card, attempts to connect to local credentials, and falls back to starting an Access Point: `SSID: P4-Cam-Spool / Password: antigravity` on IP `192.168.4.1`.*

#### 🧪 LilyGO T-Embed Testing Board
To flash and test the firmware on a **LilyGO T-Embed** (ESP32-S3), the configuration has a pre-configured `lilygo-t-embed` target:
1. Compile and upload by passing the environment flag (`-e`) to the PlatformIO tool:
   ```bash
   # Compile T-Embed binary
   pio run -e lilygo-t-embed
   
   # Upload T-Embed binary
   pio run -e lilygo-t-embed --target upload
   ```
2. The compiler automatically references the ESP32-S3 DevKit configuration and links the `src/main-lilygo.cpp` source code, bypassing `src/main.cpp`.
3. The T-Embed configuration:
   * Sets **GPIO 46** `HIGH` to power up internal display and SD peripherals.
   * Maps the shared SPI bus pins explicitly: **SCK: 40, MISO: 38, MOSI: 41, CS: 39**.
   * Mounts the SD card using the standard Arduino `SD.h` library over SPI instead of SDMMC.

---

### 2. Launch the Web Application Client (`/web`)
To run the PWA client locally and serve dynamic demo photos:

1. Launch a static server from the `/web` directory:
   ```bash
   python3 -m http.server 8000
   ```
2. Open your browser and navigate to `http://localhost:8000`.
3. To test the PWA with Service Workers and offline caching, connect via an HTTPS secure context (e.g. through Ngrok or Tailscale).
