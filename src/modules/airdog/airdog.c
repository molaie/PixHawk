#include <nuttx/config.h>
#include <nuttx/sched.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <nuttx/wqueue.h>
#include <nuttx/clock.h>

#include <systemlib/systemlib.h>
#include <systemlib/err.h>
#include <uORB/topics/vehicle_command.h>
#include <uORB/topics/vehicle_status.h>
#include <uORB/topics/debug_key_value.h>
#include <uORB/topics/airdog_status.h>

#include <drivers/drv_gpio.h>
#include <commander/px4_custom_mode.h>

enum REMOTE_BUTTON_STATE {
	PAUSE=1,
	START=2,
};

enum MAV_MODE_FLAG {
	MAV_MODE_FLAG_CUSTOM_MODE_ENABLED = 1, /* 0b00000001 Reserved for future use. | */
	MAV_MODE_FLAG_TEST_ENABLED = 2, /* 0b00000010 system has a test mode enabled. This flag is intended for temporary system tests and should not be used for stable implementations. | */
	MAV_MODE_FLAG_AUTO_ENABLED = 4, /* 0b00000100 autonomous mode enabled, system finds its own goal positions. Guided flag can be set or not, depends on the actual implementation. | */
	MAV_MODE_FLAG_GUIDED_ENABLED = 8, /* 0b00001000 guided mode enabled, system flies MISSIONs / mission items. | */
	MAV_MODE_FLAG_STABILIZE_ENABLED = 16, /* 0b00010000 system stabilizes electronically its attitude (and optionally position). It needs however further control inputs to move around. | */
	MAV_MODE_FLAG_HIL_ENABLED = 32, /* 0b00100000 hardware in the loop simulation. All motors / actuators are blocked, but internal software is full operational. | */
	MAV_MODE_FLAG_MANUAL_INPUT_ENABLED = 64, /* 0b01000000 remote control input is enabled. | */
	MAV_MODE_FLAG_SAFETY_ARMED = 128, /* 0b10000000 MAV safety set to armed. Motors are enabled / running / can start. Ready to fly. | */
	MAV_MODE_FLAG_ENUM_END = 129, /*  | */
};

struct gpio_button_s {
	enum REMOTE_BUTTON_STATE state;
	int pin;
	bool button_pressed;
};

struct airdog_app_s {
	struct work_s work;
	uint8_t base_mode;
	int gpio_fd;
	int inputs;
	struct gpio_button_s follow_button;
	struct airdog_status_s airdog_status;
	int airdog_status_sub;
	int current_custom_mode;
};

static struct airdog_app_s airdog_data;
static bool airdog_running = false;
static orb_advert_t cmd_pub = -1;

__EXPORT int airdog_main(int argc, char *argv[]);

/**
 * Mainloop of daemon.
 */
void airdog_cycle(FAR void *arg);
void airdog_start(FAR void *arg);

void send_set_mode(uint8_t base_mode, uint8_t custom_main_mode);

/**
 * Print the correct usage.
 */
static void usage(const char *reason);

static void
usage(const char *reason)
{
	if (reason)
		warnx("%s\n", reason);
	errx(1, "usage: airdog {start|stop|status} [-p <additional params>]\n\n");
}

int airdog_main(int argc, char *argv[]) 
{
	if (argc < 1)
		usage("missing command");

	if (!strcmp(argv[1], "start")) {

		if (airdog_running) {
			warnx("airdog remote app already running\n");
			/* this is not an error */
			exit(0);
		}

		memset(&airdog_data, 0, sizeof(airdog_data));
		int ret = work_queue(LPWORK, &airdog_data.work, airdog_start, &airdog_data, 0);

		if (ret != 0) {
			errx(1, "failed to queue work: %d", ret);
		} else {
			airdog_running = true;
			warnx("airdog button listener starting\n");
		}
		exit(0);
	}

	if (!strcmp(argv[1], "stop")) {
		if (airdog_running) {
				airdog_running = false;
				warnx("stop");
			} else {
				errx(1, "not running");
			}
	}

	if (!strcmp(argv[1], "status")) {
		if (airdog_running) {
			warnx("\trunning\n");
		} else {
			warnx("\tnot started\n");
		}
		exit(0);
	}

	usage("unrecognized command");
	exit(1);
}

