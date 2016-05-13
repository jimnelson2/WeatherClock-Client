#include <Adafruit_CC3000.h>
#include <ccspi.h>
#include <math.h>
#include "sha1.h"
#include <SPI.h>
#include <string.h>
#include "utility/debug.h"
#include <Adafruit_NeoPixel.h>
#ifdef __AVR__
#include <avr/power.h>
#endif

/*****************************************************************************/
// DEBUGGING MACROS - from http://forum.arduino.cc/index.php?topic=46900.0
/*****************************************************************************/
//#define myDEBUG // <---UNCOMMENT TO ENABLE PRINTING OF DEBUG STATEMENTS
#ifdef myDEBUG
#define myDEBUG_PRINT(x)        Serial.print(x)
#define myDEBUG_PRINTDEC(x)     Serial.print(x, DEC)
#define myDEBUG_PRINTLN(x)      Serial.println(x)
#else
#define myDEBUG_PRINT(x)
#define myDEBUG_PRINTDEC(x)
#define myDEBUG_PRINTLN(x) 
#endif 
/*****************************************************************************/


/*****************************************************************************/
// PINS
/*****************************************************************************/
//--For the CC-3000 shield
#define ADAFRUIT_CC3000_IRQ   3     // MUST be an interrupt pin!
#define ADAFRUIT_CC3000_VBAT  5     // can be any pin
#define ADAFRUIT_CC3000_CS    10    // can be any pin

//--For the NEO pixels
#define NEOPIXEL_PIN 6              // This PIN sends data to the pixels
/*****************************************************************************/



/*****************************************************************************/
// EXTERNAL HARDWARE REFERENCES
/*****************************************************************************/
//--For the WiFi shield
Adafruit_CC3000 cc3000 = Adafruit_CC3000(ADAFRUIT_CC3000_CS,
	ADAFRUIT_CC3000_IRQ,
	ADAFRUIT_CC3000_VBAT,
	SPI_CLOCK_DIVIDER);

//--For the NeoPixel
#define PIXEL_COUNT 60
Adafruit_NeoPixel pixel_strip = Adafruit_NeoPixel(PIXEL_COUNT,
	NEOPIXEL_PIN,
	NEO_GRB + NEO_KHZ800);
/*****************************************************************************/



/*****************************************************************************/
// GLOBALS
/*****************************************************************************/
//--Access point
#define WLAN_SSID     "Linksys00775" // 32 char limit
#define WLAN_PASS     "gsnvcwjm0z"   // no idea if there's a limit on this
#define WLAN_SECURITY WLAN_SEC_WPA2

//--API location
#define WEBSITE      "nelson-weather-clock.azurewebsites.net"
#define WEBPAGE      "/api/Forecast/39.5917/-86.1406/"
uint32_t ip;         // ip address of the API

//--API Timings
#define IDLE_TIMEOUT_MS  5000      // API time to wait (in milliseconds)

//--NTP Timings and time
unsigned long lastPolledTime = 0L;           // Last value retrieved from time server, start low to force a poll]
const unsigned long dayOfMillis = 86400000L; // milliseconds in 24 hours (24*60*60*1000)
unsigned long sketchTime = 0L;               // CPU milliseconds since last server query
uint8_t time[] = { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 };

//-- Authentication codes
const uint8_t hmacKey1[] = { 0x48, 0x65, 0x6c, 0x6c, 0x6f, 0x21, 0xde, 0xad, 0xbe, 0xef }; // for OTP calculation
#define account      "blargh" // account corresponding to the OTP key

//--Color mappings. We get a one char hex value from the API, these are the corresponding colors
const uint32_t colors[] = {
	Adafruit_NeoPixel::Color( 0, 0, 0),  // No precip, no color
	Adafruit_NeoPixel::Color( 0,40, 0),  // rain 1
	Adafruit_NeoPixel::Color( 0,10, 0),  // rain 2
	Adafruit_NeoPixel::Color(20,20, 0),  // rain 3
	Adafruit_NeoPixel::Color(40, 0, 0),  // rain 4
	Adafruit_NeoPixel::Color(20,10,20),  // rain 5
	Adafruit_NeoPixel::Color( 0, 0,70),  // snow 1
	Adafruit_NeoPixel::Color( 0, 0,45),  // snow 2
	Adafruit_NeoPixel::Color( 0, 0,30),  // snow 3
	Adafruit_NeoPixel::Color( 0, 0,20),  // snow 4
	Adafruit_NeoPixel::Color( 0, 0,10),  // snow 5
	Adafruit_NeoPixel::Color(45,20,45),  // ice/mix 1
	Adafruit_NeoPixel::Color(35,15,35),  // ice/mix 2
	Adafruit_NeoPixel::Color(25,10,25),  // ice/mix 3
	Adafruit_NeoPixel::Color(20, 5,20),  // ice/mix 4
	Adafruit_NeoPixel::Color(10, 0,10)   // ice/mix 5
};

