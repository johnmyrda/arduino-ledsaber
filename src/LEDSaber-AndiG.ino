// external libraries
#include <avr/wdt.h>
#include <Wire.h>
#include <EEPROM.h>
#include <FastLED.h>

// define our LED blade properties
#define BLADE_LEDS_COUNT  55 // 110 total, 55 is half
#define BLADE_LEDS_PIN    A3

// default colour customization
#define BLADE_BRIGHTNESS  127
#define BLADE_SATURATION  255
#define BLADE_HUE         144
// see properties.h for the presets list

// inactivity timeout
#define INACTIVITY_TIMEOUT 8000 // about 30 seconds

// when using the gyro values to calculate angular speed, we only want the "vertical" and "horizontal" swing axes, 
// not the "handle twist" rotation. Which of the three (0,1,2) axes to use?
#define GYRO_HORIZONTAL 0
#define GYRO_VERTICAL   2

// allocate the memory buffer for the LED array
CRGB blade_leds[BLADE_LEDS_COUNT*2];

//Shutdown pin for TPA2005D1
#define AUDIO_SHUTDOWN_PIN 8

// local extentions
#include "properties.h"
#include "mpu6050.h"
#include "audio.h"

// rotary control direction pins
#define ROTARY_D1_PIN    0
#define ROTARY_D2_PIN    1
// rotary control switch pins
#define ROTARY_SW_PIN    9

// flip these if your knob goes backwards from what you expect
#define ROTARY_DIR_A    -1
#define ROTARY_DIR_B    +1

#include "rotary.h"

int ctrl_counter = 0;

int accel[3];
int accel_last[3];
int gyro[3];

float velocity[3];
float recent_impulse = 0;

float rotation_history = 0.0;
float rotation_offset = 0.0;
float rotation_factor = 0.0;

float rotation_echo = 0.0;

float velocity_offset = 26.6;
float velocity_factor = 0;

int gyro_hum1_volume = 0;
int accel_hum1_volume = 0;
int inactivity_counter = INACTIVITY_TIMEOUT;

void setup() {
  // start serial port?
  Serial.begin(57600);
  // enable watchdog timer
  //wdt_enable(WDTO_1S); // no, we cannot do this on a 32u4, it breaks the bootloader upload
  // setup the blade strip
  LEDS.addLeds<WS2812,BLADE_LEDS_PIN,GRB>(blade_leds,BLADE_LEDS_COUNT*2);
  // The "*2" is because the original design used two led strips with the same data input
  // and I use one strip with one input, so the leds need to be mirrored. 
  LEDS.setDither( 0 );
  blade_length = 0; update_blade(); LEDS.show();
  // start i2c
  Wire.begin(); 
  MPU6050_start();
  // restore our saved state
  eeprom_restore();
  // setup controls
  start_inputs();
  // start sound system
  snd_init();
}

