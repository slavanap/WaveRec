// wavetest.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"

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


HANDLE hEventStop;
HANDLE hEventStopped;

class MyAudioSink {
private:
	HANDLE m_hFile;
	char *m_header;
	WAVEHEADER *m_waveheader;
	WAVEFORMATEX *m_fmt;
	CHUNKHEADER *m_dataheader;
	DWORD m_headersize;
	bool m_error;
	DWORD m_framesCount;
public:
	MyAudioSink(LPCTSTR fn): m_hFile(INVALID_HANDLE_VALUE), m_fmt(NULL), m_header(NULL), m_error(false) {
		m_hFile = CreateFile(fn, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
		m_error = (m_hFile == INVALID_HANDLE_VALUE);
	}
	~MyAudioSink() {
		if (m_hFile != INVALID_HANDLE_VALUE)
			CloseHandle(m_hFile);
		if (m_header)
			delete[] m_header;
	}
	void printInfo() {
		float sec = (float)m_framesCount/m_fmt->nSamplesPerSec;
		int ms = (int)fmod(sec*1000, 1000);
		int s = (int)sec;
		int m = s / 60;
		int h = m / 60;
		s %= 60; m %= 60;
		printf("\rRecording: %02d:%02d:%02d.%03d", h,m,s,ms);
	}
	HRESULT SetFormat(WAVEFORMATEX* fmt) {
		DWORD fmt_size = sizeof(WAVEFORMATEX) + fmt->cbSize;
		m_headersize = sizeof(WAVEHEADER) + fmt_size + sizeof(CHUNKHEADER);
		m_header = new char[m_headersize];
		m_waveheader = (WAVEHEADER*)m_header;
		m_waveheader->dwChunkID = FOURCC_RIFF;
		m_waveheader->dwFormat = mmioFOURCC('W','A','V','E');
		m_waveheader->header.dwSubchunk1ID = mmioFOURCC('f','m','t',' ');
		m_waveheader->header.dwSubchunk1Size = fmt_size;
		m_fmt = (WAVEFORMATEX*)(m_header + sizeof(WAVEHEADER));
		memcpy(m_fmt, fmt, fmt_size);
		m_dataheader = (CHUNKHEADER*)(((char*)m_fmt) + fmt_size);
		m_dataheader->dwSubchunk1ID = mmioFOURCC('d','a','t','a');
		m_dataheader->dwSubchunk1Size = 0;
		m_waveheader->dwChunkSize = sizeof(FOURCC) +
			(sizeof(CHUNKHEADER) + m_waveheader->header.dwSubchunk1Size) +
			(sizeof(CHUNKHEADER) + m_dataheader->dwSubchunk1Size);
		m_framesCount = 0;

		DWORD temp;
		m_error = (!WriteFile(m_hFile, m_header, m_headersize, &temp, NULL) || temp != m_headersize);
		return m_error ? E_FAIL : S_OK;
	}
	HRESULT CopyData(BYTE* data, UINT32 nFrames, BOOL* fDone) {
		if (!m_error) {
			m_framesCount += nFrames;
			DWORD temp;
			DWORD towrite = nFrames * m_fmt->nBlockAlign;
			m_error = (!WriteFile(m_hFile, data, towrite, &temp, NULL) || temp != towrite);
			printInfo();
		}
		if (WaitForSingleObject(hEventStop, 0) == WAIT_OBJECT_0)
			*fDone = true;
		return m_error ? E_FAIL : S_OK;
	}
	HRESULT WriteHeader() {
		if (!m_error) {
			if (SetFilePointer(m_hFile, 0, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER)
				return E_FAIL;

			DWORD datasize = m_framesCount * m_fmt->nBlockAlign;
			m_dataheader->dwSubchunk1Size += datasize;
			m_waveheader->dwChunkSize += datasize;
			DWORD temp;
			m_error = (!WriteFile(m_hFile, m_header, m_headersize, &temp, NULL) || temp != m_headersize);
			if (!m_error)
				m_error = SetFilePointer(m_hFile, 0, NULL, FILE_END) == INVALID_SET_FILE_POINTER;
		}
		return m_error ? E_FAIL : S_OK;
	}
	void Close() {
		CloseHandle(m_hFile);
		m_hFile = INVALID_HANDLE_VALUE;
	}
	bool isError() const { return m_error; }
};

//-----------------------------------------------------------
// Record an audio stream from the default audio capture
// device. The RecordAudioStream function allocates a shared
// buffer big enough to hold one second of PCM audio data.
// The function uses this buffer to stream data from the
// capture device. The main loop runs every 1/2 second.
//-----------------------------------------------------------

// REFERENCE_TIME time units per second and per millisecond
#define REFTIMES_PER_SEC		10000000
#define REFTIMES_PER_MILLISEC	10000

#define EXIT_ON_ERROR(hres) \
	if (FAILED(hres)) { \
		goto Exit; \
	}

#define SAFE_RELEASE(punk) \
	if ((punk) != NULL) { \
		(punk)->Release(); \
		(punk) = NULL; \
	}

const CLSID CLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
const IID IID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);
const IID IID_IAudioClient = __uuidof(IAudioClient);
const IID IID_IAudioCaptureClient = __uuidof(IAudioCaptureClient);

HRESULT RecordAudioStream(MyAudioSink* pMySink) {
    HRESULT hr;
    REFERENCE_TIME hnsRequestedDuration = REFTIMES_PER_SEC;
    REFERENCE_TIME hnsActualDuration;
    UINT32 bufferFrameCount;
    UINT32 numFramesAvailable;
    IMMDeviceEnumerator *pEnumerator = NULL;
    IMMDevice *pDevice = NULL;
    IAudioClient *pAudioClient = NULL;
    IAudioCaptureClient *pCaptureClient = NULL;
    WAVEFORMATEX *pwfx = NULL;
    UINT32 packetLength = 0;
    BOOL bDone = FALSE;
    BYTE *pData;
    DWORD flags;

    hr = CoCreateInstance(
           CLSID_MMDeviceEnumerator, NULL,
           CLSCTX_ALL, IID_IMMDeviceEnumerator,
           (void**)&pEnumerator);
    EXIT_ON_ERROR(hr)

    //hr = pEnumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &pDevice);
	hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
    EXIT_ON_ERROR(hr)

    hr = pDevice->Activate(
                    IID_IAudioClient, CLSCTX_ALL,
                    NULL, (void**)&pAudioClient);
    EXIT_ON_ERROR(hr)

    hr = pAudioClient->GetMixFormat(&pwfx);
    EXIT_ON_ERROR(hr)

    hr = pAudioClient->Initialize(
                         AUDCLNT_SHAREMODE_SHARED,
                         AUDCLNT_STREAMFLAGS_LOOPBACK, //0,
                         hnsRequestedDuration,
                         0,
                         pwfx,
                         NULL);
    EXIT_ON_ERROR(hr)

    // Get the size of the allocated buffer.
    hr = pAudioClient->GetBufferSize(&bufferFrameCount);
    EXIT_ON_ERROR(hr)

    hr = pAudioClient->GetService(
                         IID_IAudioCaptureClient,
                         (void**)&pCaptureClient);
    EXIT_ON_ERROR(hr)

    // Notify the audio sink which format to use.
    hr = pMySink->SetFormat(pwfx);
    EXIT_ON_ERROR(hr)

    // Calculate the actual duration of the allocated buffer.
    hnsActualDuration = (REFERENCE_TIME)((double)REFTIMES_PER_SEC * bufferFrameCount / pwfx->nSamplesPerSec);

    hr = pAudioClient->Start();  // Start recording.
    EXIT_ON_ERROR(hr)

    // Each loop fills about half of the shared buffer.
    while (bDone == FALSE)
    {
        // Sleep for half the buffer duration.
        Sleep((DWORD)(hnsActualDuration/REFTIMES_PER_MILLISEC/2));

        hr = pCaptureClient->GetNextPacketSize(&packetLength);
        EXIT_ON_ERROR(hr)

        while (packetLength != 0)
        {
            // Get the available data in the shared buffer.
            hr = pCaptureClient->GetBuffer(
                                   &pData,
                                   &numFramesAvailable,
                                   &flags, NULL, NULL);
            EXIT_ON_ERROR(hr)

            if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
            {
                pData = NULL;  // Tell CopyData to write silence.
            }

            // Copy the available capture data to the audio sink.
            hr = pMySink->CopyData(
                              pData, numFramesAvailable, &bDone);
            EXIT_ON_ERROR(hr)

            hr = pCaptureClient->ReleaseBuffer(numFramesAvailable);
            EXIT_ON_ERROR(hr)

            hr = pCaptureClient->GetNextPacketSize(&packetLength);
            EXIT_ON_ERROR(hr)
        }
    }

    hr = pAudioClient->Stop();  // Stop recording.
    EXIT_ON_ERROR(hr)

Exit:
    CoTaskMemFree(pwfx);
    SAFE_RELEASE(pEnumerator)
    SAFE_RELEASE(pDevice)
    SAFE_RELEASE(pAudioClient)
    SAFE_RELEASE(pCaptureClient)

    return hr;
}

BOOL CtrlHandler(DWORD fdwCtrlType) {
	switch(fdwCtrlType)	{
		case CTRL_C_EVENT:
		case CTRL_CLOSE_EVENT:
			SetEvent(hEventStop);
			WaitForSingleObject(hEventStopped, INFINITE);
			return TRUE;
		default:
			return FALSE;
	}
}

#ifndef UNICODE
#define tsprintf sprintf
#define tprintf printf
#else
#define tsprintf swprintf
#define tprintf wprintf
#endif


int _tmain(int argc, _TCHAR* argv[])
{
	SYSTEMTIME st;
	GetLocalTime(&st);
	TCHAR buffer[128];
	tsprintf(buffer, TEXT("output_%04d%02d%02d-%02d%02d%02d_%03d.wav"), st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
	MyAudioSink s(buffer);
	tprintf(TEXT("Output filename: %s\n"), buffer);
	hEventStop = CreateEvent(NULL, true, false, NULL);
	hEventStopped = CreateEvent(NULL, true, false, NULL);
	if (!hEventStop || !hEventStopped || s.isError() ||
		!SetConsoleCtrlHandler((PHANDLER_ROUTINE)CtrlHandler, TRUE) || 
		!SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED) ||
		!SUCCEEDED(CoInitialize(NULL)) )
	{
		return -1;
	}
	RecordAudioStream(&s);
	CoUninitialize();
	s.WriteHeader();
	s.Close();
	SetEvent(hEventStopped);
	return 0;
}

