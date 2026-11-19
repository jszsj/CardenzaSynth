#pragma once

// USB-MIDI keyboard input for the CP0 native host.
//
// A class-compliant USB-MIDI keyboard plugged into the CP0's USB host port shows up as an ALSA
// rawmidi device (snd-usb-audio). This reader opens it, parses the standard MIDI byte stream into
// 4-byte USB-MIDI event packets, and feeds them into the same queue the firmware already consumes
// ("a computer plays the device over USB", GrooveApp::ReadComputerMidi) via CP0::Input::
// SendComputerMidi(). GrooveApp maps MIDI channel 1-4 -> synth tracks 1-4 and calls
// synth::audio::NoteOn(), so a plugged-in keyboard just plays the synth. Hotplug is handled by
// scanning /proc/asound/cards.

namespace CP0 {
namespace UsbMidi {

void Start();  // spawns the background reader thread (idempotent)
void Stop();
// Turns the CP0 panel back on if something dimmed it (backlight 0 / framebuffer blanked). The
// system "screen off" timer only sees physical console input, so a keyboard played over USB never
// wakes the panel — a MIDI message calls this so playing keeps the screen on. Cheap and idempotent:
// it only writes when the panel is actually off.
void WakePanel();

}  // namespace UsbMidi
}  // namespace CP0
