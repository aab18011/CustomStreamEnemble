#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <errno.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <cjson/cJSON.h>
#include "lan_check.h"
#include "wlan_check.h"
#include "check_dependencies.h"
#include "python3_test.h"
#include "config_handler.h"
#include "obs_handler.h"

// Macro to suppress unused parameter warnings
#define UNUSED(x) (void)(x)

volatile sig_atomic_t running = 1;

typedef enum {
	INIT_FLAG_LAN_OK	= 1 << 0,	// Bit 0: LAN connectivity
	INIT_FLAG_WLAN_OK	= 1 << 1,	// Bit 1: Internet connectivity
	INIT_FLAG_FFMPEG_OK	= 1 << 2,	// Bit 2: FFmpeg installed and runnable
	INIT_FLAG_V4L2_OK	= 1 << 3,	// Bit 3: v4l2loopback installed and loaded
	INIT_FLAG_V4L2_EXT	= 1 << 4, 	// Bit 4: v4l2loopback support 16 devices
	INIT_FLAG_PYTHON_OK	= 1 << 5 	// Bit 5: Python3 installed and runnable
} init_flag_t;

typedef enum {
	INIT_FINISHED = 1 << 0,		// Bit 0: Initialization finished
	STREAM_READY = 1 << 1,		// Bit 1: Are we in full run mode (V4L2 ext mode + python +ffmpeg?)
	OBS_SOCK_STREAM = 1 << 2,	// Bit 2: OBS websocket handler ready?
	WEB_SOCK_STREAM = 1 << 3,	// Bit 3: HTML websocket handler ready?
	CONFIG_HANDLER = 1 << 4,	// Bit 4: Configuration files handler ready? (This includes hot reload sub-handler)
	LOGGING_DAEMON = 1 << 5,	// Bit 5: Logging system ready? (Includes: File I/O ready + Console I/O ready)
	PRG_READY = 1 << 6,			// Bit 6: Is program safe to start?
	SYSRUNNING = 1 << 7,			// Bit 7: Are we now running yet?
	SYSERROR = 1 << 8				// Bit 8: Has an error been thrown?
} pre_load_flag_t;

typedef enum {
	PRGRUNNING = 1 << 0,			// Bit 0: Are we running? (we keep a unique one for the while loop we are throwing ourselves into, so we can pass control back, if need be, to the pre_load system)
	PRGERROR = 1 << 1,			// Bit 1: Any error detected (we will have an error buffer so we can set this to 1 whenever it is populated with n > 0 errors)
	STOP_SIGNAL = 1 << 2 		// Bit 2: Have we received the stop signal (from the user of course). 
} program_run_state_t;

uint32_t init_state = 0; // Bitfield for initialization status

int init_config_system() {
	//The code is setup, and these are examples of totally fine code, but useless to me.
	//I need to set this up to actually match my current setup.
	//register_config("max_streams", &max_streams, CONFIG_INT);
	//register_config("output_path", &output_path, CONFIG_STRING);
	// Register more as needed

	// Load configs from a fixed directory (create "configs/" with .json files like {"max_streams": 10, "output_path": "/path/to/output"})
	//load_configs("configs/");
	return 0; // if the init of the config system went okay; return 1 if the config files encountered an error but some loaded; 2 otherwise
}

/*
   HANDLE SHUTDOWN - Alert user of shutdown (or log it at least)
*/
void handle_shutdown(int sig) {
	fprintf(stdout, "Received signal %d, shutting down...\n", sig);
	running = 0;
}

