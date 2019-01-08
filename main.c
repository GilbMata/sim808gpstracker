/* ----------------------------------------------------------------------------------------------
 * real GPS car tracker on ATMEGA328P + SIM808 module  - version 1.0 
 * by Adam Loboda - adam.loboda@wp.pl
 * baudrate UART is 9600 as most stable using RC internal clock 1MHz (8MHz with division by 8)
 * MCU clock is lowered to 1MHz to reduce power consumption
 *
 * please configure SIM808 to fixed 9600 first by AT+IPR=9600 command 
 * and disable echo by ATE0 command
 * to ensure stability, and then save config via AT&W command
 *  
 * connections to be made :
 * SIM808 RXD to ATMEGA328 TXD PIN #3,
 * SIM808 TXD to ATMEGA328 RXD PIN #2
 * SIM808 DTR (SLEEP PIN) to ATMEGA328 PC5 PIN #28
 * optional : SIM808 RI/RING pin can be attached to INT0 PIN of ATMEGA328P (allows for POWERDOWN) 
 * VCC :
 * ATMEGA328 VCC (PIN #7) must be connected to lower voltage VCC ~3.3V than 5V of SIM808 board
 * you may use 3x 1N4007 diodes in serial to drop voltage from 5V to ~3.3V
 * GND :
 * ATMEGA328 GND (PIN #8 and PIN #22) to SIM808 GND and 0V 
 * other info :
 * the prototype board was built using www.and-global.com BK-808 breadboard
 * SIM808 breadboard which is powered from 5V DC but uses 3.3V TTL logic on RXD/TXD
 * ----------------------------------------------------------------------------------------------
 */

#include <inttypes.h>
#include <stdio.h>
#include <avr/interrupt.h>
#include <avr/io.h>
#include <avr/pgmspace.h>
#include <util/delay.h>
#include <avr/sleep.h>
#include <avr/wdt.h>
#include <string.h>
#include <avr/power.h>

#define UART_NO_DATA 0x0100
// internal RC oscillator 8MHz with divison by 8 and U2X0 = 1, gives 0.2% error rate for 9600 bps UART speed
// and lower current consumption
// for 1MHz : -U lfuse:w:0x62:m     on ATMEGA328P
#define F_CPU 1000000UL

// SIM and GSM related commands
const char AT[] PROGMEM = { "AT\n\r" }; // SPACE for wakeup from sleep mode
const char ISATECHO[] PROGMEM = { "AT" }; // SPACE for wakeup from sleep mode
const char ISOK[] PROGMEM = { "OK" };
const char ISRING[] PROGMEM = { "RING" };
const char ISREG1[] PROGMEM = { "+CREG: 1,1" };   // SIM registered in HPLMN ( with URC reporting )
const char ISREG2[] PROGMEM = { "+CREG: 1,5" };   // SIM registeref in ROAMING NETWORK (with URC reporting)
const char SHOW_REGISTRATION[] PROGMEM = {"AT+CREG?\n\r"};
const char ENABLEREGREPORT[] PROGMEM = {"AT+CREG=1\n\r"};  //  enable reporting URC of losing 2G coverage by +CREG=0
const char NOCOVERAGE1[] PROGMEM = {"+CREG: 1,0"};         //  network status - no network found  
const char NOCOVERAGE2[] PROGMEM = {"+CREG: 1,2"};         //  network status - currently searching for network
const char NOCOVERAGE[] PROGMEM = {"CREG=0"};              //  URC output of losing 2G coverage 
const char PIN_IS_READY[] PROGMEM = {"+CPIN: READY"};
const char PIN_MUST_BE_ENTERED[] PROGMEM = {"+CPIN: SIM PIN"};

const char SHOW_PIN[] PROGMEM = {"AT+CPIN?\n\r"};
const char ECHO_OFF[] PROGMEM = {"ATE0\n\r"};
const char ENTER_PIN[] PROGMEM = {"AT+CPIN=\"1111\"\n\r"};
const char CFGRIPIN[] PROGMEM = {"AT+CFGRI=1\n\r"};
const char HANGUP[] PROGMEM = {"ATH\n\r"};

const char SMS1[] PROGMEM = {"AT+CMGF=1\r\n"};
const char SMS2[] PROGMEM = {"AT+CMGS=\""};       
const char DELSMS[] PROGMEM = {"AT+CMGDA=\"DEL ALL\"\r\n"};

const char CRLF[] PROGMEM = {"\"\n\r"};
const char CLIP[] PROGMEM = {"AT+CLIP=1\r\n"};


// Flightmode ON OFF - for saving battery while in underground garage with no GSM signal
// tracker will check 2G network availability in 30 minutes intervals
// meanwhile radio will be switched off for power saving
const char FLIGHTON[] PROGMEM = { "AT+CFUN=4\r\n" };
const char FLIGHTOFF[] PROGMEM = { "AT+CFUN=1\r\n" };

// Sleepmode ON OFF - mode #1 requires DTR pin manipulation, 
// to get out of SIM808 sleepmode DTR must be LOW for at least 50 miliseconds
const char SLEEPON[] PROGMEM = { "AT+CSCLK=1\r\n" };
const char SLEEPOFF[] PROGMEM = { "AT+CSCLK=0\r\n" };

