/* -*- mode: c++ c-basic-offset: 2; indent-tabs-mode: nil; -*-
 * (c) 2013, 2014 Henner Zeller <h.zeller@acm.org>
 *
 * This file is part of BeagleG. http://github.com/hzeller/beagleg
 *
 * BeagleG is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * BeagleG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with BeagleG.  If not, see <http://www.gnu.org/licenses/>.
 */

// TODO: this is motion planner and 'other stuff printers do' in one. Separate.
// TODO: this is somewhat work-in-progress.
// TODO: after the transition from C to C++, there are still some C-isms in here.

#include "gcode-machine-control.h"

#include <assert.h>
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <unistd.h>

#include "container.h"
#include "gcode-parser.h"
#include "generic-gpio.h"
#include "motor-operations.h"
#include "pwm-timer.h"

// In case we get a zero feedrate, send this frequency to motors instead.
#define ZERO_FEEDRATE_OVERRIDE_HZ 5

#define VERSION_STRING "PROTOCOL_VERSION:0.1 FIRMWARE_NAME:BeagleG "    \
  "FIRMWARE_URL:http%3A//github.com/hzeller/beagleg"

// aux_bits
#define AUX_BIT_MIST        (1 << 0)
#define AUX_BIT_FLOOD       (1 << 1)
#define AUX_BIT_VACUUM      (1 << 2)
#define AUX_BIT_SPINDLE_ON  (1 << 3)
#define AUX_BIT_SPINDLE_DIR (1 << 4)
#define MAX_AUX_PIN         15

#define NUM_ENDSTOPS        6

// Some default settings. These are most likely overrridden via flags by user.

// All these settings are in sequence of enum GCodeParserAxes: XYZEABCUVW. Axes not
// used by default are initialized to 0
static const float kMaxFeedrate[GCODE_NUM_AXES] = {  200,  200,   90,    10, 1 };
static const float kDefaultAccel[GCODE_NUM_AXES]= { 4000, 4000, 1000, 10000, 1 };
static const float kStepsPerMM[GCODE_NUM_AXES]  = {  160,  160,  160,    40, 1 };

// Output mapping of Axis to motor connectors from left to right.
static const char kAxisMapping[] = "XYZEA";

// Default order in which axes should be homed.
static const char kHomeOrder[] = "ZXY";

// The target position vector is essentially a position in the
// GCODE_NUM_AXES-dimensional space.
//
// An AxisTarget has a position vector, in absolute machine coordinates, and a
// speed when arriving at that position.
//
// The speed is initially the aimed goal; if it cannot be reached, the
// AxisTarget will be modified to contain the actually reachable value. That is
// used in planning along the path.
struct AxisTarget {
  int position_steps[GCODE_NUM_AXES];  // Absolute position at end of segment. In steps.

  // Derived values
  int delta_steps[GCODE_NUM_AXES];     // Difference to previous position.
  enum GCodeParserAxis defining_axis;  // index into defining axis.
  float speed;                         // (desired) speed in steps/s on defining axis.
  float angle;
  unsigned short aux_bits;             // Auxillary bits in this segment; set with M42
};

typedef uint8_t DriverBitmap;

// Compact representation of an enstop configuration.
struct EndstopConfig {
  unsigned char trigger_value : 1;   // 0: trigged low 1: triggered high.
  unsigned char homing_use : 1;      // 0: no 1: yes.
  unsigned char endstop_number : 6;  // 0: no mapping; or (1..NUM_ENDSTOPS+1)
};

// The three levels of homing confidence. If we ever switch off
// power to the motors after homing, we can't be sure.
enum HomingState {
  HOMING_STATE_NEVER_HOMED,
  HOMING_STATE_HOMED_BUT_MOTORS_UNPOWERED,
  HOMING_STATE_HOMED,
};

// The GCode control implementation. Essentially we are a state machine
// driven by the events we get from the gcode parsing.
// We implement the event receiver interface directly.
class GCodeMachineControl::Impl : public GCodeParser::Events {
public:
  Impl(const MachineControlConfig &config,
       MotorOperations *motor_ops,
       FILE *msg_stream);

  ~Impl() {}

  uint32_t GetHomeEndstop(enum GCodeParserAxis axis,
                          int *dir, int *trigger_value) const;

  // -- GCodeParser::Events interface implementation --
  virtual void gcode_start() {}    // Start program. Use for initialization.
  virtual void gcode_finished();   // End of program or stream.

  virtual void inform_origin_offset(const AxesRegister &origin);

  virtual void gcode_command_done(char letter, float val);
  virtual void input_idle();
  virtual void wait_for_start();
  virtual void go_home(AxisBitmap_t axis_bitmap);
  virtual bool probe_axis(float feed_mm_p_sec, enum GCodeParserAxis axis,
                          float *probed_position);
  virtual void set_speed_factor(float factor);    // M220 feedrate factor 0..1
  virtual void set_fanspeed(float value);         // M106, M107: speed 0...255
  virtual void set_temperature(float degrees_c);  // M104, M109: Set temp. in Celsius.
  virtual void wait_temperature();                // M109, M116: Wait for temp. reached.
  virtual void dwell(float time_ms);              // G4: dwell for milliseconds.
  virtual void motors_enable(bool enable);        // M17,M84,M18: Switch on/off motors
  virtual bool coordinated_move(float feed_mm_p_sec, const AxesRegister &target);
  virtual bool rapid_move(float feed_mm_p_sec, const AxesRegister &target);
  virtual const char *unprocessed(char letter, float value, const char *);

private:
  void issue_motor_move_if_possible();
  void machine_move(float feedrate, const AxesRegister &axes);
  bool test_homing_status_ok();
  bool test_within_machine_limits(const AxesRegister &axes);
  void bring_path_to_halt();
  const char *special_commands(char letter, float value, const char *);
  float acceleration_for_move(const int *axis_steps,
                              enum GCodeParserAxis defining_axis);
  void assign_steps_to_motors(struct MotorMovement *command,
                              enum GCodeParserAxis axis,
                              int steps);
  void move_machine_steps(const struct AxisTarget *last_pos,
                          struct AxisTarget *target_pos,
                          const struct AxisTarget *upcoming);
  int move_to_endstop(enum GCodeParserAxis axis,
                      float feedrate, int backoff,
                      int dir, int trigger_value,
                      uint32_t gpio_def);
  void home_axis(enum GCodeParserAxis axis);

  // Print to msg_stream.
  void mprintf(const char *format, ...);

public:  // TODO(hzeller): these need to be private and have underscores.
  const struct MachineControlConfig cfg_;
  MotorOperations *const motor_ops_;
  FILE *msg_stream_;

