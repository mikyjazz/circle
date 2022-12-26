//
// usbsoundbasedevice.cpp
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
#include <circle/sound/usbsoundbasedevice.h>
#include <circle/devicenameservice.h>
#include <circle/sched/scheduler.h>
#include <circle/sysconfig.h>
#include <circle/string.h>
#include <circle/koptions.h>
#include <circle/logger.h>
#include <circle/atomic.h>
#include <circle/timer.h>
#include <assert.h>

LOGMODULE ("sndusb");
static const char DeviceName[] = "sndusb";

CUSBSoundBaseDevice::CUSBSoundBaseDevice (unsigned nSampleRate, TDeviceMode DeviceMode,
					  unsigned nDevice)
:	CSoundBaseDevice (  CKernelOptions::Get ()->GetSoundOption () == 24
			  ? SoundFormatSigned24 : SoundFormatSigned16,
			  0, nSampleRate),
	m_nBitResolution (CKernelOptions::Get ()->GetSoundOption () == 24 ? 24 : 16),
	m_nSubframeSize (m_nBitResolution / 8),
	m_nSampleRate (nSampleRate),
	m_DeviceMode (DeviceMode),
	m_nDevice (nDevice),
	m_nTXInterface (0),
	m_State (StateCreated),
	m_pTXUSBDevice (nullptr),
	m_pRXUSBDevice (nullptr),
	m_pTXBuffer {nullptr, nullptr},
	m_pRXBuffer {nullptr, nullptr},
	m_pSoundController (nullptr),
	m_hRemoveRegistration (0)
{
	if (!m_nDevice)
	{
		CDeviceNameService::Get ()->AddDevice (DeviceName, this, FALSE);
	}
	else
	{
		CDeviceNameService::Get ()->AddDevice (DeviceName, m_nDevice, this, FALSE);
	}
}

CUSBSoundBaseDevice::~CUSBSoundBaseDevice (void)
{
	assert (   m_State == StateCreated
		|| m_State == StateIdle);
	m_State = StateUnknown;

	if (!m_nDevice)
	{
		CDeviceNameService::Get ()->RemoveDevice (DeviceName, FALSE);
	}
	else
	{
		CDeviceNameService::Get ()->RemoveDevice (DeviceName, m_nDevice, FALSE);
	}

	if (m_hRemoveRegistration)
	{
		assert (m_pTXUSBDevice || m_pRXUSBDevice);
		(m_pTXUSBDevice ? m_pTXUSBDevice : m_pRXUSBDevice)->
			UnregisterRemovedHandler (m_hRemoveRegistration);

		m_hRemoveRegistration = 0;
	}

	delete m_pSoundController;
	m_pSoundController = nullptr;

	delete [] m_pTXBuffer[0];
	delete [] m_pTXBuffer[1];
	m_pTXBuffer[0] = nullptr;
	m_pTXBuffer[1] = nullptr;

	delete [] m_pRXBuffer[0];
	delete [] m_pRXBuffer[1];
	m_pRXBuffer[0] = nullptr;
	m_pRXBuffer[1] = nullptr;
}

int CUSBSoundBaseDevice::GetRangeMin (void) const
{
	return -32768;
}

int CUSBSoundBaseDevice::GetRangeMax (void) const
{
	return 32767;
}

