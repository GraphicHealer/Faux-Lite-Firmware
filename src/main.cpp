#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <vector>

// UART Configuration
#define RX_PIN 21
#define TX_PIN 18
// Use Serial1 (UART1) for ESP32-S2
#define ProliteSerial Serial1

// Preferences storage
Preferences preferences;

// Default settings
String signID = "01";
int baudRate = 9600;
bool apOnlyMode = false;
bool startBlank = false;
bool runMessagesOnBoot = false;

// Software-timed queue system for looping messages
struct QueueMessage
{
    String command; // Full Pro-Lite command (e.g., <CG><FQ>Hello World)
    int duration;   // Duration in seconds to display this message
};

std::vector<QueueMessage> messageQueue; // Unlimited messages
bool queueActive = false;
int currentQueueIndex = 0;
unsigned long lastQueueTime = 0;

// Web server
AsyncWebServer server(80);

// Function prototypes
void setupWiFi();
void setupWebServer();
void sendToProlite(String command);
void displayMessage(String text, String color = "G", String function = "A");
void displayBootMessage(String text, String color = "G", String function = "S", int times = 1);
void blankDisplay();
void restoreLastDisplay();
String getChipID();
void handleOTAUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final);
void startQueue();
void stopQueue();
void saveQueueToPreferences();
void loadQueueFromPreferences();

void setup()
{
    Serial.begin(115200);
    Serial.println("\n\nFaux-Lite Starting...");

    // Load preferences
    preferences.begin("faux-lite", false);
    signID = preferences.getString("signID", "01");
    baudRate = preferences.getInt("baudRate", 9600);
    apOnlyMode = preferences.getBool("apOnlyMode", false);
    startBlank = preferences.getBool("startBlank", false);
    runMessagesOnBoot = preferences.getBool("runOnBoot", false);

    // Load queue from preferences
    loadQueueFromPreferences();

    // Clear any corrupted lastDisplay command from previous firmware versions
    // This is a one-time cleanup - remove this after first boot if needed
    preferences.putString("lastDisplay", "");

    preferences.end();

    // Initialize UART to Pro-Lite display
    ProliteSerial.begin(baudRate, SERIAL_8N1, RX_PIN, TX_PIN);
    Serial.printf("Pro-Lite UART initialized: %d baud, RX=%d, TX=%d\n", baudRate, RX_PIN, TX_PIN);

    // Wait 10 seconds for Pro-Lite's internal boot sequence to complete
    Serial.println("Waiting for Pro-Lite display boot sequence...");
    delay(10000);

    // Blank the display on boot
    Serial.println("Blanking display...");
    blankDisplay();
    delay(500);

    // Show boot message on temporary page (Page Z) that will auto-return to Page A
    displayBootMessage("Faux-Lite Booting...", "H", "S", 3);
    delay(3000); // Wait 3 seconds for message to display

    // Setup WiFi with captive portal
    setupWiFi();

    // Setup mDNS for network discovery
    String mdnsName = "faux-lite-" + getChipID();
    if (MDNS.begin(mdnsName.c_str()))
    {
        MDNS.addService("http", "tcp", 80);
        MDNS.addServiceTxt("http", "tcp", "type", "faux-lite");
        MDNS.addServiceTxt("http", "tcp", "version", "1.0");
        Serial.printf("mDNS responder started: %s.local\n", mdnsName.c_str());
    }

    // Setup web server
    setupWebServer();

    Serial.println("Faux-Lite Ready!");
    Serial.printf("IP Address: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("mDNS Name: %s.local\n", mdnsName.c_str());

    // Display IP address on temporary page, show ~15 times (about 15 seconds) then return to Page A
    String ipMsg = "IP: " + WiFi.localIP().toString();
    displayBootMessage(ipMsg, "L", "S", 15);
    delay(15000); // Wait 15 seconds for IP message to display

    // Handle boot display options
    if (startBlank)
    {
        Serial.println("Start Blank enabled - blanking display");
        blankDisplay();
    }
    else if (runMessagesOnBoot && messageQueue.size() > 0)
    {
        Serial.println("Running message queue on boot");
        startQueue();
    }
    else
    {
        // Restore previous display state
        restoreLastDisplay();
    }
}

void loop()
{
    // Handle software-timed queue
    if (queueActive && messageQueue.size() > 0)
    {
        unsigned long currentTime = millis();
        unsigned long duration = messageQueue[currentQueueIndex].duration * 1000; // Convert seconds to milliseconds

        if (currentTime - lastQueueTime >= duration)
        {
            // Move to next message
            currentQueueIndex = (currentQueueIndex + 1) % messageQueue.size();
            sendToProlite("<PA>" + messageQueue[currentQueueIndex].command);
            lastQueueTime = currentTime;
            Serial.printf("Queue: Displaying message %d/%d\n", currentQueueIndex + 1, messageQueue.size());
        }
    }
    delay(10);
}

void setupWiFi()
{
    WiFiManager wm;

    // Generate AP name with last 4 of MAC
    String apName = "Faux-Lite_" + getChipID();

    Serial.printf("Starting WiFi Manager with AP: %s\n", apName.c_str());

    // Check if AP-only mode is enabled
    if (apOnlyMode)
    {
        Serial.println("AP-Only Mode enabled");
        displayBootMessage("AP Mode: " + apName, "H", "S", 5);
        delay(5000); // Wait 5 seconds for AP mode message to display

        // Start in AP mode only
        WiFi.mode(WIFI_AP);
        WiFi.softAP(apName.c_str());
        Serial.printf("AP Started: %s\n", apName.c_str());
        Serial.printf("AP IP: %s\n", WiFi.softAPIP().toString().c_str());

        String apIpMsg = "AP IP: " + WiFi.softAPIP().toString();
        displayBootMessage(apIpMsg, "L", "S", 10);
        delay(10000); // Wait 10 seconds for AP IP message to display
        return;
    }

    // Show connecting message
    displayBootMessage("Connecting WiFi...", "H", "S", 5);
    delay(5000); // Wait 5 seconds for connecting message to display

    // Set custom HTML for captive portal
    const char *customHTML = "<br/><div style='text-align:center;'>"
                             "<h3>Faux-Lite Options</h3>"
                             "<p><a href='/'>Open Main Page</a></p>"
                             "<p><a href='/aponly'>Enable AP-Only Mode</a></p>"
                             "<p style='font-size:12px;color:#888;'>AP-Only Mode disables WiFi client and runs as Access Point only</p>"
                             "</div>";
    wm.setCustomHeadElement(customHTML);
    wm.setConfigPortalTimeout(180);

    // Callback when entering AP mode
    wm.setAPCallback([&apName](WiFiManager *myWiFiManager)
                     {
        Serial.println("Entered AP mode");
        String apMsg = "Connect to AP: " + apName;
        displayBootMessage(apMsg, "C", "S", 10); });

    // Try to connect, if fails start AP
    if (!wm.autoConnect(apName.c_str()))
    {
        Serial.println("Failed to connect and hit timeout");
        displayBootMessage("WiFi Timeout!", "C", "S", 5);
        delay(3000);
        ESP.restart();
    }

    Serial.println("WiFi connected!");
    Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());

    // Show WiFi connected message
    String ssid = WiFi.SSID();
    displayBootMessage("WiFi: " + ssid, "L", "S", 5);
    delay(5000); // Wait 5 seconds for WiFi connected message to display
}

