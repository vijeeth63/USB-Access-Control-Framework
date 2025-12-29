# USB Access Control Kernel Module

### Overview

This project implements a Linux kernel module that decides whether a USB storage device is allowed or blocked at the moment it is plugged in.

It works by intercepting USB device enumeration inside the Linux kernel and applying simple access control rules based on Vendor ID (VID), Product ID (PID), and optional device serial numbers. Unauthorized devices are rejected before the operating system assigns them drivers or storage access.

This project was developed as an operating systems mini-project and serves as an introduction to Linux kernel programming, USB subsystem interaction, and kernel-level security enforcement.

### What This Project Demonstrates (OS Perspective)

How the Linux kernel handles USB device enumeration
How kernel modules can enforce security policies
Kernel ↔ userspace interaction using sysfs
Early rejection of hardware devices (-EACCES)
Safe, minimal kernel design for educational purposes

### Key Features

Per-Device USB Access Control
USB devices are allowed or blocked based on their VID/PID combination.

Kernel-Level Enforcement
Devices are blocked during the probe stage, before userspace access.

Static Rule Loading
Allowed devices can be specified in /etc/usbguard.rules, loaded during module initialization.

Dynamic Rule Management via sysfs
Rules can be added at runtime using /sys/kernel/usbguard/rules without reloading the module.

Serial Number Blocking (Optional)
Specific devices can be blocked using their USB serial descriptors.

Kernel Logging for Auditing
All allow/deny decisions are logged to dmesg.
