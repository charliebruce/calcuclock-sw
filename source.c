/*
Calcuclock firmware v0.3 - Charlie Bruce, 2013
 
 
 TECH NOTES:
 
 Uses timer2 for 32.768khz timekeeping ("real time")
 Uses timer0 for delay, delayMicrosecond, as Arduino does.
 uses timer1 as display update, approximately once or twice per millisecond.
 When it hasn't been pressed for a while it goes into a very deep sleep - only C/CE/ON can wake it.
 In deep sleep, virtually nothing but the low-level timekeeping stuff is running.
 
 Brown-out detection is off in sleep, on when running? Or do we use the ADC to check the battery level every so often?
 WDT disabled.
 
 TODO stopwatch, finish calculation
 
 
 
 */

#include <avr/sleep.h>  //Needed for sleep_mode
#include <avr/power.h>  //Needed for powering down perihperals such as the ADC/TWI and Timers
#include <stdint.h>     //Needed for uint8_t, suits Charlie's coding style better
#include <util/delay.h> //Needed for small delays such as when showing LEDs

#include <math.h>

//The state of each segment (A..DP for devices, left = 0, right = 5).
volatile uint8_t segstates[6];

//Has the CE button been pressed?
volatile boolean button_pressed = false;


//Pin numbers for the 7-segment display
const uint8_t segs[8] = {
  8,9,10,11,12,13,6,7};
const uint8_t cols[6] = {
  4,5,A2,A3,A4,A5};

const uint8_t numbersToSegments[11] =
{
  /*0*/  0b00111111,
  /*1*/  0b00000110,
  /*2*/  0b01011011,
  /*3*/  0b01001111,
  /*4*/  0b01100110,
  /*5*/  0b01101101,
  /*6*/  0b01111101,
  /*7*/  0b00000111,
  /*8*/  0b01111111,
  /*9*/  0b01100111,   //Note: If you prefer curly 9s, use 0b01101111 instead
  /*BLANK*/  0
};

//Input buttons
const uint8_t ceButton = 2;
const uint8_t btnsA = A0;
const uint8_t btnsB = A1;
const uint8_t ledPin = 3;

#define SEGMENT_OFF HIGH
#define SEGMENT_ON LOW

#define COLUMN_OFF HIGH
#define COLUMN_ON LOW

//preload timer with 65535 - 4000 - 4,000 cycles at 8mhz is 2khz.
#define PWM_TIME (65535-4000)

#define SUNDAY 0
#define MONDAY 1
#define TUESDAY 2
#define MWEDNESDAY 3
#define THURSDAY 4
#define FRIDAY 5
#define SATURDAY 6


#define WITH_DECIMAL_PLACE |(1<<7)

enum Keys {
  KEY_0 = 0,
  KEY_1,
  KEY_2,
  KEY_3,
  KEY_4,
  KEY_5,
  KEY_6,
  KEY_7,
  KEY_8,
  KEY_9,
  KEY_DP,
  KEY_EQ,
  KEY_ADD,
  KEY_SUB,
  KEY_MUL,
  KEY_DIV,
  NO_KEY
};

//Time variables - GMT - 24-hour

volatile uint8_t hours = 01;
volatile uint8_t minutes = 17;
volatile uint8_t seconds = 00;


//Date variables - GMT
volatile int year = 2013;
volatile int month = 7;
volatile int day = 29;



uint8_t tzc_hours = 0;
uint8_t tzc_day = 1;
uint8_t tzc_month = 1;
int tzc_year = 2000;

//Timezone - 0 is GMT, 1 is BST
//where 1 means that the displayed time is one hour greater than GMT
uint8_t timezone = 1;

#define MIN_SAFE_BATTERY_VOLTAGE 2600