void setupWebServer()
{
    // Handle AP-only mode activation from captive portal
    server.on("/aponly", HTTP_GET, [](AsyncWebServerRequest *request)
              {
        String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>AP-Only Mode</title></head>";
        html += "<body style='font-family:Arial;text-align:center;padding:50px;'>";
        html += "<h1>AP-Only Mode Enabled</h1>";
        html += "<p>The device will restart in AP-Only mode.</p>";
        html += "<p>You can change this in Settings later.</p>";
        html += "<p>Restarting in 3 seconds...</p>";
        html += "</body></html>";
        request->send(200, "text/html", html);

        // Save AP-only mode preference
        preferences.begin("faux-lite", false);
        preferences.putBool("apOnlyMode", true);
        preferences.end();

        // Restart after a short delay
        delay(3000);
        ESP.restart(); });

    // Serve main page
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
              {
        String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Faux-Lite Controller</title>
    <style>
        body {
            font-family: Arial, Helvetica, sans-serif;
            background: #FFFF00;
            color: #000000;
            margin: 0;
            padding: 0;
        }
        .navbar {
            background: #000000;
            color: #FFFF00;
            padding: 10px;
            border: 3px solid #000000;
            display: flex;
            align-items: center;
        }
        .navbar img {
            height: 50px;
            margin-right: 15px;
        }
        .navbar h1 {
            margin: 0;
            font-size: 24px;
            color: #FFFF00;
        }
        .container {
            max-width: 1200px;
            margin: 20px auto;
            padding: 0 10px;
        }
        table {
            width: 100%;
            border-collapse: collapse;
        }
        td {
            vertical-align: top;
            width: 50%;
            padding: 10px;
        }
        .panel {
            background: #FFFFFF;
            padding: 15px;
            border: 3px solid #000000;
            margin-bottom: 20px;
        }
        .panel h2 {
            color: #000000;
            margin: 0 0 15px 0;
            padding-bottom: 5px;
            border-bottom: 2px solid #000000;
            font-size: 18px;
        }
        .form-group {
            margin-bottom: 10px;
        }
        label {
            display: block;
            margin-bottom: 3px;
            font-weight: bold;
        }
        input, select, textarea {
            width: 100%;
            padding: 5px;
            border: 2px solid #000000;
            background: #FFFFFF;
            font-family: Arial, sans-serif;
            font-size: 14px;
        }
        input[type="checkbox"] {
            width: auto;
            display: inline;
            margin-right: 5px;
        }
        button {
            background: #000000;
            color: #FFFF00;
            padding: 8px 20px;
            border: 2px solid #000000;
            cursor: pointer;
            font-size: 14px;
            font-weight: bold;
            width: 100%;
            margin-top: 10px;
        }
        button:hover {
            background: #333333;
        }
        .cheat-sheet {
            font-size: 12px;
            line-height: 1.4;
        }
        .cheat-sheet h3 {
            margin: 15px 0 5px 0;
            font-size: 14px;
        }
        .cheat-sheet pre {
            background: #EEEEEE;
            padding: 8px;
            border: 1px solid #000000;
            overflow-x: auto;
            margin: 5px 0;
            font-family: 'Courier New', monospace;
        }
        .status {
            padding: 8px;
            margin-top: 10px;
            display: none;
            border: 2px solid #000000;
        }
        .status.success {
            background: #00FF00;
            color: #000000;
            display: block;
        }
        .status.error {
            background: #FF0000;
            color: #FFFFFF;
            display: block;
        }
        .settings-link {
            text-align: center;
            margin-top: 15px;
        }
        .settings-link a {
            color: #0000FF;
            text-decoration: underline;
            font-weight: bold;
        }
        hr {
            border: 1px solid #000000;
            margin: 20px 0;
        }
        @media (max-width: 768px) {
            table, tr, td {
                display: block;
                width: 100%;
            }
            td {
                padding: 0;
            }
        }
    </style>
</head>
<body>
    <div class="navbar">
        <img src="data:image/png;base64,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" alt="Icon" style="width: 86px; height: 40px; margin-right: 10px;">
        <h1>Faux-Lite Controller</h1>
    </div>

    <div class="container">
        <table>
            <tr>
                <td>
                    <div class="panel">
                        <h2>Send Message</h2>
                        <form id="messageForm">
                            <div class="form-group">
                                <label>Page (A-Z)</label>
                                <select id="page" name="page">
)rawliteral";

        // Add page options A-Z
        for (char c = 'A'; c <= 'Z'; c++) {
            html += "<option value=\"" + String(c) + "\">" + String(c) + "</option>";
        }

        html += R"rawliteral(
                    </select>
                </div>
                <div class="form-group">
                    <label>Text</label>
                    <textarea id="text" name="text" rows="3" placeholder="Enter your message..."></textarea>
                </div>
                <div class="form-group">
                    <label>Color (Optional)</label>
                    <select id="color" name="color">
                        <option value="">None</option>
                        <option value="CA">Dim Red</option>
                        <option value="CB">Red</option>
                        <option value="CC">Bright Red</option>
                        <option value="CD">Orange</option>
                        <option value="CE">Bright Orange</option>
                        <option value="CF">Light Yellow</option>
                        <option value="CG">Yellow</option>
                        <option value="CH">Bright Yellow</option>
                        <option value="CI">Lime</option>
                        <option value="CJ">Dim Lime</option>
                        <option value="CK">Bright Lime</option>
                        <option value="CL">Bright Green</option>
                        <option value="CM">Green</option>
                        <option value="CN">Dim Green</option>
                        <option value="CO">Yellow/Green/Red</option>
                        <option value="CP">Rainbow</option>
                        <option value="CQ">Red/Green 3D</option>
                        <option value="CR">Red/Yellow 3D</option>
                        <option value="CS">Green/Red 3D</option>
                        <option value="CT">Green/Yellow 3D</option>
                        <option value="CU">Green on Red</option>
                        <option value="CV">Red on Green</option>
                        <option value="CW">Orange on Green 3D</option>
                        <option value="CX">Lime on Red 3D</option>
                        <option value="CY">Green on Red 3D</option>
                        <option value="CZ">Red on Green 3D</option>
                    </select>
                </div>
                <div class="form-group">
                    <label>Font (Optional)</label>
                    <select id="font" name="font">
                        <option value="">None</option>
                        <option value="SA">Normal</option>
                        <option value="SB">Bold</option>
                        <option value="SC">Italic</option>
                        <option value="SD">Bold Italic</option>
                        <option value="SE">Flash Normal</option>
                        <option value="SF">Flash Bold</option>
                        <option value="SG">Flash Italic</option>
                        <option value="SH">Flash Bold Italic</option>
                    </select>
                </div>
                <div class="form-group">
                    <label>Animation (Optional)</label>
                    <select id="function" name="function">
                        <option value="">None</option>
                        <option value="FA">Auto</option>
                        <option value="FB">Open</option>
                        <option value="FC">Cover</option>
                        <option value="FD">Date</option>
                        <option value="FE">Cycling</option>
                        <option value="FF">Close Right</option>
                        <option value="FG">Close Left</option>
                        <option value="FH">Close Both</option>
                        <option value="FI">Scroll Up</option>
                        <option value="FJ">Scroll Down</option>
                        <option value="FK">Overlap</option>
                        <option value="FL">Stacking</option>
                        <option value="FM">Comic 1</option>
                        <option value="FN">Comic 2</option>
                        <option value="FO">Beep</option>
                        <option value="FP">Pause</option>
                        <option value="FQ">Appear</option>
                        <option value="FR">Random</option>
                        <option value="FS">Shift Left</option>
                        <option value="FT">Time</option>
                        <option value="FU">Magic</option>
                        <option value="FV">Thank You</option>
                        <option value="FW">Welcome</option>
                        <option value="FZ">Link Page</option>
                        <option value="F1">Target</option>
                        <option value="F2">Current</option>
                        <option value="F3">Day Left</option>
                        <option value="F4">Hour Left</option>
                        <option value="F5">Minute Left</option>
                        <option value="F6">Second Left</option>
                    </select>
                </div>
                <div class="form-group">
                    <label>
                        <input type="checkbox" id="switchToPage" name="switchToPage" checked>Switch to Page
                    </label>
                </div>
                <button type="submit">Send Message</button>
                <div id="messageStatus" class="status"></div>
            </form>

            <hr>
            <h2>Send Raw Command</h2>
            <form id="rawForm">
                <div class="form-group">
                    <label>Raw Protocol Command</label>
                    <textarea id="rawCommand" name="rawCommand" rows="2" placeholder="e.g., <PA>Hello World"></textarea>
                </div>
                <button type="submit">Send Raw Command</button>
                <div id="rawStatus" class="status"></div>
            </form>

            <div class="settings-link">
                <a href="/messages">Message List</a> |
                <a href="/settings">Settings & OTA Update</a>
            </div>
        </div>
    </td>
    <td>
        <div class="panel">
            <h2>Pro-Lite Protocol Guide</h2>
            <div class="cheat-sheet">
                <h3>Basic Structure</h3>
                <p>All commands follow this format:</p>
                <pre>&lt;ID##&gt;&lt;Command&gt;</pre>
                <p>Where <code>##</code> is the sign ID (default: 01)</p>

                <h3>Page Selection</h3>
                <pre>&lt;PA&gt; to &lt;PZ&gt; - Select page A through Z</pre>

                <h3>Colors</h3>
                <pre>&lt;CA&gt; Dim Red       &lt;CB&gt; Red           &lt;CC&gt; Bright Red
&lt;CD&gt; Orange        &lt;CE&gt; Bright Orange &lt;CF&gt; Light Yellow
&lt;CG&gt; Yellow        &lt;CH&gt; Bright Yellow &lt;CI&gt; Lime
&lt;CJ&gt; Dim Lime      &lt;CK&gt; Bright Lime   &lt;CL&gt; Bright Green
&lt;CM&gt; Green         &lt;CN&gt; Dim Green     &lt;CO&gt; Yel/Grn/Red
&lt;CP&gt; Rainbow       &lt;CQ&gt; Red/Grn 3D    &lt;CR&gt; Red/Yel 3D
&lt;CS&gt; Grn/Red 3D    &lt;CT&gt; Grn/Yel 3D    &lt;CU&gt; Grn on Red
&lt;CV&gt; Red on Grn    &lt;CW&gt; Org on Grn 3D &lt;CX&gt; Lime on Red 3D
&lt;CY&gt; Grn on Red 3D &lt;CZ&gt; Red on Grn 3D</pre>

                <h3>Fonts</h3>
                <pre>&lt;SA&gt; Normal            &lt;SB&gt; Bold
&lt;SC&gt; Italic            &lt;SD&gt; Bold Italic
&lt;SE&gt; Flash Normal      &lt;SF&gt; Flash Bold
&lt;SG&gt; Flash Italic      &lt;SH&gt; Flash Bold Italic</pre>

                <h3>Character Size</h3>
                <pre>&lt;SI&gt; 5x7 (default)  &lt;SJ&gt; 4x7</pre>

                <h3>Display Functions</h3>
                <pre>&lt;FA&gt; Auto           &lt;FB&gt; Open          &lt;FC&gt; Cover
&lt;FD&gt; Date           &lt;FE&gt; Cycling       &lt;FF&gt; Close Right
&lt;FG&gt; Close Left     &lt;FH&gt; Close Both    &lt;FI&gt; Scroll Up
&lt;FJ&gt; Scroll Down    &lt;FK&gt; Overlap       &lt;FL&gt; Stacking
&lt;FM&gt; Comic 1        &lt;FN&gt; Comic 2       &lt;FO&gt; Beep
&lt;FP&gt; Pause          &lt;FQ&gt; Appear        &lt;FR&gt; Random
&lt;FS&gt; Shift Left     &lt;FT&gt; Time          &lt;FU&gt; Magic
&lt;FV&gt; Thank You      &lt;FW&gt; Welcome       &lt;FZ&gt; Link Page
&lt;F1&gt; Target         &lt;F2&gt; Current       &lt;F3&gt; Day Left
&lt;F4&gt; Hour Left      &lt;F5&gt; Minute Left   &lt;F6&gt; Second Left</pre>

                <h3>Page Management</h3>
                <pre>&lt;DP*&gt; Delete all pages
&lt;DPA&gt; Delete page A
&lt;RPA&gt; Run page A
&lt;D*&gt;  Delete all (pages + graphics)</pre>

                <h3>Examples</h3>
                <pre>&lt;ID01&gt;&lt;PA&gt;&lt;CA&gt;&lt;AB&gt;HELLO
  └─ Sign 01, Page A, Red, Bold, "HELLO"

&lt;ID01&gt;&lt;PB&gt;&lt;FI&gt;&lt;CG&gt;Scrolling Text
  └─ Sign 01, Page B, Scroll Up, Yellow

&lt;ID01&gt;&lt;PA&gt;&lt;CA&gt;RED &lt;CG&gt;YELLOW &lt;CM&gt;GREEN
  └─ Multi-color message</pre>

                <h3>Special Commands</h3>
                <pre>&lt;ST&gt;  Set time (sync with system)
&lt;RST&gt; Reset to factory defaults
&lt;?&gt;   Request sign info</pre>
            </div>
        </div>
    </div>

    <script>
        document.getElementById('messageForm').addEventListener('submit', async (e) => {
            e.preventDefault();
            const status = document.getElementById('messageStatus');

            const page = document.getElementById('page').value;
            const text = document.getElementById('text').value;
            const color = document.getElementById('color').value;
            const font = document.getElementById('font').value;
            const func = document.getElementById('function').value;
            const switchToPage = document.getElementById('switchToPage').checked;

            if (!text) {
                status.className = 'status error';
                status.textContent = 'Please enter text';
                return;
            }

            let command = '<P' + page + '>';
            if (func) command += '<' + func + '>';
            if (color) command += '<' + color + '>';
            if (font) command += '<' + font + '>';
            command += text + ' ';

            try {
                const response = await fetch('/api/send', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ command: command })
                });

                if (response.ok) {
                    // If switchToPage is checked, send run page command
                    if (switchToPage) {
                        await fetch('/api/send', {
                            method: 'POST',
                            headers: { 'Content-Type': 'application/json' },
                            body: JSON.stringify({ command: '<RP' + page + '>' })
                        });
                    }
                    status.className = 'status success';
                    status.textContent = 'Message sent successfully!';
                } else {
                    status.className = 'status error';
                    status.textContent = 'Failed to send message';
                }
            } catch (err) {
                status.className = 'status error';
                status.textContent = 'Error: ' + err.message;
            }
        });

        document.getElementById('rawForm').addEventListener('submit', async (e) => {
            e.preventDefault();
            const status = document.getElementById('rawStatus');
            const command = document.getElementById('rawCommand').value;

            if (!command) {
                status.className = 'status error';
                status.textContent = 'Please enter a command';
                return;
            }

            try {
                const response = await fetch('/api/send', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ command: command })
                });

                if (response.ok) {
                    status.className = 'status success';
                    status.textContent = 'Command sent successfully!';
                } else {
                    status.className = 'status error';
                    status.textContent = 'Failed to send command';
                }
            } catch (err) {
                status.className = 'status error';
                status.textContent = 'Error: ' + err.message;
            }
        });
    </script>