// Fix UART speed to 9600 bps
const char SET9600[] PROGMEM = { "AT+IPR=9600\r\n" };

// Save settings to SIM808
const char SAVECNF[] PROGMEM = { "AT&W\r\n" };

// Disable SIM808 LED for further reduction of power consumption
const char DISABLELED[] PROGMEM = { "AT+CNETLIGHT=0\r\n" };

// for sending SMS predefined text 
const char GOOGLELOC1[] PROGMEM = {"\r\n http://maps.google.com/maps?q="};
const char GOOGLELOC2[] PROGMEM = {","};
const char GOOGLELOC3[] PROGMEM = {"\r\n"};
const char LONG[] PROGMEM = {" UTC\n LONGTITUDE="};
const char LATT[] PROGMEM = {" LATTITUDE="};
const char BATT[] PROGMEM = {"\nBATTERY[mV]="};

// definition of APN used for GPRS communication
// please put correct APN, USERNAME and PASSWORD here appropriate
// for your Mobile Network provider 
const char SAPBR1[] PROGMEM = {"AT+SAPBR=3,1,\"CONTYPE\",\"GPRS\"\r\n"};  
const char SAPBR2[] PROGMEM = {"AT+SAPBR=3,1,\"APN\",\"internet\"\r\n"};    // Put your mobile operator APN name here
const char SAPBR3[] PROGMEM = {"AT+SAPBR=3,1,\"USER\",\"internet\"\r\n"};   // Put your mobile operator APN username here
const char SAPBR4[] PROGMEM = {"AT+SAPBR=3,1,\"PWD\",\"internet\"\r\n"};    // Put your mobile operator APN password here 
// PDP context commands
const char SAPBROPEN[] PROGMEM = {"AT+SAPBR=1,1\r\n"};     // open IP bearer
const char SAPBRQUERY[] PROGMEM = {"AT+SAPBR=2,1\r\n"};    // query IP bearer
const char SAPBRCLOSE[] PROGMEM = {"AT+SAPBR=0,1\r\n"};    // close bearer 
const char SAPBRSUCC[] PROGMEM = {"+SAPBR: 1,1"};          // bearer was succesfull we are not checking IP assigned

// check statuses & cells
const char CHECKGPS[] PROGMEM = {"AT+CIPGSMLOC=1,1\r\n"};  // check AGPS position of nearest GSM CELL 
const char CHECKBATT[] PROGMEM = {"AT+CBC\r\n"};           // check battery voltage 

// GPS SIM808 only related AT commands and responses
const char GPSPWRON[] PROGMEM = {"AT+CGPSPWR=1\r\n"};                 // enable GPS inside SIM808 
const char GPSSTAT[] PROGMEM = {"AT+CGPSSTATUS?\r\n"};                // check GPS fixation
const char GPSISFIXED1[] PROGMEM = {"+CGPSSTATUS: Location 3D Fix"};  // GPS in now fixed for 3D Response code
const char GPSISFIXED2[] PROGMEM = {"+CGPSSTATUS: Location 2D Fix"};  // GPS in now fixed for 2D Response code
const char GPSINFO[] PROGMEM = {"AT+CGPSINF=0\r\n"};                  // Read GPS LAT & LONG for SMS
const char GPSCLDSTART[] PROGMEM = {"AT+CGPSRST=0\r\n"};              // GPS cold restart
const char GPSHOTSTART[] PROGMEM = {"AT+CGPSRST=1\r\n"};              // GPS hot restart
const char GPSPWROFF[] PROGMEM = {"AT+CGPSPWR=0\r\n"};                // disable GPS inside SIM808 
const char GNSPWROFF[] PROGMEM = {"AT+CGNSPWR=0\r\n"};                // shutoff GNSS section inside SIM808 
const char GNSPWRON[] PROGMEM = {"AT+CGNSPWR=1\r\n"};                 // switch on GNSS section inside SIM808 

/*
  GPS data retrieval procedure 

AT+CGPSPWR=1 sets the GPS engine ON
AT+CGPSRST=0 gives a COLD RESTART to GPS
AT+CGSINF=0 returns a single NMEA sentence of GPS.
AT+CGPSSTATUS?   returns the Status of GPS whether it has got FIX or not.
If GPS has not received FIX , the NMEA sentence will read all  0 s

*/

 
// Initialize UART to 9600 baud with 8N1. 
void init_uart(void);

// Send and receive functions, that run without ISRs
uint8_t receive_uart();
void send_uart(uint8_t c);

// Send a string
void uart_puts(const char *s);
// Send a PROGMEM string
void uart_puts_P(const char *s);


#ifndef TRUE
#define TRUE 1
#define FALSE 0 
#endif


#define BAUD 9600
// formula for 1MHz clock and U2X0 = 1 double UART speed 
#define MYUBBR ((F_CPU / (BAUD * 8L)) - 1)
#define BUFFER_SIZE 40