boolean CUSBSoundBaseDevice::Start (void)
{
	assert (   m_State == StateCreated
		|| m_State == StateIdle);
	if (m_State == StateCreated)
	{
		if (!m_pSoundController)
		{
			m_pSoundController = new CUSBSoundController (
							this,
							m_DeviceMode != DeviceModeRXOnly,
							m_DeviceMode != DeviceModeTXOnly,
							m_nDevice);
			assert (m_pSoundController);

			if (!m_pSoundController->Probe ())
			{
				LOGWARN ("Probing sound controller failed");

				delete m_pSoundController;
				m_pSoundController = nullptr;

				return FALSE;
			}
		}

		// first do the initializations, which could fail

		if (m_DeviceMode != DeviceModeRXOnly)
		{
			m_pTXUSBDevice = GetStreamingDevice (TRUE, m_nTXInterface);
			if (!m_pTXUSBDevice)
			{
				return FALSE;
			}

			if (!m_pTXUSBDevice->Setup (m_nSampleRate))
			{
				LOGWARN ("USB audio device setup failed");

				m_pTXUSBDevice = nullptr;

				return FALSE;
			}
		}

		if (m_DeviceMode != DeviceModeTXOnly)
		{
			m_pRXUSBDevice = GetStreamingDevice (FALSE, 0);
			if (!m_pRXUSBDevice)
			{
				return FALSE;
			}

			if (!m_pRXUSBDevice->Setup (m_nSampleRate))
			{
				LOGWARN ("USB audio device setup failed");

				m_pRXUSBDevice = nullptr;

				return FALSE;
			}
		}

		// now do the other initializations

		if (m_DeviceMode != DeviceModeRXOnly)
		{
			m_nTXChunkSizeBytes = m_pTXUSBDevice->GetChunkSizeBytes ();

			// The actual chunk size varies in operation. A maximum of twice
			// the initial size should not be exceeded.
			assert (!m_pTXBuffer[0]);
			assert (!m_pTXBuffer[1]);
			m_pTXBuffer[0] = new u8[m_nTXChunkSizeBytes * 2];
			m_pTXBuffer[1] = new u8[m_nTXChunkSizeBytes * 2];
		}

		if (m_DeviceMode != DeviceModeTXOnly)
		{
			m_nRXChunkSizeBytes = m_pRXUSBDevice->GetChunkSizeBytes ();

			// The actual chunk size varies in operation. A maximum of twice
			// the initial size should not be exceeded.
			assert (!m_pRXBuffer[0]);
			assert (!m_pRXBuffer[1]);
			m_pRXBuffer[0] = new u8[m_nRXChunkSizeBytes * 2];
			m_pRXBuffer[1] = new u8[m_nRXChunkSizeBytes * 2];

			CUSBAudioStreamingDevice::TDeviceInfo Info =
				m_pRXUSBDevice->GetDeviceInfo ();
			m_nRXChannels = Info.NumChannels;
		}

		assert (!m_hRemoveRegistration);
		assert (m_pTXUSBDevice || m_pRXUSBDevice);
		m_hRemoveRegistration = (m_pTXUSBDevice ? m_pTXUSBDevice : m_pRXUSBDevice)->
					RegisterRemovedHandler (DeviceRemovedStub, this);
		assert (m_hRemoveRegistration);

		m_State = StateIdle;
	}

	assert (m_State == StateIdle);
	m_State = StateRunning;

	m_nOutstanding = 0;

	if (m_DeviceMode != DeviceModeRXOnly)
	{
		m_nTXCurrentBuffer = 0;

		// Two pending transfers on Raspberry Pi 4,
		// RPi 1-3 and Zero cannot handle this yet.
		if (   !SendChunk ()
#if RASPPI >= 4
		    || !SendChunk ()
#endif
		   )
		{
			LOGWARN ("Cannot send chunk");

			m_State = StateIdle;

			return FALSE;
		}
	}

	if (m_DeviceMode != DeviceModeTXOnly)
	{
		m_nRXCurrentBuffer = 0;

		if (   !ReceiveChunk ()
#if RASPPI >= 4
		    || !ReceiveChunk ()
#endif
		   )
		{
			LOGWARN ("Cannot receive chunk");

			Cancel ();

			while (IsActive ())
			{
#ifdef NO_BUSY_WAIT
				CScheduler::Get ()->Yield ();
#endif
			}

			return FALSE;
		}
	}

	return TRUE;
}

void CUSBSoundBaseDevice::Cancel (void)
{
	m_SpinLock.Acquire ();

	if (m_State == StateRunning)
	{
		m_State = StateCanceled;
	}

	m_SpinLock.Release ();
}

boolean CUSBSoundBaseDevice::IsActive (void) const
{
	return    m_State == StateRunning
	       || m_State == StateCanceled;
}

CSoundController *CUSBSoundBaseDevice::GetController (void)
{
	return m_pSoundController;
}

CUSBAudioStreamingDevice *CUSBSoundBaseDevice::GetStreamingDevice (boolean bTX, unsigned nIndex)
{
	for (unsigned nInterface = 0; TRUE; nInterface++)
	{
		CString USBDeviceName;
		USBDeviceName.Format ("uaudio%u-%u", m_nDevice+1, nInterface+1);

		CDevice *pDevice = CDeviceNameService::Get ()->GetDevice (USBDeviceName, FALSE);
		if (!pDevice)
		{
			LOGWARN ("USB audio streaming device not found (%cX)", bTX ? 'T' : 'R');

			break;
		}

		CUSBAudioStreamingDevice *pUSBDevice =
			static_cast<CUSBAudioStreamingDevice *> (pDevice);

		CUSBAudioStreamingDevice::TDeviceInfo Info = pUSBDevice->GetDeviceInfo ();
		if (Info.IsOutput == bTX)
		{
			if (!nIndex--)
			{
				return pUSBDevice;
			}
		}
	}

	return nullptr;
}

