#include <Arduino.h>
#include "motor.h"


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
	motor_init();
	motor_test_loop();
}

void motor_routine() {
	
}

void loop() {
	motor_routine();
	delay(2000);
}