int main(int argc, char *argv[]) {
	UNUSED(argc); // Suppress unused parameter warning
	UNUSED(argv); // Suppress unused parameter warning

	// Setup Shutdown Handles
	signal(SIGINT, handle_shutdown);
	signal(SIGTERM, handle_shutdown);

	fprintf(stdout, "UNIFIED STREAMING SYSTEM STARTING ...\n");

	// On program start, we can gather LAN connectivity information to ensure we can see any remote servers running on the network.
	// This is used to setup the connection to IP-PoE cameras via RTMP/RTSP, and OBS websocket server, as well as the local
	// scoreboard webservers that contain HTML/CSS/JS code for showing real-time game data.
	lan_info_t info;
	if (check_LAN(&info) == 0 && info.reachable) {
		init_state |= INIT_FLAG_LAN_OK;
	}
	fprintf(stdout, "LAN Interface: %s, Gateway: %s, Local IP: %s, Reachable: %s\n",
		info.ifname, info.gateway, info.local_addr, info.reachable ? "Yes" : "No");

	// Now we check for internet access, as this will be used later on in the program
	int inet_access = check_public_dns();
	if (inet_access) {
		init_state |= INIT_FLAG_WLAN_OK;
	}
	fprintf(stdout, "Internet Access: %s\n", inet_access ? "Yes" : "No");

	// Check for ffmpeg and v4l2 + v4l2loopback modules
	if (check_application("ffmpeg")) {
		init_state |= INIT_FLAG_FFMPEG_OK;
		fprintf(stdout, "FFmpeg check: Found\n");
	} else {
		fprintf(stdout, "FFmpeg check: Not found\n");
	}

	const char *v4l2_modules[] = {"videodev", "v4l2loopback"};
	int v4l2_results[2];
	check_multiple_modules(v4l2_modules, 2, v4l2_results);
	if (v4l2_results[0] && v4l2_results[1]) {
		init_state |= INIT_FLAG_V4L2_OK;
		fprintf(stdout, "v4l2loopback check: videodev=%d, v4l2loopback=%d\n", v4l2_results[0], v4l2_results[1]);

		// Check for extended support (exactly 16 devices) by verifying /dev/video0 to /dev/video15
		int max_dev = 0;
		for (int i = 0; i < 16; i++) {
			char path[32];
			snprintf(path, sizeof(path), "/dev/video%d", i);
			struct stat st;
			if (stat(path, &st) == 0 && S_ISCHR(st.st_mode)) {
				max_dev++;
			} else {
				break; // Stop at first missing device
			}
		}
		if (max_dev == 16) {
			init_state |= INIT_FLAG_V4L2_EXT;
			fprintf(stdout, "v4l2loopback devices: %d (EXT supported)\n", max_dev);
		} else {
			fprintf(stdout, "v4l2loopback devices: %d (EXT not supported)\n", max_dev);
		}
	} else {
		fprintf(stdout, "v4l2loopback check: videodev=%d, v4l2loopback=%d\n", v4l2_results[0], v4l2_results[1]);
	}

	// Check for python3
	if (check_application("python3")) {
		init_state |= INIT_FLAG_PYTHON_OK;
		fprintf(stdout, "Python3 check: Found\n");
	} else {
		fprintf(stdout, "Python3 check: Not found\n");
	}

	// This key check is important since its a huge portion of whats important to the functionality of the system. If these dont work, then we cannot stream
	// our IP-PoE cameras, no matter the camera.
	bool streaming_enabled = ((init_state & (INIT_FLAG_FFMPEG_OK | INIT_FLAG_V4L2_OK | INIT_FLAG_V4L2_EXT)) ==
	                          (INIT_FLAG_FFMPEG_OK | INIT_FLAG_V4L2_OK | INIT_FLAG_V4L2_EXT));
	if (!streaming_enabled) {
		fprintf(stdout, "Streaming functionality disabled due to missing dependencies.\n");
	} else {
		fprintf(stdout, "Streaming functionality enabled.\n");
	}

	// Test Python integration if available
	if (init_state & INIT_FLAG_PYTHON_OK) {
		fprintf(stdout, "Testing Python integration...\n");
		if (test_python_integration() == 0) {
			fprintf(stdout, "Python test completed successfully.\n");
		} else {
			fprintf(stdout, "Python test failed.\n");
		}
	} else {
		fprintf(stdout, "Python not available, skipping integration test.\n");
	}


	init_config_system();
	// State Loop, we can set running to
	// false when the inside of the loop
	// has decided to finish
	while (running) {
		// Here is where the actual main program runs, and based on the flags set from earlier, we can move forward
		sleep(1);
	}

	fprintf(stdout, "SHUTDOWN COMPLETE.\n");
	return EXIT_SUCCESS;
}