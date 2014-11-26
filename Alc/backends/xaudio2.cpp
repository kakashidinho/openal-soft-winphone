/**
 * OpenAL cross platform audio library
 * Copyright (C) 2014 by Le Hoang Quyen
 * This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 *  License along with this library; if not, write to the
 *  Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 *  Boston, MA  02111-1307, USA.
 * Or go to http://www.gnu.org/copyleft/lgpl.html
 */

#include "config.h"

#include <stdlib.h>
#include <XAudio2.h>
#include <mmreg.h>
#ifndef _WAVEFORMATEXTENSIBLE_
#include <ks.h>
#include <ksmedia.h>
#endif

#include "alMain.h"
#include "AL/al.h"
#include "AL/alc.h"
#include "thread_msg_queue_cpp11.h"

#define EVENT_ACCESS_MASK (DELETE | SYNCHRONIZE | EVENT_MODIFY_STATE) 

#define MONO SPEAKER_FRONT_CENTER
#define STEREO (SPEAKER_FRONT_LEFT|SPEAKER_FRONT_RIGHT)
#define QUAD (SPEAKER_FRONT_LEFT|SPEAKER_FRONT_RIGHT|SPEAKER_BACK_LEFT|SPEAKER_BACK_RIGHT)
#define X5DOT1 (SPEAKER_FRONT_LEFT|SPEAKER_FRONT_RIGHT|SPEAKER_FRONT_CENTER|SPEAKER_LOW_FREQUENCY|SPEAKER_BACK_LEFT|SPEAKER_BACK_RIGHT)
#define X5DOT1SIDE (SPEAKER_FRONT_LEFT|SPEAKER_FRONT_RIGHT|SPEAKER_FRONT_CENTER|SPEAKER_LOW_FREQUENCY|SPEAKER_SIDE_LEFT|SPEAKER_SIDE_RIGHT)
#define X6DOT1 (SPEAKER_FRONT_LEFT|SPEAKER_FRONT_RIGHT|SPEAKER_FRONT_CENTER|SPEAKER_LOW_FREQUENCY|SPEAKER_BACK_CENTER|SPEAKER_SIDE_LEFT|SPEAKER_SIDE_RIGHT)
#define X7DOT1 (SPEAKER_FRONT_LEFT|SPEAKER_FRONT_RIGHT|SPEAKER_FRONT_CENTER|SPEAKER_LOW_FREQUENCY|SPEAKER_BACK_LEFT|SPEAKER_BACK_RIGHT|SPEAKER_SIDE_LEFT|SPEAKER_SIDE_RIGHT)

#ifndef SAFE_RELEASE
#define SAFE_RELEASE(p) {if (p) {(p)->Release(); (p) = 0;}}
#endif

static const ALCchar xaudio2_device[] = "XAudio2 Default";

class XAudio2SourceVoiceCallback;

struct XAudio2Buffer{
	XAudio2Buffer()
	{
		memset(this, 0, sizeof(XAudio2Buffer));
	}

	BYTE* data;
	size_t bufferSize;
	size_t pendingBufferRegionSize;
	size_t lastWritePosition;//last written position

	ULONG refCount;
};

typedef struct
{
	IXAudio2 * xAudioObj;
	IXAudio2MasteringVoice * masterVoice;
	IXAudio2SourceVoice* sourceVoice;
	XAudio2SourceVoiceCallback* sourceVoiceCallback;

	XAudio2Buffer * buffers;
	size_t numBuffers;

	size_t frameSize;

	HANDLE MsgEvent;

	size_t currentBufferIdx;
	LONG running;
} XAudio2Data;

static MsgQueue_t *g_MsgQueue = nullptr;
static ALvoid* g_MsgQueueThread = nullptr;

typedef struct {
	HANDLE FinishedEvt;
	union {
		HRESULT hresult;
		ALCboolean albresult;
	};
} ThreadRequest;

#define TM_USER_OpenDevice  (WM_USER+0)
#define TM_USER_ResetDevice (WM_USER+1)
#define TM_USER_StopDevice  (WM_USER+2)
#define TM_USER_CloseDevice (WM_USER+3)

static HRESULT WaitForResponseHR(ThreadRequest *req)
{
	if (WaitForSingleObjectEx(req->FinishedEvt, INFINITE, FALSE) == WAIT_OBJECT_0)
		return req->hresult;
	ERR("Message response error: %lu\n", GetLastError());
	return E_FAIL;
}

