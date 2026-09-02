var SETTINGS_KEY = 'fwordle_settings';

// Replace this with the HTTPS address where you host config/index.html.
var CONFIG_URL = 'https://furchee.com/wordle/fwordle-config.html';

function loadSettings() {
  try {
    return JSON.parse(localStorage.getItem(SETTINGS_KEY)) || { hardMode: false };
  } catch (error) {
    return { hardMode: false };
  }
}

function sendSettings() {
  var settings = loadSettings();
  var message = {};

  message[0] = settings.hardMode ? 1 : 0;

  Pebble.sendAppMessage(message);
}

Pebble.addEventListener('ready', function() {
  sendSettings();
});

Pebble.addEventListener('showConfiguration', function() {
  var settings = loadSettings();
  Pebble.openURL(CONFIG_URL + '?hardMode=' + (settings.hardMode ? '1' : '0'));
});

Pebble.addEventListener('webviewclosed', function(event) {
  if (!event.response) {
    return;
  }

  try {
    var settings = JSON.parse(decodeURIComponent(event.response));
    settings.hardMode = Boolean(settings.hardMode);
    localStorage.setItem(SETTINGS_KEY, JSON.stringify(settings));
    sendSettings();
  } catch (error) {
    console.log('Could not parse configuration: ' + error.message);
  }
});
