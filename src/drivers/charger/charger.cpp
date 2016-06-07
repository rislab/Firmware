/****************************************************************************
 *
 *   Copyright (C) 2012 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

#include <px4_config.h>

#include <sys/types.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <ctype.h>
#include <poll.h>

#include <px4_workqueue.h>

#include <systemlib/perf_counter.h>
#include <systemlib/err.h>
#include <systemlib/systemlib.h>

#include <board_config.h>

#include <drivers/device/i2c.h>
#include <drivers/drv_charger.h>

#include <uORB/uORB.h>
#include <uORB/topics/batt_info.h>

static const float MAX_CELL_VOLTAGE	= 4.3f;

class charger : public device::I2C
{
public:
	charger(int bus, int address, const char *path);
	virtual ~charger();
	virtual int		init();
	virtual int		probe();
	virtual int		ioctl(device::file_t *filp, int cmd, unsigned long arg);
	virtual int		setMode(int mode);
	int get_hss_current(int* voltage);
	int get_ups_current(int* current);
	int get_voltage(int* voltage);
	int get_gpio(bool* toggle);
	int set_gpio(bool toggle);

private:
	work_s	_work;
	bool systemstate_run;
	int monitor_interval;
	orb_advert_t batt_info_pub;
	bool topic_initialized;
	static void		monitor_trampoline(void *arg);
	void			monitor();
};

extern "C" __EXPORT int charger_main(int argc, char *argv[]);

charger::charger(int bus, int address, const char *path) :
	I2C("charger", path, bus, address
#ifdef __PX4_NUTTX
	    , 100000
#endif
	   ),
	systemstate_run(false),
	monitor_interval(1000),
	topic_initialized(false)
{
	memset(&_work, 0, sizeof(_work));
}

charger::~charger()
{
}

int charger::init()
{
	int ret;
	ret = I2C::init();

	if (ret != OK) {
		warnx("I2C init failed");
		return ret;
	}

	if (this->get_address()==HSS_ADC_ADDRESS) {
		const uint8_t msg[2] = { HSS_ADCCONF, 0xF4 };
		ret = transfer(msg, sizeof(msg), nullptr, 0);
		if (ret != OK) {
			warnx("HSS_ADC init failed");
			return ret;
		}
	}
	else if (this->get_address()==HSS_GPIO_ADDRESS) {
		const uint8_t msg[2] = { HSS_GPIOCONF, 0xF4 };
		ret = transfer(msg, sizeof(msg), nullptr, 0);
		if (ret != OK) {
			warnx("HSS_GPIO init failed");
			return ret;
		}
	}

	return OK;
}

int charger::get_voltage(int* voltage) {
	
	int ret;
	uint8_t data[2];
	uint8_t msg=I2C_BALANCER_VOLTAGE;
	ret = transfer(&msg, sizeof(msg), data, sizeof(data));
	*voltage = (data[1] << 8) | data[0];
	return ret;
}

int charger::get_ups_current(int* current) {
	
	int ret;
	uint8_t data[2];
	uint8_t msg=I2C_BALANCER_CURRENT;
	ret = transfer(&msg, sizeof(msg), data, sizeof(data));
	*current = (data[1] << 8) | data[0];
    if(*current > 32767){
        *current -= 65535;
    }
	return ret;
}

int charger::get_hss_current(int* current) {
	int ret;
	uint8_t data[2];
	ret = transfer(nullptr, 0, data, sizeof(data));
	*current = ((data[0] & 0x03) << 8) | data[1];
	return ret;
}

int charger::get_gpio(bool* toggle) {
	int ret;
	uint8_t msg=HSS_GPIODAT;
	uint8_t data[1];
	ret = transfer(&msg, sizeof(msg), data, sizeof(data));
	*toggle = (data[0] == HSS_ACTIVE) ? true : false;
	return ret;
}

int charger::set_gpio(bool toggle) {
	int ret;
	uint8_t data = (toggle == true) ? HSS_ACTIVE : HSS_INACTIVE;
	uint8_t msg[2] = { HSS_GPIODAT, data };
	ret = transfer(msg, sizeof(msg), nullptr, 0);
	return ret;
}

int
charger::setMode(int mode)
{
	if (mode == 1) {
		if (systemstate_run == false) {
			systemstate_run = true;
			work_queue(LPWORK, &_work, (worker_t)&charger::monitor_trampoline, this, 1);
		}

	} else {
		systemstate_run = false;
	}

	return OK;
}

int charger::probe() {
	int ret;
	uint8_t data[2];
	uint8_t msg=I2C_BALANCER_VOLTAGE;
	ret = transfer(&msg, sizeof(msg), data, sizeof(data));
	return ret;
}

int	charger::ioctl(device::file_t *filp, int cmd, unsigned long arg) {
	int ret = ENOTTY;
	int current;
	switch (cmd) {
	case CHARGER_GET_VOLTAGE:
		int voltage;
		get_voltage(&voltage);
		printf("%d\n", voltage);
		break;

	case CHARGER_GET_HSS_CURRENT:
		get_hss_current(&current);
		printf("%d\n", current);
		break;

	case CHARGER_GET_UPS_CURRENT:
		get_ups_current(&current);
		printf("%d\n", current);
		break;

	default:
		break;
	}

	return ret;
}

void
charger::monitor_trampoline(void *arg)
{
	charger *bm = (charger *)arg;

	bm->monitor();
}



void
charger::monitor()
{
	int voltage=0;
	int ups_current=0;
	int hss_current=0;
	bool gpio_status=false;

	if (this->get_address()==UPS0_ADDRESS) {
		if (OK != get_voltage(&voltage)) {
			warnx("failed to get voltage");
			return;
		}
		if (voltage==VOLTAGE_ERR) {
			get_voltage(&voltage);
		}
		if (OK != get_ups_current(&ups_current)) {
			warnx("failed to get current");
			return;
		}
		if (ups_current==UPS_CURRENT_ERR) {
			get_ups_current(&ups_current);
		}
	}

	if (this->get_address()==HSS_ADC_ADDRESS) {
		if (OK != get_hss_current(&hss_current)) {
			warnx("failed to get current");
			return;
		}
	}

	if (this->get_address()==HSS_GPIO_ADDRESS) {
		if (OK != get_gpio(&gpio_status)) {
			warnx("failed to get gpio status");
			return;
		}
	}

	struct batt_info_s bi = {.voltage = voltage, .ups_current = ups_current, .hss_current = hss_current, .gpio_status = gpio_status};

	if (voltage!=VOLTAGE_ERR && ups_current!=UPS_CURRENT_ERR) {
		if (!topic_initialized) {
			batt_info_pub = orb_advertise(ORB_ID(batt_info), &bi);
			topic_initialized = true;
		}
		else {
			orb_publish(ORB_ID(batt_info), batt_info_pub, &bi);
		}
		printf("voltage: %d, current: %d\n", voltage, ups_current);
	}


	if (systemstate_run == true) {
		/* re-queue ourselves to run again later */
		work_queue(LPWORK, &_work, (worker_t)&charger::monitor_trampoline, this, monitor_interval);

	} else {
		return;
	}
}