static ALCboolean WaitForResponseALboolean(ThreadRequest *req)
{
	if (WaitForSingleObjectEx(req->FinishedEvt, INFINITE, FALSE) == WAIT_OBJECT_0)
		return req->albresult;
	return ALC_FALSE;
}

/*--------callback handler class for XAudio2 Source Voice----------*/
class XAudio2SourceVoiceCallback : public  IXAudio2VoiceCallback
{
public:
	XAudio2SourceVoiceCallback(ALCdevice* _device)
	: device(_device){

	}

	STDMETHOD_(void, OnBufferEnd) (void * pBufferContext);
	STDMETHOD_(void, OnVoiceProcessingPassStart)(UINT32 numBytesRequired);

	STDMETHOD_(void, OnBufferStart) (void * pBufferContext) {
	}

	STDMETHOD_(void, OnLoopEnd) (void * pBufferContext) {
	}

	STDMETHOD_(void, OnStreamEnd) () {
	}
	STDMETHOD_(void, OnVoiceProcessingPassEnd) () {
	}

	STDMETHOD_(void, OnVoiceError) (void * pBufferContext, HRESULT Error) {
		//TO DO
	}
private:
	void SubmitBufferRegion(UINT numByteRequired);

	ALCdevice* device;
};

void XAudio2SourceVoiceCallback::SubmitBufferRegion(UINT numBytesRequired)
{
	HRESULT hr;
	XAudio2Data* data = (XAudio2Data*)device->ExtraData;
	auto buffer = data->buffers + data->currentBufferIdx;//buffer to be used
	size_t spaceLeft = buffer->bufferSize - buffer->lastWritePosition;
	size_t bufferRegionSize = min(numBytesRequired, spaceLeft);
	BYTE* bufferRegionData = buffer->data + buffer->lastWritePosition;

	size_t samples = bufferRegionSize / data->frameSize;
	bufferRegionSize = samples * data->frameSize;//must be divisible by frame size

	aluMixData(device, bufferRegionData, samples);//mix data

	buffer->lastWritePosition += bufferRegionSize;

	/*------------submit buffer region-------------*/
	XAUDIO2_BUFFER bufferRegionToSubmit;
	memset(&bufferRegionToSubmit, 0, sizeof(bufferRegionToSubmit));
	bufferRegionToSubmit.AudioBytes = bufferRegionSize;
	bufferRegionToSubmit.pAudioData = bufferRegionData;
	bufferRegionToSubmit.pContext = buffer;

	hr = data->sourceVoice->SubmitSourceBuffer(&bufferRegionToSubmit);
	if (FAILED(hr))
	{
		ERR("SubmitSourceBuffer() failed: 0x%08lx\n", hr);
	}
	else
		buffer->refCount++;
}

/* this is called when xaudio2 engine finished reading a region of data */
COM_DECLSPEC_NOTHROW void STDMETHODCALLTYPE XAudio2SourceVoiceCallback::OnBufferEnd(void * bufferContext)
{
	XAudio2Data* data = (XAudio2Data*)device->ExtraData;
	LONG running;
	InterlockedExchange(&running, data->running);
	if (running)
	{
		auto buffer = (XAudio2Buffer*)bufferContext;
		buffer->refCount--;
		if (buffer->refCount == 0)//buffer is available again
		{
			if (buffer->pendingBufferRegionSize)
			{
				SubmitBufferRegion(buffer->pendingBufferRegionSize);
				buffer->pendingBufferRegionSize = 0;
			}
			else
				buffer->lastWritePosition = 0;
		}
	}//if (isRunning)
}

COM_DECLSPEC_NOTHROW void STDMETHODCALLTYPE XAudio2SourceVoiceCallback::OnVoiceProcessingPassStart(UINT32 numBytesRequired)
{
	XAudio2Data* data = (XAudio2Data*)device->ExtraData;
	LONG running;
	InterlockedExchange(&running, data->running);
	if (running)
	{
		auto buffer = data->buffers + data->currentBufferIdx;
		size_t spaceLeft = buffer->bufferSize - buffer->lastWritePosition;

		if (numBytesRequired > spaceLeft)//not enough space left, jumpt to next buffer
		{
			size_t numTried = 1;
			do//find next available buffer
			{
				data->currentBufferIdx = (data->currentBufferIdx + 1) % data->numBuffers;
				buffer = data->buffers + data->currentBufferIdx;
				spaceLeft = buffer->bufferSize - buffer->lastWritePosition;

				++numTried;
			} while (numTried < data->numBuffers && numBytesRequired > spaceLeft);
		}

		if (numBytesRequired > spaceLeft)//no buffer available
		{
			buffer->lastWritePosition = 0;
			//we must wait for the buffer to be fully used by XAudio2 before submitting data again
			buffer->pendingBufferRegionSize += numBytesRequired;
		}
		else
		{
			this->SubmitBufferRegion(numBytesRequired);
		}//if (numBytesRequired > spaceLeft)
	}//if (state & STATE_RUNNING)
}