void loop() {
  int delta;
  float av,rv;
  // the program is alive
  //wdt_reset();
  // alternately read from the gyro and accelerometer
  ctrl_counter = ctrl_counter ^ 1;
  if((ctrl_counter & 1)==1) {
    // sample gyro 
    MPU6050_gyro_vector(gyro);
    add_entropy(gyro[0], 0x0F);
    // int3_print(gyro);
    // rotation vector, made from only two axis components (ignore 'twist')
    float gv[3]; 
    gv[0] = gyro[GYRO_HORIZONTAL];
    gv[1] = gyro[GYRO_VERTICAL];
    gv[2] = 0.0;
    // vector length
    float rot = vec3_length(gv);
    rotation_offset -= (rotation_offset - rot) / 300.0;
    // rotation_history = rotation_history * 0.95 + (rot - rotation_offset) / 1000.0;
    rv = (rot-rotation_offset) / 50.0;
    rv = (rotation_history + rv)/2.0;
    rotation_history = rv;
    // Serial.println(rv);
  } else {
    // sample accel 
    MPU6050_accel_vector(accel);
    add_entropy(accel[0], 0x0F);
    // update the velocity vector
    vec3_addint( velocity, accel );
    vec3_scale( velocity, 0.99 );
    // turn velocity vector into scalar factor
    av = vec3_length(velocity) / 10000.0;
    velocity_offset -= (velocity_offset - av) / 10.0;
    velocity_factor = sqrt( abs( velocity_offset - av ) );
    if(velocity_factor>1.0) velocity_factor = 1.0;
  }


  // read inputs
  check_button();
  check_rotary();
  // use some entropy to modulate the sound volume
  snd_buzz_speed = snd_buzz_freq + (entropy & 3);
  snd_hum1_speed = snd_hum1_freq;

  // current mode
  switch(blade_mode) {
    case BLADE_MODE_OFF:
      digitalWrite(AUDIO_SHUTDOWN_PIN, LOW); // enable TPA2005D1
      snd_buzz_volume = 0;
      snd_hum1_volume = 0;
      snd_hum2_volume = 0;
      rotation_echo = 0;
      // ignite if the rotation has exceeded the critical value
      if(rotation_history > 100.0) ignite();
      break;
    case BLADE_MODE_ON: 
      // rotation hum and pitch-bend
      rv = rotation_history;
      if(rv<0.0) rv = 0.0;
      if(rv>140.0) rv = 120.0;
      // update the rotation echo
      if(rv > rotation_echo) {
        // the echo is maximised
        rotation_echo = rv;
      } else {
        // decay the previous echo
        rotation_echo = rotation_echo * ( 0.975f + (float)snd_echo_decay / 10240.0f);
        // use the louder of the original value and 1/1.6 the echo
        rv = max(rv, rotation_echo/1.6);
      }
      // rotation volume term
      rv = rv / 256.0 * global_volume;
      delta = 0;
      if(rv>snd_hum2_volume) { delta = 16; } 
      else if(rv<snd_hum2_volume) { delta = -8;  }
      snd_hum2_volume = value_delta(snd_hum2_volume, delta, 0, 255);
      
      snd_hum1_speed = snd_hum1_freq + (rotation_history / snd_hum2_doppler);
      snd_hum2_speed = snd_hum2_freq + (rotation_history / snd_hum2_doppler);
      // turn velocity into volume modifications
      av = velocity_factor;
      if(av>1.0) av = 1.0;
      snd_buzz_volume = 8 + (int)(av * 32.0); snd_buzz_volume = ((unsigned int)snd_buzz_volume*(unsigned int)global_volume) / 256; 
      snd_hum1_volume = 12 + (int)(av * 40.0); snd_hum1_volume = max(((unsigned int)snd_hum1_volume*(unsigned int)global_volume) / 256, snd_hum2_volume);
      // check for inactivity
      if((velocity_factor < 0.4) && (rotation_history < 10.0)) {
        // inactive
        if(inactivity_counter == 0) {
          extinguish();      
        } else {
          inactivity_counter--;
        }
      } else {
        // active
        inactivity_counter = INACTIVITY_TIMEOUT;
        // Serial.print(velocity_factor); Serial.print(' ');
        // Serial.println(rotation_history);
      }
      break;
    case BLADE_MODE_IGNITE: 
      if(blade_length < BLADE_LEDS_COUNT) {
        blade_length += extend_speed;
        if(blade_length > BLADE_LEDS_COUNT) blade_length = BLADE_LEDS_COUNT;
        update_blade();
        // loud volume
        snd_buzz_volume = (40 * (unsigned int)global_volume) / 256; 
        snd_hum1_volume = (140 * (unsigned int)global_volume) / 256; 
        snd_hum2_volume = (120 * (unsigned int)global_volume) / 256;       
        // bend pitch
        snd_hum1_speed = snd_hum1_freq + (BLADE_LEDS_COUNT - blade_length);
        snd_hum2_speed = snd_hum2_freq + (BLADE_LEDS_COUNT - blade_length);
      } else {
        blade_mode = BLADE_MODE_ON;
        rotation_history = 100.0;
        inactivity_counter = INACTIVITY_TIMEOUT;
      }
      break;
    case BLADE_MODE_EXTINGUISH: 
      if(blade_length > 0) {
        blade_length--; update_blade(); 
        // limit the volume on the way down
        int v = (blade_length * global_volume) / BLADE_LEDS_COUNT;
        snd_buzz_volume = min(v, snd_buzz_volume); 
        snd_hum1_volume = min(v, snd_hum1_volume); 
        snd_hum2_volume = min(v, snd_hum2_volume);       
        // bend pitch
        snd_hum1_speed = snd_hum1_freq + (BLADE_LEDS_COUNT - blade_length);
        snd_hum2_speed = snd_hum2_freq + (BLADE_LEDS_COUNT - blade_length);
      } else {
        blade_mode = BLADE_MODE_OFF;
      }
      break;
  }
}