</body>
</html>
)rawliteral";

        request->send(200, "text/html", html); });

    // Message List page
    server.on("/messages", HTTP_GET, [](AsyncWebServerRequest *request)
              {
        String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Faux-Lite Message List</title>
    <style>
        body {
            font-family: Arial, Helvetica, sans-serif;
            background: #FFFF00;
            color: #000000;
            margin: 0;
            padding: 0;
        }
        .navbar {
            background: #000000;
            color: #FFFF00;
            padding: 10px;
            border: 3px solid #000000;
            display: flex;
            align-items: center;
        }
        .navbar h1 {
            margin: 0;
            font-size: 24px;
            color: #FFFF00;
        }
        .container {
            max-width: 800px;
            margin: 20px auto;
            padding: 0 10px;
        }
        .panel {
            background: #FFFFFF;
            padding: 15px;
            border: 3px solid #000000;
            margin-bottom: 20px;
        }
        .panel h2 {
            color: #000000;
            margin: 0 0 15px 0;
            padding-bottom: 5px;
            border-bottom: 2px solid #000000;
            font-size: 18px;
        }
        .form-group {
            margin-bottom: 10px;
        }
        label {
            display: block;
            margin-bottom: 3px;
            font-weight: bold;
        }
        textarea {
            width: 100%;
            padding: 5px;
            border: 2px solid #000000;
            background: #FFFFFF;
            font-family: 'Courier New', monospace;
            font-size: 12px;
            min-height: 200px;
        }
        button {
            background: #000000;
            color: #FFFF00;
            padding: 8px 20px;
            border: 2px solid #000000;
            cursor: pointer;
            font-size: 14px;
            font-weight: bold;
            width: 100%;
            margin-top: 10px;
        }
        button:hover {
            background: #333333;
        }
        button.danger {
            background: #FF0000;
            color: #FFFFFF;
        }
        button.danger:hover {
            background: #CC0000;
        }
        .status {
            padding: 8px;
            margin-top: 10px;
            display: none;
            border: 2px solid #000000;
        }
        .status.success {
            background: #00FF00;
            color: #000000;
            display: block;
        }
        .status.error {
            background: #FF0000;
            color: #FFFFFF;
            display: block;
        }
        .back-link {
            text-align: center;
            margin-top: 15px;
        }
        .back-link a {
            color: #0000FF;
            text-decoration: underline;
            font-weight: bold;
        }
        .info {
            background: #EEEEEE;
            padding: 8px;
            border: 1px solid #000000;
            margin-bottom: 10px;
            font-size: 12px;
        }
        .loop-controls {
            display: flex;
            gap: 10px;
            margin-top: 10px;
        }
        .loop-controls button {
            flex: 1;
        }
    </style>
</head>
<body>
    <div class="navbar">
        <h1>Faux-Lite Message List</h1>
    </div>

    <div class="container">
        <div class="panel">
            <h2>Message Queue Configuration</h2>
            <div class="info">
                <strong>Software-Timed Queue System:</strong> Messages loop continuously with configurable duration.<br>
                Add unlimited messages to the queue - each will display for the specified duration.<br>
                <strong>Duration:</strong> Time in seconds to display each message before switching to the next.
            </div>
            <form id="messageForm">
                <div id="queueContainer"></div>
                <button type="button" onclick="addMessage()" style="margin-bottom: 10px;">+ Add Message</button>
                <div class="form-group">
                    <label>
                        <input type="checkbox" id="runOnBoot" name="runOnBoot" style="width: auto;">
                        Run message queue on boot
                    </label>
                </div>
                <button type="submit">Save Queue</button>
                <div id="saveStatus" class="status"></div>
            </form>
        </div>

        <div class="panel">
            <h2>Queue Control</h2>
            <div class="info">
                Current Status: <strong id="loopStatus">Loading...</strong><br>
                Messages in queue: <strong id="messageCount">0</strong><br>
                <small>Queue uses software timing for infinite looping</small>
            </div>
            <div class="loop-controls">
                <button onclick="startQueue()">Start Queue</button>
                <button onclick="stopQueue()" class="danger">Stop Queue</button>
            </div>
            <div id="loopControlStatus" class="status"></div>
        </div>

        <div class="back-link">
            <a href="/">← Back to Main</a>
        </div>
    </div>

    <script>
        let queueMessages = [];

        // Load current settings
        fetch('/api/messages')
            .then(r => r.json())
            .then(data => {
                queueMessages = data.messages || [];
                document.getElementById('runOnBoot').checked = data.runOnBoot;
                document.getElementById('messageCount').textContent = data.queueSize || 0;
                document.getElementById('loopStatus').textContent = data.queueActive ? 'Running' : 'Stopped';
                renderQueue();
            });

        function renderQueue() {
            const container = document.getElementById('queueContainer');
            container.innerHTML = '';

            queueMessages.forEach((msg, index) => {
                const div = document.createElement('div');
                div.style.cssText = 'border: 2px solid #000; padding: 10px; margin-bottom: 10px; background: #f9f9f9;';
                div.innerHTML = `
                    <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 5px;">
                        <strong>Message ${index + 1}</strong>
                        <div>
                            <button type="button" onclick="testMessage(${index})" style="width: auto; padding: 5px 10px; background: #0066cc; margin-right: 5px;">Test</button>
                            <button type="button" onclick="removeMessage(${index})" style="width: auto; padding: 5px 10px; background: #ff0000;">Remove</button>
                        </div>
                    </div>
                    <label style="font-size: 12px;">Command:</label>
                    <input type="text" value="${msg.command || ''}" onchange="updateCommand(${index}, this.value)"
                           style="width: 100%; margin-bottom: 5px;" placeholder="<CG><FQ>Hello World">
                    <label style="font-size: 12px;">Duration (seconds):</label>
                    <input type="number" value="${msg.duration || 5}" min="1"
                           onchange="updateDuration(${index}, this.value)" style="width: 100px;">
                `;
                container.appendChild(div);
            });
        }

        function addMessage() {
            queueMessages.push({ command: '', duration: 5 });
            renderQueue();
        }

        function removeMessage(index) {
            queueMessages.splice(index, 1);
            renderQueue();
        }

        function updateCommand(index, value) {
            queueMessages[index].command = value;
        }

        function updateDuration(index, value) {
            queueMessages[index].duration = parseInt(value) || 5;
        }

        async function testMessage(index) {
            const msg = queueMessages[index];
            if (!msg.command) {
                alert('Please enter a command first');
                return;
            }

            try {
                const response = await fetch('/api/send', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ command: '<PA>' + msg.command })
                });

                if (response.ok) {
                    console.log('Test message sent successfully');
                } else {
                    alert('Failed to send test message');
                }
            } catch (err) {
                alert('Error: ' + err.message);
            }
        }

        document.getElementById('messageForm').addEventListener('submit', async (e) => {
            e.preventDefault();
            const status = document.getElementById('saveStatus');
            const runOnBoot = document.getElementById('runOnBoot').checked;

            try {
                const response = await fetch('/api/messages', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ messages: queueMessages, runOnBoot })
                });

                if (response.ok) {
                    status.className = 'status success';
                    status.textContent = 'Queue saved!';
                    document.getElementById('messageCount').textContent = queueMessages.length;
                } else {
                    status.className = 'status error';
                    status.textContent = 'Failed to save queue';
                }
            } catch (err) {
                status.className = 'status error';
                status.textContent = 'Error: ' + err.message;
            }
        });

        async function startQueue() {
            const status = document.getElementById('loopControlStatus');
            try {
                const response = await fetch('/api/loop/start', { method: 'POST' });
                if (response.ok) {
                    status.className = 'status success';
                    status.textContent = 'Queue started!';
                    document.getElementById('loopStatus').textContent = 'Running';
                } else {
                    status.className = 'status error';
                    status.textContent = 'Failed to start queue';
                }
            } catch (err) {
                status.className = 'status error';
                status.textContent = 'Error: ' + err.message;
            }
        }

        async function stopQueue() {
            const status = document.getElementById('loopControlStatus');
            try {
                const response = await fetch('/api/loop/stop', { method: 'POST' });
                if (response.ok) {
                    status.className = 'status success';
                    status.textContent = 'Queue stopped!';
                    document.getElementById('loopStatus').textContent = 'Stopped';
                } else {
                    status.className = 'status error';
                    status.textContent = 'Failed to stop queue';
                }
            } catch (err) {
                status.className = 'status error';
                status.textContent = 'Error: ' + err.message;
            }
        }
    </script>