boolean CUSBSoundBaseDevice::SendChunk (void)
{
	assert (m_pTXUSBDevice);
	unsigned nChunkSizeBytes = m_pTXUSBDevice->GetChunkSizeBytes ();
	assert (nChunkSizeBytes);
	assert (nChunkSizeBytes % m_nSubframeSize == 0);
	assert (nChunkSizeBytes <= m_nTXChunkSizeBytes * 2);

	assert (m_nTXCurrentBuffer < 2);
	assert (m_pTXBuffer[m_nTXCurrentBuffer]);
	unsigned nChunkSize;
	if (m_nSubframeSize == 2)
	{
		nChunkSize = GetChunk (reinterpret_cast<s16 *> (m_pTXBuffer[m_nTXCurrentBuffer]),
				       nChunkSizeBytes / m_nSubframeSize);
	}
	else
	{
		assert (m_nSubframeSize == 3);
		nChunkSize = GetChunk (reinterpret_cast<u32 *> (m_pTXBuffer[m_nTXCurrentBuffer]),
				       nChunkSizeBytes / m_nSubframeSize);
	}

	if (!nChunkSize)
	{
		return FALSE;
	}

	AtomicIncrement (&m_nOutstanding);

	if (!m_pTXUSBDevice->SendChunk (m_pTXBuffer[m_nTXCurrentBuffer], nChunkSize * m_nSubframeSize,
				        TXCompletionStub, this))
	{
		AtomicDecrement (&m_nOutstanding);

		return FALSE;
	}

	m_nTXCurrentBuffer ^= 1;

	return TRUE;
}

boolean CUSBSoundBaseDevice::ReceiveChunk (void)
{
	assert (m_pRXUSBDevice);
	unsigned nChunkSizeBytes = m_pRXUSBDevice->GetChunkSizeBytes ();
	assert (nChunkSizeBytes);
	assert (nChunkSizeBytes % m_nSubframeSize == 0);
	assert (nChunkSizeBytes <= m_nRXChunkSizeBytes * 2);

	AtomicIncrement (&m_nOutstanding);

	assert (m_nRXCurrentBuffer < 2);
	assert (m_pRXBuffer[m_nRXCurrentBuffer]);
	if (!m_pRXUSBDevice->ReceiveChunk (m_pRXBuffer[m_nRXCurrentBuffer], nChunkSizeBytes,
					   RXCompletionStub, this))
	{
		AtomicDecrement (&m_nOutstanding);

		return FALSE;
	}

	m_nRXCurrentBuffer ^= 1;

	return TRUE;
}

void CUSBSoundBaseDevice::TXCompletionRoutine (unsigned nBytesTransferred)
{
	boolean bContinue = FALSE;

	m_SpinLock.Acquire ();

	switch (m_State)
	{
	case StateCreated:
		AtomicDecrement (&m_nOutstanding);
		break;

	case StateRunning:
		AtomicDecrement (&m_nOutstanding);
		bContinue = TRUE;
		break;

	case StateCanceled:
		if (!AtomicDecrement (&m_nOutstanding))
		{
			m_State = StateIdle;
		}
		break;

	case StateIdle:
	default:
		assert (0);
		break;
	}

	m_SpinLock.Release ();

	if (!bContinue)
	{
		return;
	}

	if (!SendChunk ())
	{
		LOGWARN ("Cannot send chunk");

		m_State = StateCanceled;
	}
}

void CUSBSoundBaseDevice::TXCompletionStub (unsigned nBytesTransferred, void *pParam)
{
	CUSBSoundBaseDevice *pThis = static_cast<CUSBSoundBaseDevice *> (pParam);
	assert (pThis);

	pThis->TXCompletionRoutine (nBytesTransferred);
}