//--MAIN LOOP timings
#define LOOP_INTERVAL     1000     // End of loop method delay
unsigned long lastAPICall = 0L;      // Sketch time of our last API call
const unsigned long APICallInterval = 120000; // Time we want between API calls

//--HEARTBEAT
uint32_t priorColor = 0; // as a heartbeat, turn a pixel white every loop.
int      priorPixel = 0; // the last pixel we set
						 // this let's us put the right color back.
/*****************************************************************************/

/*****************************************************************************/
/*****************************************************************************/
/*****************************************************************************/
void setup(void)
{
	pixel_strip.begin();   // turn off all the pixels
	pixel_strip.show();    // Initialize all pixels to 'off'

	Serial.begin(115200);
	init_network();

	while (!displayConnectionDetails()) {
		delay(1000);
	}

	ip = 0;
	// Try looking up the website's IP address
	// TODO
	// this is not the best place for this...what if IP address changes?
	// need to move into the LOOP, and perform a lookup every so often.
	myDEBUG_PRINT(WEBSITE);
	myDEBUG_PRINT(F(" -> "));
	while (ip == 0) {
		if (!cc3000.getHostByName(WEBSITE, &ip)) {
			myDEBUG_PRINTLN(F("Couldn't resolve host name!"));
		}
		delay(500);
	}

	cc3000.printIPdotsRev(ip);
}

/*****************************************************************************/
/*****************************************************************************/
/*****************************************************************************/
void loop(void)
{

	/* We want to make an API call once every APICallInterval */
	/* otherwise, we'll just run pretty updates to the pixels */
	/* Note the check for interval is accounting for millis() */
	/* rolling over*/
	pixel_strip.setPixelColor(priorPixel, priorColor); // heartbeat pixel reset
	if (
		((millis() - lastAPICall) > APICallInterval) ||
		(lastAPICall > millis())
		)
	{

		/* Try connecting to the API.
		HTTP/1.1 protocol is used to keep the server from closing the
		connection before all data is read.
		*/
		if (!cc3000.checkConnected())      // make sure we are still connected to wireless network
		{
			myDEBUG_PRINTLN(F("not connected to AP...reconnect..."));
			if (!init_network())    // try a cold start connect to WLAN
			{
				myDEBUG_PRINTLN(F("Unable to reconnect..."));
				delay(15 * 1000);     // if we couldn't connect, try again later
				return;
			}
		}

		lastAPICall = millis();
		/* Our API requires that we provide a OTP value on every call */
		/* so we need to determine that value now */
		unsigned long currentTime = getCurrentTime();
		long OTP = getOTP(currentTime);
		char sOTP[6];
		sprintf(sOTP, "%06lu\n", OTP);
		myDEBUG_PRINTLN(sOTP);

		Adafruit_CC3000_Client www = cc3000.connectTCP(ip, 80);
		if (www.connected()) {
			myDEBUG_PRINTLN(F("Connected to API...will make request"));
			myDEBUG_PRINT(F("Free RAM: "));
			myDEBUG_PRINTDEC(getFreeRam());
			www.fastrprint(F("GET ")); delay(50); // delays per https://forums.adafruit.com/viewtopic.php?f=53&t=70936
			www.fastrprint(WEBPAGE); delay(50);
			www.fastrprint(F(" HTTP/1.1\r\n")); delay(50);
			www.fastrprint(F("Host: ")); delay(50);
			www.fastrprint(WEBSITE); delay(50);
			www.fastrprint(F("\r\n")); delay(50);
			www.fastrprint(F("id: blargh\r\n")); delay(50);
			www.fastrprint(F("key: ")); delay(50);
			www.fastrprint(sOTP); delay(50);
			www.fastrprint("\r\n"); delay(50);
			www.fastrprint(F("\r\n")); delay(50);
			www.println();
		}
		else {
			myDEBUG_PRINTLN(F("Connection to API failed"));
			return;
		}
		myDEBUG_PRINTLN(F("-------------------------------------"));

		/* Read data until either the connection is closed, or the idle timeout is reached. */
		/* This is a good job for a finite state machine...but for now, do it this way. */
		/* We're expecting 60 tuples of [NUM,NUM,NUM] delimited by a commma .*/
		String apiResponse;
		int pixel = 0;
		int idx;
		boolean inDataZone = false;
		unsigned long lastRead = millis();
		while (www.connected() && (millis() - lastRead < IDLE_TIMEOUT_MS)) {
			while (www.available()) {
				char inbyte = www.read();
				myDEBUG_PRINT(inbyte);
				if (inbyte == '"') {
					inDataZone = true;
				}
				else
				{
					if (inDataZone && pixel < PIXEL_COUNT) {

						// this weirdness here converts 0-f to 0-15. I'm sure there's a better way...
						idx = 0;
						if ('0' <= inbyte && inbyte <= '9')
							idx = inbyte - '0';
						if ('a' <= inbyte && inbyte <= 'f')
							idx = inbyte - 'a' + 10;
						//myDEBUG_PRINT("set PIXEL ");myDEBUG_PRINT(pixel);myDEBUG_PRINT(" to ");myDEBUG_PRINTLN(idx);
						pixel_strip.setPixelColor(pixel, colors[idx]);
						pixel = pixel + 1;
					}
				}

				lastRead = millis();
			}
		}
		www.close();
		myDEBUG_PRINTLN(F("Done processing API response"));
		myDEBUG_PRINT("Free RAM: ");
		myDEBUG_PRINTDEC(getFreeRam());
		myDEBUG_PRINTLN(F("-------------------------------------"));
	}

	// light a random pixel so I know we're looping
	priorPixel = millis() % 60;
	priorColor = pixel_strip.getPixelColor(priorPixel);
	pixel_strip.setPixelColor(priorPixel, pixel_strip.Color(20, 20, 20));
	pixel_strip.show();


	delay(LOOP_INTERVAL);

}