  // Derived configuration
  float g0_feedrate_mm_per_sec_;         // Highest of all axes; used for G0
                                         // (will be trimmed if needed)
  // Pre-calculated per axis limits in steps, steps/s, steps/s^2
  // All arrays are indexed by axis.
  AxesRegister max_axis_speed_;  // max travel speed hz
  AxesRegister max_axis_accel_;  // acceleration hz/s
  float highest_accel_;                   // hightest accel of all axes.

  // "axis_to_driver": Which axis is mapped to which physical output drivers.
  // This allows to have a logical axis (e.g. X, Y, Z) output to any physical
  // or a set of multiple drivers (mirroring).
  // Bitmap of drivers output should go.
  DriverBitmap axis_to_driver_[GCODE_NUM_AXES];

  FixedArray<int, GCODE_NUM_AXES> axis_flip_;  // 1 or -1 for direction flip of axis
  FixedArray<int, BEAGLEG_NUM_MOTORS> driver_flip_;  // 1 or -1 for for individual driver

  // Mapping of Axis to which endstop it affects.
  FixedArray<EndstopConfig, GCODE_NUM_AXES> min_endstop_;
  FixedArray<EndstopConfig, GCODE_NUM_AXES> max_endstop_;

  // Current machine configuration
  AxesRegister coordinate_display_origin_; // parser tells us
  float current_feedrate_mm_per_sec_;    // Set via Fxxx and remembered
  float prog_speed_factor_;              // Speed factor set by program (M220)
  unsigned short aux_bits_;              // Set via M42.
  unsigned int spindle_rpm_;             // Set via Sxxx of M3/M4 and remembered

  // Next buffered positions. Written by incoming gcode, read by outgoing
  // motor movements.
  RingDeque<AxisTarget, 4> planning_buffer_;

  enum HomingState homing_state_;
};

static uint32_t get_endstop_gpio_descriptor(struct EndstopConfig config);

static inline int round2int(float x) { return (int) roundf(x); }

GCodeMachineControl::Impl::Impl(const MachineControlConfig &config,
                                MotorOperations *motor_ops,
                                FILE *msg_stream)
  : cfg_(config), motor_ops_(motor_ops), msg_stream_(msg_stream) {
}

// machine-printf. Only prints if there is a msg-stream.
void GCodeMachineControl::Impl::mprintf(const char *format, ...) {
  va_list ap;
  va_start(ap, format);
  if (msg_stream_) vfprintf(msg_stream_, format, ap);
  va_end(ap);
}

// Dummy implementations of callbacks not yet handled.
void GCodeMachineControl::Impl::set_temperature(float f) {
  mprintf("// BeagleG: set_temperature(%.1f) not implemented.\n", f);
}
void GCodeMachineControl::Impl::wait_temperature() {
  mprintf("// BeagleG: wait_temperature() not implemented.\n");
}
void GCodeMachineControl::Impl::motors_enable(bool b) {
  bring_path_to_halt();
  motor_ops_->MotorEnable(b);
  if (homing_state_ == HOMING_STATE_HOMED) {
    homing_state_ = HOMING_STATE_HOMED_BUT_MOTORS_UNPOWERED;
  }
}
void GCodeMachineControl::Impl::gcode_command_done(char l, float v) {
  mprintf("ok\n");
}
void GCodeMachineControl::Impl::inform_origin_offset(const AxesRegister &o) {
  coordinate_display_origin_ = o;
}

void GCodeMachineControl::Impl::set_fanspeed(float speed) {
  if (speed < 0.0 || speed > 255.0) return;
  float duty_cycle = speed / 255.0;
  // The fan can be controlled by a GPIO or PWM (TIMER) signal
  if (duty_cycle == 0.0) {
    clr_gpio(FAN_GPIO);
    pwm_timer_start(FAN_GPIO, 0);
  } else {
    set_gpio(FAN_GPIO);
    pwm_timer_set_duty(FAN_GPIO, duty_cycle);
    pwm_timer_start(FAN_GPIO, 1);
  }
}

void GCodeMachineControl::Impl::wait_for_start() {
  int flash_usec = 100 * 1000;
  int value = get_gpio(START_GPIO);
  while (value == 1) {
    set_gpio(LED_GPIO);
    usleep(flash_usec);
    clr_gpio(LED_GPIO);
    usleep(flash_usec);
    value = get_gpio(START_GPIO);
  }
}

const char *GCodeMachineControl::Impl::unprocessed(char letter, float value,
                                                   const char *remaining) {
  return special_commands(letter, value, remaining);
}