// buffers for number of phone, responses from modem, longtitude & latitude data
volatile static uint8_t response[BUFFER_SIZE] = "1234567890123456789012345678901234567890";
volatile static uint8_t response_pos = 0;
volatile static uint8_t phonenumber[15] = "123456789012345";
volatile static uint8_t phonenumber_pos = 0;
volatile static uint8_t latitude[10] = "1234567890";
volatile static uint8_t latitude_pos = 0;
volatile static uint8_t longtitude[10] = "1234567890";
volatile static uint8_t longtitude_pos = 0;
volatile static uint8_t buf[20];  // buffer to copy string from PROGMEM
volatile static uint8_t battery[5] = "12345";  // for battery voltage checking
volatile static uint8_t battery_pos = 0;


// ----------------------------------------------------------------------------------------------
// init_uart
// ----------------------------------------------------------------------------------------------
void init_uart(void) {
  // double speed by U2X0 flag = 1 to have 0.2% error rate on 9600 baud
 UCSR0A = (1<<U2X0);
  // set baud rate from PRESCALER
 UBRR0H = (uint8_t)(MYUBBR>>8);
 UBRR0L = (uint8_t)(MYUBBR);
 UCSR0B|=(1<<TXEN0); //enable TX
 UCSR0B|=(1<<RXEN0); //enable RX
  // set frame format for SIM808 communication
 UCSR0C|=(1<<UCSZ00)|(1<<UCSZ01); // no parity, 1 stop bit, 8-bit data 
}



// ----------------------------------------------------------------------------------------------
// send_uart
// Sends a single char to UART without ISR
// ----------------------------------------------------------------------------------------------
void send_uart(uint8_t c) {
  // wait for empty data register
  while (!(UCSR0A & (1<<UDRE0)));
  // set data into data register
  UDR0 = c;
}



// ----------------------------------------------------------------------------------------------
// receive_uart
// Receives a single char without ISR
// ----------------------------------------------------------------------------------------------
uint8_t receive_uart() {
  while ( !(UCSR0A & (1<<RXC0)) ) 
    ; 
  return UDR0; 
}


// ----------------------------------------------------------------------------------------------
// function to search RX buffer for response  SUB IN RX_BUFFER STR
// ----------------------------------------------------------------------------------------------
uint8_t is_in_rx_buffer(char *str, char *sub) {
   uint8_t i, j=0, k;
    for(i=0; i<BUFFER_SIZE; i++)
    {
      if(str[i] == sub[j])
     {
       for(k=i, j=0; str[k] && sub[j]; j++, k++)  // if NOT NULL on each of strings
            if(str[k]!=sub[j])   break; // if different - start comparision with next char
       // if(!sub[j]) return 1; 
        if(j == strlen(sub)) return 1;  // full substring has been found        
      }
     }
     // substring not found
    return 0;
}

 

// ----------------------------------------------------------------------------------------------
// uart_puts
// Sends a string.
// ----------------------------------------------------------------------------------------------
void uart_puts(const char *s) {
  while (*s) {
    send_uart(*s);
    s++;
  }
}



// ----------------------------------------------------------------------------------------------
// uart_puts_P
// Sends a PROGMEM string.
// ----------------------------------------------------------------------------------------------
void uart_puts_P(const char *s) {
  while (pgm_read_byte(s) != 0x00) {
    send_uart(pgm_read_byte(s++));
  }
}


// ------------------------------------------------------------------------------------------------------------
// READLINE from serial port that starts with CRLF and ends with CRLF and put to 'response' buffer what read
// ------------------------------------------------------------------------------------------------------------
uint8_t readline()
{
  uint16_t char1, i , wholeline ;
  // wait for first CR-LF or exit after timeout i cycles
   i = 0;
   wholeline = 0;
   response_pos = 0;
  //
   do {
      // read chars in pairs to find combination CR LF
      char1 = receive_uart();
      // if CR-LF combination detected start to copy the response
      if   (  char1 != 0x0a && char1 != 0x0d ) 
         { response[response_pos] = char1; 
           response_pos++;
         };
      if    (  char1 == 0x0a || char1 == 0x0d )              
         {  
           // if the line was received and this is only CR/LF ending :
           if (response_pos > 0) // this is EoL
               { response[response_pos] = NULL;
                 response_pos = 0;
                  wholeline = 1;  };
          // just skip this CRLF character and wait for valuable one
               
         };
      // if buffer is empty exit from function there is nothing to read from
      i++;
      } while ( wholeline == 0);

return(1);
}


// --------------------------------------------------------------------------------------------------
// READ CELL GPS from AT+CIPGSMLOC output and put output to 'lattitude' and 'longtitude' buffers
// --------------------------------------------------------------------------------------------------
uint8_t readcellgps()
{
  uint16_t char1;
  uint8_t i;
  // wait for first COMMA, set positions to start of each buffer
   longtitude_pos = 0;
   latitude_pos = 0;
   response_pos = 0;

      // wait for first COMMA sign
      do { 
           char1 = receive_uart();
         } while ( char1 != ',' );

          // if COMMA detected start to copy the response - LONGTITUDE first
      do  { 
           char1 = receive_uart();
           longtitude[longtitude_pos] = char1; 
           longtitude_pos++;
         } while ( char1 != ',' );
           longtitude[longtitude_pos-1] = NULL; 
           longtitude_pos=0;

      // if COMMA detected start to copy the response - LATITUDE second
      do  { 
           char1 = receive_uart();
          latitude[latitude_pos] = char1; 
          latitude_pos++;
         } while ( char1 != ',' );
           // put end of string to latitude
           latitude[latitude_pos-1] = NULL; 
           latitude_pos=0;

      // Now copy DATE & TIME UTC to response buffer and wait for CRLF to finish
        do  { 
           char1 = receive_uart();
           response[response_pos] = char1; 
           response_pos++;
         } while ( (char1 != '\r') && (char1 != '\n') );       // WAIT FOR CR LF
           response[response_pos-1] = NULL; 
           response_pos=0;
           char1 = receive_uart();  // read last CR or LF and exit

return (1);
}