</body>
</html>
)rawliteral";

        request->send(200, "text/html", html); });

    // Settings page
    server.on("/settings", HTTP_GET, [](AsyncWebServerRequest *request)
              {
        String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Faux-Lite Settings</title>
    <style>
        body {
            font-family: Arial, Helvetica, sans-serif;
            background: #FFFF00;
            color: #000000;
            margin: 0;
            padding: 0;
        }
        .navbar {
            background: #000000;
            color: #FFFF00;
            padding: 10px;
            border: 3px solid #000000;
            display: flex;
            align-items: center;
        }
        .navbar img {
            height: 50px;
            margin-right: 15px;
        }
        .navbar h1 {
            margin: 0;
            font-size: 24px;
            color: #FFFF00;
        }
        .container {
            max-width: 600px;
            margin: 20px auto;
            padding: 0 10px;
        }
        .panel {
            background: #FFFFFF;
            padding: 15px;
            border: 3px solid #000000;
            margin-bottom: 20px;
        }
        .panel h2 {
            color: #000000;
            margin: 0 0 15px 0;
            padding-bottom: 5px;
            border-bottom: 2px solid #000000;
            font-size: 18px;
        }
        .form-group {
            margin-bottom: 10px;
        }
        label {
            display: block;
            margin-bottom: 3px;
            font-weight: bold;
        }
        input, select {
            width: 100%;
            padding: 5px;
            border: 2px solid #000000;
            background: #FFFFFF;
            font-family: Arial, sans-serif;
            font-size: 14px;
        }
        button {
            background: #000000;
            color: #FFFF00;
            padding: 8px 20px;
            border: 2px solid #000000;
            cursor: pointer;
            font-size: 14px;
            font-weight: bold;
            width: 100%;
            margin-top: 10px;
        }
        button:hover {
            background: #333333;
        }
        button.danger {
            background: #FF0000;
            color: #FFFFFF;
        }
        button.danger:hover {
            background: #CC0000;
        }
        .status {
            padding: 8px;
            margin-top: 10px;
            display: none;
            border: 2px solid #000000;
        }
        .status.success {
            background: #00FF00;
            color: #000000;
            display: block;
        }
        .status.error {
            background: #FF0000;
            color: #FFFFFF;
            display: block;
        }
        .back-link {
            text-align: center;
            margin-top: 15px;
        }
        .back-link a {
            color: #0000FF;
            text-decoration: underline;
            font-weight: bold;
        }
        .info {
            background: #EEEEEE;
            padding: 8px;
            border: 1px solid #000000;
            margin-bottom: 10px;
            font-size: 12px;
        }
    </style>
</head>
<body>
    <div class="navbar">
        <img src="data:image/png;base64,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" alt="Icon" style="width: 86px; height: 40px; margin-right: 10px;">
        <h1>Faux-Lite Settings</h1>
    </div>

    <div class="container">
        <div class="panel">
            <h2>Pro-Lite Configuration</h2>
            <form id="settingsForm">
                <div class="form-group">
                    <label>Sign ID (01-99)</label>
                    <input type="text" id="signID" name="signID" value=")rawliteral" + signID + R"rawliteral(" maxlength="2" pattern="[0-9]{2}">
                </div>
                <div class="form-group">
                    <label>Baud Rate</label>
                    <select id="baudRate" name="baudRate">
                        <option value="2400">2400</option>
                        <option value="4800">4800</option>
                        <option value="9600">9600</option>
                        <option value="19200">19200</option>
                    </select>
                </div>
                <div class="form-group">
                    <label>
                        <input type="checkbox" id="startBlank" name="startBlank" style="width: auto;">
                        Start Blank (display blank on boot)
                    </label>
                </div>
                <div class="form-group">
                    <label>
                        <input type="checkbox" id="apOnlyMode" name="apOnlyMode" style="width: auto;">
                        AP-Only Mode (no WiFi connection)
                    </label>
                </div>
                <button type="submit">Save Settings</button>
                <div id="settingsStatus" class="status"></div>
            </form>
        </div>

        <div class="panel">
            <h2>Firmware Update (OTA)</h2>
            <div class="info">
                Upload a new firmware binary (.bin file) to update the device.
                The device will restart after a successful update.
            </div>
            <form id="otaForm" enctype="multipart/form-data">
                <div class="form-group">
                    <label>Select Firmware File</label>
                    <input type="file" id="firmware" name="firmware" accept=".bin">
                </div>
                <button type="submit">Upload Firmware</button>
                <div id="otaStatus" class="status"></div>
                <div id="otaProgress" style="display:none; margin-top: 10px;">
                    <div style="background: #1a1a1a; border-radius: 5px; overflow: hidden;">
                        <div id="progressBar" style="background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); height: 30px; width: 0%; transition: width 0.3s; text-align: center; line-height: 30px; color: white; font-weight: 600;"></div>
                    </div>
                </div>
            </form>
        </div>

        <div class="panel">
            <h2>WiFi Configuration</h2>
            <div class="info">
                Current IP: <strong id="ipAddress">Loading...</strong>
            </div>
            <button onclick="resetWiFi()">Reset WiFi Settings</button>
            <div id="wifiStatus" class="status"></div>
        </div>

        <div class="back-link">
            <a href="/">← Back to Main</a>
        </div>
    </div>

    <script>
        // Load current settings
        document.getElementById('baudRate').value = ')rawliteral" + String(baudRate) + R"rawliteral(';
        document.getElementById('startBlank').checked = )rawliteral" + String(startBlank ? "true" : "false") + R"rawliteral(;
        document.getElementById('apOnlyMode').checked = )rawliteral" + String(apOnlyMode ? "true" : "false") + R"rawliteral(;

        // Get IP address
        fetch('/api/info')
            .then(r => r.json())
            .then(data => {
                document.getElementById('ipAddress').textContent = data.ip;
            });

        document.getElementById('settingsForm').addEventListener('submit', async (e) => {
            e.preventDefault();
            const status = document.getElementById('settingsStatus');

            const signID = document.getElementById('signID').value;
            const baudRate = document.getElementById('baudRate').value;
            const startBlank = document.getElementById('startBlank').checked;
            const apOnlyMode = document.getElementById('apOnlyMode').checked;

            try {
                const response = await fetch('/api/settings', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({
                        signID,
                        baudRate: parseInt(baudRate),
                        startBlank,
                        apOnlyMode
                    })
                });

                if (response.ok) {
                    status.className = 'status success';
                    status.textContent = 'Settings saved! Device will restart in 3 seconds...';
                    setTimeout(() => location.reload(), 3000);
                } else {
                    status.className = 'status error';
                    status.textContent = 'Failed to save settings';
                }
            } catch (err) {
                status.className = 'status error';
                status.textContent = 'Error: ' + err.message;
            }
        });

        document.getElementById('otaForm').addEventListener('submit', async (e) => {
            e.preventDefault();
            const status = document.getElementById('otaStatus');
            const progress = document.getElementById('otaProgress');
            const progressBar = document.getElementById('progressBar');
            const fileInput = document.getElementById('firmware');

            if (!fileInput.files[0]) {
                status.className = 'status error';
                status.textContent = 'Please select a firmware file';
                return;
            }

            const formData = new FormData();
            formData.append('firmware', fileInput.files[0]);

            progress.style.display = 'block';
            progressBar.style.width = '0%';
            progressBar.textContent = '0%';

            try {
                const xhr = new XMLHttpRequest();

                xhr.upload.addEventListener('progress', (e) => {
                    if (e.lengthComputable) {
                        const percent = Math.round((e.loaded / e.total) * 100);
                        progressBar.style.width = percent + '%';
                        progressBar.textContent = percent + '%';
                    }
                });

                xhr.addEventListener('load', () => {
                    if (xhr.status === 200) {
                        status.className = 'status success';
                        status.textContent = 'Firmware uploaded! Device restarting...';
                        setTimeout(() => location.reload(), 5000);
                    } else {
                        status.className = 'status error';
                        status.textContent = 'Upload failed';
                        progress.style.display = 'none';
                    }
                });

                xhr.addEventListener('error', () => {
                    status.className = 'status error';
                    status.textContent = 'Upload error';
                    progress.style.display = 'none';
                });

                xhr.open('POST', '/update');
                xhr.send(formData);
            } catch (err) {
                status.className = 'status error';
                status.textContent = 'Error: ' + err.message;
                progress.style.display = 'none';
            }
        });

        function resetWiFi() {
            if (confirm('This will reset WiFi settings and restart the device. Continue?')) {
                fetch('/api/reset-wifi', { method: 'POST' })
                    .then(() => {
                        document.getElementById('wifiStatus').className = 'status success';
                        document.getElementById('wifiStatus').textContent = 'WiFi reset! Device restarting...';
                    });
            }
        }
    </script>
</body>
</html>
)rawliteral";

        request->send(200, "text/html", html); });

    // API endpoint to send raw command
    server.on("/api/send", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
              {
            StaticJsonDocument<256> doc;
            DeserializationError error = deserializeJson(doc, data, len);

            if (error) {
                request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
                return;
            }

            String command = doc["command"] | "";
            if (command.length() == 0) {
                request->send(400, "application/json", "{\"error\":\"Missing command\"}");
                return;
            }

            sendToProlite(command);

            // Save this command as the last display state (without ID wrapper)
            preferences.begin("faux-lite", false);
            preferences.putString("lastDisplay", command);
            preferences.end();

            request->send(200, "application/json", "{\"status\":\"success\"}"); });

    // API endpoint for settings
    server.on("/api/settings", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
              {
            StaticJsonDocument<256> doc;
            DeserializationError error = deserializeJson(doc, data);

            if (error) {
                request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
                return;
            }

            String newSignID = doc["signID"].as<String>();
            int newBaudRate = doc["baudRate"].as<int>();
            bool newStartBlank = doc["startBlank"].as<bool>();
            bool newApOnlyMode = doc["apOnlyMode"].as<bool>();

            preferences.begin("faux-lite", false);
            preferences.putString("signID", newSignID);
            preferences.putInt("baudRate", newBaudRate);
            preferences.putBool("startBlank", newStartBlank);
            preferences.putBool("apOnlyMode", newApOnlyMode);
            preferences.end();

            request->send(200, "application/json", "{\"status\":\"success\"}");

            delay(1000);
            ESP.restart(); });

    // API endpoint for device info (for Home Assistant discovery)
    server.on("/api/info", HTTP_GET, [](AsyncWebServerRequest *request)
              {
        StaticJsonDocument<512> doc;
        doc["device"] = "faux-lite";
        doc["version"] = "1.0";
        doc["chip_id"] = getChipID();
        doc["sign_id"] = signID;
        doc["baud_rate"] = baudRate;
        doc["ip"] = WiFi.localIP().toString();
        doc["mac"] = WiFi.macAddress();

        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response); });

    // API endpoint for message list GET
    server.on("/api/messages", HTTP_GET, [](AsyncWebServerRequest *request)
              {
        StaticJsonDocument<4096> doc;
        JsonArray messagesArray = doc.createNestedArray("messages");

        for (size_t i = 0; i < messageQueue.size(); i++) {
            JsonObject msg = messagesArray.createNestedObject();
            msg["command"] = messageQueue[i].command;
            msg["duration"] = messageQueue[i].duration;
        }

        doc["runOnBoot"] = runMessagesOnBoot;
        doc["queueActive"] = queueActive;
        doc["queueSize"] = messageQueue.size();

        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response); });

    // API endpoint for message queue POST
    server.on("/api/messages", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
              {
            StaticJsonDocument<8192> doc;
            DeserializationError error = deserializeJson(doc, data, len);

            if (error) {
                request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
                return;
            }

            JsonArray messagesArray = doc["messages"];
            bool newRunOnBoot = doc["runOnBoot"].as<bool>();

            // Clear and rebuild queue
            messageQueue.clear();
            for (JsonVariant v : messagesArray) {
                JsonObject msg = v.as<JsonObject>();
                QueueMessage qm;
                qm.command = msg["command"].as<String>();
                qm.duration = msg["duration"].as<int>();
                if (qm.duration < 1) qm.duration = 5;
                messageQueue.push_back(qm);
            }

            // Save to preferences
            runMessagesOnBoot = newRunOnBoot;
            preferences.begin("faux-lite", false);
            preferences.putBool("runOnBoot", newRunOnBoot);
            preferences.end();
            saveQueueToPreferences();

            request->send(200, "application/json", "{\"status\":\"success\"}"); });

    // API endpoint to start queue
    server.on("/api/loop/start", HTTP_POST, [](AsyncWebServerRequest *request)
              {
        if (messageQueue.size() > 0) {
            startQueue();
            request->send(200, "application/json", "{\"status\":\"success\"}");
        } else {
            request->send(400, "application/json", "{\"error\":\"No messages in queue\"}");
        } });

    // API endpoint to stop queue
    server.on("/api/loop/stop", HTTP_POST, [](AsyncWebServerRequest *request)
              {
        stopQueue();
        request->send(200, "application/json", "{\"status\":\"success\"}"); });

    // API endpoint for WiFi reset
    server.on("/api/reset-wifi", HTTP_POST, [](AsyncWebServerRequest *request)
              {
        request->send(200, "application/json", "{\"status\":\"success\"}");

        delay(1000);
        WiFiManager wm;
        wm.resetSettings();
        ESP.restart(); });

    // OTA Update handler
    server.on("/update", HTTP_POST, [](AsyncWebServerRequest *request)
              {
            bool success = !Update.hasError();
            request->send(200, "application/json", success ? "{\"status\":\"success\"}" : "{\"error\":\"Update failed\"}");

            if (success) {
                delay(1000);
                ESP.restart();
            } }, handleOTAUpload);

    server.begin();
    Serial.println("Web server started");
}

