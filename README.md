# P4_DARKROOM // WebGL Batch Film Darkroom

Repo for the WIP_NAME Camera. Webapp and firmware for the Hardware.

`P4_DARKROOM` is an hardware-integrated batch film developer project. It contains a high-performance offline Progressive Web App (PWA) client and an ESP32-P4 microcontroller server firmware designed to mount an SD card and stream raw camera exposures over Wi-Fi.

---

## Repository Structure

The project is split into two main sections:
* **/web**: Progressive Web Application (PWA) client. Implements batch GPU color-grading (WebGL 3D LUTs), procedural Hoskins grain, Black Mist diffusion highlight bloom, and swipe comparison previews.
* **/firmware**: ESP32-P4 server firmware. Mounts the SD card (supports 1-bit/4-bit mode configuration), handles Soft AP Wi-Fi broadcasts with an integrated Captive Portal DNS interceptor (stabilizes iOS connections), and streams JSON spools using chunked transfer encoding to prevent heap fragmentation.

---

## Key Features

* **WebGL Batch Emulsion Processor**: Parallel color-grading of multi-frame rolls directly on the GPU.
* **Emulation Profiles**: Built-in emulations mapped to iconic film stocks.
* **Custom LUT Ingestion**: Upload custom `.cube` 3D LUT matrices or Hald CLUT image maps (`.png`, `.jpg`) at runtime.
* **Large Screen Preset Comparison Swiper**: A scroll-snapped fullscreen swipe compare gallery displaying the user's first frame graded in all available styles in real-time.
* **Cinematic Effects Suite**:
  * **Black Mist Diffusion**: Dilative highlight bloom filter that creates soft halation and light leaks without washing out shadow details.
  * **Sine-Free Isotropic Film Grain**: Procedural random grain utilizing Hoskins sine-free GPU hash functions to resolve diagonal aliasing artifacts on Apple GPUs.
  * **Lens Aberrations**: Corner chromatic color separation and radial vignette falloff.
* **PWA & Offline Capability**: Full service worker caching with versioning, manifest metadata, and responsive layouts designed with bezel-chin bounds for iPhone home indicator safety.

---

## API & Connection Protocol

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
