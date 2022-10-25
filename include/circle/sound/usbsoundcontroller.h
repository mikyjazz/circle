//
// usbsoundcontroller.h
//
// Circle - A C++ bare metal environment for Raspberry Pi
// Copyright (C) 2022  R. Stange <rsta2@o2online.de>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
#ifndef _circle_sound_usbsoundcontroller_h
#define _circle_sound_usbsoundcontroller_h

#include <circle/sound/soundcontroller.h>
#include <circle/usb/usbaudiostreaming.h>
#include <circle/types.h>

class CUSBSoundBaseDevice;

class CUSBSoundController : public CSoundController	/// Sound controller for USB sound devices
{
public:
	CUSBSoundController (CUSBSoundBaseDevice *pSoundDevice);
	~CUSBSoundController (void);

	boolean Probe (void) override;

	void SelectOutput (TOutputSelector Selector) override;

	void SetOutputVolume (int ndB) override;

	const TRange GetOutputVolumeRange (void) const override;

private:
	// returns matching priority with selector (0..N, 0: best)
	static const unsigned NoMatch = 99;
	unsigned MatchTerminalType (u16 usTerminalType, TOutputSelector Selector);

private:
	CUSBSoundBaseDevice *m_pSoundDevice;
	unsigned m_nInterface;

	CUSBAudioStreamingDevice *m_pStreamingDevice;
	CUSBAudioStreamingDevice::TFormatInfo m_FormatInfo;
};

#endif