void sendToProlite(String command)
{
    String fullCommand = "<ID" + signID + ">" + command + "\r\n";

    Serial.printf("Sending to Pro-Lite: %s", fullCommand.c_str());
    Serial.printf("Command length: %d bytes\n", fullCommand.length());
    Serial.printf("Sign ID: %s\n", signID.c_str());

    // Hex dump for debugging
    Serial.print("Hex: ");
    for (int i = 0; i < fullCommand.length(); i++)
    {
        Serial.printf("%02X ", (uint8_t)fullCommand[i]);
    }
    Serial.println();

    // Use write() instead of print() to send raw bytes
    ProliteSerial.write((const uint8_t *)fullCommand.c_str(), fullCommand.length());
    ProliteSerial.flush(); // Ensure data is sent
}

void displayMessage(String text, String color, String function)
{
    // Build Pro-Lite command for page A with specified color and function
    // Note: Page A is already running from boot, so we just update the content
    // Trailing space ensures gap between end and beginning when looping
    String command = "<PA><F" + color + "><F" + function + ">" + text + " ";
    sendToProlite(command);
}

void displayBootMessage(String text, String color, String function, int times)
{
    // Write boot message to Page Z (temporary page)
    // Always use <FQ> (APPEAR) animation for system messages
    String command = "<PZ><C" + color + "><F" + function + ">" + text + " ";
    sendToProlite(command);
    delay(100);

    // Run Page Z for 'times' iterations, then return to Page A
    // Format: <RPZ><nn><RPA> where nn is 01-99
    String timesStr = String(times);
    if (times < 10)
        timesStr = "0" + timesStr;
    String runCommand = "<RPZ><" + timesStr + "><RPA>";
    sendToProlite(runCommand);
}