void CUSBSoundBaseDevice::RXCompletionRoutine (unsigned nBytesTransferred)
{
	boolean bContinue = FALSE;

	m_SpinLock.Acquire ();

	switch (m_State)
	{
	case StateCreated:
		AtomicDecrement (&m_nOutstanding);
		break;

	case StateRunning:
		AtomicDecrement (&m_nOutstanding);
		bContinue = TRUE;
		break;

	case StateCanceled:
		if (!AtomicDecrement (&m_nOutstanding))
		{
			m_State = StateIdle;
		}
		break;

	case StateIdle:
	default:
		assert (0);
		break;
	}

	m_SpinLock.Release ();

	if (!bContinue)
	{
		return;
	}

	if (nBytesTransferred)
	{
		assert (nBytesTransferred % m_nSubframeSize == 0);
		assert (nBytesTransferred <= m_nRXChunkSizeBytes * 2);

		unsigned nRXCurrentBuffer = m_nRXCurrentBuffer ^ 1;
		assert (nRXCurrentBuffer < 2);
		assert (m_pRXBuffer[nRXCurrentBuffer]);

		assert (m_nRXChannels == 1 || m_nRXChannels == 2);
		if (m_nRXChannels == 2)
		{
			if (m_nSubframeSize == 2)
			{
				PutChunk (reinterpret_cast<s16 *> (m_pRXBuffer[nRXCurrentBuffer]),
								   nBytesTransferred / m_nSubframeSize);
			}
			else
			{
				assert (m_nSubframeSize == 3);
				PutChunk (reinterpret_cast<u32 *> (m_pRXBuffer[nRXCurrentBuffer]),
								   nBytesTransferred / m_nSubframeSize);
			}
		}
		else
		{
			// convert Mono to Stereo, because the upper layer expects it
			if (m_nSubframeSize == 2)
			{
				s16 *p = (s16 *) m_pRXBuffer[nRXCurrentBuffer];
				u32 TempBuffer[nBytesTransferred / sizeof (s16)];
				for (unsigned i = 0; i < nBytesTransferred / sizeof (s16); i++)
				{
					u32 nSample = *p++;
					nSample |= nSample << 16;
					TempBuffer[i] = nSample;
				}

				PutChunk (reinterpret_cast<s16 *> (TempBuffer), nBytesTransferred);
			}
			else
			{
				assert (m_nSubframeSize == 3);
				u8 *p = m_pRXBuffer[nRXCurrentBuffer];
				u8 TempBuffer[nBytesTransferred * 2];
				u8 *pp = TempBuffer;
				unsigned nFrames = nBytesTransferred / 3;
				for (unsigned i = 0; i < nFrames; i++)
				{
					pp[0] = pp[3] = p[0];
					pp[1] = pp[4] = p[1];
					pp[2] = pp[5] = p[2];

					p += 3;
					pp += 6;
				}

				PutChunk (reinterpret_cast<u32 *> (TempBuffer), nFrames * 2);
			}
		}
	}

	if (!ReceiveChunk ())
	{
		LOGWARN ("Cannot receive chunk");

		m_State = StateCanceled;
	}
}

void CUSBSoundBaseDevice::RXCompletionStub (unsigned nBytesTransferred, void *pParam)
{
	CUSBSoundBaseDevice *pThis = static_cast<CUSBSoundBaseDevice *> (pParam);
	assert (pThis);

	pThis->RXCompletionRoutine (nBytesTransferred);
}

void CUSBSoundBaseDevice::DeviceRemovedHandler (CDevice *pDevice)
{
	m_hRemoveRegistration = 0;

	Cancel ();

	for (unsigned nStartTicks = CTimer::Get ()->GetTicks (); IsActive ();)
	{
		if (CTimer::Get ()->GetTicks () - nStartTicks > MSEC2HZ (200))
		{
			LOGWARN ("Timeout while going idle");

			m_State = StateIdle;

			break;
		}

#ifdef NO_BUSY_WAIT
		CScheduler::Get ()->Yield ();
#endif
	}

	assert (   m_State == StateCreated
		|| m_State == StateIdle);
	m_State = StateCreated;

	delete m_pSoundController;
	m_pSoundController = nullptr;

	delete [] m_pTXBuffer[0];
	delete [] m_pTXBuffer[1];
	m_pTXBuffer[0] = nullptr;
	m_pTXBuffer[1] = nullptr;

	delete [] m_pRXBuffer[0];
	delete [] m_pRXBuffer[1];
	m_pRXBuffer[0] = nullptr;
	m_pRXBuffer[1] = nullptr;

	m_pTXUSBDevice = nullptr;
	m_pRXUSBDevice = nullptr;
}

void CUSBSoundBaseDevice::DeviceRemovedStub (CDevice *pDevice, void *pContext)
{
	CUSBSoundBaseDevice *pThis = static_cast<CUSBSoundBaseDevice *> (pContext);
	assert (pThis);

	pThis->DeviceRemovedHandler (pDevice);
}

boolean CUSBSoundBaseDevice::SetTXInterface (unsigned nInterface)
{
	boolean bWasActive = IsActive ();
	if (bWasActive)
	{
		Cancel ();

		while (IsActive ())
		{
#ifdef NO_BUSY_WAIT
			CScheduler::Get ()->Yield ();
#endif
		}
	}

	assert (m_State == StateIdle);

	assert (m_hRemoveRegistration);
	assert (m_pTXUSBDevice || m_pRXUSBDevice);
	(m_pTXUSBDevice ? m_pTXUSBDevice : m_pRXUSBDevice)->
		UnregisterRemovedHandler (m_hRemoveRegistration);
	m_hRemoveRegistration = 0;

	delete [] m_pTXBuffer[0];
	delete [] m_pTXBuffer[1];
	m_pTXBuffer[0] = nullptr;
	m_pTXBuffer[1] = nullptr;

	delete [] m_pRXBuffer[0];
	delete [] m_pRXBuffer[1];
	m_pRXBuffer[0] = nullptr;
	m_pRXBuffer[1] = nullptr;

	m_pTXUSBDevice = nullptr;
	m_pRXUSBDevice = nullptr;

	m_State = StateCreated;

	m_nTXInterface = nInterface;

	if (bWasActive)
	{
		return Start ();
	}

	return TRUE;
}
