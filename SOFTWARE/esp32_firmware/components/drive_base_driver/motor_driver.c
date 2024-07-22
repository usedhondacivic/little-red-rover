/*
 * Motor control rountines for LRR.
 * Closed loop velocity control for TT Motors, using an encoder with no
 * direction information.
 *
 * References
 *
 * PWM:
 * https://github.com/espressif/esp-idf/blob/master/examples/peripherals/ledc/ledc_basic/main/ledc_basic_example_main.c
 *
 * PID CONTROL:
 * https://github.com/espressif/esp-idf/blob/master/examples/peripherals/mcpwm/mcpwm_bdc_speed_control/main/mcpwm_bdc_control_example_main.c
 *
 * PULSE COUNTER:
 * https://github.com/espressif/esp-idf/blob/master/examples/peripherals/pcnt/rotary_encoder/main/rotary_encoder_example_main.c
 */

#include "motor_driver.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <rcl/rcl.h>
#include <rclc/executor_handle.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/pulse_cnt.h"
#include "esp_timer.h"
#include "hal/gpio_types.h"
#include "hal/pcnt_types.h"
#include "pid_ctrl.h"

#include <math.h>

#define PWM_TIMER_RESOLUTION LEDC_TIMER_10_BIT
#define PWM_FREQ_HZ 4000

void set_motor_enabled(motor_handle_t *motor, bool enable)
{
    gpio_set_direction(motor->enable_pin, GPIO_MODE_OUTPUT);

    if (enable) {
        gpio_set_level(motor->enable_pin, 1);
    } else {
        gpio_set_level(motor->enable_pin, 0);
    }
}

void set_motor_velocity(motor_handle_t *motor, float velocity)
{
    motor->cmd_velocity = velocity;
}

static void set_motor_power(motor_handle_t *motor, float power)
{
    if (power > 0) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE,
                      motor->chan_b,
                      (uint32_t)(power * (float)(1 << PWM_TIMER_RESOLUTION)));
        ledc_set_duty(LEDC_LOW_SPEED_MODE, motor->chan_a, 0);
        pcnt_channel_set_edge_action(motor->encoder.channel,
                                     PCNT_CHANNEL_EDGE_ACTION_DECREASE,
                                     PCNT_CHANNEL_EDGE_ACTION_DECREASE);
    } else {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, motor->chan_b, 0);
        ledc_set_duty(LEDC_LOW_SPEED_MODE,
                      motor->chan_a,
                      (uint32_t)(-power * (float)(1 << PWM_TIMER_RESOLUTION)));
        pcnt_channel_set_edge_action(motor->encoder.channel,
                                     PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                     PCNT_CHANNEL_EDGE_ACTION_INCREASE);
    }
    if (power == 0) {
        pcnt_channel_set_edge_action(motor->encoder.channel,
                                     PCNT_CHANNEL_EDGE_ACTION_HOLD,
                                     PCNT_CHANNEL_EDGE_ACTION_HOLD);
    }

    ledc_update_duty(LEDC_LOW_SPEED_MODE, motor->chan_a);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, motor->chan_b);
}

void configure_pwm(ledc_channel_t channel, int gpio)
{
    ledc_channel_config_t pwm_channel = { .speed_mode = LEDC_LOW_SPEED_MODE,
                                          .channel = channel,
                                          .timer_sel = LEDC_TIMER_0,
                                          .intr_type = LEDC_INTR_DISABLE,
                                          .gpio_num = gpio,
                                          .duty = 0,
                                          .hpoint = 0 };

    ESP_ERROR_CHECK(ledc_channel_config(&pwm_channel));
}

#define PULSES_PER_ROTATION 30.0
#define PULSES_TO_RAD(pulses)                                                  \
    (((float)pulses / PULSES_PER_ROTATION) * (2 * M_PI))
#define PID_LOOP_PERIOD_MS 10.0 // 100 Hz