uint16_t checkFirmwareVersion(void)
{
	uint8_t major, minor;
	uint16_t version;

#ifndef CC3000_TINY_DRIVER  
	if (!cc3000.getFirmwareVersion(&major, &minor))
	{
		Serial.println(F("Unable to retrieve the firmware version!\r\n"));
		version = 0;
	}
	else
	{
		Serial.print(F("Firmware V. : "));
		Serial.print(major); Serial.print(F(".")); Serial.println(minor);
		version = major; version <<= 8; version |= minor;
	}
#endif
	return version;
}


void displayMACAddress(void)
{
	uint8_t macAddress[6];

	if (!cc3000.getMacAddress(macAddress))
	{
		Serial.println(F("Unable to retrieve MAC Address!\r\n"));
	}
	else
	{
		Serial.print(F("MAC Address : "));
		cc3000.printHex((byte*)&macAddress, 6);
	}
}

bool displayConnectionDetails(void)
{
	uint32_t ipAddress, netmask, gateway, dhcpserv, dnsserv;

	if (!cc3000.getIPAddress(&ipAddress, &netmask, &gateway, &dhcpserv, &dnsserv))
	{
		Serial.println(F("Unable to retrieve the IP Address!\r\n"));
		return false;
	}
	else
	{
		Serial.print(F("\nIP Addr: ")); cc3000.printIPdotsRev(ipAddress);
		Serial.print(F("\nNetmask: ")); cc3000.printIPdotsRev(netmask);
		Serial.print(F("\nGateway: ")); cc3000.printIPdotsRev(gateway);
		Serial.print(F("\nDHCPsrv: ")); cc3000.printIPdotsRev(dhcpserv);
		Serial.print(F("\nDNSserv: ")); cc3000.printIPdotsRev(dnsserv);
		Serial.println();
		return true;
	}
}

// Minimalist time server query; adapted from Adafruit Gutenbird sketch,
// which in turn has roots in Arduino UdpNTPClient tutorial.
unsigned long getTimeFromNTP(void) {

	uint8_t       buf[48];
	unsigned long ntpIP, startTime, t = 0L;
	const unsigned long
		connectTimeout = 15L * 1000L, // Max time to wait for server connection
		responseTimeout = 15L * 1000L; // Max time to wait for data from server
	Adafruit_CC3000_Client client;

	if (!cc3000.checkConnected())      // make sure we are still connected to wireless network
	{
		if (!init_network())    // try a cold start connect to WLAN
		{
			delay(15 * 1000);     // if we couldn't connect, try again later
			return 0;
		}
	}

	myDEBUG_PRINT(F("Locating time server..."));

	// Hostname to IP lookup; use NTP pool (rotates through servers)
	if (cc3000.getHostByName("pool.ntp.org", &ntpIP)) {
		static const char PROGMEM
			timeReqA[] = { 227,  0,  6, 236 },
			timeReqB[] = { 49, 78, 49,  52 };

		myDEBUG_PRINTLN(F("\r\nAttempting connection to time server..."));
		startTime = millis();
		do {
			client = cc3000.connectUDP(ntpIP, 123);
		} while ((!client.connected()) &&
			((millis() - startTime) < connectTimeout));

		if (client.connected()) {
			myDEBUG_PRINT(F("Connected to time server!\r\nIssuing request..."));

			// Assemble and issue request packet
			memset(buf, 0, sizeof(buf));
			memcpy_P(buf, timeReqA, sizeof(timeReqA));
			memcpy_P(&buf[12], timeReqB, sizeof(timeReqB));
			client.write(buf, sizeof(buf));

			myDEBUG_PRINTLN(F("\r\nAwaiting response from time server..."));
			memset(buf, 0, sizeof(buf));
			startTime = millis();
			while ((!client.available()) &&
				((millis() - startTime) < responseTimeout));
			if (client.available()) {
				client.read(buf, sizeof(buf));
				t = (((unsigned long)buf[40] << 24) |
					((unsigned long)buf[41] << 16) |
					((unsigned long)buf[42] << 8) |
					(unsigned long)buf[43]) - 2208988800UL;
				myDEBUG_PRINT(F("OK\r\n"));
			}
			client.close();
		}
	}
	if (!t) myDEBUG_PRINTLN(F("error in NTP "));
	return t;
}


