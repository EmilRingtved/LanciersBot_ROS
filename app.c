// standart C libs
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <inttypes.h>
#include <math.h>

// esp gpio
#include <driver/ledc.h>
#include <driver/gpio.h>
#include <driver/adc.h>
// free Rtos
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// esp sys
#include "esp_system.h"

// microros util
#include <rcl/rcl.h>
#include <rclc/rclc.h>
#include <rclc/executor.h>
#include <rcl/error_handling.h>
#include <rmw_microros/rmw_microros.h>
#include <std_msgs/msg/float32.h>
#include <geometry_msgs/msg/twist.h>

#define RCCHECK(fn)                                                                      \
    {                                                                                    \
        rcl_ret_t temp_rc = fn;                                                          \
        if ((temp_rc != RCL_RET_OK))                                                     \
        {                                                                                \
            printf("Failed status on line %d: %d. Aborting.\n", __LINE__, (int)temp_rc); \
            vTaskDelete(NULL);                                                           \
        }                                                                                \
    }
#define RCSOFTCHECK(fn)                                                                    \
    {                                                                                      \
        rcl_ret_t temp_rc = fn;                                                            \
        if ((temp_rc != RCL_RET_OK))                                                       \
        {                                                                                  \
            printf("Failed status on line %d: %d. Continuing.\n", __LINE__, (int)temp_rc); \
        }                                                                                  \
    }

// define system pins
#define stayOn GPIO_NUM_17
#define vSens ADC1_CHANNEL_5
#define ledb GPIO_NUM_4
#define ledg GPIO_NUM_16

// define motor pins
#define PinR1 GPIO_NUM_21 // Forward right
#define PinR2 GPIO_NUM_22 // Backwards right
#define PinL1 GPIO_NUM_26 // Forward left
#define PinL2 GPIO_NUM_25 // Backwards left

// PWM parameters
#define PWM_resolution LEDC_TIMER_10_BIT
#define PWM_timer LEDC_TIMER_1
#define PWM_FREQ 30
#define PWM_spdMode LEDC_LOW_SPEED_MODE

// setting PWM channels
#define PWM_R1 LEDC_CHANNEL_0
#define PWM_R2 LEDC_CHANNEL_1
#define PWM_L1 LEDC_CHANNEL_2
#define PWM_L2 LEDC_CHANNEL_3

static rcl_publisher_t publisher;
std_msgs__msg__Float32 battery_msg;

static rcl_subscription_t subscriber;
geometry_msgs__msg__Twist msg;

void setup();
void motorControl();
float batteryVoltage();
void cmd_vel_callback();
void timer_callback();
void microRosTask();