/*--------------------------------*/
static void xaudio2_cleanup_sourcevoice_data(XAudio2Data* data)
{
	if (data == NULL)
		return;
	//cleanup source voice
	if (data->sourceVoice != NULL)
	{
		data->sourceVoice->DestroyVoice();
		data->sourceVoice = NULL;
	}
	if (data->sourceVoiceCallback)
	{
		delete (data->sourceVoiceCallback);
		data->sourceVoiceCallback = NULL;
	}

	//cleanup buffers
	if (data->buffers != NULL)
	{
		for (size_t i = 0; i < data->numBuffers; ++i)
		{
			if (data->buffers[i].data != NULL)
				delete[] data->buffers[i].data;
			data->buffers[i].data = NULL;
			data->buffers[i].bufferSize = 0;
		}
		delete[](data->buffers);

		data->buffers = NULL;
	}
}

static void xaudio2_cleanup_engine_data(XAudio2Data* data)
{
	if (data == NULL)
		return;
	xaudio2_cleanup_sourcevoice_data(data);


	if (data->masterVoice != NULL)
	{
		data->masterVoice->DestroyVoice();
		data->masterVoice = NULL;
	}

	if (data->xAudioObj != NULL)
	{
		data->xAudioObj->Release();
		data->xAudioObj = NULL;
	}
}

HRESULT xaudio2_do_open_playback(ALCdevice * device)
{
	HRESULT hr;
	XAudio2Data* data = (XAudio2Data*)device->ExtraData;

	/*-------init XAudio2-------------*/
	hr = XAudio2Create(&data->xAudioObj, 0, XAUDIO2_DEFAULT_PROCESSOR);
	if (SUCCEEDED(hr))
	{
		hr = data->xAudioObj->CreateMasteringVoice(&data->masterVoice);
	}
	else
		ERR("XAudio2Create() failed: 0x%08lx\n", hr);

	if (FAILED(hr))
	{
		ERR("CreateMasteringVoice() failed: 0x%08lx\n", hr);
		xaudio2_cleanup_engine_data(data);
	}

	return hr;
}