const char *GCodeMachineControl::Impl::special_commands(char letter, float value,
                                                        const char *remaining) {
  const int code = (int)value;

  if (letter == 'M') {
    int pin = -1;
    int aux_bit = -1;

    switch (code) {
    case 0: set_gpio(ESTOP_SW_GPIO); break;
    case 3:
    case 4:
      for (;;) {
        const char* after_pair
          = GCodeParser::ParsePair(remaining, &letter, &value, msg_stream_);
        if (after_pair == NULL) break;
        else if (letter == 'S') spindle_rpm_ = round2int(value);
        else break;
        remaining = after_pair;
      }
      if (spindle_rpm_) {
        aux_bits_ |= AUX_BIT_SPINDLE_ON;
	if (code == 3) aux_bits_ &= ~AUX_BIT_SPINDLE_DIR;
        else aux_bits_ |= AUX_BIT_SPINDLE_DIR;
      }
      break;
    case 5: aux_bits_ &= ~(AUX_BIT_SPINDLE_ON | AUX_BIT_SPINDLE_DIR); break;
    case 7: aux_bits_ |= AUX_BIT_MIST; break;
    case 8: aux_bits_ |= AUX_BIT_FLOOD; break;
    case 9: aux_bits_ &= ~(AUX_BIT_MIST | AUX_BIT_FLOOD); break;
    case 10: aux_bits_ |= AUX_BIT_VACUUM; break;
    case 11: aux_bits_ &= ~AUX_BIT_VACUUM; break;
    case 42:
    case 62:
    case 63:
    case 64:
    case 65:
      for (;;) {
        const char* after_pair
          = GCodeParser::ParsePair(remaining, &letter, &value, msg_stream_);
        if (after_pair == NULL) break;
        if (letter == 'P') pin = round2int(value);
        else if (letter == 'S' && code == 42) aux_bit = round2int(value);
        else break;
        remaining = after_pair;
      }
      if (code == 62 || code == 64)
        aux_bit = 1;
      else if (code == 63 || code == 65)
        aux_bit = 0;
      if (pin >= 0 && pin <= MAX_AUX_PIN) {
        if (aux_bit >= 0 && aux_bit <= 1) {
          if (aux_bit) aux_bits_ |= 1 << pin;
          else aux_bits_ &= ~(1 << pin);
          if (code == 64 || code == 65) {
            // update the AUX pin now
            uint32_t gpio_def = GPIO_NOT_MAPPED;
            switch (pin) {
            case 0:  gpio_def = AUX_1_GPIO; break;
            case 1:  gpio_def = AUX_2_GPIO; break;
            case 2:  gpio_def = AUX_3_GPIO; break;
            case 3:  gpio_def = AUX_4_GPIO; break;
            case 4:  gpio_def = AUX_5_GPIO; break;
            case 5:  gpio_def = AUX_6_GPIO; break;
            case 6:  gpio_def = AUX_7_GPIO; break;
            case 7:  gpio_def = AUX_8_GPIO; break;
            case 8:  gpio_def = AUX_9_GPIO; break;
            case 9:  gpio_def = AUX_10_GPIO; break;
            case 10: gpio_def = AUX_11_GPIO; break;
            case 11: gpio_def = AUX_12_GPIO; break;
            case 12: gpio_def = AUX_13_GPIO; break;
            case 13: gpio_def = AUX_14_GPIO; break;
            case 14: gpio_def = AUX_15_GPIO; break;
            case 15: gpio_def = AUX_16_GPIO; break;
            }
            if (gpio_def != GPIO_NOT_MAPPED) {
              if (aux_bit) set_gpio(gpio_def);
              else clr_gpio(gpio_def);
            }
          }
        } else if (code == 42 && msg_stream_) {  // Just read operation.
          mprintf("%d\n", (aux_bits_ >> pin) & 1);
        }
      }
      break;
    case 80: set_gpio(MACHINE_PWR_GPIO); break;
    case 81: clr_gpio(MACHINE_PWR_GPIO); break;
    case 105: mprintf("T-300\n"); break;  // no temp yet.
    case 114:
      if (planning_buffer_.size() > 0) {
        struct AxisTarget *current = planning_buffer_[0];
        const int *mpos = current->position_steps;
        const float x = 1.0f * mpos[AXIS_X] / cfg_.steps_per_mm[AXIS_X];
        const float y = 1.0f * mpos[AXIS_Y] / cfg_.steps_per_mm[AXIS_Y];
        const float z = 1.0f * mpos[AXIS_Z] / cfg_.steps_per_mm[AXIS_Z];
        const float e = 1.0f * mpos[AXIS_E] / cfg_.steps_per_mm[AXIS_E];
        const AxesRegister &origin = coordinate_display_origin_;
        mprintf("X:%.3f Y:%.3f Z:%.3f E:%.3f",
                x - origin[AXIS_X], y - origin[AXIS_Y], z - origin[AXIS_Z],
                e - origin[AXIS_E]);
        mprintf(" [ABS. MACHINE CUBE X:%.3f Y:%.3f Z:%.3f]", x, y, z);
        switch (homing_state_) {
        case HOMING_STATE_NEVER_HOMED:
          mprintf(" (Unsure: machine never homed!)\n");
          break;
        case HOMING_STATE_HOMED_BUT_MOTORS_UNPOWERED:
          mprintf(" (Lower confidence: motor power off at "
                  "least once after homing)\n");
          break;
        case HOMING_STATE_HOMED:
          mprintf(" (confident: machine was homed)\n");
          break;
        }
      } else {
        mprintf("// no current pos\n");
      }
      break;
    case 115: mprintf("%s\n", VERSION_STRING); break;
    case 117:
      mprintf("// Msg: %s\n", remaining); // TODO: different output ?
      remaining = NULL;  // consume the full line.
      break;
    case 119: {
      char any_enstops_found = 0;
      for (int ai = 0; ai < GCODE_NUM_AXES; ++ai) {
        GCodeParserAxis axis = (GCodeParserAxis) ai;
        struct EndstopConfig config = min_endstop_[axis];
        if (config.endstop_number) {
          int value = get_gpio(get_endstop_gpio_descriptor(config));
          mprintf("%c_min:%s ",
                  tolower(gcodep_axis2letter(axis)),
                  value == config.trigger_value ? "TRIGGERED" : "open");
          any_enstops_found = 1;
        }
        config = max_endstop_[axis];
        if (config.endstop_number) {
          int value = get_gpio(get_endstop_gpio_descriptor(config));
          mprintf("%c_max:%s ",
                  tolower(gcodep_axis2letter(axis)),
                  value == config.trigger_value ? "TRIGGERED" : "open");
          any_enstops_found = 1;
        }
      }
      if (any_enstops_found) {
        mprintf("\n");
      } else {
        mprintf("// This machine has no endstops configured.\n");
      }
    }
      break;
    case 999: clr_gpio(ESTOP_SW_GPIO); break;
    default:
      mprintf("// BeagleG: didn't understand ('%c', %d, '%s')\n",
              letter, code, remaining);
      remaining = NULL;  // In this case, let's just discard remainig block.
      break;
    }
  }
  return remaining;
}

void GCodeMachineControl::Impl::gcode_finished() {
  bring_path_to_halt();
}

static float euclid_distance(float x, float y, float z) {
  return sqrtf(x*x + y*y + z*z);
}

// Number of steps to accelerate or decelerate (negative "a") from speed
// v0 to speed v1. Modifies v1 if we can't reach the speed with the allocated
// number of steps.
static float steps_for_speed_change(float a, float v0, float *v1, int max_steps) {
  // s = v0 * t + a/2 * t^2
  // v1 = v0 + a*t
  const float t = (*v1 - v0) / a;
  // TODO:
  if (t < 0) fprintf(stderr, "Error condition: t=%.1f INSUFFICIENT LOOKAHEAD\n", t);
  float steps = a/2 * t*t + v0 * t;
  if (steps <= max_steps) return steps;
  // Ok, we would need more steps than we have available. We correct the speed to what
  // we actually can reach.
  *v1 = sqrtf(v0*v0 + 2 * a * max_steps);
  return max_steps;
}

float GCodeMachineControl::Impl::acceleration_for_move(const int *axis_steps,
                                                       enum GCodeParserAxis defining_axis) {
  return max_axis_accel_[defining_axis];
  // TODO: we need to scale the acceleration if one of the other axes could't
  // deal with it. Look at axis steps for that.
}

// Given that we want to travel "s" steps, start with speed "v0",
// accelerate peak speed v1 and slow down to "v2" with acceleration "a",
// what is v1 ?
static float get_peak_speed(float s, float v0, float v2, float a) {
  return sqrtf(v2*v2 + v0*v0 + 2 * a * s) / sqrtf(2);
}

