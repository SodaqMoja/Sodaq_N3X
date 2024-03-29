/*
Copyright (c) 2020 - 2021, SODAQ
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation
and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors
may be used to endorse or promote products derived from this software without
specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
*/

#include "Sodaq_N3X.h"
#include <Sodaq_wdt.h>
#include "time.h"

//#define DEBUG

#define ATTACH_TIMEOUT          180000
#define ATTACH_NEED_REBOOT      40000
#define COPS_TIMEOUT            180000
#define EPOCH_TIME_YEAR_OFF     100        // years since 1900
#define ISCONNECTED_CSQ_TIMEOUT 10000
#define REBOOT_DELAY            1250
#define REBOOT_TIMEOUT          15000
#define SOCKET_CLOSE_TIMEOUT    120000
#define SOCKET_CONNECT_TIMEOUT  120000
#define SOCKET_WRITE_TIMEOUT    120000

#define AUTOMATIC_OPERATOR      "0"

#define SODAQ_GSM_TERMINATOR "\r\n"
#define SODAQ_GSM_MODEM_DEFAULT_INPUT_BUFFER_SIZE 1024
#define SODAQ_GSM_TERMINATOR_LEN (sizeof(SODAQ_GSM_TERMINATOR) - 1)

#define STR_AT                 "AT"
#define STR_RESPONSE_OK        "OK"
#define STR_RESPONSE_ERROR     "ERROR"
#define STR_RESPONSE_CME_ERROR "+CME ERROR:"
#define STR_RESPONSE_CMS_ERROR "+CMS ERROR:"

#define NIBBLE_TO_HEX_CHAR(i)  ((i <= 9) ? ('0' + i) : ('A' - 10 + i))
#define HIGH_NIBBLE(i)         ((i >> 4) & 0x0F)
#define LOW_NIBBLE(i)          (i & 0x0F)
#define HEX_CHAR_TO_NIBBLE(c)  ((c >= 'A') ? (c - 'A' + 0x0A) : (c - '0'))
#define HEX_PAIR_TO_BYTE(h, l) ((HEX_CHAR_TO_NIBBLE(h) << 4) + HEX_CHAR_TO_NIBBLE(l))

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

#ifdef DEBUG
#define debugPrint(...)   { if (_diagStream) _diagStream->print(__VA_ARGS__); }
#define debugPrintln(...) { if (_diagStream) _diagStream->println(__VA_ARGS__); }
#warning "Debug mode is ON"
#else
#define debugPrint(...)
#define debugPrintln(...)
#endif

#define CR '\r'
#define LF '\n'

#define SOCKET_FAIL -1
#define NOW (uint32_t)millis()

static inline bool is_timedout(uint32_t from, uint32_t nr_ms) __attribute__((always_inline));
static inline bool is_timedout(uint32_t from, uint32_t nr_ms) { return (millis() - from) > nr_ms; }


/******************************************************************************
* Main
*****************************************************************************/

Sodaq_N3X::Sodaq_N3X() :
    _modemStream(0),
    _diagStream(0),
    _appendCommand(false),
    _CSQtime(0),
    _startOn(0)
{
    _isBufferInitialized = false;
    _inputBuffer         = 0;
    _inputBufferSize     = SODAQ_GSM_MODEM_DEFAULT_INPUT_BUFFER_SIZE;
    _lastRSSI            = 0;
    _minRSSI             = -113;  // dBm
    _onoff               = 0;

    memset(_socketClosedBit,    1, sizeof(_socketClosedBit));
    memset(_socketPendingBytes, 0, sizeof(_socketPendingBytes));
}

// Initializes the modem instance. Sets the modem stream and the on-off power pins.
void Sodaq_N3X::init(Sodaq_OnOffBee* onoff, Stream& stream, uint8_t cid)
{
    debugPrintln("[init] started.");

    initBuffer(); // safe to call multiple times

    setModemStream(stream);

    _onoff = onoff;
    _cid   = cid;
}

// Turns the modem on and returns true if successful.
bool Sodaq_N3X::on()
{
    bool timeout;
    uint8_t i;

    _startOn = millis();

    if (!isOn() && _onoff) {
        _onoff->on();
    }

    // wait for power up
    timeout = true;
    for (i = 0; i < 10; i++) {
        if (isAlive()) {
            timeout = false;
            break;
        }
    }

    if (timeout) {
        debugPrintln("Error: No Reply from Modem");
        return false;
    }

    return isOn(); // this essentially means isOn() && isAlive()
}

// Turns the modem off and returns true if successful.
bool Sodaq_N3X::off()
{
    // No matter if it is on or off, turn it off.
    if (_onoff) {
        _onoff->off();
    }

    return !isOn();
}