ALCboolean xaudio2_do_reset_playback(ALCdevice * device)
{
	XAudio2Data* data = (XAudio2Data*)device->ExtraData;

	HRESULT hr;
	DWORD outputchannelMasks;
	XAUDIO2_VOICE_DETAILS outputDetails;
	WAVEFORMATEXTENSIBLE SourceType;

	device->UpdateSize = (ALuint64)device->UpdateSize * 44100 / device->Frequency;
	device->UpdateSize = device->UpdateSize * device->NumUpdates / 2;
	device->NumUpdates = 2;

	data->masterVoice->GetChannelMask(&outputchannelMasks);
	data->masterVoice->GetVoiceDetails(&outputDetails);

	if (!(device->Flags&DEVICE_FREQUENCY_REQUEST))
		device->Frequency = outputDetails.InputSampleRate;
	if (!(device->Flags&DEVICE_CHANNELS_REQUEST))
	{
		if (outputDetails.InputChannels == 1 && outputchannelMasks == MONO)
			device->FmtChans = DevFmtMono;
		else if (outputDetails.InputChannels == 2 && outputchannelMasks == STEREO)
			device->FmtChans = DevFmtStereo;
		else if (outputDetails.InputChannels == 4 && outputchannelMasks == QUAD)
			device->FmtChans = DevFmtQuad;
		else if (outputDetails.InputChannels == 6 && outputchannelMasks == X5DOT1)
			device->FmtChans = DevFmtX51;
		else if (outputDetails.InputChannels == 6 && outputchannelMasks == X5DOT1SIDE)
			device->FmtChans = DevFmtX51Side;
		else if (outputDetails.InputChannels == 7 && outputchannelMasks == X6DOT1)
			device->FmtChans = DevFmtX61;
		else if (outputDetails.InputChannels == 8 && outputchannelMasks == X7DOT1)
			device->FmtChans = DevFmtX71;
		else
			ERR("Unhandled channel config: %d -- 0x%08lx\n", outputDetails.InputChannels, outputchannelMasks);
	}

	SetDefaultWFXChannelOrder(device);

	switch (device->FmtChans)
	{
	case DevFmtMono:
		SourceType.Format.nChannels = 1;
		SourceType.dwChannelMask = MONO;
		break;
	case DevFmtStereo:
		SourceType.Format.nChannels = 2;
		SourceType.dwChannelMask = STEREO;
		break;
	case DevFmtQuad:
		SourceType.Format.nChannels = 4;
		SourceType.dwChannelMask = QUAD;
		break;
	case DevFmtX51:
		SourceType.Format.nChannels = 6;
		SourceType.dwChannelMask = X5DOT1;
		break;
	case DevFmtX51Side:
		SourceType.Format.nChannels = 6;
		SourceType.dwChannelMask = X5DOT1SIDE;
		break;
	case DevFmtX61:
		SourceType.Format.nChannels = 7;
		SourceType.dwChannelMask = X6DOT1;
		break;
	case DevFmtX71:
		SourceType.Format.nChannels = 8;
		SourceType.dwChannelMask = X7DOT1;
		break;
	}
	switch (device->FmtType)
	{
	case DevFmtByte:
		device->FmtType = DevFmtUByte;
		/* fall-through */
	case DevFmtUByte:
		SourceType.Format.wBitsPerSample = 8;
		SourceType.Samples.wValidBitsPerSample = 8;
		SourceType.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
		break;
	case DevFmtUShort:
		device->FmtType = DevFmtShort;
		/* fall-through */
	case DevFmtShort:
		SourceType.Format.wBitsPerSample = 16;
		SourceType.Samples.wValidBitsPerSample = 16;
		SourceType.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
		break;
	case DevFmtFloat:
		SourceType.Format.wBitsPerSample = 32;
		SourceType.Samples.wValidBitsPerSample = 32;
		SourceType.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
		break;
	}
	SourceType.Format.cbSize = (sizeof(SourceType) - sizeof(WAVEFORMATEX));
	SourceType.Format.nSamplesPerSec = device->Frequency;

	SourceType.Format.nBlockAlign = SourceType.Format.nChannels *
		SourceType.Format.wBitsPerSample / 8;
	SourceType.Format.nAvgBytesPerSec = SourceType.Format.nSamplesPerSec *
		SourceType.Format.nBlockAlign;

	SourceType.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;

	/*-----------create source voice-------------*/
	data->sourceVoiceCallback = new (std::nothrow) XAudio2SourceVoiceCallback(device);
	if (data->sourceVoiceCallback == NULL)
	{
		ERR("create XAudio2SourceVoiceCallback() failed\n");
		return ALC_FALSE;
	}

	hr = data->xAudioObj->CreateSourceVoice(&data->sourceVoice, &SourceType.Format, 0,
		XAUDIO2_DEFAULT_FREQ_RATIO, data->sourceVoiceCallback);
	if (FAILED(hr))
	{
		ERR("CreateSourceVoice() failed: 0x%08lx\n", hr);
		return ALC_FALSE;
	}

	/*---------create buffers--------------*/
	//set the running flag
	InterlockedExchange(&data->running, TRUE);

	//calculate frame size
	data->frameSize = FrameSizeFromDevFmt(device->FmtChans, device->FmtType);

	data->numBuffers = 2;//TO DO: configurate this value
	size_t bufferSize = device->NumUpdates * device->UpdateSize * data->frameSize / data->numBuffers;
	data->buffers = new (std::nothrow) XAudio2Buffer[data->numBuffers];
	if (data->buffers == NULL)
	{
		ERR("Failed to create buffers for XAudio2 source voice\n");
		return ALC_FALSE;
	}

	for (size_t i = 0; i < data->numBuffers; ++i)
	{
		auto & buffer = data->buffers[i];
		buffer.bufferSize = bufferSize;
		buffer.data = new (std::nothrow) BYTE[buffer.bufferSize];
		if (buffer.data == NULL)
		{
			ERR("Failed to create one buffer's storage for XAudio2 source voice\n");
			return ALC_FALSE;
		}
	}

	return ALC_TRUE;
}

