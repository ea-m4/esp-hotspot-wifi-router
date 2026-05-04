#pragma once
#include <stdbool.h>

/* Initialise WiFi (AP+STA), register event handlers, start the radio.
   Returns immediately; bridge_activate() is called from the event handler
   once the upstream IP is obtained. */
void wifi_bridge_start(void);

/* Current STA link state — read by the web UI to show online/offline. */
bool wifi_bridge_sta_connected(void);
