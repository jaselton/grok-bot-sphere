#!/bin/zsh
# Sends Wi-Fi credentials to the Grok Sphere over USB. The password is read hidden and passed to Python
# through the environment (not the command line), then over the cable. Nothing is written to disk or logged.
# Find the Sphere by USB product name ("USB JTAG_serial debug unit", the ESP32-S3 native port), or set SPHERE_PORT.