// Turns on and initializes the modem, then connects to the network and activates the data connection.
bool Sodaq_N3X::connect(const char* apn, const char* forceOperator, const char* bandSel)
{
    uint32_t tm;
    uint8_t i;
    int8_t j;

    if (!on()) {
        return false;
    }

    purgeAllResponsesRead();

    if (!execCommand("ATE0")) {
        return false;
    }

    if (!setVerboseErrors(true)) {
        return false;
    }

    if (!execCommand("AT+CIPCA=0")) {
        return false;
    }

    if (!checkCFUN()) {
        return false;
    }

    if(bandSel != 0 && !setBandSel(bandSel))
    {
        return false;
    }

    if (!setDefaultApn(apn)) {
        return false;
    }

    if (!setOperator(forceOperator)) {
        return false;
    }

    if (!setApn(apn)) {
        return false;
    }

    if (!execCommand("AT+CGACT=1")) {
        return false;
    }

    j = 0;
    for (i = 0; i < 20; i++) {
        j = checkApn(apn);
        sodaq_wdt_safe_delay(3000);
        if (j > 0) {
            break;
        }
    }
    if (j < 0) {
        return false;
    }

    tm = millis();

    if (!waitForSignalQuality()) {
        return false;
    }

    if (j == 0 && !attachGprs(ATTACH_TIMEOUT)) {
        return false;
    }

    if (millis() - tm > ATTACH_NEED_REBOOT) {
        reboot();

        if (!waitForSignalQuality()) {
            return false;
        }

        if (!attachGprs(ATTACH_TIMEOUT)) {
            return false;
        }
    }

    return doSIMcheck();
}

// Disconnects the modem from the network.
bool Sodaq_N3X::disconnect()
{
    return execCommand("AT+COPS=2", 40000);
}


/******************************************************************************
* Public
*****************************************************************************/

bool Sodaq_N3X::attachGprs(uint32_t timeout)
{
    uint32_t start = millis();
    uint32_t delay_count = 500;

    while (!is_timedout(start, timeout)) {
        if (isDefinedIP4()) {
            return true;
        }

        sodaq_wdt_safe_delay(delay_count);

        // Next time wait a little longer, but not longer than 5 seconds
        if (delay_count < 5000) {
            delay_count += 1000;
        }
    }

    return false;
}

// Gets Integrated Circuit Card ID.
// Should be provided with a buffer of at least 21 bytes.
// Returns true if successful.
bool Sodaq_N3X::getCCID(char* buffer, size_t size)
{
    if (buffer == NULL || size < 20 + 1) {
        return false;
    }

    println("AT+CCID");

    return (readResponse(buffer, size, "+CCID: ") == GSMResponseOK) && (strlen(buffer) > 0);
}

bool Sodaq_N3X::getCellId(uint16_t* tac, uint32_t* cid)
{
    char responseBuffer[64];

    println("AT+CEREG=2");

    if (readResponse() != GSMResponseOK) {
        return false;
    }

    println("AT+CEREG?");

    memset(responseBuffer, 0, sizeof(responseBuffer));

    if ((readResponse(responseBuffer, sizeof(responseBuffer), "+CEREG: ") == GSMResponseOK) && (strlen(responseBuffer) > 0)) {

        if (sscanf(responseBuffer, "2,%*d,\"%hx\",\"%lx\",", tac, cid) == 2) {
            return true;
        }
    }

    return false;
}

bool Sodaq_N3X::getEpoch(uint32_t* epoch)
{
    char buffer[128];

    println("AT+CCLK?");

    if (readResponse(buffer, sizeof(buffer), "+CCLK: ") != GSMResponseOK) {
        return false;
    }

    // format: "yy/MM/dd,hh:mm:ss+TZ
    int y, m, d, h, min, sec, tz;
    if (sscanf(buffer, "\"%u/%u/%u,%u:%u:%u+%u\"", &y, &m, &d, &h, &min, &sec, &tz) == 7 ||
        sscanf(buffer, "\"%u/%u/%u,%u:%u:%u\"", &y, &m, &d, &h, &min, &sec) == 6)
    {
        *epoch = convertDatetimeToEpoch(y, m, d, h, min, sec);
        return true;
    }

    return false;
}

bool Sodaq_N3X::getFirmwareVersion(char* buffer, size_t size)
{
    if (buffer == NULL || size < 30 + 1) {
        return false;
    }

    return execCommand("AT+CGMR", DEFAULT_READ_MS, buffer, size);
}

bool Sodaq_N3X::getFirmwareRevision(char* buffer, size_t size)
{
    if (buffer == NULL || size < 30 + 1) {
        return false;
    }

    return execCommand("ATI9", DEFAULT_READ_MS, buffer, size);
}

// Gets International Mobile Equipment Identity.
// Should be provided with a buffer of at least 16 bytes.
// Returns true if successful.
bool Sodaq_N3X::getIMEI(char* buffer, size_t size)
{
    char responseBuffer[64];

    if (buffer == NULL || size < 15 + 1) {
        return false;
    }

    println("AT+CGSN=1");

    memset(responseBuffer, 0, sizeof(responseBuffer));

    if ((readResponse(responseBuffer, sizeof(responseBuffer), "+CGSN: ") == GSMResponseOK) && (strlen(responseBuffer) > 0)) {

        if (sscanf(responseBuffer, "\"%[^\"]\"", buffer) == 1) {
            return true;
        }
    }

    return false;
}

