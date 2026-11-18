# CardputerZero (CP0) device family: a Linux/aarch64 framebuffer+evdev+ALSA target.
# Built host-side for the P0 bring-up (shares the simulator facade); the on-device drivers
# (fbdev / evdev / ALSA) are added behind CP0_DEVICE guards. Host builds present headlessly via CP0Shot.
set(DEVICE_SIMULATOR 1)
