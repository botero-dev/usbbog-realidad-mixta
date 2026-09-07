#ifndef MOTOR_H
#define MOTOR_H

#include <Arduino.h>

const int PWM_FREQ = 5000;
const int PWM_BITS = 8;

struct Motor {
	int pin_pwm;
	int pin_dir_a;
	int pin_dir_b;
};

Motor motors[] = {
	{ 23, 21, 22 },
	{ 4,  18, 19 },
	{ 14, 26, 27 },
	{ 32, 25, 33 },
};

const int MOTOR_COUNT = sizeof(motors) / sizeof(motors[0]);

enum Direction {
	DIRECTION_OFF,
	DIRECTION_FORWARD,
	DIRECTION_BACKWARD,
	DIRECTION_BRAKE
};

void motor_stop(int index);

void motor_init() {
    for (int i = 0; i < MOTOR_COUNT; i++) {
        Motor motor = motors[i];
        pinMode(motor.pin_dir_a, OUTPUT);
        pinMode(motor.pin_dir_b, OUTPUT);
        ledcSetup(i, PWM_FREQ, PWM_BITS);
        ledcAttachPin(motor.pin_pwm, i);
        motor_stop(i);
    }
}

void motor_set_pwm(int index, int duty) {
	ledcWrite(index, duty);
}

void motor_set_direction(int index, Direction dir) {
	int a_level = LOW;
	int b_level = LOW;
	switch (dir) {
		case DIRECTION_FORWARD:
			a_level = HIGH;
			b_level = LOW;
			break;
		case DIRECTION_BACKWARD:
			a_level = LOW;
			b_level = HIGH;
			break;
		case DIRECTION_BRAKE:
			a_level = HIGH;
			b_level = HIGH;
			break;
		case DIRECTION_OFF:
		default:
			a_level = LOW;
			b_level = LOW;
			break;
	}
	digitalWrite(motors[index].pin_dir_a, a_level);
	digitalWrite(motors[index].pin_dir_b, b_level);
}

void motor_stop(int index) {
	motor_set_pwm(index, 0);
	motor_set_direction(index, DIRECTION_OFF);
}

void motor_set_pwm_all(int duty) {
	for (int i = 0; i < MOTOR_COUNT; i++) {
		motor_set_pwm(i, duty);
	}
}

void motor_set_direction_all(Direction dir) {
	for (int i = 0; i < MOTOR_COUNT; i++) {
		motor_set_direction(i, dir);
	}
}

void stop_all_motors() {
	for (int i = 0; i < MOTOR_COUNT; i++) {
		motor_stop(i);
	}
}

#endif