bool Sodaq_N3X::getOperatorInfo(uint16_t* mcc, uint16_t* mnc)
{
    uint32_t operatorCode = 0;

    char responseBuffer[64];

    println("AT+COPS?");

    memset(responseBuffer, 0, sizeof(responseBuffer));

    if ((readResponse(responseBuffer, sizeof(responseBuffer), "+COPS: ") == GSMResponseOK) && (strlen(responseBuffer) > 0)) {

        if (sscanf(responseBuffer, "%*d,%*d,\"%lu\"", &operatorCode) == 1) {
            uint16_t divider = (operatorCode > 100000) ? 1000 : 100;

            *mcc = operatorCode / divider;
            *mnc = operatorCode % divider;

            return true;
        }
    }

    return false;
}

bool Sodaq_N3X::getOperatorInfoString(char* buffer, size_t size)
{
    char responseBuffer[64];

    if (size < 32 + 1) {
         return false;
    }

    buffer[0] = 0;

    println("AT+COPS?");

    memset(responseBuffer, 0, sizeof(responseBuffer));

    if ((readResponse(responseBuffer, sizeof(responseBuffer), "+COPS: ") == GSMResponseOK) && (strlen(responseBuffer) > 0)) {

        if (sscanf(responseBuffer, "%*d,%*d,\"%[^\"]\"", buffer) == 1) {
            return true;
        }
    }

    return false;
}

SimStatuses Sodaq_N3X::getSimStatus()
{
    char buffer[32];

    println("AT+CPIN?");

    if (readResponse(buffer, sizeof(buffer)) != GSMResponseOK) {
        return SimStatusUnknown;
    }

    char status[16];

    if (sscanf(buffer, "+CPIN: %" STR(sizeof(status) - 1) "s", status) == 1) {
        return startsWith("READY", status) ? SimReady : SimNeedsPin;
    }

    return SimMissing;
}

bool Sodaq_N3X::execCommand(const char* command, uint32_t timeout, char* buffer, size_t size)
{
    println(command);

    return (readResponse(buffer, size, NULL, timeout) == GSMResponseOK);
}

// Returns true if the modem replies to "AT" commands without timing out.
bool Sodaq_N3X::isAlive()
{
    return execCommand(STR_AT, 450);
}

// Returns true if the modem is connected to the network and IP address is not 0.0.0.0.
bool Sodaq_N3X::isConnected()
{
    return isDefinedIP4() && waitForSignalQuality(ISCONNECTED_CSQ_TIMEOUT);
}

// Returns true if defined IP4 address is not 0.0.0.0.
bool Sodaq_N3X::isDefinedIP4()
{
    char buffer[256];

    println("AT+CGDCONT?");

    if (readResponse(buffer, sizeof(buffer), "+CGDCONT: ") != GSMResponseOK) {
        return false;
    }

    if (strncmp(buffer, "1,\"IP\"", 6) != 0 || strncmp(buffer + 6, ",\"\"", 3) == 0) {
        return false;
    }

    char ip[32];

    if (sscanf(buffer + 6, ",\"%*[^\"]\",\"%[^\"]\",0,0,0,0", ip) != 1) {
        return false;
    }

    return strlen(ip) >= 7 && strcmp(ip, "0.0.0.0") != 0;
}

bool Sodaq_N3X::ping(const char* ip)
{
    print("AT+UPING=\"");
    print(ip);
    println('"');

    return (readResponse() == GSMResponseOK);
}

void Sodaq_N3X::purgeAllResponsesRead()
{
    uint32_t start = millis();

    // make sure all the responses within the timeout have been read
    while ((readResponse(NULL, 0, NULL, 500) != GSMResponseTimeout) && !is_timedout(start, 2000)) {}
}

bool Sodaq_N3X::setApn(const char* apn)
{
    if (apn == NULL || apn[0] == 0) {
        return false;
    }

    print("AT+CGDCONT=");
    print(_cid);
    print(",\"IP\",\"");
    print(apn);
    println('"');

    return (readResponse() == GSMResponseOK);
}

bool Sodaq_N3X::setBandSel(const char* bandSel)
{
    if (bandSel == NULL || bandSel == NULL) {
        return false;
    }

    print("AT+UBANDSEL=");
    println(bandSel);

    return (readResponse() == GSMResponseOK);
}

