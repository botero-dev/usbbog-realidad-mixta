/*
 * Network settings for network-test — TEMPLATE
 * --------------------------------------------
 * Copy this file to include/secrets.h:
 *
 *     cp include/secrets.example.h include/secrets.h
 *
 * The board no longer joins a network — it CREATES one (SoftAP):
 *
 *     AP_SSID       the network name you pick from your WiFi menu
 *     AP_PASSWORD   "" for an open network, otherwise 8..63 characters
 *
 * The ESP32 is always 192.168.4.1 on that network, and runs its own DHCP
 * server, so connected devices need no configuration.
 *
 * include/secrets.h is listed in .gitignore, so your password never reaches
 * the repository.
 */
#pragma once

#ifndef AP_SSID
#define AP_SSID "network-test"
#endif

#ifndef AP_PASSWORD
#define AP_PASSWORD "networktest"
#endif