void setup(){

  //We can't sleep any more deeply than this, or else we'll start losing track of trime.
  set_sleep_mode(SLEEP_MODE_PWR_SAVE);
  sleep_enable();

  //All inputs, no pullups
  for(int x=1;x<18;x++){
    pinMode(x, INPUT);
    digitalWrite(x, LOW);
  }

  pinMode(ledPin, OUTPUT);

  pinMode(ceButton, INPUT); //C/CE/ON button
  digitalWrite(ceButton, HIGH); //Internal pull-up on this button.

  //Set up the 7-segment display pins as outputs.
  for(uint8_t i=0;i<8;i++){
    pinMode(segs[i], OUTPUT);
    digitalWrite(segs[i], SEGMENT_OFF);
  }
  for(uint8_t i=0;i<6;i++){
    pinMode(cols[i], OUTPUT);
    segstates[i] = 0;
  }


  //Turn off unused hardware on the chip to save power.
  power_twi_disable();
  power_spi_disable();
  power_usart0_disable();

  //Debug only - remove these in the final version.
  //Serial.begin(9600);



  //Set up timer 1 - display update
  TCCR1A = 0;
  TCCR1B = 0;

  TCNT1 = PWM_TIME;
  TCCR1B |= (1 << CS10);    //no prescaler

  //  TCNT1 = 62410;            //10hz
  //  TCCR1B |= (1 << CS12);    //256 prescaler

  TIMSK1 |= (1 << TOIE1);   //enable timer overflow interrupt

  //Set up timer 2 - real time clock
  TCCR2A = 0;
  TCCR2B = (1<<CS22)|(1<<CS20); //1-second resolution
  //TCCR2B = (1<<CS22)|(1<<CS21)|(1<<CS20); //8-second resolution saves power.
  ASSR = (1<<AS2); //Enable asynchronous operation
  TIMSK2 = (1<<TOIE2); //Enable the timer 2 interrupt

  //Interrupt when CE pressed
  EICRA = (1<<ISC01); //falling edge (button press not release)
  EIMSK = (1<<INT0); //Enable the interrupt INT0



    //Enable interrupts
  sei();
}

void loop() {

  
  //TODO sleep here - turn off display segments, any pullups (except on CE), screen timer, timer0, ADC, USART (leave only timer2 and INT0 running)
  goSleepUntilButton();

  _delay_ms(100);
  button_pressed = false;



  //Single press of CE button enters calculator mode, double press enters clock mode, triple press triggers TV-B-GONE, holding enters clockset mode.

  //Clockset mode should account for BST or NOT-BST (whenever the hour, day or month increments, check against the BST conditions?)
  //Or just require user intervention...

  //clockMode();

  calculatorMode();

  //Measure the battery voltage - if we're at less than MIN_SAFE_BATTERY_VOLTAGE, show a warning. 
  
  //Note that this measurement happens when the LECD is ON! not OFF
  long vcc = 0;
  if ((vcc = readVcc()) < MIN_SAFE_BATTERY_VOLTAGE) {
    showLowBatteryWarning();
    _delay_ms(3000);
    displayLong(vcc);
    segstates[2] |= 128;//DP
    _delay_ms(3000);
  }
  
}



void clockMode() {



  //CLOCK MODE

  //Accounta for timezone (tzc_ means timezone-corrected)
  //Seconds, minutes never change between timezones, only hours/days/months
  //This only allows for positive timezone change of (timezone) hours WRT. GMT. Wouldn't try this over more than a 23 hour shift


  calculateTimezoneCorrection();
  displayDate();
  _delay_ms(3000);


  for(int i=0;i<300;i++) {
    calculateTimezoneCorrection();
    displayTime();
    _delay_ms(10);
  }


}


void calculatorMode() {

  displayLong(123);//0 crashes this
  
  //Sleep time?
  unsigned long sleepTime = millis();
  
  //Wait for a keypad button to be pressed
  uint8_t kpb = NO_KEY;
  
  while((kpb = readKeypad()) == NO_KEY) {
   if ((millis() - sleepTime) > 15000)
      return; //After 15s go to sleep again.
  }





}