bool Sodaq_N3X::setDefaultApn(const char* apn)
{
    char buffer[100];
    int pdp_type;
    char default_apn[80];

    if (apn == NULL || apn[0] == 0) {
        return false;
    }

    /* Don't set it again, if it is equal to what we want.
     */
    println("AT+CFGDFTPDN?");

    /* +CFGDFTPDN: 1,"internet.nbiot.telekom.de"
     * First number:
     *  1: IP
     *  2: IPv6
     *  3: IPv4v6
     *  4: NON-IP
     * We only doing / expecting IP!
     */

    if (readResponse(buffer, sizeof(buffer), "+CFGDFTPDN: ") != GSMResponseOK) {
        return false;
    }

    if (sscanf(buffer, "%d,\"%[^\"]\"", &pdp_type, default_apn) != 2) {
        if (sscanf(buffer, "%d,\"\"", &pdp_type) != 1) {
            return false;
        }
    }
    else if (pdp_type == 1 && strcmp(default_apn, apn) == 0) {
        return true;
    }

    /* Either the command failed, or the default wasn't what we expected.
     * Set the default (will be stored in NVM).
     */
    print("AT+CFGDFTPDN=1,\"");
    print(apn);
    println('"');

    return (readResponse() == GSMResponseOK);
}

bool Sodaq_N3X::setOperator(const char* opr)
{
    if (opr == NULL || opr[0] == 0) {
        debugPrintln("Skipping empty operator");
        return true;
    }

    if (strcmp(opr, AUTOMATIC_OPERATOR) == 0) {
        println("AT+COPS=0");
    }
    else {
        print("AT+COPS=1,2,\"");
        print(opr);
        println('"');
    }

    return (readResponse(NULL, 0, NULL, COPS_TIMEOUT) == GSMResponseOK);
}

bool Sodaq_N3X::setRadioActive(bool on)
{
    print("AT+CFUN=");
    println(on ? '1' : '0');

    return (readResponse() == GSMResponseOK);
}

bool Sodaq_N3X::setVerboseErrors(bool on)
{
    print("AT+CMEE=");
    println(on ? '1' : '0');

    return (readResponse() == GSMResponseOK);
}


/******************************************************************************
* RSSI and CSQ
*****************************************************************************/

/*
    The range is the following:
    0: -113 dBm or less
    1: -111 dBm
    2..30: from -109 to -53 dBm with 2 dBm steps
    31: -51 dBm or greater
    99: not known or not detectable or currently not available
*/
int8_t Sodaq_N3X::convertCSQ2RSSI(uint8_t csq) const
{
    return -113 + 2 * csq;
}

uint8_t Sodaq_N3X::convertRSSI2CSQ(int8_t rssi) const
{
    return (rssi + 113) / 2;
}

// Gets the Received Signal Strength Indication in dBm and Bit Error Rate.
// Returns true if successful.
bool Sodaq_N3X::getRSSIAndBER(int8_t* rssi, uint8_t* ber)
{
    static char berValues[] = { 49, 43, 37, 25, 19, 13, 7, 0 }; // 3GPP TS 45.008 [20] subclause 8.2.4

    char buffer[256];
    int csqRaw;
    int berRaw;

    println("AT+CSQ");

    if (readResponse(buffer, sizeof(buffer), "+CSQ: ") != GSMResponseOK) {
        return false;
    }

    if (sscanf(buffer, "%d,%d", &csqRaw, &berRaw) != 2) {
        return false;
    }

    *rssi = ((csqRaw == 99) ? 0 : convertCSQ2RSSI(csqRaw));
    *ber  = ((berRaw == 99 || static_cast<size_t>(berRaw) >= sizeof(berValues)) ? 0 : berValues[berRaw]);

    return true;
}


/******************************************************************************
* Sockets
*****************************************************************************/

bool Sodaq_N3X::socketClose(uint8_t socketID, bool async)
{
    print("AT+USOCL=");
    print(socketID);

    if (async) {
        println(",1");
    }
    else {
        println();
    }

    _socketClosedBit   [socketID] = true;
    _socketPendingBytes[socketID] = 0;

    return readResponse(NULL, 0, NULL, SOCKET_CLOSE_TIMEOUT) == GSMResponseOK;
}

int Sodaq_N3X::socketCloseAll() {

    int closedCount = 0;

    for (uint8_t i = 0; i < SOCKET_COUNT; i++) {
        if (socketClose(i, false)) {
            closedCount++;
        }
    }

    // return the number of sockets we closed
    return closedCount;
}

bool Sodaq_N3X::socketConnect(uint8_t socketID, const char* remoteHost, const uint16_t remotePort)
{
    bool b;

    print("AT+USOCO=");
    print(socketID);
    print(",\"");
    print(remoteHost);
    print("\",");
    println(remotePort);

    b = readResponse(NULL, 0, NULL, SOCKET_CONNECT_TIMEOUT) == GSMResponseOK;

    _socketClosedBit  [socketID] = !b;

    return b;
}