// ---------------------------------------------------------------------------------------------
// read PHONE NUMBER from AT+CLIP output and copy it to buffer 'phonenumber' for SMS sending
// ---------------------------------------------------------------------------------------------
uint8_t readphonenumber()
{
  uint16_t char1;
  // wait for first quotation
   phonenumber_pos = 0;
      // wait for first quotation sign
      do { 
           char1 = receive_uart();
         } while ( char1 != '\"' );
      // if quotation detected start to copy the response - phonenumber 
      do  { 
           char1 = receive_uart();
           phonenumber[phonenumber_pos] = char1; 
           phonenumber_pos++;
         } while ( char1 != '\"' );    // until end of quotation
     // put NULL to end the string phonenumber
           phonenumber[phonenumber_pos-1] = NULL; 
           phonenumber_pos=0;
     // wait for CRLF for new response to empty RX buffer
           do { 
           char1 = receive_uart(); 
           } while ( char1 != 0x0a && char1 != 0x0d  );
return (1);
}

// ----------------------------------------------------------------------------------------------
// Read BATTERY VOLTAGE in milivolts from AT+CBC output and put results to 'battery' buffer
// ----------------------------------------------------------------------------------------------

uint8_t readbattery()
{
  uint16_t char1;
 
   battery_pos = 0;

      // wait for first COMMA sign
      do { 
           char1 = receive_uart();
         } while ( char1 != ',' );

      // wait for second COMMA sign
      do { 
           char1 = receive_uart();
         } while ( char1 != ',' );

      // if 2 COMMA detected start to copy battery voltage to buffer 
      do  { 
           char1 = receive_uart();
           battery[battery_pos] = char1; 
           battery_pos++;
         } while ( char1 != 0x0a && char1 != 0x0d   );

          battery[battery_pos-1] = NULL; 
          battery_pos=0;

return (1);
}


// -----------------------------------------------------------------------------------------------------
// READ SIM808 GPS from AT+CGPSINF output and put output to 'lattitude' and 'longtitude' buffers
// -----------------------------------------------------------------------------------------------------
uint8_t readsim808gps()
{
  uint16_t char1;

  // wait for first COMMA, set positions to start of each buffer
   longtitude_pos = 0;
   latitude_pos = 0;
 
   uart_puts_P(GPSINFO);      // try to retrieve GPS position

      // wait for first COMMA sign
      do { 
           char1 = receive_uart();
         } while ( char1 != ',' );

      // if COMMA detected start to copy the response - LATITUDE comes first
      do  { 
           char1 = receive_uart();
          latitude[latitude_pos] = char1; 
          latitude_pos++;
         } while ( char1 != ',' );
           // put end of string to latitude
           latitude[latitude_pos-1] = NULL; 
           latitude_pos=0;

          // if COMMA detected start to copy the response - LONGTITUDE is second
      do  { 
           char1 = receive_uart();
           longtitude[longtitude_pos] = char1; 
           longtitude_pos++;
         } while ( char1 != ',' );
           longtitude[longtitude_pos-1] = NULL; 
           longtitude_pos=0;

return (1);
}