void displayPressedKey() {

  uint8_t k = readKeypad();


  while (k == NO_KEY)
    k = readKeypad();

  if (k<10) {

    segstates[0] = numbersToSegments[k];
    segstates[1] = 0;
    segstates[2] = 0;
    segstates[3] = 0;
    segstates[4] = 0;
    segstates[5] = 0;

  }

  if(k == KEY_DP) {

    segstates[0] = 0b01011110;
    segstates[1] = 0b01110011;
    segstates[2] = 0;
    segstates[3] = 0;
    segstates[4] = 0;
    segstates[5] = 0;

  }

  if(k == KEY_EQ) {

    segstates[0] = 0b01111001;
    segstates[1] = 0b01100111;
    segstates[2] = 0;
    segstates[3] = 0;
    segstates[4] = 0;
    segstates[5] = 0;

  }

  if(k == KEY_ADD) {

    segstates[0] = 0b01110111; 
    segstates[1] = 0b01011110;
    segstates[2] = 0b01011110;
    segstates[3] = 0;
    segstates[4] = 0;
    segstates[5] = 0;

  }
  if(k == KEY_SUB) { //Display the text Sub

    segstates[0] = 0b01101101; 
    segstates[1] = 0b00011100;
    segstates[2] = 0b01111100;
    segstates[3] = 0;
    segstates[4] = 0;
    segstates[5] = 0;

  }
  if(k == KEY_MUL) { //Display the text Tpl

    segstates[0] = 0b01111000; 
    segstates[1] = 0b01110011;
    segstates[2] = 0b00111000;
    segstates[3] = 0;
    segstates[4] = 0;
    segstates[5] = 0;

  }
  if(k == KEY_DIV) { //Display the text Div

    segstates[0] = 0b01011110;
    segstates[1] = 0b00010000;
    segstates[2] = 0b00011100;
    segstates[3] = 0;
    segstates[4] = 0;
    segstates[5] = 0;

  }



  _delay_ms(2000);




}



//32.768kHz interrupt handler - this overflows once a second (or once every 8 seconds for maximum power savings?)
//This updates GMT time, detects BST transition
//TODO simplify/optimise this for power saving
SIGNAL(TIMER2_OVF_vect){

  seconds++;
  minutes +=(seconds/60);
  seconds = seconds % 60;
  hours += (minutes/60);
  minutes = minutes % 60;



  if (hours == 24) {
    //Advance once a day.
    hours = 0;
    day++;

    if (day > daysInMonth(year, month)) {
      //If we get to, for example, day 32 of January, this will be true
      //So we need to advance to the next month
      month++;
      day = 1;

      if(month > 12) {
        year++;
        month = 1;
        //Happy New Year! 
      }

    }


  }


  //BST begins at 01:00 GMT on the last Sunday of March and ends at 01:00 GMT on the last Sunday of October

  //Fire at 1AM on Sundays
  if ((seconds == 0) && (minutes == 0) && (hours == 1) && (dayOfWeek(year, month, day) == SUNDAY)) {
    if((month == 3) && ((day+7)>31)){
      //If it's the last Sunday of the month we're entering BST
      timezone = 1;
    } 
    else
      if((month == 10) && ((day+7)>31)){
        //If it's the last Sunday of the month we're leaving BST
        timezone = 0;
      }            
  }

}

//The interrupt occurs when you push the button
SIGNAL(INT0_vect){
  button_pressed = true;
}


//This interrupt (overflow) should happen once every few milliseconds.
//Display update - works with an even brightness. TODO move this to an interrupt every millisecond
SIGNAL(TIMER1_OVF_vect){
  updateDisplay();
  TCNT1 = PWM_TIME;
}






//TODO draw accounting for brightness better than this (primitive left-right shifting)
//TODO account for the transistor with the code. Will update this on the "real" hardware.
//TODO optimise - this is an interrupt, so it should be FAST otherwise we'll miss button C/CE button presses
//(if this consumes more than a few hundred cycles, it's using too many. This runs every 4000 cycles so it shouldn't take moe than 400 or so.
//OR we could nest an interrupt, at the risk of making things VERY messy...
volatile uint8_t onDisplay = 0;
void updateDisplay() {

  digitalWrite(cols[onDisplay],COLUMN_OFF);
  onDisplay = (onDisplay+1)%6;

  for(uint8_t s=0;s<8;s++) {
    digitalWrite(segs[s], ((segstates[onDisplay] & (1 << s))?SEGMENT_ON:SEGMENT_OFF));
  }

  digitalWrite(cols[onDisplay],COLUMN_ON);

}