long getCurrentTime(void) {

	/* Every 24 hours, sync up with NTP server*/
	myDEBUG_PRINT("millis : "); myDEBUG_PRINTLN(millis());
	myDEBUG_PRINT("sketchTime : "); myDEBUG_PRINTLN(sketchTime);
	myDEBUG_PRINT("elapsed time since NTP sync : "); myDEBUG_PRINTLN((millis() - sketchTime));
	if (((millis() - sketchTime) > dayOfMillis) ||     // 24 hours has elapsed
		(sketchTime == 0) ||                             // sketch launched, need to make initial call
		(sketchTime > millis())) {                    // millis has rolled over, need to begin again
		myDEBUG_PRINTLN("will try NTP sync...");
		unsigned long t = getTimeFromNTP(); // Query time server
		if (t) {                            // Success?
			lastPolledTime = t;             // Save time
			sketchTime = millis();          // Save sketch time of last valid time query
		}
	}
	return lastPolledTime + (millis() - sketchTime) / 1000;
}


long getOTP(unsigned long currentTime) {
	// for TOTP, we do 30-second windows...
	unsigned long timeWindow = (currentTime / 30);
	// we need to convert the time to a byte array for OTP calc
	// convert from an unsigned long int to a 4-byte array
	time[0] = (int)((timeWindow >> 56) & 0xFF);
	time[1] = (int)((timeWindow >> 48) & 0xFF);
	time[2] = (int)((timeWindow >> 40) & 0XFF);
	time[3] = (int)((timeWindow >> 32) & 0xFF);
	time[4] = (int)((timeWindow >> 24) & 0xFF);
	time[5] = (int)((timeWindow >> 16) & 0xFF);
	time[6] = (int)((timeWindow >> 8) & 0XFF);
	time[7] = (int)((timeWindow & 0XFF));

	uint8_t* hash;
	uint8_t otp[4];

	Sha1.initHmac(hmacKey1, 10);
	Sha1.writebytes(time, 8);
	hash = Sha1.resultHmac();

	memset(otp, 0, 4);
	int offset = hash[19] & 0x0f;
	memcpy(otp, hash + offset, 4);

	unsigned long o = 0x22000000;
	unsigned long l = 0x7FFFFFFF;
	long val = ((long)otp[0] & 0x7F) << 24;
	val |= ((long)otp[1]) << 16;
	val |= ((long)otp[2]) << 8;
	val |= otp[3];

	val = val % 1000000;

	return val;
}

// from https://forums.adafruit.com/viewtopic.php?f=22&t=58976&start=15
int8_t init_network(void)
{
	cc3000.reboot();
	// Set up the CC3000, connect to the access point, and get an IP address.
	myDEBUG_PRINTLN(F("Initializing CC3000..."));

	if (!cc3000.begin())  // fatal
	{  // the following never gets executed.  If .begin() fails, it just hangs
		myDEBUG_PRINTLN(F("Couldn't begin()"));
		while (1);
	}

	if (!cc3000.connectToAP(WLAN_SSID, WLAN_PASS, WLAN_SECURITY)) {
		myDEBUG_PRINTLN(F("Failed to connect to AP!"));
		return 0; // zero = failure.
	}
	delay(5000); // time to make sure we are actually connected
	myDEBUG_PRINTLN(F("Connected to Wireless Network!"));
	myDEBUG_PRINTLN(F("Request DHCP..."));

	while (!cc3000.checkDHCP())
	{
		delay(100);
	}
	myDEBUG_PRINTLN(F("Got IP"));
	return -1;  // non-zero = success
}
