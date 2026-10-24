#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <vector>
#include <time.h>
#include <FS.h>
#include <LittleFS.h>

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

// Queue system: each queue is a list of message segments stored on the
// ESP32 (LittleFS) and only written to a sign page when the queue runs.
// Each <Fx> tag starts a new message on the page and the sign sequences
// through them natively - no software timing required.
// Queues run on page A; a chained "next queue" is written to page B.
// Page Y is used for message tests, Z for system/boot messages.
#define RUN_PAGE 'A'
#define CHAIN_PAGE 'B'
#define TEST_PAGE 'Y'

struct QueueMessage
{
    String command; // Message segment (e.g., <FB><CC><SJ>Hello World)
    int duration;   // Pause in seconds after this message (~2s per <FP> tag)
};

struct MessageQueue
{
    String name;
    int runTimes = 0;   // Times to loop this queue before switching (0 = loop forever)
    int nextQueue = -1; // Queue index to switch to after runTimes (-1 = none)
    std::vector<QueueMessage> messages;
};

struct QueueSchedule
{
    int queue = -1;      // Queue index to start
    int hour = 0;        // 0-23
    int minute = 0;      // 0-59
    uint8_t days = 0;    // Bitmask: bit 0 = Sunday ... bit 6 = Saturday
    bool enabled = true;
};

// Queues live in LittleFS so they aren't limited by sign page count or NVS
// space - this cap just bounds JSON size and keeps the UI manageable.
#define MAX_QUEUES 100
#define MAX_SCHEDULES 50
#define QUEUES_FILE "/queues.json"

std::vector<MessageQueue> queues;
std::vector<QueueSchedule> schedules;
int activeQueue = -1; // Index of queue currently running on the sign (-1 = none)
int bootQueue = -1;   // Queue index to run on boot (-1 = none)
String timeZone = "UTC0"; // POSIX TZ string for NTP/scheduling
bool timeSynced = false;
int lastCheckedMinute = -1;

// Web server
AsyncWebServer server(80);