//We have designed the resistor ladder to produce approximately the following ADC values when read:
//for PinsA
//7 - 0
//4 - 128
//1 - 256
//0 - 384
//8 - 512
//5 - 640
//2 - 768
//. - 896
//No key - 1023

static const Keys keymap[] = {
  KEY_7, KEY_4, KEY_1, KEY_0, KEY_8, KEY_5, KEY_2, KEY_DP, KEY_9, KEY_6, KEY_3, KEY_EQ, KEY_ADD, KEY_SUB, KEY_MUL, KEY_DIV, NO_KEY};
/*
Read the value of the keypad
 */
uint8_t readKeypad(void) {
  //Switch the ADC on

  //TODO Read the pins until the range is low enough to consider it "settled"?
  int val = analogRead(btnsA);
  if (val > (1023-64))
    val = analogRead(btnsB) + 1024;

  //Find out what key this value corresponds with ie 0-63 is key0, 64-191 is key1, ..
  uint8_t keycnt = 0;
  while((val = val-128)>=-64)
  {
    // val = val - 128;
    keycnt++;
  }

  return keymap[keycnt];

  //Switch the ADC off to save power.
}


//Day of week - 0=Sunday, 1=Monday, 2=Tuesday, 3=Wednesday, 4=Thursday, 5=Friday, 6=Saturday
int dayOfWeek(int y, int m, int d)
{
  static const int t[] = {
    0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4  };
  //TODO does this modify the year, unintentionally?
  y -= m < 3;
  return (y + y/4 - y/100 + y/400 + t[m-1] + d) % 7;
}


//Is the current year a leap year? ie does February have a 29th?
boolean leapYear(int y) {
  //Here's the weird set of rules for determining if the year is a leap year...
  if ((y%400) == 0)
    return true;
  else if ((y%100) == 0)
    return false;
  else if ((y%4) == 0)
    return true;
  else
    return false;
}

