# ROC System Main Controller

**Version 0.0.1-alpha.1** | **License: MIT** | **Repository: [aab18011/CustomStreamEnemble](https://github.com/aab18011/CustomStreamEnemble)**

#### Author: Aidan A. Bradley

## Overview

The ROC System Main Controller is a robust, fault-tolerant camera streaming system designed for real-time video processing, initially developed for paintball tournament livestreaming. It manages network-connected IP PoE cameras, streaming video to virtual devices (`/dev/video10` to `/dev/video25`) using FFmpeg and v4l2loopback. The system ensures reliability in challenging environments, handling network disconnects (e.g., unplugged cables, cut wires, or network loops) of up to 10-15 minutes with immediate reconnection upon restoration. Built in C for performance, this project is a complete rewrite of an earlier Python-based system, offering lower latency, reduced resource usage, and enhanced stability.

The system uses a multi-daemon architecture, with `main.c` orchestrating daemons for network monitoring, camera health monitoring, and video pipeline processing (`videopipe.c`). It supports interactive camera configuration, persistent stream optimization, and comprehensive logging, making it suitable for production-grade streaming applications.

**Current Status**: This is an alpha release (v0.0.1-alpha.1), validated with a single camera (`192.168.1.21`). Future releases will add support for multiple cameras, OBS WebSocket integration, a scoreboard webserver, and custom input handling.

## Key Features

- **Multi-Daemon Architecture**: Separate daemons for network monitoring (`DAEMON_NETWORK_MONITOR`) and camera health monitoring (`DAEMON_CAMERA_STREAMER`), managed by `main.c`, ensure fault tolerance and scalability.
- **Robust Disconnect/Reconnect Handling**: The `videopipe` daemon probes camera connections (e.g., `192.168.1.21:1935`) every 60 seconds, using exponential backoff (5s to 30s) to recover from outages, ensuring streams resume quickly after up to 15-minute disconnects.
- **v4l2loopback Integration**: Automatically configures virtual video devices (`/dev/video10` to `/dev/video25`), verifying at least 16 devices with custom names (e.g., `Cam10`, `Cam11`).
- **Interactive Camera Configuration**: If `/etc/roc/cameras.json` is missing, an interactive wizard in `main.c` guides users to configure camera details (IP, username, password).
- **Network and Camera Health Monitoring**: Network monitor checks LAN connectivity every 30 seconds; camera health monitor restarts `videopipe` if needed and logs errors to `/var/log/videopipe.log`.
- **Persistent Stream Optimization**: Caches optimal stream settings (e.g., `ext` stream at 896x512 @ 19fps) in `/var/lib/roc/camera_discovery.json` for faster reconnects.
- **Comprehensive Logging**: Logs to `/var/log/videopipe.log`, `/var/log/cameras/camera*.log`, and `/var/log/ffmpeg_errors.log` for easy debugging.
- **Performance-Driven C Implementation**: Rewritten from a Python prototype to eliminate GIL-related bottlenecks, reducing CPU usage by up to 50% and improving real-time performance.

## Project Evolution

Originally developed as a Python-based system (`main.py`, v2.5.0b) for automating OBS scene transitions in paintball tournaments, the ROC System faced limitations in performance (GIL overhead, high CPU usage) and reliability (no caching of FFmpeg stream settings, redundant v4l2loopback recompilation). The C-based rewrite (v0.0.1-alpha.1) addresses these issues with:
- **Faster Execution**: Pthread-based daemons and native C code reduce latency and resource usage.
- **Stream Settings Caching**: Persistent storage of optimal FFmpeg settings in `/var/lib/roc/camera_discovery.json` eliminates delays in stream initialization.
- **Optimized Initialization**: Checks for existing v4l2loopback devices to avoid unnecessary recompilation, saving up to 30 seconds at startup.
- **Enhanced Reliability**: Active TCP probing and robust recovery mechanisms handle network disruptions effectively.

See the [Development Journey](#development-journey-from-python-to-c) in the [release notes](RELEASE.md) for details.

## Requirements

- **Operating System**: Linux (tested on Ubuntu/Debian-based distributions).
- **Dependencies**:
  - FFmpeg (`ffmpeg`)
  - Python 3 (`python3`)
  - GCC (`gcc`)
  - Make (`make`)
  - v4l2loopback kernel module
  - cJSON library for JSON parsing
- **Hardware**: Network-connected IP PoE cameras (e.g., at `192.168.1.21:1935`).
- **Permissions**: Root access required for v4l2loopback and configuration file management.

## Installation

1. **Clone the Repository**:
   ```bash:disable-run
   git clone https://github.com/aab18011/CustomStreamEnemble.git
   cd CustomStreamEnemble
   ```

2. **Install Dependencies**:
   Ensure system dependencies are installed:
   ```bash
   sudo apt update
   sudo apt install -y ffmpeg python3 gcc make libjson-c-dev
   ```

3. **Compile the Project**:
   Build the executables (`main_controller`, `videopipe`, `v4l2loopback_mod_install`):
   ```bash
   make clean && make
   ```
   This generates binaries in the `bin` folder.

4. **Add `bin` Folder to PATH (Optional)**:
   To run executables without installing them to `/usr/local/bin`, add the `bin` folder to your PATH:
   ```bash
   export PATH=$PATH:/path/to/CustomStreamEnemble/bin
   ```
   Replace `/path/to/CustomStreamEnemble/bin` with the absolute path to the `bin` directory (e.g., `/home/user/CustomStreamEnemble/bin`). To make this persistent, add the export command to `~/.bashrc` or `~/.bash_profile`.

5. **Install v4l2loopback (Optional)**:
   If not already installed, the system will attempt to run `v4l2loopback_mod_install` during initialization. Ensure the kernel module source is available or pre-install it manually.

## Usage

1. **Run the Main Controller**:
   Execute as root to access v4l2loopback devices and create configuration files:
   ```bash
   sudo ./bin/main_controller
   ```

2. **Configure Cameras**:
   If `/etc/roc/cameras.json` is missing, the program prompts for camera details (IP, username, password). Example configuration:
   ```json
   [
       {
           "ip": "192.168.1.21",
           "user": "admin",
           "password": "your_password"
       }
   ]
   ```

3. **Monitor Output**:
   - Verify video streams on virtual devices:
     ```bash
     ffplay /dev/video10
     ```
   - Check logs for debugging:
     ```bash
     tail -f /var/log/videopipe.log
     ```

4. **Test Disconnect/Reconnect**:
   - Simulate a network disconnect:
     ```bash
     sudo iptables -A INPUT -p tcp --dport 1935 -j DROP
     ```
   - Wait 10-15 minutes, then reconnect:
     ```bash
     sudo iptables -D INPUT -p tcp --dport 1935 -j DROP
     ```
   - Confirm stream resumes via logs and `ffplay /dev/video10`.

## Project Structure

- **`src/main.c`**: Orchestrates the system, managing initialization, daemon spawning, and cleanup.
- **`src/videopipe.c`**: Handles camera stream processing, FFmpeg execution, and disconnect/reconnect logic.
- **`bin/`**: Contains compiled executables (`main_controller`, `videopipe`, `v4l2loopback_mod_install`).
- **`/etc/roc/cameras.json`**: Stores camera configurations (IP, credentials).
- **`/var/lib/roc/camera_discovery.json`**: Caches optimal stream settings.
- **`/var/log/`**: Logs (`videopipe.log`, `cameras/camera*.log`, `ffmpeg_errors.log`).

## Known Limitations

- **Single Camera Testing**: Validated with one camera (`192.168.1.21`); multi-camera support pending.
- **Network Monitor Scope**: Limited to LAN checks every 30 seconds; camera-specific IP monitoring planned.
- **FFmpeg Hangs**: Rare cases may require timeout mechanisms.
- **Dependencies**: Assumes `v4l2loopback_mod_install` is in PATH or `bin` folder.

## Future Improvements

- **OBS WebSocket Integration**: Add a daemon (`DAEMON_OBS_WEBSOCKET`) to communicate with a remote OBS client, enabling dynamic scene switching based on game states.
- **Scoreboard Webserver Daemon**: Introduce a daemon (`DAEMON_SCOREBOARD_WEBSERVER`) to serve real-time scoreboard data via HTML/CSS/JS, replacing Python’s Selenium-based parsing.
- **Feed Continuity**: Ensure `/dev/video*` devices remain active with fallback images (e.g., “No Signal” PNG) to prevent OBS restarts during camera failures.
- **Secure Password Storage**: Store passwords as SHA-256 hashes in configurations, using PBKDF2 for key derivation.
- **Custom I/O via Function Keys**: Add a daemon (`DAEMON_INPUT_HANDLER`) to handle custom keyboard inputs (e.g., F1-F12) for actions like scene switching or process diagnostics.
- **Dynamic Camera Management**: Support runtime addition/removal of cameras without restarting.
- **Monitoring Integration**: Export metrics (e.g., camera uptime, network latency) to Prometheus.
- **FFmpeg Timeouts**: Implement watchdog timers to handle hung FFmpeg processes.


## Contributing

Contributions are welcome! To contribute:
1. Fork the repository.
2. Create a feature branch (`git checkout -b feature/your-feature`).
3. Commit changes (`git commit -m "Add your feature"`).
4. Push to the branch (`git push origin feature/your-feature`).
5. Open a pull request.

Please include tests and update documentation as needed.

## License

This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for details.
