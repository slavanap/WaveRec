//
// Copyright (c) 2016 Vyacheslav Napadovsky.
// Distributed under the MIT License (http://opensource.org/licenses/MIT)
//
// Software sound recorder for OS Windows.
// 


#define  _CRT_SECURE_NO_WARNINGS
#include <tchar.h>
#include <math.h>
#include <Windows.h>
#include <Mmdeviceapi.h>
#include <Audioclient.h>

#include <stdexcept>        // std::runtime_error
#include <fstream>          // std::ofstream
#include <iterator>         // std::ostream_iterator

// ATL is for CComPtr
#define _ATL_CSTRING_EXPLICIT_CONSTRUCTORS
#include <atlbase.h>
#include <atlstr.h>

class IAudioWriter {
public:
	// Set format of output audio. Called once before CopyData.
	virtual void SetFormat(WAVEFORMATEX* fmt) abstract;

	// Actually writes data. If return value is 'false' then breaks from recording loop.
	virtual bool CopyData(BYTE* data, UINT32 nFrames) abstract;
};

void RecordAudioStream(IAudioWriter& writer) {

	// Code in this function is a refactored version of code from here:
	// https://msdn.microsoft.com/ru-ru/library/windows/desktop/dd370800(v=vs.85).aspx

	//-----------------------------------------------------------
	// Record an audio stream from the default audio capture
	// device. The RecordAudioStream function allocates a shared
	// buffer big enough to hold one second of PCM audio data.
	// The function uses this buffer to stream data from the
	// capture device. The main loop runs every 1/2 second.
	//-----------------------------------------------------------

	// REFERENCE_TIME time units per second and per millisecond
	constexpr auto REFTIMES_PER_SEC = 10000000;
	constexpr auto REFTIMES_PER_MILLISEC = 10000;

	CComPtr<IMMDeviceEnumerator> pEnumerator;
	if (FAILED(pEnumerator.CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL)))
		throw std::runtime_error("Can't receive DeviceEnumerator instance");

	CComPtr<IMMDevice> pDevice;
	// if you want to capture data from microphone use 'eCapture' instead 'eRender'
	if (FAILED(pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice)))
		throw std::runtime_error("Can't get default audio endpoint");

	CComPtr<IAudioClient> pAudioClient;
	if (FAILED(pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&pAudioClient)))
		throw std::runtime_error("Can't activate device");

	WAVEFORMATEX *pwfx = nullptr;
	if (FAILED(pAudioClient->GetMixFormat(&pwfx)))
		throw std::runtime_error("Can't get audio format");

	// Notify the audio sink which format to use.
	writer.SetFormat(pwfx);


	const REFERENCE_TIME hnsRequestedDuration = REFTIMES_PER_SEC;
	REFERENCE_TIME hnsActualDuration;
	UINT32 bufferFrameCount;
	if (FAILED(
		pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
			hnsRequestedDuration, 0, pwfx, nullptr)
	))
		throw std::runtime_error("Device initialization failed");

	// Get the size of the allocated buffer.
	if (FAILED(pAudioClient->GetBufferSize(&bufferFrameCount)))
		throw std::runtime_error("GetBufferSize failed");

	// Calculate the actual duration of the allocated buffer.
	hnsActualDuration = (REFERENCE_TIME)((double)hnsRequestedDuration * bufferFrameCount / pwfx->nSamplesPerSec);

	CComPtr<IAudioCaptureClient> pCaptureClient;
	if (FAILED(pAudioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&pCaptureClient)))
		throw std::runtime_error("GetService failed");

	// Start recording.
	if (FAILED(pAudioClient->Start()))
		throw std::runtime_error("failed to start recording");

	// Each loop fills about half of the shared buffer.
	bool bContinue = true;
	while (bContinue) {
		UINT32 packetLength = 0;
		if (FAILED(pCaptureClient->GetNextPacketSize(&packetLength)))
			throw std::runtime_error("GetNextPacketSize failed");

		if (packetLength == 0) {
			// Sleep for half the buffer duration.
			Sleep((DWORD)(hnsActualDuration / REFTIMES_PER_MILLISEC / 2));
			continue;
		}

		// Get the available data in the shared buffer.
		BYTE *pData;
		UINT32 numFramesAvailable;
		DWORD flags;
		if (FAILED(pCaptureClient->GetBuffer(&pData, &numFramesAvailable, &flags, nullptr, nullptr)))
			throw std::runtime_error("GetBuffer failed");

		if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0) {
			pData = nullptr;  // Tell CopyData to write silence.
		}
		
		// Copy the available capture data to the audio sink.
		bContinue = writer.CopyData(pData, numFramesAvailable);

		if (FAILED(pCaptureClient->ReleaseBuffer(numFramesAvailable)))
			throw std::runtime_error("ReleaseBuffer failed");
	}

	// Stop recording.
	if (FAILED(pAudioClient->Stop()))
		throw std::runtime_error("Stop recording failed");

	CoTaskMemFree(pwfx);
}




volatile bool flag_stop = false;
HANDLE hEventCompleted = nullptr;