// Speed relative to defining axis
static float get_speed_factor_for_axis(const struct AxisTarget *t,
                                       enum GCodeParserAxis request_axis) {
  if (t->delta_steps[t->defining_axis] == 0) return 0.0f;
  return 1.0f * t->delta_steps[request_axis] / t->delta_steps[t->defining_axis];
}

// Get the speed for a particular axis. Depending on the direction, this can
// be positive or negative.
static float get_speed_for_axis(const struct AxisTarget *target,
                                enum GCodeParserAxis request_axis) {
  return target->speed * get_speed_factor_for_axis(target, request_axis);
}

static char within_acceptable_range(float new_val, float old_val, float fraction) {
  const float max_diff = fraction * old_val;
  if (new_val < old_val - max_diff) return 0;
  if (new_val > old_val + max_diff) return 0;
  return 1;
}

// Determine the fraction of the speed that "from" should decelerate
// to at the end of its travel.
// The way trapezoidal moves work, be still have to decelerate to zero in
// most times, which is inconvenient. TODO(hzeller): speed matching is not
// cutting it :)
static float determine_joining_speed(const struct AxisTarget *from,
                                     const struct AxisTarget *to,
                                     const float threshold,
                                     const float angle) {
  // Our goal is to figure out what our from defining speed should
  // be at the end of the move.
  char is_first = 1;
  float from_defining_speed = from->speed;
  for (int ai = 0; ai < GCODE_NUM_AXES; ++ai) {
    const GCodeParserAxis axis = (GCodeParserAxis) ai;
    const int from_delta = from->delta_steps[axis];
    const int to_delta = to->delta_steps[axis];

    // Quick integer decisions
    if (angle < threshold) continue;
    if (from_delta == 0 && to_delta == 0) continue;   // uninteresting: no move.
    if (from_delta == 0 || to_delta == 0) return 0.0f; // accel from/to zero
    if ((from_delta < 0 && to_delta > 0) || (from_delta > 0 && to_delta < 0))
      return 0.0f;  // turing around

    float to_speed = get_speed_for_axis(to, axis);
    // What would this speed translated to our defining axis be ?
    float speed_conversion = 1.0f * from->delta_steps[from->defining_axis] / from->delta_steps[axis];
    float goal = to_speed * speed_conversion;
    if (goal < 0.0f) return 0.0f;
    if (is_first || within_acceptable_range(goal, from_defining_speed, 1e-5)) {
      if (goal < from_defining_speed) from_defining_speed = goal;
      is_first = 0;
    } else {
      return 0.0f;  // Too far off.
    }
  }
  return from_defining_speed;
}

// Assign steps to all the motors responsible for given axis.
void GCodeMachineControl::Impl::assign_steps_to_motors(struct MotorMovement *command,
                                                       enum GCodeParserAxis axis,
                                                       int steps) {
  const DriverBitmap motormap_for_axis = axis_to_driver_[axis];
  for (int motor = 0; motor < BEAGLEG_NUM_MOTORS; ++motor) {
    if (motormap_for_axis & (1 << motor)) {
      command->steps[motor] =
        axis_flip_[axis] * driver_flip_[motor] * steps;
    }
  }
}

// Returns true, if all results in zero movement
static uint8_t substract_steps(struct MotorMovement *value,
                               const struct MotorMovement *substract) {
  uint8_t has_nonzero = 0;
  for (int i = 0; i < BEAGLEG_NUM_MOTORS; ++i) {
    value->steps[i] -= substract->steps[i];
    has_nonzero |= (value->steps[i] != 0);
  }
  return has_nonzero;
}

// Move the given number of machine steps for each axis.
//
// This will be up to three segments: accelerating from last_pos speed to
// target speed, regular travel, and decelerating to the speed that the
// next segment is never forced to decelerate, but stays at speed or accelerate.
//
// The segments are sent to the motor operations backend.
//
// Since we calculate the deceleration, this modifies the speed of target_pos
// to reflect what the last speed was at the end of the move.
void GCodeMachineControl::Impl::move_machine_steps(const struct AxisTarget *last_pos,
                                                   struct AxisTarget *target_pos,
                                                   const struct AxisTarget *upcoming) {
  if (target_pos->delta_steps[target_pos->defining_axis] == 0) {
    return;
  }
  struct MotorMovement accel_command = {0};
  struct MotorMovement move_command = {0};
  struct MotorMovement decel_command = {0};

  assert(target_pos->speed > 0);  // Speed is always a positive scalar.

  // Aux bits are set synchronously with what we need.
  move_command.aux_bits = target_pos->aux_bits;
  const enum GCodeParserAxis defining_axis = target_pos->defining_axis;

  // Common settings.
  memcpy(&accel_command, &move_command, sizeof(accel_command));
  memcpy(&decel_command, &move_command, sizeof(decel_command));

  move_command.v0 = target_pos->speed;
  move_command.v1 = target_pos->speed;

  // Let's see what our defining axis had as speed in the previous segment. The
  // last segment might have had a different defining axis, so we calculate
  // what the the fraction of the speed that our _current_ defining axis had.
  const float last_speed = fabsf(get_speed_for_axis(last_pos, defining_axis));

  // We need to arrive at a speed that the upcoming move does not have
  // to decelerate further (after all, it has a fixed feed-rate it should not
  // go over).
  float next_speed = determine_joining_speed(target_pos, upcoming,
                                             cfg_.threshold_angle,
                                             fabsf(last_pos->angle - target_pos->angle));

  const int *axis_steps = target_pos->delta_steps;  // shortcut.
  const int abs_defining_axis_steps = abs(axis_steps[defining_axis]);
  const float a = acceleration_for_move(axis_steps, defining_axis);
  const float peak_speed = get_peak_speed(abs_defining_axis_steps,
                                          last_speed, next_speed, a);
  assert(peak_speed > 0);

  // TODO: if we only have < 5 steps or so, we should not even consider
  // accelerating or decelerating, but just do one speed.

  if (peak_speed < target_pos->speed) {
    target_pos->speed = peak_speed;  // Don't manage to accelerate to desired v
  }

  const float accel_fraction =
    (last_speed < target_pos->speed)
    ? steps_for_speed_change(a, last_speed, &target_pos->speed,
                             abs_defining_axis_steps) / abs_defining_axis_steps
    : 0;

  // We only decelerate if the upcoming speed is _slower_
  float dummy_next_speed = next_speed;  // Don't care to modify; we don't have
  const float decel_fraction =
    (next_speed < target_pos->speed)
    ? steps_for_speed_change(-a, target_pos->speed, &dummy_next_speed,
                             abs_defining_axis_steps) / abs_defining_axis_steps
    : 0;

  assert(accel_fraction + decel_fraction <= 1.0 + 1e-4);

#if 1
  // fudging: if we have tiny acceleration segments, don't do these at all
  // but only do speed; otherwise we have a lot of rattling due to many little
  // segments of acceleration/deceleration (e.g. for G2/G3).
  // This is not optimal. Ideally, we would actually calculate in terms of
  // jerk and optimize to stay within that constraint.
  const int accel_decel_steps
    = (accel_fraction + decel_fraction) * abs_defining_axis_steps;
  const float accel_decel_mm
    = (accel_decel_steps / cfg_.steps_per_mm[defining_axis]);
  const char do_accel = (accel_decel_mm > 2 || accel_decel_steps > 16);
#else
  const char do_accel = 1;
#endif

  char has_accel = 0;
  char has_move = 0;
  char has_decel = 0;

  if (do_accel && accel_fraction * abs_defining_axis_steps > 0) {
    has_accel = 1;
    accel_command.v0 = last_speed;           // Last speed of defining axis
    accel_command.v1 = target_pos->speed;    // New speed of defining axis

    // Now map axis steps to actual motor driver
    for (int i = 0; i < GCODE_NUM_AXES; ++i) {
      const int accel_steps = round2int(accel_fraction * axis_steps[i]);
      assign_steps_to_motors(&accel_command, (GCodeParserAxis)i,
                             accel_steps);
    }
  }

  if (do_accel && decel_fraction * abs_defining_axis_steps > 0) {
    has_decel = 1;
    decel_command.v0 = target_pos->speed;
    decel_command.v1 = next_speed;
    target_pos->speed = next_speed;

    // Now map axis steps to actual motor driver
    for (int i = 0; i < GCODE_NUM_AXES; ++i) {
      const int decel_steps = round2int(decel_fraction * axis_steps[i]);
      assign_steps_to_motors(&decel_command, (GCodeParserAxis)i,
                             decel_steps);
    }
  }

  // Move is everything that hasn't been covered in speed changes.
  // So we start with all steps and substract steps done in acceleration and
  // deceleration.
  for (int i = 0; i < GCODE_NUM_AXES; ++i) {
    assign_steps_to_motors(&move_command, (GCodeParserAxis)i,
                           axis_steps[i]);
  }
  substract_steps(&move_command, &accel_command);
  has_move = substract_steps(&move_command, &decel_command);

  if (cfg_.synchronous) {
    motor_ops_->WaitQueueEmpty();
  }
  if (has_accel) motor_ops_->Enqueue(accel_command, msg_stream_);
  if (has_move) motor_ops_->Enqueue(move_command, msg_stream_);
  if (has_decel) motor_ops_->Enqueue(decel_command, msg_stream_);
}