void blankDisplay()
{
    // Delete all pages and graphics
    sendToProlite("<D*>");
    delay(500);
    // Create blank page with single space to properly blank the display
    sendToProlite("<PA> ");
    delay(500);
    sendToProlite("<RPA>");
}

void restoreLastDisplay()
{
    // Load last saved display state from preferences
    preferences.begin("faux-lite", true);
    String lastCommand = preferences.getString("lastDisplay", "");
    preferences.end();

    if (lastCommand.length() > 0)
    {
        Serial.println("Restoring last display state...");
        // Send the stored command (it's already in raw format without ID wrapper)
        sendToProlite(lastCommand);
    }
    else
    {
        Serial.println("No previous display state found");
        // Just blank it
        blankDisplay();
    }
}

String getChipID()
{
    uint64_t mac = ESP.getEfuseMac();
    uint16_t chipId = (uint16_t)(mac >> 32);
    char chipIdStr[5];
    snprintf(chipIdStr, sizeof(chipIdStr), "%04X", chipId);
    return String(chipIdStr);
}

void handleOTAUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final)
{
    if (!index)
    {
        Serial.printf("OTA Update Start: %s\n", filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN))
        {
            Update.printError(Serial);
        }
    }

    if (Update.write(data, len) != len)
    {
        Update.printError(Serial);
    }

    if (final)
    {
        if (Update.end(true))
        {
            Serial.printf("OTA Update Success: %u bytes\n", index + len);
        }
        else
        {
            Update.printError(Serial);
        }
    }
}