BOOL CtrlHandler(DWORD fdwCtrlType) {
	switch (fdwCtrlType) {
		case CTRL_C_EVENT:
		case CTRL_CLOSE_EVENT:
			flag_stop = true;
			WaitForSingleObject(hEventCompleted, INFINITE);
			return TRUE;
		default:
			return FALSE;
	}
}

class AudioWriter : public IAudioWriter {
public:

	AudioWriter(LPCTSTR fn) :
		_f(fn, std::ios::binary),
		_header(nullptr),
		_fmt(nullptr),
		_framesCount(0)
	{
		if (!_f.good())
			throw std::runtime_error("Can't open file for write");
	}

	~AudioWriter() {
		WriteHeader();
		delete[] _header;
	}

	void SetFormat(WAVEFORMATEX* fmt) override {
		DWORD fmt_size = sizeof(WAVEFORMATEX) + fmt->cbSize;
		_headersize = sizeof(WAVEHEADER) + fmt_size + sizeof(CHUNKHEADER);
		_header = new char[_headersize];
		_waveheader = (WAVEHEADER*)_header;
		_waveheader->dwChunkID = FOURCC_RIFF;
		_waveheader->dwFormat = mmioFOURCC('W', 'A', 'V', 'E');
		_waveheader->header.dwSubchunk1ID = mmioFOURCC('f', 'm', 't', ' ');
		_waveheader->header.dwSubchunk1Size = fmt_size;

		_fmt = (WAVEFORMATEX*)(_header + sizeof(WAVEHEADER));
		memcpy(_fmt, fmt, fmt_size);

		_dataheader = (CHUNKHEADER*)(((char*)_fmt) + fmt_size);
		_dataheader->dwSubchunk1ID = mmioFOURCC('d', 'a', 't', 'a');
		_dataheader->dwSubchunk1Size = 0;

		_waveheader->dwChunkSize = sizeof(FOURCC) +
			(sizeof(CHUNKHEADER) + _waveheader->header.dwSubchunk1Size) +
			(sizeof(CHUNKHEADER) + _dataheader->dwSubchunk1Size);

		_f.write(_header, _headersize);
	}

	bool CopyData(BYTE* data, UINT32 nFrames) override {
		_framesCount += nFrames;
		UINT32 bytes = nFrames * _fmt->nBlockAlign;
		if (data == nullptr)
			std::fill_n(std::ostream_iterator<char>(_f), bytes, 0);
		else
			_f.write((char*)data, bytes);
		_f.flush();
		printInfo();

		// Check if termination via Ctrl+C handler initiated.
		return !flag_stop;
	}

private:
	struct CHUNKHEADER {
		FOURCC dwSubchunk1ID;
		DWORD dwSubchunk1Size;
	};

	struct WAVEHEADER {
		FOURCC dwChunkID;
		DWORD dwChunkSize;
		FOURCC dwFormat;
		CHUNKHEADER header;
	};

	std::ofstream _f;
	char *_header;
	DWORD _headersize;

	DWORD _framesCount;

	WAVEFORMATEX *_fmt;
	WAVEHEADER *_waveheader;
	CHUNKHEADER *_dataheader;

	void WriteHeader() {
		_f.seekp(0);
		DWORD datasize = _framesCount * _fmt->nBlockAlign;
		_waveheader->dwChunkSize += datasize;
		_dataheader->dwSubchunk1Size += datasize;
		_f.write(_header, _headersize);
		_f.seekp(0, std::ios::end);
		_f.flush();
	}

	void printInfo() {
		float sec = (float)_framesCount / _fmt->nSamplesPerSec;
		int ms = (int)fmod(sec * 1000, 1000);
		int s = (int)sec;
		int m = s / 60;
		int h = m / 60;
		s %= 60; m %= 60;
		printf("\rRecording: %02d:%02d:%02d.%03d", h, m, s, ms);
	}

};

int _tmain(int argc, TCHAR* argv[])
{
	SYSTEMTIME st;
	GetLocalTime(&st);
	TCHAR buffer[128];
	_stprintf(buffer, TEXT("output_%04d%02d%02d-%02d%02d%02d_%03d.wav"),
		st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

	try {
		if (FAILED(CoInitialize(nullptr)))
			throw std::runtime_error("CoInitialize call failed");

		// Create writer
		AudioWriter writer(buffer);
		_tprintf(TEXT("Output filename: %s\n"), buffer);

		// Install Ctrl+C handler
		hEventCompleted = CreateEvent(nullptr, true, false, nullptr);
		if (hEventCompleted == nullptr)
			throw std::runtime_error("Can't create event");

		if (!SetConsoleCtrlHandler((PHANDLER_ROUTINE)CtrlHandler, TRUE))
			throw std::runtime_error("Can't set Ctrl+C handler");

		// Prevent PC from sleep
		SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);

		RecordAudioStream(writer);
	}
	catch (const std::exception& e) {
		printf("ERROR: %s\n", e.what());
	}

	CoUninitialize();
	if (hEventCompleted != nullptr)
		SetEvent(hEventCompleted);
	return 0;
}