void send_set_mode(uint8_t base_mode, enum PX4_CUSTOM_MAIN_MODE custom_main_mode) {
	/* TODO this is very ugly, need to rewrite app to C++ and use class fields instead of static var */
	struct vehicle_command_s cmd;
	memset(&cmd, 0, sizeof(cmd));

	int state_sub = orb_subscribe(ORB_ID(vehicle_status));
	struct vehicle_status_s state;
	orb_copy(ORB_ID(vehicle_status), state_sub, &state);

	/* fill command */
	cmd.command = VEHICLE_CMD_DO_SET_MODE;
	cmd.confirmation = false;
	cmd.param1 = base_mode;
	cmd.param2 = custom_main_mode;
	cmd.source_system = state.system_id;
	cmd.source_component = state.component_id;
	// TODO add parameters AD_VEH_SYSID, AD_VEH_COMP to set target id
	cmd.target_system = 1;
	cmd.target_component = 50;

	if (cmd_pub < 0) {
		cmd_pub = orb_advertise(ORB_ID(vehicle_command), &cmd);

	} else {
		orb_publish(ORB_ID(vehicle_command), cmd_pub, &cmd);
	}
}

void airdog_start(FAR void *arg)
{
	FAR struct airdog_app_s *priv = (FAR struct airdog_app_s *)arg;

	priv->base_mode = MAV_MODE_FLAG_SAFETY_ARMED | MAV_MODE_FLAG_CUSTOM_MODE_ENABLED;
	priv->follow_button.pin = 0;
	priv->follow_button.button_pressed = false;
	priv->follow_button.state = PAUSE;
/*
	button2.pin = 1;
	button2.type = REMOTE_BUTTON_TAKEOFF_LAND;
	button2.button_pressed = false;
*/
	priv->inputs = priv->follow_button.pin + 1;
	

	/* open GPIO device */
	priv->gpio_fd = open(PX4FMU_DEVICE_PATH, 0);
	if (priv->gpio_fd < 0) {
		// TODO find way to print errors
		//printf("airdog: GPIO device \"%s\" open fail\n", PX4FMU_DEVICE_PATH);
		airdog_running = false;
		return;
	}
	ioctl(priv->gpio_fd, GPIO_SET_INPUT, priv->inputs);

	/* initialize vehicle status structure */
	memset(&priv->airdog_status, 0, sizeof(priv->airdog_status));

	/* subscribe to vehicle status topic */
	priv->airdog_status_sub = orb_subscribe(ORB_ID(airdog_status));

	/* add worker to queue */
	int ret = work_queue(LPWORK, &priv->work, airdog_cycle, priv, 0);

	if (ret != 0) {
		// TODO find way to print errors
		//printf("gpio_led: failed to queue work: %d\n", ret);
		airdog_running = false;
		return;
	}
};

void airdog_cycle(FAR void *arg) {

	FAR struct airdog_app_s *priv = (FAR struct airdog_app_s *)arg;

	bool updated;
	orb_check(priv->airdog_status_sub, &updated);

	if (updated) {
		orb_copy(ORB_ID(airdog_status), priv->airdog_status_sub, &priv->airdog_status);
	}
	/*warnx("testing: %d", priv->airdog_status.custom_mode);*/
	int custom_mode = priv->airdog_status.custom_mode >> 16;
	

	/* check the GPIO */
	uint32_t gpio_values;
	ioctl(priv->gpio_fd, GPIO_GET, &gpio_values);

	if (!(gpio_values & (1 << priv->follow_button.pin))) {
		if (priv->follow_button.button_pressed == false){
			warnx("button 1 pressed %d", priv->current_custom_mode);

			if (custom_mode == PX4_CUSTOM_MAIN_MODE_FOLLOW){
				send_set_mode(priv->base_mode, PX4_CUSTOM_MAIN_MODE_EASY);

			} else if (custom_mode == (PX4_CUSTOM_MAIN_MODE_EASY)){
				send_set_mode(priv->base_mode, PX4_CUSTOM_MAIN_MODE_FOLLOW);

			} else if (custom_mode == (PX4_CUSTOM_MAIN_MODE_SEATBELT)){
				send_set_mode(priv->base_mode, PX4_CUSTOM_MAIN_MODE_FOLLOW);
			}
			priv->follow_button.button_pressed = true;
		}
	} else {
		if (priv->follow_button.button_pressed == true){
			warnx("button 1 let go");
			priv->follow_button.button_pressed = false;
		}
	}
	/* repeat cycle at 10 Hz */
	if (airdog_running) {
		work_queue(LPWORK, &priv->work, airdog_cycle, priv, USEC2TICK(100000));

	} else {
		/* switch off LED on stop */
		ioctl(priv->gpio_fd, GPIO_CLEAR, priv->inputs);
	}
}