void charger_usage();

void charger_usage()
{
	warnx("missing command: try 'start', 'systemstate', 'ledoff', 'list' or a script name {options}");
	warnx("options:");
	warnx("\t-b --bus i2cbus (3)");
	warnx("\t-a --blinkmaddr blinkmaddr (9)");
}

namespace
{
charger *g_charger;
}

int
charger_main(int argc, char *argv[])
{
	int i2cdevice = PX4_I2C_BUS_EXPANSION;
	int chargeraddr=UPS0_ADDRESS;
	const char *path="/dev/ups0";

	if (argc < 2) {
		charger_usage();
		return 1;
	}

	for (int x = 1; x < argc; x++) {
		if (strcmp(argv[x], "-b") == 0 || strcmp(argv[x], "--bus") == 0) {
			if (argc > x + 1) {
				i2cdevice = atoi(argv[x + 1]);
			}
		}

		if (strcmp(argv[x], "-a") == 0 || strcmp(argv[x], "--chargeraddr") == 0) {
			if (argc > x + 1) {
				chargeraddr = atoi(argv[x + 1]);
			}
		}
		if (strcmp(argv[x], "-p") == 0 || strcmp(argv[x], "--path") == 0) {
			if (argc > x + 1) {
				path = argv[x + 1];
			}
		}

	}

	if (!strcmp(argv[1], "start")) {
		if (g_charger != nullptr) {
			warnx("already started");
			return 1;
		}

		g_charger = new charger(i2cdevice, chargeraddr, path);

		if (g_charger == nullptr) {
			warnx("new failed");
			return 1;
		}

		if (OK != g_charger->init()) {
			delete g_charger;
			g_charger = nullptr;
			warnx("init failed");
			return 1;
		}

		return 0;
	}

	if (g_charger == nullptr) {
		fprintf(stderr, "not started\n");
		charger_usage();
		return 0;
	}

	if (!strcmp(argv[1], "monitor")) {
		g_charger->setMode(1);
		return 0;
	}

	if (!strcmp(argv[1], "off")) {
		g_charger->setMode(0);
		return 0;
	}

	if (!strcmp(argv[1], "voltage")) {
		int voltage;
		if (OK != g_charger->get_voltage(&voltage)) {
			warnx("failed to get voltage");
			return 1;
		}
		printf("%d\n", voltage);
		return 0;
	}

	charger_usage();
	return 0;
}