// If we have enough data in the queue, issue motor move.
void GCodeMachineControl::Impl::issue_motor_move_if_possible() {
  if (planning_buffer_.size() >= 3) {
    move_machine_steps(planning_buffer_[0],  // Current established position.
                       planning_buffer_[1],  // Position we want to move to.
                       planning_buffer_[2]); // Next upcoming.
    planning_buffer_.pop_front();
  }
}

void GCodeMachineControl::Impl::machine_move(float feedrate,
                                             const AxesRegister &axis) {
  // We always have a previous position.
  struct AxisTarget *previous = planning_buffer_.back();
  struct AxisTarget *new_pos = planning_buffer_.append();
  int max_steps = -1;
  enum GCodeParserAxis defining_axis = AXIS_X;

  // Real world -> machine coordinates. Here, we are rounding to the next full
  // step, but we never accumulate the error, as we always use the absolute
  // position as reference.
  for (int i = 0; i < GCODE_NUM_AXES; ++i) {
    new_pos->position_steps[i] = round2int(axis[i] * cfg_.steps_per_mm[i]);
    new_pos->delta_steps[i] = new_pos->position_steps[i] - previous->position_steps[i];

    // The defining axis is the one that has to travel the most steps. It defines
    // the frequency to go.
    // All the other axes are doing a fraction of the defining axis.
    if (abs(new_pos->delta_steps[i]) > max_steps) {
      max_steps = abs(new_pos->delta_steps[i]);
      defining_axis = (enum GCodeParserAxis) i;
    }
  }
  new_pos->aux_bits = aux_bits_;
  new_pos->defining_axis = defining_axis;
  new_pos->angle = previous->angle + 180.0f; // default angle to force a speed change

  // Now let's calculate the travel speed in steps/s on the defining axis.
  if (max_steps > 0) {
    float travel_speed = feedrate * cfg_.steps_per_mm[defining_axis];

    // If we're in the euclidian space, choose the step-frequency according to
    // the relative feedrate of the defining axis.
    // (A straight 200mm/s should be the same as a diagnoal 200mm/s)
    if (defining_axis == AXIS_X || defining_axis == AXIS_Y || defining_axis == AXIS_Z) {
      // We need to calculate the feedrate in real-world coordinates as each
      // axis can have a different amount of steps/mm
      const float x = new_pos->delta_steps[AXIS_X] / cfg_.steps_per_mm[AXIS_X];
      const float y = new_pos->delta_steps[AXIS_Y] / cfg_.steps_per_mm[AXIS_Y];
      const float z = new_pos->delta_steps[AXIS_Z] / cfg_.steps_per_mm[AXIS_Z];
      const float total_xyz_len_mm = euclid_distance(x, y, z);
      const float steps_per_mm = cfg_.steps_per_mm[defining_axis];
      const float defining_axis_len_mm = new_pos->delta_steps[defining_axis] / steps_per_mm;
      const float euclid_fraction = fabsf(defining_axis_len_mm) / total_xyz_len_mm;
      travel_speed *= euclid_fraction;

      // If this is a true XY vector, calculate the angle of the vector
      if (z == 0)
        new_pos->angle = (atan2f(y, x) / 3.14159265359) * 180.0f;
    }
    if (travel_speed > max_axis_speed_[defining_axis]) {
      travel_speed = max_axis_speed_[defining_axis];
    }
    new_pos->speed = travel_speed;
  } else {
    new_pos->speed = 0;
  }

  issue_motor_move_if_possible();
}

void GCodeMachineControl::Impl::bring_path_to_halt() {
  // Enqueue a new position that is the same position as the last
  // one seen, but zero speed. That will allow the previous segment to
  // slow down. Enqueue.
  struct AxisTarget *previous = planning_buffer_.back();
  struct AxisTarget *new_pos = planning_buffer_.append();
  for (int i = 0; i < GCODE_NUM_AXES; ++i) {
    new_pos->position_steps[i] = previous->position_steps[i];
    new_pos->delta_steps[i] = 0;
  }
  new_pos->defining_axis = AXIS_X;
  new_pos->speed = 0;
  new_pos->aux_bits = aux_bits_;
  issue_motor_move_if_possible();
}

