#include <Arduino.h>
#include "motor.h"

// UART2 TX (GPIO17) on the classic ESP32.
const int TX2_PIN = 17;

const int MAX_DUTY      = (1 << PWM_BITS) - 1;
const int STEP_DELAY_MS = 1;

void motor_test_loop() {
	stop_all_motors();
	for (int idx = 0; idx < MOTOR_COUNT; ++idx) {

		motor_set_direction(idx, DIRECTION_FORWARD);
		int duty = 0;
		while (duty <= MAX_DUTY) {
			motor_set_pwm(idx, duty);
			delay(STEP_DELAY_MS);
			duty++;
		}
		while (duty >= 0) {
			motor_set_pwm(idx, duty);
			delay(STEP_DELAY_MS);
			duty--;
		}
	}
}		

void setup() {
	// Drive TX2 high as soon as the app starts.
	pinMode(TX2_PIN, OUTPUT);
	digitalWrite(TX2_PIN, HIGH);

	motor_init();
	motor_test_loop();
	delay(3000);
}


struct Task {
	int time_ms;
	int input_all;
	int input_front_rear; // added to front wheels, subtracted from rear wheels
	int input_left_right; // added to left wheels, subtracted from right wheels
};

Task tasks[] = {
	// slow fwd
	{ 1000, 128, 0, 0},
	// slow back
	{ 1000, -128, 0, 0},

	// fast fwd
	{ 500, 255, 0, 0 },
	// fast back
	{ 500, -255, 0, 0},

	// turn in place left
	{ 1000, 0, 0, 256 },
	// turn in place right
	{ 1000, 0, 0, -256 },

	// star dance
	{ 1000, 192, 0, 64 },
	{ 1000, -192, 0, 64 },
	{ 1000, 192, 0, 64 },
	{ 1000, -192, 0, 64 },
	{ 1000, 192, 0, 64 },
	{ 1000, -192, 0, 64 },
	{ 1000, 192, 0, 64 },
	{ 1000, -192, 0, 64 },
};

int current_task_index = 0;

void run_task(int index) {
	Task task = tasks[index];

	// order is fl, rl, rr, fr
	int duty_motors[4] = { 0, 0, 0, 0 };

	duty_motors[0] = task.input_all + task.input_front_rear + task.input_left_right; // front left
	duty_motors[1] = task.input_all - task.input_front_rear + task.input_left_right; // rear left
	duty_motors[2] = task.input_all - task.input_front_rear - task.input_left_right; // rear right
	duty_motors[3] = task.input_all + task.input_front_rear - task.input_left_right; // front right

	for (int idx = 0; idx < 4; ++idx) {
		int duty = duty_motors[idx];
		if (duty == 0) {
			motor_set_direction(idx, DIRECTION_OFF);
		} else if (duty > 0) {
			motor_set_direction(idx, DIRECTION_FORWARD);
		} else {
			motor_set_direction(idx, DIRECTION_BACKWARD);
		}
		motor_set_pwm(idx, constrain(abs(duty), 0, MAX_DUTY));
	}
	delay(task.time_ms);

	motor_set_direction_all(DIRECTION_OFF);
}

void loop() {
	run_task(current_task_index);
	current_task_index++;
	if (current_task_index >= sizeof(tasks) / sizeof(tasks[0])) {
		current_task_index = 0;
	}

	delay(1000);
}