// --------------------------------------------------------------------------------------------------------------------
// Power on GPS and retrieve position from GPS and put it to LOC and LATT buffers by calling 'readsim808gps' function
// --------------------------------------------------------------------------------------------------------------------
uint8_t readgpsinfo()
{
  uint8_t gpsattempts, gpsfixed;   // counter on attempts to get proper GPS position from SIM808 - for indoor scenarios
  gpsattempts = 0;
  gpsfixed = 0;

  delay_sec(1);
  uart_puts_P(GNSPWRON);      // enable SIM808 GNSS power just in case
  delay_sec(2); 
  uart_puts_P(GPSPWRON);      // enable SIM808 GPS power
  delay_sec(2);  
  uart_puts_P(GPSCLDSTART);   // cold start of SIM808 GPS

  // now we have to wait some time until SIM808 gets GPS signal, assume we are waiting 10 minutes 
  // check the status in 30 sec intervals and then quit
  // if GPS info unavailable, AT+CIPGSMLOC (2G cell position) will be send 
  do 
  {
          delay_sec(30);          // wait one minute

          uart_puts_P(GPSSTAT);   // check GPS status

          if (readline()>0)       // check GPS status response if NOT FIXED
           {
                    gpsfixed = 0;
                    // check if already fixed for 3D coordinated ?
                    memcpy_P(buf, GPSISFIXED1, sizeof(GPSISFIXED1));   
                    if (is_in_rx_buffer(response, buf ) == 1)  gpsfixed = 1;
                    // maybe at least 2D coordinates available ?
                    memcpy_P(buf, GPSISFIXED2, sizeof(GPSISFIXED2));   
                    if (is_in_rx_buffer(response, buf ) == 1)  gpsfixed = 1;
                    // there was some fix, poll the data from GPS
                    if ( gpsfixed == 1)        
                      { 
                        readsim808gps();         // poll GPS position and parse to buffers LAT & LONG
                        delay_sec(2); 
                        uart_puts_P(GPSPWROFF);  // disable SIM808 GPS power to save battery
                        delay_sec(2);  
                        return(1);               // succesful GPS position decoding from SIM808  
                       }                         // end of second IF               
                    else   gpsattempts++;        // if not fixed just increase attempts counter
            };                                   // end of first IF       
     
    } while (gpsattempts<20); // end of DO loop - only 10 attempts in 30 sec intervals to get GPS fixation - 10 minutes

    // GPS position retrieval not succesful - we are disabling GPS/GNSS power 
    delay_sec(2); 
    uart_puts_P(GPSPWROFF);      // disable SIM808 GPS power to save battery
    delay_sec(2);  
    uart_puts_P(GNSPWROFF); 
    delay_sec(2);

return(0); // 0 means that GPS was unable to fix on sattelites
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////
// delay procedure ASM based because _delay_ms() is working bad for 1 MHz clock MCU
/////////////////////////////////////////////////////////////////////////////////////////////////////////////

// delay partucular number of seconds 

void delay_sec(uint8_t i)
{
while(i > 0)
{
// Delay 1 000 000 cycles
// 1s at 1 MHz

asm volatile (
    "    ldi  r18, 6"	"\n"
    "    ldi  r19, 19"	"\n"
    "    ldi  r20, 174"	"\n"
    "1:  dec  r20"	"\n"
    "    brne 1b"	"\n"
    "    dec  r19"	"\n"
    "    brne 1b"	"\n"
    "    dec  r18"	"\n"
    "    brne 1b"	"\n"
    "    rjmp 1f"	"\n"
    "1:"	"\n"
);

i--;  // decrease another second

};    // repeat until i not zero

}


//////////////////////////////////////////
// SIM808 initialization procedures
//////////////////////////////////////////

// -------------------------------------------------------------------------------
// wait for first AT in case SIM808 is starting up
// -------------------------------------------------------------------------------
uint8_t checkat()
{
  uint8_t initialized2;

// wait for first OK while sending AT - autosensing speed on SIM808, but we are working 9600 bps
// SIM 800L can be set by AT+IPR=9600  to fix this speed
// which I do recommend by connecting SIM808 to PC using putty and FTD232 cable

                 initialized2 = 0;
              do { 
               uart_puts_P(AT);
                if (readline()>0)
                   {  // check if OK was received
                    memcpy_P(buf, ISOK, sizeof(ISOK));                     
                   if (is_in_rx_buffer(response, buf) == 1)  initialized2 = 1;                  
                   }
                else
                   { // maybe ECHO is ON and first line was AT
                    memcpy_P(buf, ISATECHO, sizeof(ISATECHO));                     
                   if (is_in_rx_buffer(response, buf) == 1)  initialized2 = 1;                  
                   };

               delay_sec(1);
               } while (initialized2 == 0);

        // send ECHO OFF
		delay_sec(2);
                uart_puts_P(ECHO_OFF);

             return initialized2;
}

// -------------------------------------------------------------------------------
// check if PIN is needed and enter PIN 1111 
// -------------------------------------------------------------------------------
uint8_t checkpin()
{

  uint8_t initialized2;
     // readline and wait for PIN CODE STATUS if needed send PIN 1111 to SIM card if required
                  initialized2 = 0;
              do { 
		delay_sec(2);
                uart_puts_P(SHOW_PIN);
                if (readline()>0)
                   {
                    memcpy_P(buf, PIN_IS_READY, sizeof(PIN_IS_READY));
                  if (is_in_rx_buffer(response, buf) == 1)       initialized2 = 1;                                         
                    memcpy_P(buf, PIN_MUST_BE_ENTERED, sizeof(PIN_MUST_BE_ENTERED));
                  if (is_in_rx_buffer(response, buf) == 1)     
                        {  uart_puts_P(ENTER_PIN);   // ENTER PIN 1111
                           delay_sec(1);
                        };                  
                    };
                  
              } while (initialized2 == 0);
   return initialized2;
}


// -------------------------------------------------------------------------------
// check if registered to the network
// -------------------------------------------------------------------------------
uint8_t checkregistration()
{
  uint8_t initialized2, attempt2, nbrminutes;
     // readline and wait for STATUS NETWORK REGISTRATION from SIM808
     // first 2 networks preferred from SIM list are OK
     initialized2 = 0;
     nbrminutes = 0;
              do { 
                 // give reasonable time to search for 2G network, maybe on the move...
		 delay_sec(180);
                 // now after searching check if already registered to 2G GSM
                 uart_puts_P(SHOW_REGISTRATION);

                 if (readline()>0)
                   {			   
                    memcpy_P(buf, ISREG1, sizeof(ISREG1));
                   if (is_in_rx_buffer(response, buf) == 1)  initialized2 = 1; 
                    memcpy_P(buf, ISREG2, sizeof(ISREG2));
                   if (is_in_rx_buffer(response, buf) == 1)  initialized2 = 1; 
                    memcpy_P(buf, NOCOVERAGE1, sizeof(NOCOVERAGE1)); 
                   if (is_in_rx_buffer(response, buf) == 1)  initialized2 = 0; 
                    memcpy_P(buf, NOCOVERAGE2, sizeof(NOCOVERAGE2)); 
                   if (is_in_rx_buffer(response, buf) == 1)  initialized2 = 0; 
                  // if not registered do a backoff for 1 hour, maybe in underground garage or something
                   if (initialized2 == 0)
                     {  
                      // if not registered or something wrong turn off RADIO for  minutes 
                      // this is not to drain battery in underground garage 
                      delay_sec(1);
                      uart_puts_P(FLIGHTON);    // enable airplane mode - turn off radio
                      delay_sec(1);
                    // enter SLEEP MODE of SIM800L for power saving when no coverage 
                      uart_puts_P(GNSPWROFF); 
                      delay_sec(1);
                      uart_puts_P(GPSPWROFF); 
                      delay_sec(1);
                      uart_puts_P(SLEEPON); 
                     // now wait XX min before turning on radio again, here XX = 60 min
                       for (nbrminutes = 0; nbrminutes<60; nbrminutes++) 
                          { 
                          delay_sec(60); 
                          };  
   
                      // Wake up SIM800L and search for network again                  

                        // disable SLEEPMODE 
                        PORTC &= ~_BV(PC5);  // Toggle LOW the DTR pin of SIM808

                        // send first dummy AT command
                        uart_puts_P(AT);
                         delay_sec(1); 

                        uart_puts_P(SLEEPOFF);  // switch off to SLEEPMODE = 0
                         delay_sec(1); 
                        uart_puts_P(SLEEPOFF);  // switch off to SLEEPMODE = 0
                         delay_sec(1); 

                        PORTC |= _BV(PC5);   // Toggles again HIGH the DTR pin
                      delay_sec(1);

                      uart_puts_P(FLIGHTOFF);  // disable airplane mode - turn on radio and start to search for networks

                     }; // end of no-coverage IF
                   }

                attempt2++; // increase number of attempts - max 48 attempts = 48 hours of no signal 2G network

                // end of DO loop
                } while ( (initialized2 == 0) && (attempt2 < 48) );

      return initialized2;
}
 
// ----------------------------------------------------------------------------------------------
// provision GPRS APNs and passwords - we are not checking if any error not to get deadlocks
// ----------------------------------------------------------------------------------------------
uint8_t provisiongprs()
{
     // connection to GPRS for AGPS basestation data - provision APN and username
               	delay_sec(1);
                uart_puts_P(SAPBR1);
		delay_sec(1);
                uart_puts_P(SAPBR2);
             // only if username password in APN is needed
		delay_sec(1);
                uart_puts_P(SAPBR3);
		delay_sec(1);
                uart_puts_P(SAPBR4);
        	delay_sec(1);   
  return 1;
}

//////////////////////////////////////////////////////////////////////////////////
// POWER SAVING mode on ATMEGA 328P handling to reduce the battery consumption
// this part of code can be used ONLY if you have SIM808 RI/RING PIN connected
// to ATMEGA D2/INT0 interrupt input - otherwise program will hang
//////////////////////////////////////////////////////////////////////////////////

void sleepnow(void)
{

    set_sleep_mode(SLEEP_MODE_PWR_DOWN);

    sleep_enable();

    DDRD &= ~(1 << DDD2);     // Clear the PD2 pin
    // PD2 (PCINT0 pin) is now an input

    PORTD |= (1 << PORTD2);    // turn On the Pull-up
    // PD2 is now an input with pull-up enabled

    // stop interrupts for configuration period
    cli(); 

    // update again INT0 conditions
    EICRA &= ~(1 << ISC01);    // set INT0 to trigger on low level
    EICRA &= ~(1 << ISC00);    // set INT0 to trigger on low level
    EIMSK |= (1 << INT0);     // Turns on INT0 (set bit)

    sei();                         //ensure interrupts enabled so we can wake up again

    sleep_cpu();                   //go to sleep

    // MCU ATTMEGA328P sleeps here until INT0 interrupt

    sleep_disable();               //wake up here

}

// when interrupt from INT0 disable next interrupts from RING pin of SIM808 and go back to main code
ISR(INT0_vect)
{

   EIMSK &= ~(1 << INT0);     // Turns off INT0 (clear bit)
}




// *********************************************************************************************************
//
//                                                    MAIN PROGRAM
//
// *********************************************************************************************************

int main(void) {

  uint8_t initialized, attempt, gpsdataavailable = 0;

  //char buf[20];  // buffer to copy string from PROGMEM

  // enable output to control DTR signal of SIM808 
  // ATMEGA pin PC5 must be connected to DTR/SLEEP signal of SIM808 board
  DDRC |= (1<<5) ; // set OUTPUT mode for PC5
  PORTC &= ~_BV(PC5);  // Toggle LOW the DTR pin of SIM808
  PORTC |= _BV(PC5);   // Toggles HIGH the DTR pin
 
  // pull up RXD input on ATMEGA
  //DDRD &= ~(1 << DDD0);     // Clear the PD2 pin
  // PD2 (PCINT0 pin) is now an input
  //PORTD |= (1 << PORTD0);    // turn On the Pull-up
  // PD2 is now an input with pull-up enabled

  // initialize 9600 baud 8N1 RS232
  init_uart();

  // delay 10 seconds for safe SIM808 startup and network registration
  delay_sec(10);


       // send ECHO OFF
       uart_puts_P(ECHO_OFF);
       delay_sec(2);
      
       // try to communicate with SIM808 over AT
        checkat();
	delay_sec(2);

       // Fix UART speed to 9600 bps to disable autosensing
        uart_puts_P(SET9600); 
	delay_sec(2);

       // if you have connected SIM808 board RI/RING pint to ATMEGA328P INT0 pin
       // configure RI PIN activity for URC ( unsolicited messages like restart of the modem or battery low)
       // uart_puts_P(CFGRIPIN);
       // delay_sec(2);

       // enable reporting URC of losing 2G coverage by +CREG=0
        uart_puts_P(ENABLEREGREPORT);
        delay_sec(2);


       // Save settings to SIM808
        uart_puts_P(SAVECNF);
        delay_sec(3);

       // FOR TESTING ONLY - GPS checking
       // uart_puts_P(GNSPWRON); 
       // delay_sec(2);
       // uart_puts_P(GPSPWRON);      // enable SIM808 GPS power
       // delay_sec(2);  
       // uart_puts_P(GPSCLDSTART);   // cold start of SIM808 GPS
       // delay_sec(20);          // wait one minute
       // uart_puts_P(GPSSTAT);   // check GPS status
       // delay_sec(2);          // wait one minute
       // uart_puts_P(GPSINFO);      // try to retrieve GPS position
       // delay_sec(2);          // wait one minute
       // uart_puts_P(GPSPWROFF);      // enable SIM808 GPS power
       // delay_sec(2);  
 
 
       // check pin status, registration status and provision APN settings
        checkpin();
        checkregistration();
        provisiongprs();
 
       // read phone number of incoming voice call by CLIP, will be needed for SMS sending 
        uart_puts_P(CLIP); 
        delay_sec(2);

     // neverending LOOP

       while (1) {

             do { 

                // WAIT FOR RING message - incoming voice call and send SMS or restart RADIO module if no signal
                   initialized = 0;
                
                // marker for SIM808 GPS data availability  
                   gpsdataavailable = 0;

                // OPTIONAL
                // Disable LED blinking on  SIM808
                //   uart_puts_P(DISABLELED);
                //   delay_sec(2);

               // enter SLEEP MODE #1 of SIM808 for power saving 
               // ( will be interrupted by incoming voice call or SMS ) 
                    uart_puts_P(GNSPWROFF); 
                    delay_sec(1);
                    uart_puts_P(GPSPWROFF); 
                    delay_sec(1);
                    uart_puts_P(SLEEPON); 
                    delay_sec(2);
     
               // ONLY if you have connected SIM808 RI/RING pin to ATMEGA328P INT0 pin
               // otherwise program will HANG here
               // enter SLEEP MODE on ATMEGA328P for power saving
               //    sleepnow(); // sleep function called here 

               // THERE WAS RI / INT0 INTERRUPT OR SOMETHING WAS RECEIVED OVER SERIAL (URC) 
               // for example SMS or 2G coverage was lost (+CREG=0)
               // WE NEED TO GET OFF SLEEPMODE AND READ SERIAL PORT
                if (readline()>0)
                  {
                    memcpy_P(buf, ISRING, sizeof(ISRING));  
                    if (is_in_rx_buffer(response, buf) == 1) 
                     { 
                        readphonenumber(); 

                        // disable SLEEPMODE 
                        PORTC &= ~_BV(PC5);  // Toggle LOW the DTR pin of SIM808

                        // send first dummy AT command
                        uart_puts_P(AT);
                         delay_sec(1); 

                        uart_puts_P(SLEEPOFF);  // switch off to SLEEPMODE = 0
                         delay_sec(1); 
                        uart_puts_P(SLEEPOFF);  // switch off to SLEEPMODE = 0
                         delay_sec(1); 

                        PORTC |= _BV(PC5);   // Toggles again HIGH the DTR pin

                        delay_sec(1);

                        uart_puts_P(HANGUP);
                        delay_sec(1);
                        // provision APN names
                        provisiongprs();
                        // delete all previous SMSes and SMS confirmation to keep SIM808 memory empty   
                        uart_puts_P(SMS1);
                        delay_sec(2); 
                        uart_puts_P(DELSMS);
                        delay_sec(3);

                        // mark 'initialized' flag to further proceed outside do-while loop
                        initialized = 1; 
                      } 

                     // if some other message than RING check if network is avaialble and SIM808 is operational  
                     // for example SMS or 2G coverage was lost (+CREG=0)
                     else 
                      {

                        // disable SLEEPMODE                  
                        // first pull LOW DTR pin for AT LEAST 50 miliseconds
                        PORTC &= ~_BV(PC5);  // Toggle LOW the DTR pin of SIM808

                        // send first dummy AT command
                        uart_puts_P(AT);
                        delay_sec(1); 

                        uart_puts_P(SLEEPOFF);  // switch off to SLEEPMODE = 0
                        delay_sec(1); 
                        uart_puts_P(SLEEPOFF);  // switch off to SLEEPMODE = 0
                        delay_sec(1); 

                        PORTC |= _BV(PC5);   // Toggles again HIGH the DTR pin

                        delay_sec(1);
 
                       // check status of all functions, just in case the SIM808 restarted itself 
                       checkpin();
                       // check if network was lost and we have to search for it
                       checkregistration();
                       // network found so we can proceed with provisioning of APN
                       provisiongprs();

                      // read phone number of incoming voice call by CLIP, will be needed for SMS sending       
                      uart_puts_P(CLIP);
                      delay_sec(2);

                      // delete all SMSes and SMS confirmation to keep SIM808 memory empty   
                      uart_puts_P(SMS1);
                      delay_sec(2); 
                      uart_puts_P(DELSMS);
                      delay_sec(3);

                      //and close the beare just in case it was open
                      uart_puts_P(SAPBRCLOSE);
                      delay_sec(2);

                      // there was something different than RING so we need to go back to the beginning - clear the flag
                      initialized = 0;
                      }; // end of ELSE

                    }; // END of READLINE IF
                 
                } while ( initialized == 0);    // end od DO-WHILE, go to begging and enter SLEEPMODE again 

               // clear the 'initialized' and 'gpsdataavailable' flags 
               initialized = 0;
               gpsdataavailable = 0; 

               // call polling from GPS SIM808, if it was successful mark it by flag 'gpsdataavailable' 
               if (readgpsinfo()==1) 
                  { 
                   gpsdataavailable = 1; 
                   }
               // else no GPS data from SIM808 so we have to relay on Google API and Cell ID accuracy
               // to do it we have to connect to GPRS and querry Google with AT+CIPGSMLOC command
               else    
                  {   
                   // Create connection to GPRS network - 3 attempts if needed, if not succesfull restart the modem 	
                   attempt = 0;
                   gpsdataavailable = 0;

        	   do { 

        	    //and close the bearer first maybe there was an error or something
        	      delay_sec(5);
        	      uart_puts_P(SAPBRCLOSE);
           
        	   // make GPRS network attach and open IP bearer
        	      delay_sec(2);
        	      uart_puts_P(SAPBROPEN);

        	   // query PDP context for IP address after several seconds
        	   // check if GPRS attach was succesfull, do it several times if needed
        	      initialized = 0; 
        	      delay_sec(5);
         	      uart_puts_P(SAPBRQUERY);
        	      if (readline()>0)
      		             {
          		         // checking for properly attached
        		            memcpy_P(buf, SAPBRSUCC, sizeof(SAPBRSUCC));                     
          			   if (is_in_rx_buffer(response, buf) == 1)  initialized = 1;
                 		 // other responses simply ignored as there was no attach
                    	      };
                         // increase attempt counter and repeat until not attached
                      attempt++;
                      } while ( (attempt < 3) && (initialized == 0) );
           
                      // if GPRS was  succesfull the it is time send cell info to Google and query the GPS location
                      if (initialized == 1)
   		           {
               			// GET CELL ID OF BASE STATION and query Google for coordinates then send over SMS with google map loc
		        	delay_sec(1);
		                uart_puts_P(CHECKGPS);
   		                // parse GPS coordinates from the SIM808 answer to 'longtitude' & 'latitude' buffers
       			        readcellgps();
		                //and close the bearer 
		                delay_sec(5);
		                uart_puts_P(SAPBRCLOSE);
		          } /// end of commands when GPRS is working
 
                       }; // END of GPSINFO ELSE 


               // check battery voltage
        	delay_sec(1);
                uart_puts_P(CHECKBATT);
                readbattery();

       		// send a SMS in plain text format
                delay_sec(1); 
       	        uart_puts_P(SMS1);
                delay_sec(1); 
     	        // compose an SMS from fragments - interactive mode CTRL Z at the end
        	uart_puts_P(SMS2);
	        uart_puts(phonenumber);  // send phone number received from CLIP
	        uart_puts_P(CRLF);   			   
                delay_sec(1); 
         	// put info about DATE,TIME, LONG, LATT
                if (  gpsdataavailable == 0)  uart_puts(response);  // send Date & Time info from SIM808 2G cell info  
	        uart_puts_P(LONG);  // send LONGTITUDE
	        uart_puts(longtitude); // from buffer
	        uart_puts_P(LATT);  // send LATITUDE
	        uart_puts(latitude);  // from buffer
	        // put battery info
	        uart_puts_P(BATT);  // send BATTERY VOLTAGE in milivolts
                uart_puts(battery);  // from buffer
                // put link to GOOGLE MAPS
                uart_puts_P(GOOGLELOC1); // send http ****
                uart_puts(latitude);   // send Cellinfo
                uart_puts_P(GOOGLELOC2); // send comma
                uart_puts(longtitude);   // send Cellinfo
		uart_puts_P(GOOGLELOC3); // send CRLF
		delay_sec(1); 
		// end the SMS message
		send_uart(26);   // ctrl Z to end SMS

                // wait ten seconds just in case SMS confirmation was received not to trigger anything
        	delay_sec(10);

              // go to the beginning and enter sleepmode on SIM808 and ATMEGA328P again for power saving

        // end of neverending loop
        };

 
    // end of MAIN code 
}