int Sodaq_N3X::socketCreate(uint16_t localPort, Protocols protocol)
{
    char buffer[32];
    int socketID;

    print("AT+USOCR=");
    print(protocol == UDP ? "17" : "6");

    if (localPort > 0) {
        print(',');
        println(localPort);
    }
    else {
        println();
    }

    if (readResponse(buffer, sizeof(buffer), "+USOCR: ") != GSMResponseOK) {
        return SOCKET_FAIL;
    }

    if ((sscanf(buffer, "%d", &socketID) != 1) || (socketID < 0) || (socketID > SOCKET_COUNT)) {
        return SOCKET_FAIL;
    }

    _socketClosedBit   [socketID] = true;
    _socketPendingBytes[socketID] = 0;

    return socketID;
}

size_t Sodaq_N3X::socketGetPendingBytes(uint8_t socketID)
{
    return _socketPendingBytes[socketID];
}

bool Sodaq_N3X::socketHasPendingBytes(uint8_t socketID)
{
    return _socketPendingBytes[socketID] > 0;
}

bool Sodaq_N3X::socketIsClosed(uint8_t socketID)
{
    return _socketClosedBit[socketID];
}

size_t Sodaq_N3X::socketReceive(uint8_t socketID, uint8_t* buffer, size_t size)
{
    char   outBuffer[SODAQ_N3X_MAX_UDP_BUFFER];
    int    retSocketID;
    size_t retSize;

    if (!socketHasPendingBytes(socketID)) {
        // no URC has happened, no socket to read
        debugPrintln("Reading from without available bytes!");
        return 0;
    }

    size = min(size, min(SODAQ_N3X_MAX_UDP_BUFFER, _socketPendingBytes[socketID]));

    print("AT+USORF=");
    println(socketID);

    if (readResponse(outBuffer, sizeof(outBuffer)) != GSMResponseOK) {
        return 0;
    }

    if (sscanf(outBuffer, "+USORF: %d,\"%*[^\"]\",%*d,%d,\"%[^\"]\"", &retSocketID, &retSize, outBuffer) != 3) {
        return 0;
    }

    if ((retSocketID < 0) || (retSocketID >= SOCKET_COUNT)) {
        return 0;
    }

    _socketPendingBytes[socketID] -= retSize;

    if (buffer != NULL && size > 0) {
        for (size_t i = 0; i < retSize * 2; i += 2) {
            buffer[i / 2] = HEX_PAIR_TO_BYTE(outBuffer[i], outBuffer[i + 1]);
        }
    }

    return retSize;
}

size_t Sodaq_N3X::socketSend(uint8_t socketID, const char* remoteHost, const uint16_t remotePort, const uint8_t* buffer, size_t size)
{
    char outBuffer[64];
    int retSocketID;
    int sentLength;

    if (size > SODAQ_MAX_SEND_MESSAGE_SIZE) {
        debugPrintln("Message exceeded maximum size!");
        return 0;
    }

    execCommand("AT+UDCONF=1,1");

    print("AT+USOST=");
    print(socketID);
    print(",\"");
    print(remoteHost);
    print("\",");
    print(remotePort);
    print(',');
    print(size);
    print(",\"");

    for (size_t i = 0; i < size; ++i) {
        print(static_cast<char>(NIBBLE_TO_HEX_CHAR(HIGH_NIBBLE(buffer[i]))));
        print(static_cast<char>(NIBBLE_TO_HEX_CHAR(LOW_NIBBLE(buffer[i]))));
    }

    println('"');

    if (readResponse(outBuffer, sizeof(outBuffer), "+USOST: ", SOCKET_WRITE_TIMEOUT) != GSMResponseOK) {
        return 0;
    }

    if ((sscanf(outBuffer, "%d,%d", &retSocketID, &sentLength) != 2) || (retSocketID < 0) || (retSocketID > SOCKET_COUNT)) {
        return 0;
    }

    return sentLength;
}

bool Sodaq_N3X::socketWaitForReceive(uint8_t socketID, uint32_t timeout)
{
    uint32_t startTime;

    if (socketHasPendingBytes(socketID)) {
        return true;
    }

    startTime = millis();

    while (!socketHasPendingBytes(socketID) && (millis() - startTime) < timeout) {
        isAlive();
        sodaq_wdt_safe_delay(10);
    }

    return socketHasPendingBytes(socketID);
}


/******************************************************************************
* Private
*****************************************************************************/

int8_t Sodaq_N3X::checkApn(const char* requiredAPN)
{
    char buffer[256];

    println("AT+CGDCONT?");

    if (readResponse(buffer, sizeof(buffer), "+CGDCONT: ") != GSMResponseOK) {
        return -1;
    }

    if (strncmp(buffer, "1,\"IP\"", 6) == 0 && strncmp(buffer + 6, ",\"\"", 3) != 0) {
        char apn[64];
        char ip[32];

        if (sscanf(buffer + 6, ",\"%[^\"]\",\"%[^\"]\",0,0,0,0", apn, ip) != 2) { return -1; }

        if (strcmp(apn, requiredAPN) == 0) {
            if (strlen(ip) >= 7 && strcmp(ip, "0.0.0.0") != 0) {
                return 1;
            }
            else {
                return 0;
            }
        }
    }

    return -1;
}

