#include "web_server.h"

void handleRoot(WebServer &server)
{
    String html = R"DELIMITER(
<!DOCTYPE html>
<html>
<head>
    <title>ESP32-S3 LED Control</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { font-family: Arial; text-align: center; margin: 20px; background-color: #f0f0f0; }
        .container { max-width: 600px; margin: 0 auto; background-color: white; padding: 20px; border-radius: 8px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }
        h1 { color: #333; }
        button { padding: 12px 24px; margin: 10px; font-size: 16px; cursor: pointer; background-color: #4CAF50; color: white; border: none; border-radius: 4px; }
        button:hover { background-color: #45a049; }
        .led-group { margin: 20px 0; padding: 15px; border: 1px solid #ddd; border-radius: 4px; }
        #status { padding: 10px; background-color: #f9f9f9; border: 1px solid #ddd; border-radius: 4px; word-wrap: break-word; }
    </style>
</head>
<body>
    <div class="container">
        <h1>ESP32-S3 LED Control</h1>
        <div class="led-group">
            <h2>LED Control</h2>
            <button onclick="turnOnLED()">All LEDs ON</button>
            <button onclick="turnOffLED()">All LEDs OFF</button>
            <p id="status">Loading...</p>
        </div>
    </div>
    <script>
        function turnOnLED() {
            fetch('/api/led/on').then(function() { updateStatus(); });
        }
        function turnOffLED() {
            fetch('/api/led/off').then(function() { updateStatus(); });
        }
        function updateStatus() {
            fetch('/api/leds').then(function(response) { 
                return response.json(); 
            }).then(function(data) {
                document.getElementById('status').innerText = JSON.stringify(data, null, 2);
            });
        }
        updateStatus();
        setInterval(updateStatus, 2000);
    </script>
</body>
</html>
)DELIMITER";
    server.send(200, "text/html", html);
}