void startQueue()
{
    if (messageQueue.size() == 0)
    {
        Serial.println("Queue is empty, nothing to run");
        return;
    }

    queueActive = true;
    currentQueueIndex = 0;
    lastQueueTime = millis();

    // Display first message
    sendToProlite("<PA>" + messageQueue[0].command);
    Serial.printf("Queue started with %d messages\n", messageQueue.size());
}

void stopQueue()
{
    queueActive = false;
    Serial.println("Queue stopped");
}

void saveQueueToPreferences()
{
    preferences.begin("faux-lite", false);

    // Save queue size
    preferences.putInt("queueSize", messageQueue.size());

    // Save each message in the queue
    for (size_t i = 0; i < messageQueue.size(); i++)
    {
        String cmdKey = "qCmd" + String(i);
        String durKey = "qDur" + String(i);
        preferences.putString(cmdKey.c_str(), messageQueue[i].command);
        preferences.putInt(durKey.c_str(), messageQueue[i].duration);
    }

    preferences.end();
    Serial.printf("Saved queue with %d messages to preferences\n", messageQueue.size());
}

void loadQueueFromPreferences()
{
    preferences.begin("faux-lite", true);

    // Load queue size
    int savedQueueSize = preferences.getInt("queueSize", 0);

    // Clear and load messages
    messageQueue.clear();
    for (int i = 0; i < savedQueueSize; i++)
    {
        String cmdKey = "qCmd" + String(i);
        String durKey = "qDur" + String(i);
        QueueMessage qm;
        qm.command = preferences.getString(cmdKey.c_str(), "");
        qm.duration = preferences.getInt(durKey.c_str(), 5);
        if (!qm.command.isEmpty())
        {
            messageQueue.push_back(qm);
        }
    }

    preferences.end();
    Serial.printf("Loaded queue with %d messages from preferences\n", messageQueue.size());
}