//"30 days have September, April, June and November, except February, which has 28, or 29 in a leap year."
const uint8_t dim[] = {
  31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
uint8_t daysInMonth(int y, int m) {
  if (leapYear(y) && (m==2))
    return 29;
  else
    return dim[m];  
}

//Account for timezone.
void calculateTimezoneCorrection() {
  tzc_hours = hours + timezone;
  tzc_day = day;
  tzc_month = month;
  tzc_year = year;

  if (tzc_hours >= 24) 
  {
    tzc_hours = tzc_hours % 24;
    tzc_day++;

    if(tzc_day > daysInMonth(tzc_year, tzc_month)) {
      tzc_day = 1;
      tzc_month++; 

      if(tzc_month == 13){
        tzc_month = 1;
        tzc_year++;
      }
    }  
  } 
}

void displayDate() {

  segstates[0] = numbersToSegments[(tzc_day/10)%10];
  segstates[1] = numbersToSegments[tzc_day%10] WITH_DECIMAL_PLACE;
  segstates[2] = numbersToSegments[(tzc_month/10)%10];
  segstates[3] = numbersToSegments[tzc_month%10] WITH_DECIMAL_PLACE;
  segstates[4] = numbersToSegments[((tzc_year-2000)/10)%10];
  segstates[5] = numbersToSegments[(tzc_year-2000)%10]; 

}

void displayTime() {

  segstates[0] = numbersToSegments[(tzc_hours/10)%10];
  segstates[1] = numbersToSegments[tzc_hours%10] WITH_DECIMAL_PLACE;
  segstates[2] = numbersToSegments[(minutes/10)%10];
  segstates[3] = numbersToSegments[minutes%10] WITH_DECIMAL_PLACE;
  segstates[4] = numbersToSegments[(seconds/10)%10];
  segstates[5] = numbersToSegments[seconds%10];

}

//Display any signed long number from 9.9999E9 to -9.99E9. Not smart enough to do 99999E9 yet.
void displayLong(long num) {

  //Clear the screen
  segstates[0] = 0;
  segstates[1] = 0;
  segstates[2] = 0;
  segstates[3] = 0;
  segstates[4] = 0;
  segstates[5] = 0;
  segstates[6] = 0;


  boolean negative = false;
  if (num<0) {
    negative = true; 
    num = num * -1;
    segstates[0] = 0b01000000; // Minus Sign
  }

  //Base10 length (number of digits
  uint8_t base10log = floor(log10(num)) + 1;



  boolean needsExp = false;//Is the number too big to show in one go?
  if (num > (negative?99999:999999))
    needsExp = true; 

  if(needsExp) {
    //Display a number in the format 1.234 E 5 because it's too big...
    uint8_t exponent = base10log - 1;

    double floaty = num / (pow (10.0, (uint8_t) exponent));
    //Write digits.
    for(int i=(negative?1:0);i<3;i++) {
      uint8_t digit = floor(floaty);
      segstates[i] = numbersToSegments[digit];
      floaty = floaty - digit;
      floaty = floaty * 10;
    }
    //Final digit needs rounding to the nearest value (for example, if we're left with 1.55 it needs to round to 2
    uint8_t digit = lround(floaty);
    segstates[3] = numbersToSegments[digit];



    //Add the decimal place.
    segstates[(negative?1:0)] |= 0b10000000;

    segstates[4] = 0b01111001;//E
    segstates[5] = numbersToSegments[exponent % 10];

  } 
  else 
  {
    //Display a number normally (not exponential notation)

    long temp = num;
    long mod = 10;

    //Split the number into digits and display.
    for(int i=5;i>=(6-base10log);i--) {
      long digit = temp % mod;
      temp = temp - digit;
      digit = digit / (mod/10);
      segstates[i] = numbersToSegments[digit];
      mod = mod * 10;
    }
  }

}


void displayDouble(double num) {

  num = 123.45;
  //TODO remove multiple calculations with floating point numbers to improve accuracy of displayed numbers.

  if(isnan(num))
  {
    //Display error since it's NaN
    segstates[0] = 0b01111001;// E
    segstates[1] = 0b00110001;// r
    segstates[2] = 0b00111001;// r
    segstates[3] = 0b01011100;// o
    segstates[4] = 0b00110001;// r
    segstates[5] = 0;
    return; 
  }


  //Clear the screen
  segstates[0] = 0;
  segstates[1] = 0;
  segstates[2] = 0;
  segstates[3] = 0;
  segstates[4] = 0;
  segstates[5] = 0;
  segstates[6] = 0;


  boolean negative = false;
  if (num<0) {
    negative = true; 
    num = num * -1;
    segstates[0] = 0b01000000; // Minus Sign
  }

  //Do we get more accuracy by using exponential notatrion?
  //If the log is negative, ourt number is less than one. If it's E-4 or lower, it's more accurate to use E
  //If it's longer than 6 digits, we also need to use E
  double base10log = log10(num);

  boolean useExp = false;
  //If less than -3 use exponential notation
  if(base10log<-3.0)
    useExp = true;

  if(num > (negative?99999:999999))
    useExp = true;

  if(useExp) {


  } 
  else {

    uint8_t numDigitsAboveDP = floor(base10log) + 1;
    //normalise to x.xxxxxx
    while(numDigitsAboveDP > 1)
    {
      numDigitsAboveDP--;
      num*=0.1;
    }

    for(int i=(negative?1:0);i<6;i++) {
      //Digit is just floor(num)
      uint8_t digit = floor(num);
      segstates[i] = numbersToSegments[digit];    

      num = num - digit;
      num = num * 10;

    }  

    //.Add the decimal place. Only works for >1
    segstates[(uint8_t)floor(base10log)]|=0b10000000;

  }


//TODO Sprintf or fmt_fp (format float point)



}

/*
Improving Accuracy
 While the large tolerance of the internal 1.1 volt reference greatly limits the accuracy of this measurement, for individual projects we can compensate for greater accuracy. To do so, simply measure your Vcc with a voltmeter and with our readVcc() function. Then, replace the constant 1125300L with a new constant:
 
 scale_constant = internal1.1Ref * 1023 * 1000
 
 where
 
 internal1.1Ref = 1.1 * Vcc1 (per voltmeter) / Vcc2 (per readVcc() function)
 
 This calibrated value will be good for the AVR chip measured only, and may be subject to temperature variation. Feel free to experiment with your own measurements.
 */
long readVcc() {
  // Read 1.1V reference against AVcc
  // set the reference to Vcc and the measurement to the internal 1.1V reference
#if defined(__AVR_ATmega32U4__) || defined(__AVR_ATmega1280__) || defined(__AVR_ATmega2560__)
  ADMUX = _BV(REFS0) | _BV(MUX4) | _BV(MUX3) | _BV(MUX2) | _BV(MUX1);
#elif defined (__AVR_ATtiny24__) || defined(__AVR_ATtiny44__) || defined(__AVR_ATtiny84__)
  ADMUX = _BV(MUX5) | _BV(MUX0);
#elif defined (__AVR_ATtiny25__) || defined(__AVR_ATtiny45__) || defined(__AVR_ATtiny85__)
  ADMUX = _BV(MUX3) | _BV(MUX2);
#else
  ADMUX = _BV(REFS0) | _BV(MUX3) | _BV(MUX2) | _BV(MUX1);
#endif  

  _delay_ms(2); // Wait for Vref to settle
  ADCSRA |= _BV(ADSC); // Start conversion
  while (bit_is_set(ADCSRA,ADSC)); // measuring

  uint8_t low  = ADCL; // must read ADCL first - it then locks ADCH  
  uint8_t high = ADCH; // unlocks both

  long result = (high<<8) | low;

  //Original constant: 1125300
  result =  1125300L / result; // Calculate Vcc (in mV); 1125300 = 1.1*1023*1000
  return result; // Vcc in millivolts
}

/*
 *  Go to sleep (low power) and don't leave this function intil the C/CE/ON button  is pressed.
 */
void goSleepUntilButton() {

  //Switch Timer1 off


  //No clock source for Timer/Counter 1
  TCCR1B &= ~((1 << CS10) | (1 << CS11) | (1 << CS12));

  power_timer1_disable();

  //Switch timer0 off
  power_timer0_disable();



  //Switch the segments off
  //All inputs, no pullups
  for(uint8_t i=0;i<8;i++){
    pinMode(segs[i], INPUT);
    digitalWrite(segs[i], LOW);
  }
  for(uint8_t i=0;i<6;i++){
    pinMode(cols[i], INPUT);
    digitalWrite(cols[i], LOW);
  }


  //Switch ADC off

  ADCSRA &= ~(1<<ADEN); //Disable ADC
  ACSR = (1<<ACD); //Disable the analog comparator
  DIDR0 = 0x3F; //Disable digital input buffers on all ADC0-ADC5 pins
  DIDR1 = (1<<AIN1D)|(1<<AIN0D); //Disable digital input buffer on AIN1/0

  power_adc_disable();


  button_pressed = false;

  while (!button_pressed)
    sleep_mode();

  //This point will be reached only after the button has been pressed - now we need to wake up again.
  sleep_disable();



  //Switch vital peripherals back on again

  //Set up the 7-segment display pins as outputs.
  for(uint8_t i=0;i<8;i++){
    pinMode(segs[i], OUTPUT);
    digitalWrite(segs[i], SEGMENT_OFF);
  }

  for(uint8_t i=0;i<6;i++){
    pinMode(cols[i], OUTPUT);
    digitalWrite(cols[i], COLUMN_OFF);
    segstates[i] = 0;  
  }

  power_timer1_enable();
  TCCR1B |= (1 << CS10);


  power_timer0_enable();

  power_adc_enable();

  ADCSRA |= (1 << ADEN); //Enable ADC
  //ACSR = (1<<ACD); //Disable the analog comparator
  DIDR0 = 0; //Enable digital input buffers on all ADC0-ADC5 pins
  DIDR1 &= ~((1<<AIN1D)|(1<<AIN0D)); //Enable digital input buffer on AIN1/0

}

void showLowBatteryWarning() {

  segstates[0] = 0b00111000;// L
  segstates[1] = 0b11011100;// o.

  segstates[2] = 0b01111100;// b
  segstates[3] = 0b01110111;// A
  segstates[4] = 0b01111000;// t
  segstates[5] = 0b01111000;// t



}