// Shared logo (SVG source is ~9KB vs ~50KB base64 PNG)
String logoUri = "data:image/svg+xml;base64,PD94bWwgdmVyc2lvbj0iMS4wIiBlbmNvZGluZz0iVVRGLTgiIHN0YW5kYWxvbmU9Im5vIj8+DQo8IS0tIENyZWF0ZWQgd2l0aCBJbmtzY2FwZSAoaHR0cDovL3d3dy5pbmtzY2FwZS5vcmcvKSAtLT4NCg0KPHN2Zw0KICAgdmVyc2lvbj0iMS4xIg0KICAgaWQ9InN2ZzEiDQogICB3aWR0aD0iODUzIg0KICAgaGVpZ2h0PSIzOTgiDQogICB2aWV3Qm94PSIwIDAgODUzIDM5OCINCiAgIHNvZGlwb2RpOmRvY25hbWU9Imljb24uc3ZnIg0KICAgaW5rc2NhcGU6dmVyc2lvbj0iMS4zLjIgKDA5MWUyMGUsIDIwMjMtMTEtMjUsIGN1c3RvbSkiDQogICBpbmtzY2FwZTpleHBvcnQtZmlsZW5hbWU9Imljb24tZXhwb3J0LnBuZyINCiAgIGlua3NjYXBlOmV4cG9ydC14ZHBpPSI5NiINCiAgIGlua3NjYXBlOmV4cG9ydC15ZHBpPSI5NiINCiAgIHhtbG5zOmlua3NjYXBlPSJodHRwOi8vd3d3Lmlua3NjYXBlLm9yZy9uYW1lc3BhY2VzL2lua3NjYXBlIg0KICAgeG1sbnM6c29kaXBvZGk9Imh0dHA6Ly9zb2RpcG9kaS5zb3VyY2Vmb3JnZS5uZXQvRFREL3NvZGlwb2RpLTAuZHRkIg0KICAgeG1sbnM9Imh0dHA6Ly93d3cudzMub3JnLzIwMDAvc3ZnIg0KICAgeG1sbnM6c3ZnPSJodHRwOi8vd3d3LnczLm9yZy8yMDAwL3N2ZyI+DQogIDxkZWZzDQogICAgIGlkPSJkZWZzMSI+DQogICAgPHJlY3QNCiAgICAgICB4PSI3OC4xNzU5MDUiDQogICAgICAgeT0iNjYuODI3NzkiDQogICAgICAgd2lkdGg9IjE0Ni4yNjQ2Ig0KICAgICAgIGhlaWdodD0iNjAuNTIzMjc5Ig0KICAgICAgIGlkPSJyZWN0MSIgLz4NCiAgPC9kZWZzPg0KICA8c29kaXBvZGk6bmFtZWR2aWV3DQogICAgIGlkPSJuYW1lZHZpZXcxIg0KICAgICBwYWdlY29sb3I9IiNmZmZmZmYiDQogICAgIGJvcmRlcmNvbG9yPSIjMDAwMDAwIg0KICAgICBib3JkZXJvcGFjaXR5PSIwLjI1Ig0KICAgICBpbmtzY2FwZTpzaG93cGFnZXNoYWRvdz0iMiINCiAgICAgaW5rc2NhcGU6cGFnZW9wYWNpdHk9IjAuMCINCiAgICAgaW5rc2NhcGU6cGFnZWNoZWNrZXJib2FyZD0iMCINCiAgICAgaW5rc2NhcGU6ZGVza2NvbG9yPSIjZDFkMWQxIg0KICAgICBpbmtzY2FwZTp6b29tPSIxLjU4NjE2NjUiDQogICAgIGlua3NjYXBlOmN4PSI0MjYuNDk5OTkiDQogICAgIGlua3NjYXBlOmN5PSIxOTkuNTM3NjkiDQogICAgIGlua3NjYXBlOndpbmRvdy13aWR0aD0iMjU2MCINCiAgICAgaW5rc2NhcGU6d2luZG93LWhlaWdodD0iMTUxMSINCiAgICAgaW5rc2NhcGU6d2luZG93LXg9Ii05Ig0KICAgICBpbmtzY2FwZTp3aW5kb3cteT0iLTkiDQogICAgIGlua3NjYXBlOndpbmRvdy1tYXhpbWl6ZWQ9IjEiDQogICAgIGlua3NjYXBlOmN1cnJlbnQtbGF5ZXI9ImcxIiAvPg0KICA8Zw0KICAgICBpbmtzY2FwZTpncm91cG1vZGU9ImxheWVyIg0KICAgICBpbmtzY2FwZTpsYWJlbD0iSW1hZ2UiDQogICAgIGlkPSJnMSI+DQogICAgPHBhdGgNCiAgICAgICBzdHlsZT0iZmlsbDojZWQxYzI0O3N0cm9rZTpub25lO2ZpbGwtb3BhY2l0eToxIg0KICAgICAgIGQ9Im0gMTczLjk1NDE0LDM4NC40MzM0MSBjIC0xNS4xNjc0NCwtMS42NjMwOSAtMjYuMTA5MDMsLTMuOTQwOTMgLTM5LjUsLTguMjIzMTggQyA3NC4xNDA3NjUsMzU2LjkyMjgxIDI2LjcxMzE2NSwzMDYuODA0MjEgMTEuMDA1NjY4LDI0NS43NTczOSA4LjI2NjgyNjYsMjM1LjExMjk1IDcuNTcyNjQ5MiwyMzEuMTc3ODcgNC45ODUzMjQ5LDIxMS42Mjk5NCAzLjUzNDY1MTksMjAwLjY2OTcyIDMuNjg3NDc0OSwxOTUuNTM0NzIgNS45ODE1NDA5LDE3OC4xNTYxMiAxMS43NDQzOTMsMTM0LjQ5OTg4IDI3LjY5OTk2NywxMDAuMTI4NSA1NS45MzYyODUsNzAuNTQzOTY1IGMgMjUuMDg4NTcxLC0yNi4yODY0OSA1NS45Mjg5NjUsLTQ0LjUwNjg5IDkwLjAxNzg1NSwtNTMuMTgyMjYgMjAuMDEyNSwtNS4wOTMwMyAzLjc3NjkxLC00Ljc4MzQ0IDI3MSwtNS4xNjc2NSAxNjguNDg2NjQsLTAuMjQyMjUgMjUxLjY2NDUxLC0wLjAyMDQgMjU4LjMyODEzLDAuNjg4OTUgMzQuMTg1OCwzLjYzOTIxIDY2LjUxMTQ1LDE2LjQ2NjMxIDk0LjE1MDg3LDM3LjM1OTg5IDEwLjM0Njk4LDcuODIxNjMgMjcuNDg0MTgsMjQuOTc3NTQgMzUuMTM5NTQsMzUuMTc3ODkgNDMuMjg1NjUsNTcuNjc1ODE1IDUwLjQyNDA2LDEzMi42NDg0MDUgMTguNjYxODYsMTk2LjAwMDAwNSAtOS41MjE5LDE4Ljk5MiAtMTguMTUwNDIsMzEuMjA0NTEgLTMyLjc1NzU4LDQ2LjM2Mzk5IC0zMi4yODAxNCwzMy41MDA3IC03My4wMDU4LDUyLjcyNDMgLTEyMS4wMjI4Miw1Ny4xMjU5OCAtMTcuMzM4ODQsMS41ODk0MyAtNDgwLjcyMjA5LDEuMTQzMDMgLTQ5NS41LC0wLjQ3NzM1IHogbSA1MDkuOTIyNjQsLTI3LjMzMzE4IGMgNDAuODE3OTEsLTcuODY4MTggNzUuNTQ4ODUsLTI5LjEwODQ4IDEwMC4wNzE1MSwtNjEuMjAwNDQgMTUuMzQ3NzYsLTIwLjA4NTA3IDI0LjMwODk0LC00MC4wMjM2NyAzMC4xNTcwNiwtNjcuMDk5MzkgMS45MDE4MiwtOC44MDUwOSAyLjI0MTksLTEzLjQxMzQ4IDIuMjQxOSwtMzAuMzc5NjEgMCwtMTYuOTc1NTUgLTAuMzQwNzIsLTIxLjU4Nzg0IC0yLjI1MzEzLC0zMC41IC02LjcxMTA4LC0zMS4yNzQ3NiAtMjAuMjA4NDksLTU3LjQ2NDM0IC00MC42Mzk5OCwtNzguODU1NDM1IC0yNC42MDk1NCwtMjUuNzY1MzYgLTUxLjI1MTM4LC00MC45MTcwMyAtODUuMDU2NzgsLTQ4LjM3MzIyIGwgLTkuOTQzMjIsLTIuMTkzMSAtMjQ4LjUsLTAuMzQyNjEgYyAtMTgyLjU1MzE5LC0wLjI1MTY5IC0yNTEuMDIxMTEsLTAuMDM4MyAtMjU4LDAuODAzOTYgLTE5LjczMjIyLDIuMzgxNTIgLTQyLjUxMzcxLDkuODk2OTIgLTU5LjcwNjI1LDE5LjY5NjUzIC0zMC43OTkyOTksMTcuNTU1MzUgLTU3LjU4NzUzOSw0OC4zODQxNTUgLTcwLjMwMzY0Miw4MC45MDc3NzUgLTguMjc1OTEsMjEuMTY3MDYgLTExLjYxMzI3OCw0MS43NTA2MyAtMTAuNjc3NzQzLDY1Ljg1NjEgMC45Mzc1NjcsMjQuMTU3ODIgNS44MTM0NDEsNDMuMTI2ODUgMTYuNzA3ODU3LDY1IDI0Ljc3Mzk4OSw0OS43Mzk3MiA3NS4zNjEwMzgsODQuMDI5OTYgMTMwLjQ3OTc3OCw4OC40NDUyMSA1LjUsMC40NDA1NyAxMTguOSwwLjcxODkzIDI1MiwwLjYxODU3IGwgMjQyLC0wLjE4MjQ3IHogTSA0MzMuNjIwODEsMjc5LjI1NDEyIGMgLTAuMzY2NjcsLTAuMzY2NjYgLTAuNjY2NjcsLTM3LjY3Njg1IC0wLjY2NjY3LC04Mi45MTE1MiB2IC04Mi4yNDQ4NiBsIDExLjE1MjY0LC0wLjMzODQ3IGMgNi4xMzM5NSwtMC4xODYxNyAxNS41ODM5NSwtMC4xODM4MSAyMSwwLjAwNSBsIDkuODQ3MzYsMC4zNDM5MyB2IDYzLjI5OTMxIGMgMCw1Ni43OTgxOSAwLjE2MjQ2LDYzLjM2MTY0IDEuNTgxNzksNjMuOTA2MjkgMC44Njk5OCwwLjMzMzg1IDExLjY2OTk4LDAuNjA2OTkgMjQsMC42MDY5OSBoIDIyLjQxODIxIHYgMTkgMTkgaCAtNDQuMzMzMzMgYyAtMjQuMzgzMzQsMCAtNDQuNjMzMzQsLTAuMyAtNDUsLTAuNjY2NjcgeiBtIDEwMC4zMzMzMywtNjAuMzMzMzMgdiAtNjEgaCAxNy44Mzk2NyAxNy44Mzk2NyBsIDAuNjYwMzMsMjUuNjYyMjEgYyAwLjgzODA1LDMyLjU2ODcxIDAuODM5MzgsNzAuNTcyNTUgMC4wMDMsODUuMDg3NzkgbCAtMC42NDgyNSwxMS4yNSBoIC0xNy44NDcyMSAtMTcuODQ3MjEgeiBtIDczLjg0ODYyLDU5LjYxMjE0IGMgLTYuNDQ3MDUsLTEuNzM2NTYgLTkuNDQ4ODcsLTMuOTQxMzQgLTEyLjA4NTc2LC04Ljg3Njc2IC0yLjE5MDUxLC00LjA5OTk2IC0yLjI3ODg1LC01LjMyMjQ5IC0yLjc2Mjg2LC0zOC4yMzUzOCBsIC0wLjUsLTM0IC01LjYyNjY2LC0wLjI5NjIyIC01LjYyNjY2LC0wLjI5NjIzIC0wLjYxMjMzLC0zLjcwMzc3IGMgLTAuNzk1MDUsLTQuODA4OTUgLTAuODE2ODIsLTMxLjU2NDY4IC0wLjAyNzQsLTMzLjYyMTk5IDAuNDQ3NiwtMS4xNjYzNCAxLjk5NTM4LC0xLjU4MTc5IDUuODkzMDUsLTEuNTgxNzkgMy44OTc2NywwIDUuNDQ1NDUsLTAuNDE1NDUgNS44OTMwMSwtMS41ODE3OSAwLjMzMzg1LC0wLjg2OTk4IDAuNjA2OTksLTguNTE5OTggMC42MDY5OSwtMTcgdiAtMTUuNDE4MjEgaCAxOCAxOCB2IDE3IDE3IGggOCA4IHYgMTkuNSAxOS41IGggLTggLTggdiAyMiBjIDAsMjYuMTIzMDMgLTAuMDQ2OCwyNiA5Ljg5OTMxLDI2IGggNS44OTkzIGwgMC43MTMxNiwxMi4zNjQ1NSBjIDAuMzkyMjQsNi44MDA1IDAuMzk1MzMsMTQuNjc1NSAwLjAwNywxNy41IGwgLTAuNzA2NDUsNS4xMzU0NSAtMTYuMTU2MTYsLTAuMDY3NSBjIC05LjU4MTYsLTAuMDQgLTE4LjA0ODk4LC0wLjU3NzM0IC0yMC44MDc1NCwtMS4zMjAzNyB6IG0gNzguOTUxOTUsLTAuNDg1NTEgYyAtMTEuNTQ3MzksLTMuODA2NzcgLTIyLjg1MTUxLC0xNC4xNTg0NSAtMjkuMDc2NDcsLTI2LjYyNjYzIC03LjQyMjcxLC0xNC44NjcxNiAtOS43NTMzOSwtMzQuOTU0NzkgLTUuODYyMjIsLTUwLjUyNTI0IDguOTcyNTQsLTM1LjkwMzU2IDM5LjA1OTc5LC01NC4xNTM1OSA2Ni41NDI5MSwtNDAuMzYyOTYgMTEuODk1NTgsNS45NjkwNCAyMS45NzAwNywxOC40MTE3NCAyNy4wNTI4OCwzMy40MTIyMiAyLjQyODMyLDcuMTY2NDggMi45NDE3MSwxMC41MjMwNCAzLjMyMzAxLDIxLjcyNTk4IGwgMC40NTA5OCwxMy4yNSBoIC0zMC42MTU4MyAtMzAuNjE1ODMgdiA2LjM1MDc4IGMgMCwxMS4yNDU0NiAzLjIwODkzLDE1LjY0OTIyIDExLjQwMzI5LDE1LjY0OTIyIDUuMDA2NTUsMCA4LjI3MDI5LC0xLjc5MjM0IDEwLjAzNDgyLC01LjUxMDgyIDEuOTM1NywtNC4wNzkxOSAzLjg3MzU0LC00LjQ4OTE4IDIxLjIxODI3LC00LjQ4OTE4IDExLjI2MTYyLDAgMTMuNTAyOCwwLjI1MTI0IDE0LjAwOTAzLDEuNTcwNDYgMC4zMzQyMiwwLjg3MDk1IC0xLjEyNzM3LDQuOTkwOTMgLTMuMjgxMzgsOS4yNDk3MyAtNS4xNTYxOSwxMC4xOTQ1MiAtMTQuNDU4MDksMTkuNjcwOTYgLTIzLjc4MjI5LDI0LjIyODU0IC02LjA0MzU4LDIuOTU0MDQgLTguMDI1OTUsMy40MDgwNCAtMTYsMy42NjQzIC02LjgzNzUzLDAuMjE5NzQgLTEwLjUxOTU3LC0wLjE3NDkxIC0xNC44MDExNywtMS41ODY0IHogbSAyNC4xOTk0MywtNzYuMjYwNjEgYyAwLC0zLjA0ODA2IC0yLjU1NzIzLC05LjA0MDg5IC00LjcyODE3LC0xMS4wODAzOCAtMy4xMjk2OSwtMi45NDAxOSAtMTAuMDk2MzYsLTMuMTU5NCAtMTMuMTMwOSwtMC40MTMxOCAtMi40Njc1NywyLjIzMzEyIC01LjYzMDcxLDEwLjAyNjA0IC00LjY4ODQyLDExLjU1MDY5IDAuOTM5MjQsMS41MTk3MyAyMi41NDc0OSwxLjQ2NDk4IDIyLjU0NzQ5LC0wLjA1NzEgeiBtIC0zMzMsMTguMTMzOTggdiAtMjMgaCAyMyAyMyB2IDIzIDIzIGggLTIzIC0yMyB6IE0gNTQ2LjUzODU1LDEyOC4yODU0IGMgLTUuMDM4MzcsLTkuODEzNzkgLTE0LjQwMDMsLTE5LjE5Nzk0IC0yNS43NjEyLC0yNS44MjIzNCBsIC04LjUwMjU0LC00Ljk1NzczNCAxMC4xNzA3NiwtNi43MTcyNTEgYyAxMi4wNzA2OCwtNy45NzIwNSAxOC41MjM5MiwtMTQuNzQ3ODcgMjQuMTU5OTYsLTI1LjM2NzY3IGwgNC4xNTEzOSwtNy44MjIzMiAyLjcxODEsNS40MTEzNSBjIDYuODYyOSwxMy42NjMwNiAxNC4yMTQ4OCwyMS4zNjgxOCAyOC40OTA3MSwyOS44NTkxOCBsIDcuNzk0NDQsNC42MzU5ODEgLTEwLjAxNTMxLDUuOTQwNzY0IGMgLTExLjc3MzI3LDYuOTgzNTEgLTE3LjQ2NTU0LDEyLjc4NzMyIC0yMy43OTA3MiwyNC4yNTY4OCAtMi40NzUsNC40ODc5NiAtNC43MjUsOC4xNzExOSAtNSw4LjE4NDk1IC0wLjI3NSwwLjAxMzggLTIuMjYyMDEsLTMuNDA3MDUgLTQuNDE1NTksLTcuNjAxNzkgeiINCiAgICAgICBpZD0icGF0aDEiDQogICAgICAgc29kaXBvZGk6bm9kZXR5cGVzPSJzc3Nzc3Nzc3Nzc3Nzc3Nzc3Nzc3NzY3Njc3Nzc3NzY3Nzc2NzY2Nzc3NjY2Nzc2NjY2Nzc2NjY2Nzc3NjY2Nzc3Nzc2NjY2NjY2NjY2Njc3Njc2Njc3Nzc3Nzc3NzY2Njc3Nzc3Nzc3Nzc3Nzc3NzY2NjY2NjY2Njc3Njc3Njc3Njc3NzcyIgLz4NCiAgICA8cGF0aA0KICAgICAgIHN0eWxlPSJmb250LXN0eWxlOml0YWxpYztmb250LXNpemU6NTMuMzMzM3B4O2xpbmUtaGVpZ2h0OjEuMTtmb250LWZhbWlseTonQ29taWMgU2FucyBNUyc7LWlua3NjYXBlLWZvbnQtc3BlY2lmaWNhdGlvbjonQ29taWMgU2FucyBNUywgSXRhbGljJzt0ZXh0LWFsaWduOmNlbnRlcjt3aGl0ZS1zcGFjZTpwcmU7ZmlsbDojZWQxYzI0O3N0cm9rZS1saW5lam9pbjpyb3VuZDtwYWludC1vcmRlcjpzdHJva2UgZmlsbCBtYXJrZXJzIg0KICAgICAgIGQ9Im0gMTA3LjI1OTE0LDc2LjMzNzE1OSAtMS44MjI5MSwxMS4yNDk5OTQgcSA1LjI2MDQxLC0xLjMyODEyNCA3Ljc4NjQ1LC0xLjMyODEyNCAyLjM5NTgzLDAgNS45MTE0NSwwLjIzNDM3NCAzLjU0MTY3LDAuMjM0Mzc1IDMuOTMyMjksMC45MTE0NTggMC40MTY2NywwLjY1MTA0MSAwLjQxNjY3LDEuMzgwMjA4IDAsMS40MDYyNDkgLTAuODg1NDIsMi4zNDM3NDggLTAuODg1NDEsMC45Mzc1IC0yLjE2MTQ1LDAuOTM3NSAtMS44NDg5NiwwIC00LjA2MjUsLTAuMjA4MzMzIC0yLjEzNTQyLC0wLjIwODMzNCAtMy45ODQzNywtMC4yMDgzMzQgLTEuOTI3MDksMCAtNy45Njg3NSwxLjQ4NDM3NCBsIC0zLjAyMDgzLDE3LjkxNjY1NiBxIC0wLjIwODMzLDEuMTcxODcgLTEuMDkzNzUsMS45MDEwNCAtMC44ODU0MTYsMC43NTUyMSAtMS45NTMxMjQsMC43NTUyMSAtMS4wNDE2NjYsMCAtMS42NDA2MjQsLTAuNjUxMDQgLTAuNTcyOTE2LC0wLjYyNSAtMC41NzI5MTYsLTEuNTYyNSAwLC0wLjY1MTA0IDEuMDkzNzQ5LC02Ljg0ODk2IGwgMy4wOTg5NTUsLTE3LjcwODMxOSBxIDEuMjIzOTYsLTcuMDMxMjQ1IDEuNjQwNjMsLTEwLjEzMDIwMiAwLjEzMDIsLTEuMTcxODc0IDAuMjg2NDUsLTIuMDMxMjQ5IGwgMC4yMDgzNCwtMS40MzIyOSBxIDAuMjA4MzMsLTEuMTcxODc1IDEuMDE1NjIsLTEuOTI3MDgzIDAuODMzMzMsLTAuNzgxMjQ5IDIuMDA1MjEsLTAuNzgxMjQ5IDAuODU5MzcsMCAxLjQ1ODMzLDAuNDY4NzUgNi4yMjM5NiwtMS4xMTk3OTEgMTAuNDk0NzksLTEuMTE5NzkxIDMuNTkzNzQsMCA3LjUyNjAzLDEuMDY3NzA3IDMuOTU4MzMsMS4wNjc3MDggMy45NTgzMywyLjY4MjI5IDAsMC45ODk1ODMgLTAuODg1NDEsMS45NzkxNjYgLTAuODg1NDIsMC45ODk1ODMgLTIuMTA5MzgsMC45ODk1ODMgLTAuNTQ2ODcsMCAtMC45NjM1NCwtMC4yMDgzMzQgLTIuOTk0NzksLTEuMTcxODc0IC04LjM4NTQxLC0xLjE3MTg3NCAtMy40ODk1OCwwIC05LjMyMjkxLDEuMDE1NjI0IHogbSAzNi44NzQ5OCwzMy4yODEyMzEgcSAtNS42MjUsMi43MDgzMyAtOC4wMjA4MywyLjcwODMzIC05LjczOTU4LDAgLTkuNzM5NTgsLTkuMjcwODMgMCwtOC42MTk3ODMgNS40NDI3MSwtMTMuOTU4MzIyIDUuNDY4NzUsLTUuMzM4NTM4IDExLjk3OTE2LC01LjMzODUzOCAyLjUyNjA0LDAgNS4yMDgzMywxLjMwMjA4MyAyLjcwODMzLDEuMjc2MDQgMi43MDgzMywyLjk5NDc4OSAwLDEuMDY3NzA4IC0wLjcwMzEyLDEuNjY2NjY2IC0wLjUyMDg0LDEuMDE1NjI1IC0xLjQ4NDM4LDUuODg1NDEzIC0wLjkzNzUsNC44NDM3NDkgLTAuOTM3NSw2Ljk3OTE1OSAwLDIuNjMwMjEgMS4zNTQxNyw3LjIzOTU4IGwgMC4xMDQxNywwLjQxNjY3IHEgLTAuNTcyOTIsMi40MjE4NyAtMi44OTA2MywyLjQyMTg3IC0wLjM5MDYyLDAgLTEuNDU4MzMsLTEuMDY3NyAtMS4wNjc3MSwtMS4wNDE2NyAtMS41NjI1LC0xLjk3OTE3IHogbSAxLjkwMTA0LC0yMC4yNjA0MDUgcSAtMS42NDA2MiwtMC45NjM1NDEgLTIuNjA0MTYsLTAuOTYzNTQxIC00LjM0ODk2LDAgLTguMTI1LDMuOTMyMjg5IC0zLjc1LDMuOTA2MjQ4IC0zLjc1LDkuNzM5NTc3IDAsNS43MjkxNiA0Ljk0NzkyLDUuNzI5MTYgMy44ODAyLDAgNy4yOTE2NiwtMi44NjQ1OCAwLjQxNjY3LC0xMC45MTE0NDkgMi4yMzk1OCwtMTUuNTcyOTA1IHogbSAzMi42ODIyNyw2LjM4MDIwNSAtMC45MTE0Niw1LjY1MTA0IHEgLTAuODg1NDEsNS40NDI3IC0xLjAxNTYyLDcuMzQzNzQgLTAuMTU2MjUsMS45MDEwNCAtMC4zOTA2MywyLjQ3Mzk2IC0wLjIwODMzLDAuNTk4OTYgLTEuMDE1NjIsMS4xOTc5MiAtMC44MDcyOSwwLjYyNSAtMS43OTY4NywwLjYyNSAtMS45NzkxNywwIC0yLjEzNTQyLC0yLjQ0NzkyIC0zLjUxNTYyLDEuNjQwNjIgLTcuNzg2NDUsMS42NDA2MiAtMy42MTk3OSwwIC01LjU5ODk2LC0xLjU4ODU0IC0xLjk3OTE2LC0xLjYxNDU4IC0xLjk3OTE2LC00LjU1NzI4IDAsLTMuODgwMjEgMC4zOTA2MiwtOC4zODU0MTcgMC4zOTA2MywtNC41MDUyMDUgMS41MzY0NiwtOC44MDIwNzggMS4xNDU4MywtNC4zMjI5MTQgMS45MjcwOCwtNS4wMjYwMzggMC44MDcyOSwtMC43MDMxMjUgMS45NTMxMiwtMC43MDMxMjUgMS4wNDE2NywwIDEuNjY2NjcsMC41NzI5MTYgMC42MjUsMC41NzI5MTcgMC42MjUsMS40NTgzMzMgMCwwLjgzMzMzMyAtMS4wNDE2Nyw0LjM3NDk5NyAtMC44NTkzNywyLjcwODMzMiAtMS40MzIyOSw2Ljc5Njg3MSAtMC41NDY4Nyw0LjA2MjUwMSAtMC41NDY4Nyw3LjAzMTI0MSAwLDEuNTEwNDIgMC4xNTYyNSwyLjYwNDE3IDEuNTEwNDEsMC45Mzc1IDMuMTUxMDQsMC45Mzc1IDMuNDg5NTgsMCA3LjYzMDIsLTEuMTE5NzkgbCAxLjQ4NDM4LC0xMC4zMzg1MzcgcSAwLjIwODMzLC0xLjM1NDE2NiAxLjA2NzcsLTUuOTExNDU1IDAuODg1NDIsLTQuNTgzMzMgMS43NDQ3OSwtNS40MTY2NjMgMC44ODU0MiwtMC44MzMzMzMgMi4xODc1LC0wLjgzMzMzMyAxLjE0NTg0LDAgMS43NDQ3OSwwLjcyOTE2NiAwLjU5ODk2LDAuNzI5MTY2IDAuMzkwNjMsMS44MjI5MTYgeiBtIDI3LjE2MTQ0LC0zLjU2NzcwNyAtNS4zMzg1Myw0LjU4MzMzMSA2Ljc3MDgzLDExLjA5Mzc0NiBxIDAuNDE2NjYsMC42NzcwOCAwLjQxNjY2LDEuMzU0MTYgMCwxLjA2NzcxIC0wLjk4OTU4LDEuOTUzMTMgLTAuOTg5NTgsMC45MTE0NSAtMi4wMzEyNSwwLjkxMTQ1IC0wLjYyNSwwIC0xLjA2NzcxLC0wLjMzODU0IC0wLjQ0MjcxLC0wLjMxMjUgLTIuNDQ3OTEsLTMuNDExNDUgLTEuOTc5MTcsLTMuMDk4OTYgLTQuODY5NzksLTcuOTQyNzEgbCAtNS45MTE0Niw1LjIzNDM4IC00LjQwMTAzLDMuNzQ5OTkgcSAtMS43NzA4NCwxLjUzNjQ2IC0yLjY4MjI5LDEuNTM2NDYgLTAuODU5MzgsMCAtMS41MTA0MiwtMC42NzcwOCAtMC42NTEwNCwtMC43MDMxMyAtMC42NTEwNCwtMS41ODg1NCAwLC0xLjUzNjQ2IDMuNTQxNjYsLTQuMjk2ODggMi40MjE4OCwtMS44NzQ5OSAyLjc2MDQyLC0yLjE4NzQ5IGwgNi40NTgzMywtNS44MDcyOTMgLTIuNjU2MjUsLTQuOTIxODcyIHEgLTEuNjQwNjMsLTMuMDQ2ODczIC0yLjg5MDYyLC00LjI5Njg3MiAtMC43MDMxMywtMC43NTUyMDggLTAuNzAzMTMsLTEuNjE0NTgyIDAsLTEuMDY3NzA4IDAuOTM3NSwtMi4wMzEyNDkgMC45Mzc1LC0wLjk2MzU0MSAxLjk1MzEyLC0wLjk2MzU0MSAxLjQ4NDM4LDAgNC4wMTA0Miw0LjAxMDQxNCBsIDMuNTE1NjIsNi4yNDk5OTYgNS44MDcyOSwtNC43MTM1MzkgcSAzLjcyMzk2LC0zLjAyMDgzMSA1LjA3ODEyLC00LjY4NzQ5NyAxLjAxNTYzLC0xLjIyMzk1NyAyLjE4NzUsLTEuMjIzOTU3IDAuOTExNDYsMCAxLjU2MjUsMC42NzcwODMgMC42NTEwNCwwLjY3NzA4MiAwLjY1MTA0LDEuNTYyNDk5IDAsMS41MTA0MTUgLTMuMDcyOTEsNC4yMTg3NDcgeiINCiAgICAgICBpZD0idGV4dDEiDQogICAgICAgdHJhbnNmb3JtPSJtYXRyaXgoMi42MTg0NDIsMCwwLDQuNzkxNzA0NSwtMTcyLjYwNzk3LC0yNjAuMjM1MykiDQogICAgICAgYXJpYS1sYWJlbD0iRmF1eCIgLz4NCiAgPC9nPg0KPC9zdmc+DQo=";

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
void startQueue(int index);
void stopQueue();
void checkSchedules();
String buildQueuePageCommand(int index, char page);
String getLocalTimeString();
void serializeQueuesTo(JsonDocument &doc);
bool parseQueuesJson(JsonDocument &doc);
void saveQueues();
void loadQueues();

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
    timeZone = preferences.getString("timeZone", "UTC0");
    preferences.end();

    // Mount LittleFS for queue/schedule storage (uses the spiffs partition)
    if (!LittleFS.begin(true))
    {
        Serial.println("LittleFS mount failed - queues will not persist!");
    }

    // Load queues and schedules from storage
    loadQueues();

    preferences.begin("faux-lite", false);

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
    else if (bootQueue >= 0 && bootQueue < (int)queues.size())
    {
        Serial.printf("Running queue %d on boot\n", bootQueue);
        startQueue(bootQueue);
    }
    else
    {
        // Restore previous display state
        restoreLastDisplay();
    }
}