bool GCodeMachineControl::Impl::test_homing_status_ok() {
  if (!cfg_.require_homing)
    return true;
  if (homing_state_ > HOMING_STATE_NEVER_HOMED)
    return true;
  mprintf("// ERROR: please home machine first (G28).\n");
  return false;
}

bool GCodeMachineControl::Impl::test_within_machine_limits(const AxesRegister &axes) {
  if (!cfg_.range_check)
    return true;

  for (int i = 0; i < GCODE_NUM_AXES; ++i) {
    // Min range ...
    if (axes[i] < 0) {
      // Machine cube must be in positive range.
      if (coordinate_display_origin_[i] != 0) {
        mprintf("// ERROR outside machine limit: Axis %c < min allowed "
                "%+.1fmm in current coordinate system. Ignoring move!\n",
                gcodep_axis2letter((GCodeParserAxis)i),
                -coordinate_display_origin_[i]);
      } else {
        // No relative G92 or similar set. Display in simpler form.
        mprintf("// ERROR outside machine limit: Axis %c < 0. "
                "Ignoring move!\n", gcodep_axis2letter((GCodeParserAxis)i));
      }
      return false;
    }

    // Max range ..
    if (cfg_.move_range_mm[i] <= 0)
      continue;  // max range not configured.
    const float max_limit = cfg_.move_range_mm[i];
    if (axes[i] > max_limit) {
      // Machine cube must be within machine limits if defined.
      if (coordinate_display_origin_[i] != 0) {
        mprintf("// ERROR outside machine limit: Axis %c > max allowed %+.1fmm "
                "in current coordinate system (=%.1fmm machine absolute). "
                "Ignoring move!\n",
                gcodep_axis2letter((GCodeParserAxis)i),
                max_limit - coordinate_display_origin_[i], max_limit);
      } else {
        // No relative G92 or similar set. Display in simpler form.
        mprintf("// ERROR outside machine limit: Axis %c > %.1fmm. "
                "Ignoring move!\n", gcodep_axis2letter((GCodeParserAxis)i),
                max_limit);
      }
      return false;
    }
  }
  return true;
}

bool GCodeMachineControl::Impl::coordinated_move(float feed,
                                                 const AxesRegister &axis) {
  if (!test_homing_status_ok())
    return false;
  if (!test_within_machine_limits(axis))
    return false;
  if (feed > 0) {
    current_feedrate_mm_per_sec_ = cfg_.speed_factor * feed;
  }
  float feedrate = prog_speed_factor_ * current_feedrate_mm_per_sec_;
  machine_move(feedrate, axis);
  return true;
}

bool GCodeMachineControl::Impl::rapid_move(float feed,
                                           const AxesRegister &axis) {
  if (!test_homing_status_ok())
    return false;
  if (!test_within_machine_limits(axis))
    return false;
  float rapid_feed = g0_feedrate_mm_per_sec_;
  const float given = cfg_.speed_factor * prog_speed_factor_ * feed;
  machine_move(given > 0 ? given : rapid_feed, axis);
  return true;
}

void GCodeMachineControl::Impl::dwell(float value) {
  bring_path_to_halt();
  motor_ops_->WaitQueueEmpty();
  usleep((int) (value * 1000));
}

void GCodeMachineControl::Impl::input_idle() {
  bring_path_to_halt();
}

void GCodeMachineControl::Impl::set_speed_factor(float value) {
  if (value < 0) {
    value = 1.0f + value;   // M220 S-10 interpreted as: 90%
  }
  if (value < 0.005) {
    mprintf("// M220: Not accepting speed factors < 0.5%% (got %.1f%%)\n",
            100.0f * value);
    return;
  }
  prog_speed_factor_ = value;
}

static uint32_t get_endstop_gpio_descriptor(struct EndstopConfig config) {
  switch (config.endstop_number) {
  case 1: return END_1_GPIO;
  case 2: return END_2_GPIO;
  case 3: return END_3_GPIO;
  case 4: return END_4_GPIO;
  case 5: return END_5_GPIO;
  case 6: return END_6_GPIO;
  }
  return 0;
}

// Get the endstop for the axis.
uint32_t GCodeMachineControl::Impl::GetHomeEndstop(enum GCodeParserAxis axis,
                                                   int *dir, int *trigger_value) const {
  *dir = 1;
  struct EndstopConfig config = max_endstop_[axis];
  if (min_endstop_[axis].endstop_number
      && min_endstop_[axis].homing_use) {
    *dir = -1;
    config = min_endstop_[axis];
  }
  if (!config.homing_use)
    return 0;
  *trigger_value = config.trigger_value;
  return get_endstop_gpio_descriptor(config);
}

// Moves to endstop and returns how many steps it moved in the process.
int GCodeMachineControl::Impl::move_to_endstop(enum GCodeParserAxis axis,
                                               float feedrate, int backoff,
                                               int dir, int trigger_value,
                                               uint32_t gpio_def) {
  int total_movement = 0;
  struct MotorMovement move_command = {0};
  const float steps_per_mm = cfg_.steps_per_mm[axis];
  float target_speed = feedrate * steps_per_mm;
  if (target_speed > max_axis_speed_[axis]) {
    target_speed = max_axis_speed_[axis];
  }

  move_command.v0 = 0;
  move_command.v1 = target_speed;

  // move axis until endstop is hit
  int segment_move_steps = 0.5 * steps_per_mm * dir;
  assign_steps_to_motors(&move_command, axis, segment_move_steps);
  while (get_gpio(gpio_def) != trigger_value) {
    motor_ops_->Enqueue(move_command, msg_stream_);
    motor_ops_->WaitQueueEmpty();
    total_movement += segment_move_steps;
    // TODO: possibly acceleration over multiple segments.
    move_command.v0 = move_command.v1;
  }

  if (backoff) {
    // move axis off endstop
    segment_move_steps = 0.1 * steps_per_mm * -dir;
    assign_steps_to_motors(&move_command, axis, segment_move_steps);
    while (get_gpio(gpio_def) == trigger_value) {
      motor_ops_->Enqueue(move_command, msg_stream_);
      motor_ops_->WaitQueueEmpty();
      total_movement += segment_move_steps;
    }
  }

  return total_movement;
}

void GCodeMachineControl::Impl::home_axis(enum GCodeParserAxis axis) {
  struct AxisTarget *last = planning_buffer_.back();
  float home_pos = 0; // assume HOME_POS_ORIGIN
  int dir;
  int trigger_value;
  uint32_t gpio_def = GetHomeEndstop(axis, &dir, &trigger_value);
  if (!gpio_def)
    return;
  move_to_endstop(axis, 15, 1, dir, trigger_value, gpio_def);
  home_pos = (dir < 0) ? 0 : cfg_.move_range_mm[axis];
  last->position_steps[axis] = round2int(home_pos * cfg_.steps_per_mm[axis]);
}