bool Sodaq_N3X::checkCFUN()
{
    char buffer[64];

    println("AT+CFUN?");

    if (readResponse(buffer, sizeof(buffer), "+CFUN: ") != GSMResponseOK) {
        return false;
    }

    return (buffer[0] == '1') || setRadioActive(true);
}

bool Sodaq_N3X::checkURC(char* buffer)
{
    int param1, param2;

    if (buffer[0] != '+') {
        return false;
    }

    if (sscanf(buffer, "+UFOTAS: %d,%d", &param1, &param2) == 2) {
        #ifdef DEBUG
        debugPrint("Unsolicited: FOTA: ");
        debugPrint(param1);
        debugPrint(", ");
        debugPrintln(param2);
        #endif

        return true;
    }

    if (sscanf(buffer, "+UUSORF: %d,%d", &param1, &param2) == 2) {
        debugPrint("Unsolicited: Socket ");
        debugPrint(param1);
        debugPrint(": ");
        debugPrintln(param2);

        if (param1 >= 0 && param1 < SOCKET_COUNT) {
            _socketPendingBytes[param1] += param2;
        }

        return true;
    }

    if (sscanf(buffer, "+UUSOCL: %d", &param1) == 1) {
        debugPrint("Unsolicited: Socket ");
        debugPrintln(param1);

        if (param1 >= 0 && param1 < SOCKET_COUNT) {
            _socketClosedBit[param1] = true;
        }

        return true;
    }

    if (sscanf(buffer, "+CSCON: %d", &param1) == 1) {
        debugPrint("Unsolicited: Connected ");
        debugPrintln(param1);
        return true;
    }

    return false;
}

bool Sodaq_N3X::doSIMcheck()
{
    const uint8_t retry_count = 10;

    for (uint8_t i = 0; i < retry_count; i++) {
        if (i > 0) {
            sodaq_wdt_safe_delay(250);
        }

        if (getSimStatus() == SimReady) {
            return true;
        }
    }

    return false;
}

/**
 * 1. check echo
 * 2. check ok
 * 3. check error
 * 4. if response prefix is not empty, check response prefix, append if multiline
 * 5. check URC, if handled => continue
 * 6. if response prefis is empty, return the whole line return line buffer, append if multiline
*/
GSMResponseTypes Sodaq_N3X::readResponse(char* outBuffer, size_t outMaxSize, const char* prefix, uint32_t timeout)
{
    bool usePrefix    = prefix != NULL && prefix[0] != 0;
    bool useOutBuffer = outBuffer != NULL && outMaxSize > 0;

    uint32_t from = NOW;

    size_t outSize = 0;

    if (outBuffer) {
        outBuffer[0] = 0;
    }

    while (!is_timedout(from, timeout)) {
        int count = readLn(_inputBuffer, _inputBufferSize, 250); // 250ms, how many bytes at which baudrate?
        sodaq_wdt_reset();

        if (count <= 0) {
            continue;
        }

        debugPrint("<< ");
        debugPrintln(_inputBuffer);

        if (startsWith(STR_AT, _inputBuffer)) {
            continue; // skip echoed back command
        }

        if (startsWith(STR_RESPONSE_OK, _inputBuffer)) {
            return GSMResponseOK;
        }

        if (startsWith(STR_RESPONSE_ERROR, _inputBuffer) ||
                startsWith(STR_RESPONSE_CME_ERROR, _inputBuffer) ||
                startsWith(STR_RESPONSE_CMS_ERROR, _inputBuffer)) {
            return GSMResponseError;
        }

        bool hasPrefix = usePrefix && useOutBuffer && startsWith(prefix, _inputBuffer);

        if (!hasPrefix && checkURC(_inputBuffer)) {
            continue;
        }

        if (hasPrefix || (!usePrefix && useOutBuffer)) {
            if (outSize > 0 && outSize < outMaxSize - 1) {
                outBuffer[outSize++] = LF;
            }

            if (outSize < outMaxSize - 1) {
                char* inBuffer = _inputBuffer;
                if (hasPrefix) {
                    int i = strlen(prefix);
                    count -= i;
                    inBuffer += i;
                }
                if (outSize + count > outMaxSize - 1) {
                    count = outMaxSize - 1 - outSize;
                }
                memcpy(outBuffer + outSize, inBuffer, count);
                outSize += count;
                outBuffer[outSize] = 0;
            }
        }
    }

    debugPrintln("<< timed out");

    return GSMResponseTimeout;
}