HRESULT xaudio2_do_stop_playback(ALCdevice * device)
{
	XAudio2Data* data = (XAudio2Data*)device->ExtraData;

	if (data->running)
	{
		XAUDIO2_VOICE_STATE state;

		InterlockedExchange(&data->running, FALSE);
		data->sourceVoice->Stop();
		data->sourceVoice->FlushSourceBuffers();

		do {
			data->sourceVoice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
		} while (state.BuffersQueued > 0);//wait for all buffer to be released
	}

	xaudio2_cleanup_sourcevoice_data(data);

	return S_OK;
}

HRESULT xaudio2_do_close_playback(ALCdevice * device)
{
	XAudio2Data* data = (XAudio2Data*)device->ExtraData;
	if (data != NULL)
	{
		xaudio2_cleanup_engine_data(data);
	}

	return S_OK;
}

//we need a separate thread to do the initialize and uninitialize of Xaudio2 to avoid COM threading problem
static ALuint xaudio2_api_msg_proc(void* arg)
{
	ThreadRequest *req = (ThreadRequest *)arg;
	ALuint deviceCount = 0;
	ALCdevice *device;
	HRESULT hr;
	QueueMsg_t msg;

	TRACE("Starting message thread\n");

	req->hresult = S_OK;
	SetEvent(req->FinishedEvt);

	TRACE("Starting message loop\n");
	while (g_MsgQueue->GetMsg(&msg))
	{
		TRACE("Got message %u\n", msg.message);
		switch (msg.message)
		{
		case TM_USER_OpenDevice:
			req = (ThreadRequest*)msg.wParam;
			device = (ALCdevice*)msg.lParam;

			hr = S_OK;
			if (++deviceCount == 1)
				hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);

			if (SUCCEEDED(hr))
				hr = xaudio2_do_open_playback(device);

			req->hresult = hr;
			SetEvent(req->FinishedEvt);
			continue;

		case TM_USER_ResetDevice:
			req = (ThreadRequest*)msg.wParam;
			device = (ALCdevice*)msg.lParam;

			req->albresult = xaudio2_do_reset_playback(device);
			SetEvent(req->FinishedEvt);
			continue;

		case TM_USER_StopDevice:
			req = (ThreadRequest*)msg.wParam;
			device = (ALCdevice*)msg.lParam;

			req->hresult = xaudio2_do_stop_playback(device);;
			SetEvent(req->FinishedEvt);
			continue;

		case TM_USER_CloseDevice:
			req = (ThreadRequest*)msg.wParam;
			device = (ALCdevice*)msg.lParam;

			req->hresult = xaudio2_do_close_playback(device);

			if (--deviceCount == 0)
				CoUninitialize();

			SetEvent(req->FinishedEvt);
			continue;

		default:
			ERR("Unexpected message: %u\n", msg.message);
			continue;
		}
	}
	TRACE("Message loop finished\n");

	return TRUE;
}


static ALCenum xaudio2_open_playback(ALCdevice *device, const ALCchar *deviceName)
{
	HRESULT hr;
	XAudio2Data * data;

	if (!deviceName)
	{
		deviceName = xaudio2_device;
	}
	else if (strcmp(deviceName, xaudio2_device) != 0)
	{
		return ALC_INVALID_VALUE;
	}

	data = (XAudio2Data*)calloc(1, sizeof(*data));
	if (data == NULL)
		return ALC_OUT_OF_MEMORY;

	device->ExtraData = data;
	hr = S_OK;
	data->MsgEvent = CreateEventEx(NULL, NULL, 0, EVENT_ACCESS_MASK);
	if (data->MsgEvent == NULL)
		hr = E_FAIL;

	if (SUCCEEDED(hr))
	{
		ThreadRequest req = { data->MsgEvent , 0};

		hr = E_FAIL;
		if (g_MsgQueue->PostMsg(TM_USER_OpenDevice, &req, device))
			hr = WaitForResponseHR(&req);
	}

	if (FAILED(hr))
	{
		if (data->MsgEvent != NULL)
			CloseHandle(data->MsgEvent);
		data->MsgEvent = NULL;

		free(data);
		device->ExtraData = NULL;

		return ALC_OUT_OF_MEMORY;
	}

	/*---------------------------------*/
	device->szDeviceName = alc_strdup(deviceName);

	return ALC_NO_ERROR;
}

static void xaudio2_close_playback(ALCdevice *device)
{
	XAudio2Data* data = (XAudio2Data*)device->ExtraData;
	ThreadRequest req = { data->MsgEvent, 0 };

	if (g_MsgQueue->PostMsg(TM_USER_CloseDevice, &req, device))
		(void)WaitForResponseHR(&req);

	CloseHandle(data->MsgEvent);
	data->MsgEvent = NULL;

	free(data);
	device->ExtraData = NULL;
}