void loop()
{
    // Queued messages run natively on the sign - only schedules need checking
    checkSchedules();
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

    // Faux-Lite theme + options injected into the captive portal <head>.
    // The <style> restyles WiFiManager's markup; the div/img below are
    // hoisted to the top of <body> by the HTML parser.
    static String customHTML = String("<style>") +
        "body{background:#FFFF00;color:#000;font-family:Arial,Helvetica,sans-serif}"
        ".c{font-family:Arial,Helvetica,sans-serif}"
        ".wrap{background:#FFF;border:3px solid #000;padding:15px}"
        "input,button,select,.msg{border-radius:0}"
        "input,select,input[type='file']{border:2px solid #000;background:#FFF;color:#000;font-family:Arial,sans-serif}"
        "button,input[type='button'],input[type='submit']{background-color:#000;color:#FFFF00;border:2px solid #000;font-weight:bold}"
        "button:hover,input[type='button']:hover,input[type='submit']:hover{background:#333}"
        "button.D{background-color:#FF0000}"
        ".msg{border:2px solid #000;border-left-width:5px;background:#EEE;color:#000}"
        ".msg.P{border-left-color:#00AA00}.msg.P h4{color:#00AA00}"
        "a{color:#00F;text-decoration:underline}a:hover{color:#00A}"
        "</style>"
        "<div style='text-align:center;padding:10px;background:#000;'>"
        "<img src='" + logoUri + "' style='height:60px;vertical-align:middle;margin-right:10px;'>"
        "<span style='color:#FFFF00;font-size:24px;font-weight:bold;vertical-align:middle;'>Faux-Lite</span>"
        "</div>"
        "<div style='text-align:center;'>"
        "<h3>Faux-Lite Options</h3>"
        "<p><a href='/'>Open Main Page</a></p>"
        "<p><a href='/aponly'>Enable AP-Only Mode</a></p>"
        "<p style='font-size:12px;color:#888;'>AP-Only Mode disables WiFi client and runs as Access Point only</p>"
        "</div>";
    wm.setCustomHeadElement(customHTML.c_str());
    wm.setTitle("Faux-Lite");
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

    // Start NTP sync for queue scheduling
    configTzTime(timeZone.c_str(), "pool.ntp.org", "time.nist.gov");
    Serial.printf("NTP time sync started (TZ: %s)\n", timeZone.c_str());

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
        <img src=")rawliteral" + logoUri + R"rawliteral(" alt="Icon" style="width: 86px; height: 40px; margin-right: 10px;">
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
                <a href="/messages">Queues</a> |
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

                <h3>Multiple Messages on One Page</h3>
                <pre>&lt;PA&gt;&lt;FB&gt;&lt;CC&gt;&lt;SJ&gt;Hello World&lt;FP&gt;&lt;FB&gt;&lt;CL&gt;Second Message:&lt;FP&gt;&lt;FP&gt;&lt;FS&gt;&lt;CC&gt;Scrolling Text&lt;FP&gt;
  └─ Each &lt;F?&gt; tag begins a new message on the page
  └─ &lt;FP&gt; holds the preceding text for ~2 seconds</pre>

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
    <title>Faux-Lite Queues</title>
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
        input, select {
            padding: 5px;
            border: 2px solid #000000;
            background: #FFFFFF;
            font-family: Arial, sans-serif;
            font-size: 14px;
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
        <h1>Faux-Lite Queues</h1>
    </div>

    <div class="container">
        <div class="panel">
            <h2>Queues</h2>
            <div class="info">
                <strong>Queues are stored on this device</strong> (up to 100) and written to the
                sign only when run - the running queue always uses <strong>page A</strong>
                (a chained next-queue uses <strong>page B</strong>).<br>
                Messages in a queue are compiled into that single page - each
                <code>&lt;F?&gt;</code> tag starts a new message and the sign loops them natively.<br>
                <strong>Pause After:</strong> seconds to hold each message - converted to
                <code>&lt;FP&gt;</code> tags (~2 seconds each, rounded). Use 0 for no pause.<br>
                Do not include <code>&lt;Px&gt;</code> page tags - they are stripped automatically.
            </div>
            <form id="queueForm">
                <div class="form-group">
                    <label>Queue</label>
                    <div style="display: flex; gap: 5px;">
                        <select id="queueSelect" onchange="selectQueue(this.value)" style="flex: 1; margin-top: 0;"></select>
                        <button type="button" onclick="addQueue()" style="width: auto; margin-top: 0;">+ New</button>
                        <button type="button" class="danger" onclick="removeQueue()" style="width: auto; margin-top: 0;">Delete</button>
                    </div>
                </div>
                <div class="form-group">
                    <label>Name</label>
                    <input type="text" id="queueName" onchange="updateQueueName(this.value)">
                </div>
                <div class="form-group">
                    <label>Sign Page</label>
                    <input type="text" readonly value="Page A when running (page B for chained next-queue)">
                </div>
                <div id="queueContainer"></div>
                <button type="button" onclick="addMessage()" style="margin-bottom: 10px;">+ Add Message</button>
                <div class="form-group">
                    <label>When this queue finishes</label>
                    <select id="finishMode" onchange="updateFinishMode()">
                        <option value="loop">Loop forever</option>
                        <option value="next">Run a set number of times, then switch queues</option>
                    </select>
                </div>
                <div class="form-group" id="finishOptions" style="display: none;">
                    Run <input type="number" id="runTimes" min="1" max="99" style="width: 80px; display: inline-block;" onchange="updateRunTimes(this.value)">
                    times, then switch to
                    <select id="nextQueue" style="width: auto; display: inline-block;" onchange="updateNextQueue(this.value)"></select>
                    <br><small>The queue it switches to then loops forever (only one hop is supported).</small>
                </div>
                <div class="form-group">
                    <label>Run on boot</label>
                    <select id="bootQueue" onchange="bootQueue = parseInt(this.value)"></select>
                </div>
                <button type="submit">Save All</button>
                <div id="saveStatus" class="status"></div>
            </form>
        </div>

        <div class="panel">
            <h2>Schedules</h2>
            <div class="info">
                Sign time: <strong id="signTime">Loading...</strong><br>
                <small>Schedules start a queue at the set time on the selected days.
                Timezone is configured in <a href="/settings" style="color:#0000FF;">Settings</a>.
                Requires internet (NTP) - schedules cannot fire in AP-Only mode.</small>
            </div>
            <div id="scheduleContainer"></div>
            <button type="button" onclick="addSchedule()">+ Add Schedule</button>
        </div>

        <div class="panel">
            <h2>Queue Control</h2>
            <div class="info">
                Active queue: <strong id="activeQueueLabel">Loading...</strong><br>
                <small>Starting writes the selected queue's messages to its page as one
                command - the sign loops them natively. Stopping blanks the display.</small>
            </div>
            <div class="loop-controls">
                <button onclick="startSelected()">Start Selected Queue</button>
                <button onclick="stopQueue()" class="danger">Stop Queue</button>
            </div>
            <div class="form-group" style="margin-top: 15px;">
                <label>Compiled Page Command (selected queue):</label>
                <textarea id="compiledPreview" readonly style="min-height: 0; height: 70px; font-size: 11px;"></textarea>
            </div>
            <div id="loopControlStatus" class="status"></div>
        </div>

        <div class="back-link">
            <a href="/">← Back to Main</a>
        </div>
    </div>

    <script>
        let queues = [];
        let schedules = [];
        let bootQueue = -1;
        let currentQueue = 0;
        let maxQueues = 100;
        const RUN_PAGE = 'A';   // page the running queue is written to
        const CHAIN_PAGE = 'B'; // page the chained next-queue is written to
        const TEST_PAGE = 'Y';  // scratch page for message tests
        const DAY_LABELS = ['S', 'M', 'T', 'W', 'T', 'F', 'S'];

        // Load current settings
        fetch('/api/messages')
            .then(r => r.json())
            .then(data => {
                queues = data.queues || [];
                schedules = data.schedules || [];
                bootQueue = (data.bootQueue === undefined) ? -1 : data.bootQueue;
                maxQueues = data.maxQueues || 25;
                if (queues.length === 0) {
                    queues.push({ name: 'Queue 1', runTimes: 0, nextQueue: -1, messages: [] });
                }
                document.getElementById('signTime').textContent =
                    data.timeSynced ? data.time + ' (' + data.timeZone + ')' : 'not synced yet';
                updateActiveLabel(data.activeQueue);
                renderAll();
            });

        function curQueue() {
            return queues[currentQueue];
        }

        function updateActiveLabel(idx) {
            document.getElementById('activeQueueLabel').textContent =
                (idx >= 0 && queues[idx]) ? queues[idx].name : 'None';
        }

        function renderAll() {
            renderQueueSelect();
            renderQueueEditor();
            renderSchedules();
            renderBootSelect();
            updatePreview();
        }

        function renderQueueSelect() {
            document.getElementById('queueSelect').innerHTML = queues.map((q, i) =>
                `<option value="${i}" ${i === currentQueue ? 'selected' : ''}>${i + 1}. ${q.name}</option>`
            ).join('');
        }

        function renderQueueEditor() {
            const q = curQueue();
            if (!q) return;
            document.getElementById('queueName').value = q.name;

            const chained = q.runTimes > 0 && q.nextQueue >= 0;
            document.getElementById('finishMode').value = chained ? 'next' : 'loop';
            document.getElementById('finishOptions').style.display = chained ? 'block' : 'none';
            document.getElementById('runTimes').value = q.runTimes || 1;

            const nq = document.getElementById('nextQueue');
            nq.innerHTML = queues.map((o, i) =>
                i === currentQueue ? '' :
                `<option value="${i}" ${i === q.nextQueue ? 'selected' : ''}>${o.name}</option>`
            ).join('') || '<option value="-1">No other queues</option>';
            // Keep the model in sync with what the dropdown can actually show
            nq.value = String(q.nextQueue);
            if (nq.value !== String(q.nextQueue)) q.nextQueue = parseInt(nq.value);
            if (isNaN(q.nextQueue)) q.nextQueue = -1;

            renderMessages();
        }

        function renderMessages() {
            const container = document.getElementById('queueContainer');
            container.innerHTML = '';
            const q = curQueue();
            if (!q) return;

            q.messages.forEach((msg, index) => {
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
                    <label style="font-size: 12px;">Message Command:</label>
                    <input type="text" value="${msg.command || ''}" onchange="updateCommand(${index}, this.value)"
                           style="width: 100%; margin-bottom: 5px;" placeholder="&lt;FB&gt;&lt;CC&gt;&lt;SJ&gt;Hello World">
                    <label style="font-size: 12px;">Pause after (seconds, ~2s per &lt;FP&gt;):</label>
                    <input type="number" value="${msg.duration || 0}" min="0"
                           onchange="updateDuration(${index}, this.value)" style="width: 100px;">
                `;
                container.appendChild(div);
            });
        }

        function renderBootSelect() {
            document.getElementById('bootQueue').innerHTML = '<option value="-1">None</option>' +
                queues.map((q, i) =>
                    `<option value="${i}" ${i === bootQueue ? 'selected' : ''}>${q.name}</option>`
                ).join('');
        }

        function renderSchedules() {
            const container = document.getElementById('scheduleContainer');
            container.innerHTML = '';
            schedules.forEach((s, i) => {
                const div = document.createElement('div');
                div.style.cssText = 'border: 2px solid #000; padding: 10px; margin-bottom: 10px; background: #f9f9f9;';
                const hh = String(s.hour).padStart(2, '0') + ':' + String(s.minute).padStart(2, '0');
                let daysHtml = '';
                DAY_LABELS.forEach((lbl, d) => {
                    daysHtml += `<label style="display: inline; margin-right: 8px; font-weight: normal; font-size: 12px;"><input type="checkbox" style="width: auto;" ${(s.days >> d) & 1 ? 'checked' : ''} onchange="toggleDay(${i}, ${d}, this.checked)"> ${lbl}</label>`;
                });
                div.innerHTML = `
                    <div style="display: flex; gap: 5px; flex-wrap: wrap; align-items: center; margin-bottom: 5px;">
                        <select onchange="schedules[${i}].queue = parseInt(this.value)" style="width: auto; flex: 1; min-width: 120px;">
                            <option value="-1" ${s.queue < 0 ? 'selected' : ''}>Choose queue...</option>
                            ${queues.map((q, qi) => `<option value="${qi}" ${qi === s.queue ? 'selected' : ''}>${q.name}</option>`).join('')}
                        </select>
                        <input type="time" value="${hh}" onchange="setScheduleTime(${i}, this.value)" style="width: auto;">
                        <label style="display: inline; font-weight: normal; font-size: 12px;"><input type="checkbox" style="width: auto;" ${s.enabled ? 'checked' : ''} onchange="schedules[${i}].enabled = this.checked"> On</label>
                        <button type="button" onclick="removeSchedule(${i})" style="width: auto; padding: 3px 8px; margin-top: 0; background: #ff0000;">X</button>
                    </div>
                    <div>${daysHtml}</div>
                `;
                container.appendChild(div);
            });
        }

        // Sync the visible queue editor inputs back into the model
        function syncQueueInputs() {
            const q = curQueue();
            if (!q) return;
            q.name = document.getElementById('queueName').value;
            q.runTimes = parseInt(document.getElementById('runTimes').value) || 0;
            q.nextQueue = parseInt(document.getElementById('nextQueue').value);
            if (isNaN(q.nextQueue)) q.nextQueue = -1;
            if (document.getElementById('finishMode').value === 'loop') {
                q.runTimes = 0;
                q.nextQueue = -1;
            }
        }

        function selectQueue(i) {
            syncQueueInputs();
            currentQueue = parseInt(i);
            renderAll();
        }

        function addQueue() {
            if (queues.length >= maxQueues) {
                alert('Maximum ' + maxQueues + ' queues');
                return;
            }
            syncQueueInputs();
            queues.push({ name: 'Queue ' + (queues.length + 1), runTimes: 0, nextQueue: -1, messages: [] });
            currentQueue = queues.length - 1;
            renderAll();
        }

        function removeQueue() {
            if (queues.length <= 1) {
                alert('At least one queue is required');
                return;
            }
            if (!confirm('Delete "' + curQueue().name + '"?')) return;
            queues.splice(currentQueue, 1);
            // Fix index references to the deleted queue
            queues.forEach(q => {
                if (q.nextQueue === currentQueue) q.nextQueue = -1;
                else if (q.nextQueue > currentQueue) q.nextQueue--;
            });
            schedules.forEach(s => {
                if (s.queue === currentQueue) s.queue = -1;
                else if (s.queue > currentQueue) s.queue--;
            });
            if (bootQueue === currentQueue) bootQueue = -1;
            else if (bootQueue > currentQueue) bootQueue--;
            if (currentQueue >= queues.length) currentQueue = queues.length - 1;
            renderAll();
        }

        function updateQueueName(v) {
            curQueue().name = v;
            renderQueueSelect();
            renderBootSelect();
            renderSchedules(); // queue names appear in schedule dropdowns
        }

        function updateFinishMode() {
            const chained = document.getElementById('finishMode').value === 'next';
            document.getElementById('finishOptions').style.display = chained ? 'block' : 'none';
            syncQueueInputs();
            updatePreview();
        }

        function updateRunTimes(v) {
            const d = parseInt(v);
            curQueue().runTimes = isNaN(d) ? 1 : Math.min(99, Math.max(1, d));
            updatePreview();
        }

        function updateNextQueue(v) {
            curQueue().nextQueue = parseInt(v);
            updatePreview();
        }

        function addMessage() {
            curQueue().messages.push({ command: '', duration: 4 });
            renderMessages();
            updatePreview();
        }

        function removeMessage(index) {
            curQueue().messages.splice(index, 1);
            renderMessages();
            updatePreview();
        }

        function updateCommand(index, value) {
            curQueue().messages[index].command = value;
            updatePreview();
        }

        function updateDuration(index, value) {
            const d = parseInt(value);
            curQueue().messages[index].duration = isNaN(d) ? 4 : Math.max(0, d);
            updatePreview();
        }

        function setScheduleTime(i, v) {
            const parts = v.split(':');
            schedules[i].hour = parseInt(parts[0]) || 0;
            schedules[i].minute = parseInt(parts[1]) || 0;
        }

        function toggleDay(i, d, on) {
            if (on) schedules[i].days |= (1 << d);
            else schedules[i].days &= ~(1 << d);
        }

        function addSchedule() {
            schedules.push({ queue: -1, hour: 18, minute: 0, days: 127, enabled: true });
            renderSchedules();
        }

        function removeSchedule(i) {
            schedules.splice(i, 1);
            renderSchedules();
        }

        function buildCompiledCommand() {
            const q = curQueue();
            if (!q) return '';
            let cmd = '<P' + RUN_PAGE + '>';
            q.messages.forEach(m => {
                const pauses = Math.max(0, Math.round((m.duration || 0) / 2));
                const seg = (m.command || '').replace(/^<P[A-Z]>/, '');
                cmd += seg + '<FP>'.repeat(pauses);
            });
            let run = '<RP' + RUN_PAGE + '>';
            if (q.runTimes > 0 && q.nextQueue >= 0 && queues[q.nextQueue]) {
                run = '<RP' + RUN_PAGE + '><' +
                      String(q.runTimes).padStart(2, '0') + '><RP' + CHAIN_PAGE + '>';
            }
            return cmd + '\n' + run;
        }

        function updatePreview() {
            document.getElementById('compiledPreview').value = buildCompiledCommand();
        }

        async function saveAll() {
            syncQueueInputs();
            return await fetch('/api/messages', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ queues, schedules, bootQueue })
            });
        }

        async function testMessage(index) {
            const msg = curQueue().messages[index];
            if (!msg.command) {
                alert('Please enter a command first');
                return;
            }

            const pauses = Math.max(0, Math.round((msg.duration || 0) / 2));
            const seg = msg.command.replace(/^<P[A-Z]>/, '');
            try {
                await fetch('/api/send', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ command: '<P' + TEST_PAGE + '>' + seg + '<FP>'.repeat(pauses) })
                });
                const run = await fetch('/api/send', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ command: '<RP' + TEST_PAGE + '>' })
                });
                if (!run.ok) alert('Failed to send test message');
            } catch (err) {
                alert('Error: ' + err.message);
            }
        }

        document.getElementById('queueForm').addEventListener('submit', async (e) => {
            e.preventDefault();
            const status = document.getElementById('saveStatus');

            try {
                const response = await saveAll();
                if (response.ok) {
                    status.className = 'status success';
                    status.textContent = 'Saved!';
                } else {
                    status.className = 'status error';
                    status.textContent = 'Failed to save';
                }
            } catch (err) {
                status.className = 'status error';
                status.textContent = 'Error: ' + err.message;
            }
        });

        async function startSelected() {
            const status = document.getElementById('loopControlStatus');
            if (!curQueue() || curQueue().messages.length === 0) {
                status.className = 'status error';
                status.textContent = 'Selected queue has no messages';
                return;
            }
            try {
                // Save first so the sign runs what you see in the editor
                const save = await saveAll();
                if (!save.ok) {
                    status.className = 'status error';
                    status.textContent = 'Failed to save queue';
                    return;
                }
                const response = await fetch('/api/loop/start?queue=' + currentQueue, { method: 'POST' });
                if (response.ok) {
                    status.className = 'status success';
                    status.textContent = 'Queue started!';
                    updateActiveLabel(currentQueue);
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
                    updateActiveLabel(-1);
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
        <img src=")rawliteral" + logoUri + R"rawliteral(" alt="Icon" style="width: 86px; height: 40px; margin-right: 10px;">
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
                <div class="form-group">
                    <label>Timezone (for queue scheduling)</label>
                    <select id="tzPreset" name="tzPreset" onchange="tzPresetChanged()">
                        <option value="">Custom</option>
                        <option value="UTC0">UTC</option>
                        <option value="EST5EDT,M3.2.0,M11.1.0">US Eastern</option>
                        <option value="CST6CDT,M3.2.0,M11.1.0">US Central</option>
                        <option value="MST7MDT,M3.2.0,M11.1.0">US Mountain</option>
                        <option value="PST8PDT,M3.2.0,M11.1.0">US Pacific</option>
                        <option value="AKST9AKDT,M3.2.0,M11.1.0">US Alaska</option>
                        <option value="HST10">US Hawaii</option>
                        <option value="GMT0BST,M3.5.0/1,M10.5.0">UK</option>
                        <option value="CET-1CEST,M3.5.0,M10.5.0/3">Central Europe</option>
                        <option value="AEST-10AEDT,M10.1.0,M4.1.0/3">Australia Eastern</option>
                    </select>
                    <input type="text" id="timeZone" name="timeZone" placeholder="POSIX TZ string" style="margin-top: 5px;">
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
        document.getElementById('timeZone').value = ')rawliteral" + timeZone + R"rawliteral(';
        (function() {
            const sel = document.getElementById('tzPreset');
            const cur = document.getElementById('timeZone').value;
            for (const opt of sel.options) {
                if (opt.value && opt.value === cur) { sel.value = opt.value; break; }
            }
        })();

        function tzPresetChanged() {
            const v = document.getElementById('tzPreset').value;
            if (v) document.getElementById('timeZone').value = v;
        }

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
            const timeZone = document.getElementById('timeZone').value;

            try {
                const response = await fetch('/api/settings', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({
                        signID,
                        baudRate: parseInt(baudRate),
                        startBlank,
                        apOnlyMode,
                        timeZone
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

            // Commands that write, run, or delete pages supersede the running queue
            if (command.indexOf("<P") >= 0 || command.indexOf("<R") >= 0 || command.indexOf("<D") >= 0) {
                activeQueue = -1;
            }

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
            String newTimeZone = doc["timeZone"].as<String>();
            if (newTimeZone.isEmpty()) newTimeZone = "UTC0";

            preferences.begin("faux-lite", false);
            preferences.putString("signID", newSignID);
            preferences.putInt("baudRate", newBaudRate);
            preferences.putBool("startBlank", newStartBlank);
            preferences.putBool("apOnlyMode", newApOnlyMode);
            preferences.putString("timeZone", newTimeZone);
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
        doc["timezone"] = timeZone;
        doc["time"] = getLocalTimeString();

        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response); });

    // API endpoint for queue/schedule list GET
    server.on("/api/messages", HTTP_GET, [](AsyncWebServerRequest *request)
              {
        DynamicJsonDocument doc(32768);
        serializeQueuesTo(doc);

        doc["activeQueue"] = activeQueue;
        doc["maxQueues"] = MAX_QUEUES;
        doc["runPage"] = String(RUN_PAGE);
        doc["chainPage"] = String(CHAIN_PAGE);
        doc["time"] = getLocalTimeString();
        doc["timeSynced"] = timeSynced;
        doc["timeZone"] = timeZone;

        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response); });

    // API endpoint for queues/schedules POST
    server.on("/api/messages", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
              {
            // Accumulate the body - large queue sets arrive in multiple chunks
            String *body;
            if (index == 0) {
                request->_tempObject = new String();
                ((String *)request->_tempObject)->reserve(total);
            }
            body = (String *)request->_tempObject;
            body->concat((const char *)data, len);
            if (index + len < total)
                return;

            DynamicJsonDocument doc(32768);
            DeserializationError error = deserializeJson(doc, *body);
            delete body;
            request->_tempObject = NULL;

            if (error) {
                request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
                return;
            }

            parseQueuesJson(doc);
            saveQueues();

            request->send(200, "application/json", "{\"status\":\"success\"}"); });

    // API endpoint to start a queue (?queue=N, default 0)
    server.on("/api/loop/start", HTTP_POST, [](AsyncWebServerRequest *request)
              {
        int index = 0;
        if (request->hasParam("queue")) {
            index = request->getParam("queue")->value().toInt();
        }
        if (index >= 0 && index < (int)queues.size() && queues[index].messages.size() > 0) {
            startQueue(index);
            request->send(200, "application/json", "{\"status\":\"success\"}");
        } else {
            request->send(400, "application/json", "{\"error\":\"Queue empty or invalid\"}");
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
    String command = "<PA><C" + color + "><F" + function + ">" + text + " ";
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

String buildQueuePageCommand(int index, char page)
{
    // A single page can hold multiple messages: each <Fx> tag starts a new
    // message, and <FP> holds the preceding text for ~2 seconds.
    String command = "<P" + String(page) + ">";
    for (size_t i = 0; i < queues[index].messages.size(); i++)
    {
        String segment = queues[index].messages[i].command;
        // Strip a leading page tag if present - segments share a single page
        if (segment.length() >= 4 && segment[0] == '<' && segment[1] == 'P' &&
            segment[2] >= 'A' && segment[2] <= 'Z' && segment[3] == '>')
        {
            segment = segment.substring(4);
        }
        command += segment;
        int pauses = (queues[index].messages[i].duration + 1) / 2; // ~2s per <FP>
        for (int p = 0; p < pauses; p++)
        {
            command += "<FP>";
        }
    }
    return command;
}

void startQueue(int index)
{
    if (index < 0 || index >= (int)queues.size() || queues[index].messages.size() == 0)
    {
        Serial.println("Queue empty or invalid, nothing to run");
        return;
    }

    MessageQueue &q = queues[index];
    bool validNext = q.runTimes > 0 && q.nextQueue >= 0 &&
                     q.nextQueue < (int)queues.size() && q.nextQueue != index &&
                     queues[q.nextQueue].messages.size() > 0;

    // Write this queue to the run page (and the chain target to its page)
    sendToProlite(buildQueuePageCommand(index, RUN_PAGE));
    delay(100);
    if (validNext)
    {
        sendToProlite(buildQueuePageCommand(q.nextQueue, CHAIN_PAGE));
        delay(100);
    }

    // Run the page; optionally loop N times then switch to the chained queue
    String run = "<RP" + String(RUN_PAGE) + ">";
    if (validNext)
    {
        String nn = String(q.runTimes);
        if (q.runTimes < 10)
            nn = "0" + nn;
        run += "<" + nn + "><RP" + String(CHAIN_PAGE) + ">";
    }
    sendToProlite(run);

    activeQueue = index;
    Serial.printf("Queue %d ('%s') started: %d messages\n",
                  index, q.name.c_str(), q.messages.size());
}

void stopQueue()
{
    activeQueue = -1;
    blankDisplay();
    Serial.println("Queue stopped");
}

void checkSchedules()
{
    // Wait for NTP sync before evaluating schedules
    if (!timeSynced)
    {
        if (time(nullptr) > 1700000000)
        {
            timeSynced = true;
            Serial.printf("Time synced: %s\n", getLocalTimeString().c_str());
        }
        else
        {
            return;
        }
    }
    if (schedules.empty())
        return;

    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);

    // Evaluate at most once per minute
    if (t.tm_min == lastCheckedMinute)
        return;
    lastCheckedMinute = t.tm_min;

    for (size_t i = 0; i < schedules.size(); i++)
    {
        QueueSchedule &s = schedules[i];
        if (!s.enabled || s.queue < 0 || s.queue >= (int)queues.size())
            continue;
        if (!(s.days & (1 << t.tm_wday)))
            continue;
        if (s.hour == t.tm_hour && s.minute == t.tm_min)
        {
            Serial.printf("Schedule fired: queue %d at %02d:%02d\n", s.queue, s.hour, s.minute);
            startQueue(s.queue);
            break;
        }
    }
}

String getLocalTimeString()
{
    struct tm t;
    if (!getLocalTime(&t, 10))
        return "not synced";
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
    return String(buf);
}

void serializeQueuesTo(JsonDocument &doc)
{
    JsonArray queuesArray = doc.createNestedArray("queues");
    for (size_t i = 0; i < queues.size(); i++)
    {
        JsonObject q = queuesArray.createNestedObject();
        q["name"] = queues[i].name;
        q["runTimes"] = queues[i].runTimes;
        q["nextQueue"] = queues[i].nextQueue;
        JsonArray msgs = q.createNestedArray("messages");
        for (size_t j = 0; j < queues[i].messages.size(); j++)
        {
            JsonObject m = msgs.createNestedObject();
            m["command"] = queues[i].messages[j].command;
            m["duration"] = queues[i].messages[j].duration;
        }
    }

    JsonArray schedsArray = doc.createNestedArray("schedules");
    for (size_t i = 0; i < schedules.size(); i++)
    {
        JsonObject s = schedsArray.createNestedObject();
        s["queue"] = schedules[i].queue;
        s["hour"] = schedules[i].hour;
        s["minute"] = schedules[i].minute;
        s["days"] = schedules[i].days;
        s["enabled"] = schedules[i].enabled;
    }

    doc["bootQueue"] = bootQueue;
}

bool parseQueuesJson(JsonDocument &doc)
{
    JsonArray queuesArray = doc["queues"];

    queues.clear();
    for (JsonVariant v : queuesArray)
    {
        if (queues.size() >= MAX_QUEUES)
            break;
        JsonObject qj = v.as<JsonObject>();
        MessageQueue q;
        q.name = qj["name"].as<String>();
        if (q.name.isEmpty())
            q.name = "Queue " + String(queues.size() + 1);
        q.runTimes = qj["runTimes"] | 0;
        if (q.runTimes < 0)
            q.runTimes = 0;
        if (q.runTimes > 99)
            q.runTimes = 99;
        q.nextQueue = qj["nextQueue"] | -1;
        JsonArray msgs = qj["messages"];
        for (JsonVariant mv : msgs)
        {
            JsonObject mj = mv.as<JsonObject>();
            QueueMessage m;
            m.command = mj["command"].as<String>();
            m.duration = mj["duration"] | 4;
            if (m.duration < 0)
                m.duration = 0;
            q.messages.push_back(m);
        }
        queues.push_back(q);
    }

    schedules.clear();
    JsonArray schedsArray = doc["schedules"];
    for (JsonVariant v : schedsArray)
    {
        if (schedules.size() >= MAX_SCHEDULES)
            break;
        JsonObject sj = v.as<JsonObject>();
        QueueSchedule s;
        s.queue = sj["queue"] | -1;
        s.hour = sj["hour"] | 0;
        s.minute = sj["minute"] | 0;
        s.days = sj["days"] | 0;
        s.enabled = sj["enabled"] | true;
        if (s.hour < 0)
            s.hour = 0;
        if (s.hour > 23)
            s.hour = 23;
        if (s.minute < 0)
            s.minute = 0;
        if (s.minute > 59)
            s.minute = 59;
        schedules.push_back(s);
    }

    bootQueue = doc["bootQueue"] | -1;
    if (bootQueue >= (int)queues.size())
        bootQueue = -1;
    if (activeQueue >= (int)queues.size())
        activeQueue = -1;

    return true;
}

void saveQueues()
{
    File f = LittleFS.open(QUEUES_FILE, "w");
    if (!f)
    {
        Serial.println("Failed to open " QUEUES_FILE " for writing");
        return;
    }

    DynamicJsonDocument doc(32768);
    serializeQueuesTo(doc);
    serializeJson(doc, f);
    f.close();
    Serial.printf("Saved %d queues and %d schedules to " QUEUES_FILE "\n", queues.size(), schedules.size());
}

void loadQueues()
{
    queues.clear();
    schedules.clear();
    bootQueue = -1;

    if (LittleFS.exists(QUEUES_FILE))
    {
        File f = LittleFS.open(QUEUES_FILE, "r");
        DynamicJsonDocument doc(32768);
        DeserializationError err = deserializeJson(doc, f);
        f.close();
        if (err)
        {
            Serial.printf("Failed to parse " QUEUES_FILE ": %s\n", err.c_str());
            return;
        }
        parseQueuesJson(doc);
        Serial.printf("Loaded %d queues and %d schedules from " QUEUES_FILE "\n", queues.size(), schedules.size());
        return;
    }

    // No stored file - migrate queue data saved in NVS by older firmware
    preferences.begin("faux-lite", true);

    int numQueues = preferences.getInt("numQueues", 0);
    for (int i = 0; i < numQueues; i++)
    {
        MessageQueue q;
        String base = "q" + String(i);
        q.name = preferences.getString((base + "n").c_str(), "Queue " + String(i + 1));
        q.runTimes = preferences.getInt((base + "r").c_str(), 0);
        q.nextQueue = preferences.getInt((base + "x").c_str(), -1);
        int numMsgs = preferences.getInt((base + "m").c_str(), 0);
        for (int j = 0; j < numMsgs; j++)
        {
            String mbase = base + "m" + String(j);
            QueueMessage m;
            m.command = preferences.getString((mbase + "c").c_str(), "");
            m.duration = preferences.getInt((mbase + "d").c_str(), 4);
            if (!m.command.isEmpty())
                q.messages.push_back(m);
        }
        queues.push_back(q);
    }

    // Even older firmware: single software-timed queue
    int oldSize = preferences.getInt("queueSize", 0);
    if (queues.empty() && oldSize > 0)
    {
        MessageQueue q;
        q.name = "Default";
        for (int i = 0; i < oldSize; i++)
        {
            QueueMessage m;
            m.command = preferences.getString(("qCmd" + String(i)).c_str(), "");
            m.duration = preferences.getInt(("qDur" + String(i)).c_str(), 4);
            if (!m.command.isEmpty())
                q.messages.push_back(m);
        }
        if (!q.messages.empty())
            queues.push_back(q);
    }

    bootQueue = preferences.getInt("bootQueue", -1);
    // Migrate old runOnBoot flag: run the first queue
    if (bootQueue == -1 && preferences.getBool("runOnBoot", false) && !queues.empty())
        bootQueue = 0;
    if (bootQueue >= (int)queues.size())
        bootQueue = -1;

    int numScheds = preferences.getInt("numScheds", 0);
    for (int i = 0; i < numScheds && i < MAX_SCHEDULES; i++)
    {
        QueueSchedule s;
        String base = "s" + String(i);
        s.queue = preferences.getInt((base + "q").c_str(), -1);
        s.hour = preferences.getInt((base + "h").c_str(), 0);
        s.minute = preferences.getInt((base + "m").c_str(), 0);
        s.days = preferences.getUChar((base + "d").c_str(), 0);
        s.enabled = preferences.getBool((base + "e").c_str(), true);
        schedules.push_back(s);
    }

    preferences.end();

    if (!queues.empty() || !schedules.empty())
    {
        Serial.println("Migrated queue data from NVS to LittleFS");
        saveQueues();
    }

    Serial.printf("Loaded %d queues and %d schedules\n", queues.size(), schedules.size());
}