void GCodeMachineControl::Impl::go_home(AxisBitmap_t axes_bitmap) {
  bring_path_to_halt();
  for (const char *order = cfg_.home_order; *order; order++) {
    const enum GCodeParserAxis axis = gcodep_letter2axis(*order);
    if (axis == GCODE_NUM_AXES || !(axes_bitmap & (1 << axis)))
      continue;
    home_axis(axis);
  }
  homing_state_ = HOMING_STATE_HOMED;
}

bool GCodeMachineControl::Impl::probe_axis(float feedrate,
                                           enum GCodeParserAxis axis,
                                           float *probe_result) {
  if (!test_homing_status_ok())
    return false;

  bring_path_to_halt();

  int dir = 1;

  // -- somewhat hackish

  // We try to find the axis that is _not_ used for homing.
  // this is not yet 100% the way it should be. We should actually
  // define the probe-'endstops' somewhat differently.
  // For now, we just do the simple thing
  struct EndstopConfig config = max_endstop_[axis];
  if (min_endstop_[axis].endstop_number
      && !min_endstop_[axis].homing_use) {
    dir = -1;
    config = min_endstop_[axis];
  }
  uint32_t gpio_def = get_endstop_gpio_descriptor(config);
  if (!gpio_def || config.homing_use) {
    // We are only looking for probes that are _not_ used for homing.
    mprintf("// BeagleG: No probe - axis %c does not have a travel endstop\n",
            gcodep_axis2letter(axis));
    return false;
  }

  struct AxisTarget *last = planning_buffer_.back();
  if (feedrate <= 0) feedrate = 20;
  // TODO: if the probe fails to trigger, there is no mechanism to stop
  // it right now...
  int total_steps = move_to_endstop(axis, feedrate, 0, dir,
                                    config.trigger_value, gpio_def);
  last->position_steps[axis] += total_steps;
  *probe_result = 1.0f * last->position_steps[axis] / cfg_.steps_per_mm[axis];
  return true;
}

// Cleanup whatever is allocated. Return NULL for convenience.
GCodeMachineControl *GCodeMachineControl::Cleanup(Impl *impl) {
  delete impl;
  return NULL;
}

GCodeMachineControl::GCodeMachineControl(Impl *impl) : impl_(impl) {
}
GCodeMachineControl::~GCodeMachineControl() {
  Cleanup(impl_);
}