void Sodaq_N3X::reboot()
{
    println("AT+CFUN=16");

    // wait up to 2000ms for the modem to come up
    uint32_t start = millis();

    while ((readResponse() != GSMResponseOK) && !is_timedout(start, 2000)) {}

    // wait for the reboot to start
    sodaq_wdt_safe_delay(REBOOT_DELAY);

    while (!is_timedout(start, REBOOT_TIMEOUT)) {
        if (getSimStatus() == SimReady) {
            break;
        }
    }

    // echo off again after reboot
    execCommand("ATE0");

    // extra read just to clear the input stream
    readResponse(NULL, 0, NULL, 250);
}

bool Sodaq_N3X::waitForSignalQuality(uint32_t timeout)
{
    uint32_t start = millis();
    const int8_t minRSSI = getMinRSSI();
    int8_t rssi;
    uint8_t ber;

    uint32_t delay_count = 500;

    while (!is_timedout(start, timeout)) {
        if (getRSSIAndBER(&rssi, &ber)) {
            if (rssi != 0 && rssi >= minRSSI) {
                _lastRSSI = rssi;
                _CSQtime = (int32_t)(millis() - start) / 1000;
                return true;
            }
        }

        sodaq_wdt_safe_delay(delay_count);

        // Next time wait a little longer, but not longer than 5 seconds
        if (delay_count < 5000) {
            delay_count += 1000;
        }
    }

    return false;
}


/******************************************************************************
* Utils
*****************************************************************************/

uint32_t Sodaq_N3X::convertDatetimeToEpoch(int y, int m, int d, int h, int min, int sec)
{
    struct tm tm;

    tm.tm_isdst = -1;
    tm.tm_yday  = 0;
    tm.tm_wday  = 0;
    tm.tm_year  = y + EPOCH_TIME_YEAR_OFF;
    tm.tm_mon   = m - 1;
    tm.tm_mday  = d;
    tm.tm_hour  = h;
    tm.tm_min   = min;
    tm.tm_sec   = sec;

    return mktime(&tm);
}

bool Sodaq_N3X::startsWith(const char* pre, const char* str)
{
    return (strncmp(pre, str, strlen(pre)) == 0);
}


/******************************************************************************
* Generic
*****************************************************************************/

// Returns true if the modem is on.
bool Sodaq_N3X::isOn() const
{
    if (_onoff) {
        return _onoff->isOn();
    }

    // No onoff. Let's assume it is on.
    return true;
}

void Sodaq_N3X::writeProlog()
{
    if (!_appendCommand) {
        debugPrint(">> ");
        _appendCommand = true;
    }
}

// Write a byte, as binary data
size_t Sodaq_N3X::writeByte(uint8_t value)
{
    return _modemStream->write(value);
}

size_t Sodaq_N3X::print(const String& buffer)
{
    writeProlog();
    debugPrint(buffer);

    return _modemStream->print(buffer);
}

size_t Sodaq_N3X::print(const char buffer[])
{
    writeProlog();
    debugPrint(buffer);

    return _modemStream->print(buffer);
}

size_t Sodaq_N3X::print(char value)
{
    writeProlog();
    debugPrint(value);

    return _modemStream->print(value);
};

size_t Sodaq_N3X::print(unsigned char value, int base)
{
    writeProlog();
    debugPrint(value, base);

    return _modemStream->print(value, base);
};

size_t Sodaq_N3X::print(int value, int base)
{
    writeProlog();
    debugPrint(value, base);

    return _modemStream->print(value, base);
};

size_t Sodaq_N3X::print(unsigned int value, int base)
{
    writeProlog();
    debugPrint(value, base);

    return _modemStream->print(value, base);
};

size_t Sodaq_N3X::print(long value, int base)
{
    writeProlog();
    debugPrint(value, base);

    return _modemStream->print(value, base);
};

size_t Sodaq_N3X::print(unsigned long value, int base)
{
    writeProlog();
    debugPrint(value, base);

    return _modemStream->print(value, base);
};

size_t Sodaq_N3X::println(const __FlashStringHelper *ifsh)
{
    return print(ifsh) + println();
}

size_t Sodaq_N3X::println(const String &s)
{
    return print(s) + println();
}

size_t Sodaq_N3X::println(const char c[])
{
    return print(c) + println();
}

size_t Sodaq_N3X::println(char c)
{
    return print(c) + println();
}

size_t Sodaq_N3X::println(unsigned char b, int base)
{
    return print(b, base) + println();
}

size_t Sodaq_N3X::println(int num, int base)
{
    return print(num, base) + println();
}

size_t Sodaq_N3X::println(unsigned int num, int base)
{
    return print(num, base) + println();
}

size_t Sodaq_N3X::println(long num, int base)
{
    return print(num, base) + println();
}

size_t Sodaq_N3X::println(unsigned long num, int base)
{
    return print(num, base) + println();
}

size_t Sodaq_N3X::println(double num, int digits)
{
    writeProlog();
    debugPrint(num, digits);

    return _modemStream->println(num, digits);
}

size_t Sodaq_N3X::println(const Printable& x)
{
    return print(x) + println();
}