void setup()
{
    // TO DO define struct for setting motors
    gpio_set_direction(stayOn, GPIO_MODE_OUTPUT);
    gpio_set_direction(vSens, GPIO_MODE_INPUT);
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(vSens, ADC_ATTEN_DB_0);
    gpio_set_level(stayOn, 1);

    // pwm timer struct set to match motor drivers.
    ledc_timer_config_t ledc_timer = {
        .duty_resolution = PWM_resolution,
        .freq_hz = PWM_FREQ,
        .speed_mode = PWM_spdMode,
        .timer_num = PWM_timer
                         .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel[i] = {
        {.channel = PWM_LEFT_FORWARD,
         .duty = 0,
         .gpio_num = PIN_LEFT_FORWARD,
         .speed_mode = PWM_MODE,
         .hpoint = 0,
         .timer_sel = LEDC_TIMER_1},
        {.channel = PWM_LEFT_BACKWARD,
         .duty = 0,
         .gpio_num = PIN_LEFT_BACKWARD,
         .speed_mode = PWM_MODE,
         .hpoint = 0,
         .timer_sel = LEDC_TIMER_1},
        {.channel = PWM_RIGHT_FORWARD,
         .duty = 0,
         .gpio_num = PIN_RIGHT_FORWARD,
         .speed_mode = PWM_MODE,
         .hpoint = 0,
         .timer_sel = LEDC_TIMER_1},
        {.channel = PWM_RIGHT_BACKWARD,
         .duty = 0,
         .gpio_num = PIN_RIGHT_BACKWARD,
         .speed_mode = PWM_MODE,
         .hpoint = 0,
         .timer_sel = LEDC_TIMER_1},
    };
    for (int i = 0; i < 4; i++)
    {
        ledc_channel_config(&ledc_channel[i]);
    }
    void microRosTask()
    {
        rcl_timer_t timer = rcl_get_zero_initialized_timer();
        rcl_allocator_t allocator = rcl_get_default_allocator();

        // microRos INIT options
        rclc_support_t support;
        RCCHECK(rclc_support_init(&support, 0, NULL, &allocator));

        // create node
        rcl_node_t node;
        RCCHECK(rclc_node_init_default(&node, "LanciersBot", "", &support));

        // create subscriber, expecting msg in format of /cmd_vel, connect to topic /cmd_vel
        // initiated to connect with best effort for faster communication
        RCCHECK(rclc_subscription_init_best_effort(
            &subscriber,
            &node,
            ROSIDL_GET_MSG_TYPE_SUPPORT(geometry_msgs, msg, Twist),
            "/cmd_vel"));
        // create publisher for returning batteryVoltage
        RCCHECK(rclc_publisher_init_default(
            &publisher,
            &node,
            ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, Float32),
            "/batt_volt"));

        // Create timer.
        const unsigned int timer_timeout = 100;
        RCCHECK(rclc_timer_init_default(
            &timer,
            &support,
            RCL_MS_TO_NS(timer_timeout),
            timer_callback));

        // Create Executor.
        rclc_executor_t executor;
        RCCHECK(rclc_executor_init(&executor, &support.context, 2, &allocator));
        RCCHECK(rclc_executor_add_subscription(&executor, &subscriber, &msg, &cmd_vel_callback, ON_NEW_DATA));
        RCCHECK(rclc_executor_add_timer(&executor, &timer));

        battery_msg.data = 0;

        while (1)
        {
            rclc_executor_spin_some(&executor, RCL_MS_TO_NS(1));
            usleep(RCL_MS_TO_NS(1000));
        }
        // free mem
        RCCHECK(rcl_subscription_fini(&subscriber, &node));
        RCCHECK(rcl_publisher_fini(&publisher, &node));
        RCCHECK(rcl_node_fini(&node));

        vTaskDelete(NULL);
    }

    void cmd_vel_callback(const void *msgin)
    {
        const geometry_msgs__msg__Twist *msg = (const geometry_msgs__msg__Twist *)msgin;
        printf("Message received: %f %f\n", msg->linear.x, msg->angular.z);
    }

    void timer_callback(rcl_timer_t * timer, int64_t last_call_time)
    {
        // TO DO:
        // Write program for controlling motors
        RCLC_UNUSED(last_call_time);
        if (timer != NULL)
        {
            return;
        }
            //send battery data back
            battery_msg.data = batteryVoltage();
            RCSOFTCHECK(rcl_publish(&publisher, &battery_msg, NULL));
            printf("Sent: %f\n", battery_msg.data);
            
            //run motor,
            motorControl(msg.Linear.x,msg.angular.z)
    }
    // Calculates the current battery voltage
    float batteryVoltage()
    {
        float vMeas = adc1_get_raw(ADC1_CHANNEL_5) * 3.3 / 4096;    // get voltage from adc
        float vBatt = vMeas * (100 * 10e3 + 22 * 10e3) / 22 * 10e3; // calculate battery voltage from voltagedivider circuit
        return vBatt;
    }
    void motorControl(float vel, float a)
    {
        /* this is code for steering the robot
         * it calculates what pwm values to output, but not for how long
         * vel is the linear velocity and is a value between -1 and 1, with 1 being top speed forward.
         * a is the angle and is a value between -1 and 1. right is -1 and left is 1
         */

        // constrain input values?
        // takes input of linear velocity and angle (float?)
        // all of this code has been taken from github.com/Reinbert/ros_esp32cam_diffdrive
        vel = constrain(vel, -1, 1);
        a = constrain(a, -1, 1);

        // calculate wheel velocities
        float lVel = (vel - a) / 2.0f; // taken from github - use wheel distance/size??
        float rVel = (vel + a) / 2.0f;

        // map velocity to pwm
        uint16_t pwmLeft = (uint16_t)fmap(fabs(lVel), 0, 1, pwm_min, pwm_max);
        uint16_t pwmRight = (uint16_t)fmap(fabs(rVel), 0, 1, pwm_min, pwm_max);

        ledc_set_duty(PWM_MODE, PWM_LEFT_FORWARD, pwmLeft * (lVel > 0));
        ledc_set_duty(PWM_MODE, PWM_LEFT_BACKWARD, pwmLeft * (lVel < 0));
        ledc_set_duty(PWM_MODE, PWM_RIGHT_FORWARD, pwmRight * (rVel > 0));
        ledc_set_duty(PWM_MODE, PWM_RIGHT_BACKWARD, pwmRight * (rVel < 0));

        ledc_update_duty(PWM_MODE, PWM_LEFT_FORWARD);
        ledc_update_duty(PWM_MODE, PWM_LEFT_BACKWARD);
        ledc_update_duty(PWM_MODE, PWM_RIGHT_FORWARD);
        ledc_update_duty(PWM_MODE, PWM_RIGHT_BACKWARD);
    }
    void appMain()
    {
        setup();
        microRosTask();
    }