void pid_callback(void *arg)
{
    motor_handle_t *motor = (motor_handle_t *)arg;

    int current_encoder_count;
    pcnt_unit_get_count(motor->encoder.unit, &current_encoder_count);
    int pulses_elapsed = current_encoder_count - motor->encoder.count;
    motor->reported_speed =
      PULSES_TO_RAD(pulses_elapsed) * (PID_LOOP_PERIOD_MS / 1000.0);
    float error = motor->cmd_velocity - motor->reported_speed;

    ESP_ERROR_CHECK(
      pid_compute(motor->pid_controller, error, &motor->cmd_power));
    set_motor_power(motor, motor->cmd_power);

    motor->encoder.count = current_encoder_count;
};

void configure_motor(motor_handle_t *motor,
                     gpio_num_t pwm_a_pin,
                     ledc_channel_t pwm_a_chan,
                     gpio_num_t pwm_b_pin,
                     ledc_channel_t pwm_b_chan,
                     gpio_num_t encoder_pin)
{
    // PWM
    configure_pwm(pwm_a_chan, pwm_a_pin);
    configure_pwm(pwm_b_chan, pwm_b_pin);

    motor->chan_a = pwm_a_chan;
    motor->chan_b = pwm_b_chan;

    // ENCODER
    gpio_pullup_en(encoder_pin);

    pcnt_unit_config_t unit_config = { .low_limit = INT16_MIN,
                                       .high_limit = INT16_MAX,
                                       .flags = { .accum_count = 1 } };

    ESP_ERROR_CHECK(pcnt_new_unit(&unit_config, &motor->encoder.unit));

    pcnt_glitch_filter_config_t filter_config = { .max_glitch_ns = 10000 };
    ESP_ERROR_CHECK(
      pcnt_unit_set_glitch_filter(motor->encoder.unit, &filter_config));

    pcnt_chan_config_t chan_config = {
        .edge_gpio_num = encoder_pin,
    };
    ESP_ERROR_CHECK(pcnt_new_channel(
      motor->encoder.unit, &chan_config, &motor->encoder.channel));

    ESP_ERROR_CHECK(
      pcnt_channel_set_edge_action(motor->encoder.channel,
                                   PCNT_CHANNEL_EDGE_ACTION_HOLD,
                                   PCNT_CHANNEL_EDGE_ACTION_HOLD));

    ESP_ERROR_CHECK(pcnt_unit_add_watch_point(motor->encoder.unit, INT16_MAX));
    ESP_ERROR_CHECK(pcnt_unit_add_watch_point(motor->encoder.unit, INT16_MIN));

    ESP_ERROR_CHECK(pcnt_unit_enable(motor->encoder.unit));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(motor->encoder.unit));
    ESP_ERROR_CHECK(pcnt_unit_start(motor->encoder.unit));

    // PID
    pid_ctrl_parameter_t pid_runtime_param = {
        .kp = 0.6, // TODO: tune these (maybe make them uROS controlled?)
        .ki = 0.4,
        .kd = 0.2,
        .cal_type = PID_CAL_TYPE_INCREMENTAL,
        .max_output = 1.0,
        .min_output = -1.0,
        .max_integral = 0.5,
        .min_integral = -0.5,
    };
    pid_ctrl_block_handle_t pid_ctrl = NULL;
    pid_ctrl_config_t pid_config = {
        .init_param = pid_runtime_param,
    };
    ESP_ERROR_CHECK(pid_new_control_block(&pid_config, &pid_ctrl));
    motor->pid_controller = pid_ctrl;

    const esp_timer_create_args_t periodic_timer_args = {
        .callback = pid_callback, .arg = motor, .name = "pid_loop"
    };
    motor->pid_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args, &motor->pid_timer));
    esp_timer_start_periodic(motor->pid_timer, PID_LOOP_PERIOD_MS * 1000);
}

void init_motor_pwm()
{
    ledc_timer_config_t pwm_timer = { .speed_mode = LEDC_LOW_SPEED_MODE,
                                      .duty_resolution = PWM_TIMER_RESOLUTION,
                                      .timer_num = LEDC_TIMER_0,
                                      .freq_hz = PWM_FREQ_HZ,
                                      .clk_cfg = LEDC_AUTO_CLK };

    ESP_ERROR_CHECK(ledc_timer_config(&pwm_timer));
}