size_t Sodaq_N3X::println()
{
    debugPrintln();
    size_t i = print(CR);
    _appendCommand = false;
    return i;
}

// Initializes the input buffer and makes sure it is only initialized once.
// Safe to call multiple times.
void Sodaq_N3X::initBuffer()
{
    debugPrintln("[initBuffer]");

    // make sure the buffers are only initialized once
    if (!_isBufferInitialized) {
        _inputBuffer = static_cast<char*>(malloc(_inputBufferSize));
        _isBufferInitialized = true;
    }
}

// Sets the modem stream.
void Sodaq_N3X::setModemStream(Stream& stream)
{
    _modemStream = &stream;
}

// Returns a character from the modem stream if read within _timeout ms or -1 otherwise.
int Sodaq_N3X::timedRead(uint32_t timeout) const
{
    uint32_t _startMillis = millis();

    do {
        int c = _modemStream->read();

        if (c >= 0) {
            return c;
        }
    } while (millis() - _startMillis < timeout);

    return -1; // -1 indicates timeout
}

// Fills the given "buffer" with characters read from the modem stream up to "length"
// maximum characters and until the "terminator" character is found or a character read
// times out (whichever happens first).
// The buffer does not contain the "terminator" character or a null terminator explicitly.
// Returns the number of characters written to the buffer, not including null terminator.
size_t Sodaq_N3X::readBytesUntil(char terminator, char* buffer, size_t length, uint32_t timeout)
{
    if (length < 1) {
        return 0;
    }

    size_t index = 0;

    while (index < length) {
        int c = timedRead(timeout);

        if (c < 0 || c == terminator) {
            break;
        }

        *buffer++ = static_cast<char>(c);
        index++;
    }
    if (index < length) {
        *buffer = '\0';
    }

    return index; // return number of characters, not including null terminator
}

// Fills the given "buffer" with up to "length" characters read from the modem stream.
// It stops when a character read times out or "length" characters have been read.
// Returns the number of characters written to the buffer.
size_t Sodaq_N3X::readBytes(uint8_t* buffer, size_t length, uint32_t timeout)
{
    size_t count = 0;

    while (count < length) {
        int c = timedRead(timeout);

        if (c < 0) {
            break;
        }

        *buffer++ = static_cast<uint8_t>(c);
        count++;
    }

    return count;
}

// Reads a line (up to the SODAQ_GSM_TERMINATOR) from the modem stream into the "buffer".
// The buffer is terminated with null.
// Returns the number of bytes read, not including the null terminator.
size_t Sodaq_N3X::readLn(char* buffer, size_t size, uint32_t timeout)
{
    // Use size-1 to leave room for a string terminator
    size_t len = readBytesUntil(SODAQ_GSM_TERMINATOR[SODAQ_GSM_TERMINATOR_LEN - 1], buffer, size - 1, timeout);

    // check if the terminator is more than 1 characters, then check if the first character of it exists
    // in the calculated position and terminate the string there
    if ((SODAQ_GSM_TERMINATOR_LEN > 1) && (buffer[len - (SODAQ_GSM_TERMINATOR_LEN - 1)] == SODAQ_GSM_TERMINATOR[0])) {
        len -= SODAQ_GSM_TERMINATOR_LEN - 1;
    }

    // terminate string, there should always be room for it (see size-1 above)
    buffer[len] = '\0';

    return len;
}


/******************************************************************************
 * OnOff
 *****************************************************************************/

Sodaq_SARA_N310_OnOff::Sodaq_SARA_N310_OnOff()
{
    #ifdef PIN_SARA_ENABLE
    // First write the output value, and only then set the output mode.
    digitalWrite(SARA_ENABLE, LOW);
    pinMode(SARA_ENABLE, OUTPUT);

    digitalWrite(SARA_TX_ENABLE, LOW);
    pinMode(SARA_TX_ENABLE, OUTPUT);
    #endif

    _onoff_status = false;
}

void Sodaq_SARA_N310_OnOff::on()
{
    #ifdef PIN_SARA_ENABLE
    digitalWrite(SARA_ENABLE, HIGH);
    digitalWrite(SARA_TX_ENABLE, HIGH);

    pinMode(SARA_R4XX_TOGGLE, OUTPUT);
    digitalWrite(SARA_R4XX_TOGGLE, LOW);
    // We should be able to reduce this to 50ms or something
    sodaq_wdt_safe_delay(1000);
    pinMode(SARA_R4XX_TOGGLE, INPUT);

    _onoff_status = true;
    #endif
}

void Sodaq_SARA_N310_OnOff::off()
{
    #ifdef PIN_SARA_ENABLE
    digitalWrite(SARA_ENABLE, LOW);
    digitalWrite(SARA_TX_ENABLE, LOW);

    // Should be instant
    // Let's wait a little, but not too long
    delay(50);
    _onoff_status = false;
    #endif
}

bool Sodaq_SARA_N310_OnOff::isOn()
{
    return _onoff_status;
}