// We first do some parameter checking before we create the object.
// TODO(hzeller): this is still pretty much C-ish of the version before.
GCodeMachineControl* GCodeMachineControl::Create(const MachineControlConfig &config,
                                                 MotorOperations *motor_ops,
                                                 FILE *msg_stream) {
  Impl *result = new Impl(config, motor_ops, msg_stream);

  // Always keep the steps_per_mm positive, but extract direction for
  // final assignment to motor.
  struct MachineControlConfig cfg = config;
  for (int i = 0; i < GCODE_NUM_AXES; ++i) {
    result->axis_flip_[i] = cfg.steps_per_mm[i] < 0 ? -1 : 1;
    cfg.steps_per_mm[i] = fabsf(cfg.steps_per_mm[i]);
    if (cfg.max_feedrate[i] < 0) {
      fprintf(stderr, "Invalid negative feedrate %.1f for axis %c\n",
              cfg.max_feedrate[i], gcodep_axis2letter((GCodeParserAxis)i));
      return Cleanup(result);
    }
    if (cfg.acceleration[i] < 0) {
      fprintf(stderr, "Invalid negative acceleration %.1f for axis %c\n",
              cfg.acceleration[i], gcodep_axis2letter((GCodeParserAxis)i));
      return Cleanup(result);
    }
  }

  result->current_feedrate_mm_per_sec_ = cfg.max_feedrate[AXIS_X] / 10;
  float lowest_accel = cfg.max_feedrate[AXIS_X] * cfg.steps_per_mm[AXIS_X];
  for (int i = 0; i < GCODE_NUM_AXES; ++i) {
    if (cfg.max_feedrate[i] > result->g0_feedrate_mm_per_sec_) {
      result->g0_feedrate_mm_per_sec_ = cfg.max_feedrate[i];
    }
    result->max_axis_speed_[i] = cfg.max_feedrate[i] * cfg.steps_per_mm[i];
    const float accel = cfg.acceleration[i] * cfg.steps_per_mm[i];
    result->max_axis_accel_[i] = accel;
    if (accel > result->highest_accel_)
      result->highest_accel_ = accel;
    if (accel < lowest_accel)
      lowest_accel = accel;
  }
  result->prog_speed_factor_ = 1.0f;

  // Mapping axes to physical motors. We might have a larger set of logical
  // axes of which we map a subset to actual motors.
  const char *axis_map = cfg.axis_mapping;
  if (axis_map == NULL) axis_map = kAxisMapping;
  for (int pos = 0; *axis_map; pos++, axis_map++) {
    if (pos >= BEAGLEG_NUM_MOTORS) {
      fprintf(stderr, "Error: Axis mapping string has more elements than "
              "available %d connectors (remaining=\"..%s\").\n", pos, axis_map);
      return Cleanup(result);
    }
    if (*axis_map == '_')
      continue;
    const enum GCodeParserAxis axis = gcodep_letter2axis(*axis_map);
    if (axis == GCODE_NUM_AXES) {
      fprintf(stderr,
              "Illegal axis->connector mapping character '%c' in '%s' "
              "(Only valid axis letter or '_' to skip a connector).\n",
              toupper(*axis_map), cfg.axis_mapping);
      return Cleanup(result);
    }
    result->driver_flip_[pos] = (tolower(*axis_map) == *axis_map) ? -1 : 1;
    result->axis_to_driver_[axis] |= (1 << pos);
  }

  // Extract enstop polarity
  char endstop_trigger[NUM_ENDSTOPS] = {0};
  if (cfg.endswitch_polarity) {
    const char *map = cfg.endswitch_polarity;
    for (int switch_connector = 0; *map; switch_connector++, map++) {
      if (*map == '_' || *map == '0' || *map == '-' || *map == 'L') {
        endstop_trigger[switch_connector] = 0;
      } else if (*map == '1' || *map == '+' || *map == 'H') {
        endstop_trigger[switch_connector] = 1;
      } else {
        fprintf(stderr, "Illegal endswitch polarity character '%c' in '%s'.\n",
                *map, cfg.endswitch_polarity);
        return Cleanup(result);
      }
    }
  }

  int error_count = 0;

  // Now map the endstops. String position is position on the switch connector
  if (cfg.min_endswitch) {
    const char *map = cfg.min_endswitch;
    for (int switch_connector = 0; *map; switch_connector++, map++) {
      if (*map == '_')
        continue;
      const enum GCodeParserAxis axis = gcodep_letter2axis(*map);
      if (axis == GCODE_NUM_AXES) {
        fprintf(stderr,
                "Illegal axis->min_endswitch mapping character '%c' in '%s' "
                "(Only valid axis letter or '_' to skip a connector).\n",
                toupper(*map), cfg.min_endswitch);
        ++error_count;
        continue;
      }
      result->min_endstop_[axis].endstop_number = switch_connector + 1;
      result->min_endstop_[axis].homing_use = (toupper(*map) == *map) ? 1 : 0;
      result->min_endstop_[axis].trigger_value = endstop_trigger[switch_connector];
    }
  }

  if (cfg.max_endswitch) {
    const char *map = cfg.max_endswitch;
    for (int switch_connector = 0; *map; switch_connector++, map++) {
      if (*map == '_')
        continue;
      const enum GCodeParserAxis axis = gcodep_letter2axis(*map);
      if (axis == GCODE_NUM_AXES) {
        fprintf(stderr,
                "Illegal axis->min_endswitch mapping character '%c' in '%s' "
                "(Only valid axis letter or '_' to skip a connector).\n",
                toupper(*map), cfg.min_endswitch);
        ++error_count;
        continue;
      }
      const char for_homing = (toupper(*map) == *map) ? 1 : 0;
      if (result->cfg_.move_range_mm[axis] <= 0) {
        fprintf(stderr,
                "Error: Endstop for axis %c defined at max-endswitch which "
                "implies that we need to know that position; yet "
                "no --range value was given for that axis\n", *map);
        ++error_count;
        continue;
      }
      result->max_endstop_[axis].endstop_number = switch_connector + 1;
      result->max_endstop_[axis].homing_use = for_homing;
      result->max_endstop_[axis].trigger_value = endstop_trigger[switch_connector];
    }
  }

  // Check if things are plausible: we only allow one home endstop per axis.
  for (int axis = 0; axis < GCODE_NUM_AXES; ++axis) {
    if (result->min_endstop_[axis].endstop_number != 0
        && result->max_endstop_[axis].endstop_number != 0) {
      if (result->min_endstop_[axis].homing_use
          && result->max_endstop_[axis].homing_use) {
        fprintf(stderr, "Error: There can only be one home-origin for axis %c, "
                "but both min/max are set for homing (Uppercase letter)\n",
                gcodep_axis2letter((GCodeParserAxis)axis));
        ++error_count;
        continue;
      }
    }
  }

  // Now let's see what motors are mapped to any useful output.
  if (result->cfg_.debug_print) fprintf(stderr, "-- Config --\n");
  for (int i = 0; i < GCODE_NUM_AXES; ++i) {
    if (result->axis_to_driver_[i] == 0)
      continue;
    char is_error = (result->cfg_.steps_per_mm[i] <= 0
                     || result->cfg_.max_feedrate[i] <= 0);
    if (result->cfg_.debug_print || is_error) {
      fprintf(stderr, "%c axis: %5.1fmm/s, %7.1fmm/s^2, %9.4f steps/mm%s ",
              gcodep_axis2letter((GCodeParserAxis)i), result->cfg_.max_feedrate[i],
              result->cfg_.acceleration[i],
              result->cfg_.steps_per_mm[i],
              result->axis_flip_[i] < 0 ? " (reversed)" : "");
      if (result->cfg_.move_range_mm[i] > 0) {
        fprintf(stderr, "[ limit %5.1fmm ] ",
                result->cfg_.move_range_mm[i]);
      } else {
        fprintf(stderr, "[ unknown limit ] ");
      }
      int endstop = result->min_endstop_[i].endstop_number;
      const char *trg = result->min_endstop_[i].trigger_value ? "hi" : "lo";
      if (endstop) {
        fprintf(stderr, "min-switch %d (%s-trigger)%s; ",
		endstop, trg,
                result->min_endstop_[i].homing_use ? " [HOME]" : "       ");
      }
      endstop = result->max_endstop_[i].endstop_number;
      trg = result->max_endstop_[i].trigger_value ? "hi" : "lo";
      if (endstop) {
        fprintf(stderr, "max-switch %d (%s-trigger)%s;",
		endstop, trg,
                result->max_endstop_[i].homing_use ? " [HOME]" : "");
      }
      if (!result->cfg_.range_check)
        fprintf(stderr, "Limit checks disabled!");
      fprintf(stderr, "\n");
    }
    if (is_error) {
      fprintf(stderr, "\tERROR: that is an invalid feedrate or steps/mm.\n");
      ++error_count;
    }
  }
  if (error_count)
    return Cleanup(result);

  // Initial machine position. We assume the homed position here, which is
  // wherever the endswitch is for each axis.
  struct AxisTarget *init_axis = result->planning_buffer_.append();
  for (int axis = 0; axis < GCODE_NUM_AXES; ++axis) {
    int dir, dummy;
    if (result->GetHomeEndstop((GCodeParserAxis)axis, &dir, &dummy)) {
      const float home_pos = (dir < 0) ? 0 : result->cfg_.move_range_mm[axis];
      init_axis->position_steps[axis]
        = round2int(home_pos * result->cfg_.steps_per_mm[axis]);
    } else {
      init_axis->position_steps[axis] = 0;
    }
  }
  init_axis->speed = 0;

  return new GCodeMachineControl(result);
}

void GCodeMachineControl::GetHomePos(AxesRegister *home_pos) {
  home_pos->zero();
  int dir;
  int dummy;
  for (int axis = 0; axis < GCODE_NUM_AXES; ++axis) {
    if (!impl_->GetHomeEndstop((GCodeParserAxis)axis, &dir, &dummy))
      continue;
    (*home_pos)[axis] = (dir < 0) ? 0 : impl_->cfg_.move_range_mm[axis];
  }
}

GCodeParser::Events *GCodeMachineControl::ParseEventReceiver() {
  return impl_;
}

void GCodeMachineControl::SetMsgOut(FILE *msg_stream) {
  impl_->msg_stream_ = msg_stream;
}

MachineControlConfig::MachineControlConfig() {
  bzero(this, sizeof(*this));
  memcpy(steps_per_mm, kStepsPerMM, sizeof(steps_per_mm));
  memcpy(max_feedrate, kMaxFeedrate, sizeof(max_feedrate));
  memcpy(acceleration, kDefaultAccel, sizeof(acceleration));
  speed_factor = 1;
  debug_print = 0;
  synchronous = 0;
  range_check = 1;
  axis_mapping = kAxisMapping;
  home_order = kHomeOrder;
  threshold_angle = 10.0;
}