static ALCboolean xaudio2_reset_playback(ALCdevice *device)
{
	XAudio2Data* data = (XAudio2Data*)device->ExtraData;
	ThreadRequest req = { data->MsgEvent, 0 };
	ALCboolean re = ALC_FALSE;

	if (g_MsgQueue->PostMsg(TM_USER_ResetDevice, &req, device))
		re = WaitForResponseALboolean(&req);

	if (re == ALC_TRUE)
	{
		//start playing the audio engine
		data->sourceVoice->Start();
	}

	return re;
}

static void xaudio2_stop_playback(ALCdevice *device)
{
	XAudio2Data* data = (XAudio2Data*)device->ExtraData;
	ThreadRequest req = { data->MsgEvent, 0 };

	if (g_MsgQueue->PostMsg(TM_USER_StopDevice, &req, device))
		WaitForResponseHR(&req);
}

static ALCenum xaudio2_open_capture(ALCdevice *pDevice, const ALCchar *deviceName)
{
    (void)pDevice;
    (void)deviceName;
    
    return ALC_INVALID_DEVICE;
}

static void xaudio2_close_capture(ALCdevice *pDevice)
{
    (void)pDevice;
}

static void xaudio2_start_capture(ALCdevice *pDevice)
{
    (void)pDevice;
}

static void xaudio2_stop_capture(ALCdevice *pDevice)
{
    (void)pDevice;
}

static ALCenum xaudio2_capture_samples(ALCdevice *pDevice, ALCvoid *pBuffer, ALCuint lSamples)
{
    (void)pDevice;
    (void)pBuffer;
    (void)lSamples;
    
    return ALC_INVALID_DEVICE;
}

static ALCuint xaudio2_available_samples(ALCdevice *pDevice)
{
    (void)pDevice;
    return 0;
}

static const BackendFuncs xaudio2_funcs = {
    xaudio2_open_playback,
    xaudio2_close_playback,
    xaudio2_reset_playback,
    xaudio2_stop_playback,
    xaudio2_open_capture,
    xaudio2_close_capture,
    xaudio2_start_capture,
    xaudio2_stop_capture,
    xaudio2_capture_samples,
    xaudio2_available_samples
};

static BOOL xaudio2_api_load(void)
{
	static HRESULT InitResult;
	if (!g_MsgQueueThread)
	{
		ThreadRequest req;
		InitResult = E_FAIL;

		req.FinishedEvt = CreateEventEx(NULL, NULL, 0, EVENT_ACCESS_MASK);
		if (req.FinishedEvt == NULL)
			ERR("Failed to create event: %lu\n", GetLastError());
		else
		{
			try {
				g_MsgQueue = new MsgQueue_t();//create message queue
			}
			catch (...)
			{
				g_MsgQueue = nullptr;
			}

			if (g_MsgQueue != nullptr)
				g_MsgQueueThread = StartThread(xaudio2_api_msg_proc, &req);//create message queue thread

			if (g_MsgQueueThread != nullptr)
			{
				InitResult = WaitForResponseHR(&req);
			}
			else//failed to create message queue thread
			{
				//delete message queue
				delete g_MsgQueue;
				g_MsgQueue = nullptr;
			}

			CloseHandle(req.FinishedEvt);
		}
	}
	return SUCCEEDED(InitResult);
}

extern "C" {
	ALCboolean alc_xaudio2_init(BackendFuncs *func_list)
	{
		if (!xaudio2_api_load())
			return ALC_FALSE;

		*func_list = xaudio2_funcs;

		return ALC_TRUE;
	}

	void alc_xaudio2_deinit(void)
	{
		if (g_MsgQueueThread)
		{
			TRACE("Sending TM_QUIT\n");
			g_MsgQueue->PostMsg(TM_QUIT, 0, 0);//post quit message

			//release thread and its message queue
			StopThread(g_MsgQueueThread);
			delete g_MsgQueue;
			g_MsgQueueThread = nullptr;
			g_MsgQueue = nullptr;
		}
	}

	void alc_xaudio2_probe(int type)
	{
		if (type == DEVICE_PROBE)
		{
			AppendDeviceList(xaudio2_device);
		}
		else if (type == ALL_DEVICE_PROBE)
		{
			AppendAllDeviceList(xaudio2_device);
		}
	}
}//extern "C"
