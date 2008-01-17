/*
	SuperCollider real time audio synthesis system
    Copyright (c) 2002 James McCartney. All rights reserved.
	http://www.audiosynth.com

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/


#ifndef _SC_CoreAudio_
#define _SC_CoreAudio_

#include "MsgFifo.h"
#include "SC_FifoMsg.h"
#include "OSC_Packet.h"
#include "SC_SyncCondition.h"
#include "PriorityQueue.h"
#include "SC_Lock.h"

#define SC_AUDIO_API_COREAUDIO	1
#define SC_AUDIO_API_JACK		2
#define SC_AUDIO_API_PORTAUDIO  3

#ifdef SC_WIN32
# ifndef SC_INNERSC
#  define SC_AUDIO_API SC_AUDIO_API_PORTAUDIO
# else
#  define SC_AUDIO_API SC_AUDIO_API_INNERSC_VST
# endif
#endif

#ifndef SC_AUDIO_API
# define SC_AUDIO_API SC_AUDIO_API_COREAUDIO
#endif // SC_AUDIO_API

#if SC_AUDIO_API == SC_AUDIO_API_COREAUDIO
# include <CoreAudio/AudioHardware.h>
# include <CoreAudio/HostTime.h>
#endif

#if SC_AUDIO_API == SC_AUDIO_API_PORTAUDIO
#include "portaudio.h"
#endif

const double kSecondsToOSCunits = 4294967296.; // pow(2,32)
const double kMicrosToOSCunits = 4294.967296; // pow(2,32)/1e6
const double kNanosToOSCunits  = 4.294967296; // pow(2,32)/1e9

const int32 kSECONDS_FROM_1900_to_1970 = (int32)2208988800UL; /* 17 leap years */
const double kOSCtoSecs = 2.328306436538696e-10;

struct SC_ScheduledEvent 
{
	SC_ScheduledEvent() : mTime(0), mPacket(0) {}
	SC_ScheduledEvent(struct World *inWorld, int64 inTime, OSC_Packet *inPacket) 
		: mTime(inTime), mPacket(inPacket), mWorld(inWorld) {}
	
	int64 Time() { return mTime; }
	void Perform();
	
	int64 mTime;
	OSC_Packet *mPacket;
	struct World *mWorld;
};

typedef MsgFifo<FifoMsg, 1024> EngineFifo;

// Functions to be implemented by the driver backend
extern "C" {
	int32 server_timeseed();
};
int64 oscTimeNow();
void initializeScheduler();

class SC_AudioDriver
{
protected:
    int64 mOSCincrement;
	struct World *mWorld;
	double mOSCtoSamples;
	int mSampleTime;

	// Common members
	uint32	mHardwareBufferSize;	// bufferSize returned by kAudioDevicePropertyBufferSize
	EngineFifo mFromEngine, mToEngine;
	EngineFifo mOscPacketsToEngine;
	SC_SyncCondition mAudioSync;
	pthread_t mThread;
	bool mRunThreadFlag;
	uint32 mSafetyOffset;
	PriorityQueueT<SC_ScheduledEvent, 2048> mScheduler;
	int mNumSamplesPerCallback;
	uint32 mPreferredHardwareBufferFrameSize;
	uint32 mPreferredSampleRate;
	double mBuffersPerSecond;
	double mAvgCPU, mPeakCPU;
	int mPeakCounter, mMaxPeakCounter;
	double mOSCincrementNumerator;
	
	double mStartHostSecs;
	double mPrevHostSecs;
	double mStartSampleTime;
	double mPrevSampleTime;
	double mSmoothSampleRate;
	double mSampleRate;

    // Driver interface methods, implemented by subclasses
	virtual bool DriverSetup(int* outNumSamplesPerCallback, double* outSampleRate) = 0;
	virtual bool DriverStart() = 0;
	virtual bool DriverStop() = 0;
    
public:
	// Common methods
	SC_AudioDriver(struct World *inWorld);
	virtual ~SC_AudioDriver();

    int64 mOSCbuftime;
    
	bool Setup();
	bool Start();
	bool Stop();
	
	void ClearSched() { mScheduler.Empty(); }
	
	void RunNonRealTime(float *in, float *out, int numSamples, int64 oscTime);
	void* RunThread();

	int SafetyOffset() const { return mSafetyOffset; }
	int NumSamplesPerCallback() const { return mNumSamplesPerCallback; }
	void SetPreferredHardwareBufferFrameSize(int inSize) 
	{ 
		mPreferredHardwareBufferFrameSize = inSize;
	}
	void SetPreferredSampleRate(int inRate)
	{
		mPreferredSampleRate = inRate;
	}

	bool SendMsgToEngine(FifoMsg& inMsg);           // called by NRT thread
	bool SendMsgFromEngine(FifoMsg& inMsg);         
	bool SendOscPacketMsgToEngine(FifoMsg& inMsg);  // called by OSC socket listener threads, protected by mWorld->mDriverLock

	void AddEvent(SC_ScheduledEvent& event) { mScheduler.Add(event); }

	double GetAvgCPU() const { return mAvgCPU; }
	double GetPeakCPU() const { return mPeakCPU; }
	double GetSampleRate() const { return mSampleRate; }
	double GetActualSampleRate() const { return mSmoothSampleRate; }	
};

// the following classes should be split out into separate source files.
#if SC_AUDIO_API == SC_AUDIO_API_COREAUDIO
class SC_CoreAudioDriver : public SC_AudioDriver
{

    AudioBufferList * mInputBufList;
	AudioDeviceID	mInputDevice;
	AudioDeviceID	mOutputDevice;

	AudioStreamBasicDescription	inputStreamDesc;	// info about the default device
	AudioStreamBasicDescription	outputStreamDesc;	// info about the default device

	friend OSStatus appIOProc (		AudioDeviceID inDevice, 
									const AudioTimeStamp* inNow, 
									const AudioBufferList* inInputData,
									const AudioTimeStamp* inInputTime, 
									AudioBufferList* outOutputData, 
									const AudioTimeStamp* inOutputTime,
									void* defptr);

protected:
    // Driver interface methods
	virtual bool DriverSetup(int* outNumSamplesPerCallback, double* outSampleRate);
	virtual bool DriverStart();
	virtual bool DriverStop();
    
public:
    SC_CoreAudioDriver(struct World *inWorld);
	virtual ~SC_CoreAudioDriver();

    void Run(const AudioBufferList* inInputData, AudioBufferList* outOutputData, int64 oscTime);

	bool UseInput() { return mInputDevice != kAudioDeviceUnknown; }
	bool UseSeparateIO() { return UseInput() && mInputDevice != mOutputDevice; }
	AudioDeviceID InputDevice() { return mInputDevice; }
	AudioDeviceID OutputDevice() { return mOutputDevice; }
	
	void SetInputBufferList(AudioBufferList * inBufList) { mInputBufList = inBufList; }
	AudioBufferList* GetInputBufferList() const { return mInputBufList; }	
};

inline SC_AudioDriver* SC_NewAudioDriver(struct World *inWorld)
{
    return new SC_CoreAudioDriver(inWorld);
}
#endif // SC_AUDIO_API_COREAUDIO


#if SC_AUDIO_API == SC_AUDIO_API_JACK

extern SC_AudioDriver* SC_NewAudioDriver(struct World* inWorld);

#endif // SC_AUDIO_API_JACK


#if SC_AUDIO_API == SC_AUDIO_API_PORTAUDIO
class SC_PortAudioDriver : public SC_AudioDriver
{

    int mInputChannelCount, mOutputChannelCount;
    PaStream *mStream;

protected:
    // Driver interface methods
	virtual bool DriverSetup(int* outNumSamplesPerCallback, double* outSampleRate);
	virtual bool DriverStart();
	virtual bool DriverStop();
    
public:
    SC_PortAudioDriver(struct World *inWorld);
	virtual ~SC_PortAudioDriver();

    int PortAudioCallback( const void *input, void *output,
            unsigned long frameCount, const PaStreamCallbackTimeInfo* timeInfo,
            PaStreamCallbackFlags statusFlags );
private:
	void GetPaDeviceFromName(const char* device, int* mInOut);
};

inline SC_AudioDriver* SC_NewAudioDriver(struct World *inWorld)
{
    return new SC_PortAudioDriver(inWorld);
}
#endif // SC_AUDIO_API_PORTAUDIO

#if SC_AUDIO_API == SC_AUDIO_API_INNERSC_VST

struct VstTimeInfo;

class SC_VSTAudioDriver : public SC_AudioDriver
{

    int   mInputChannelCount, mOutputChannelCount;
    bool  mIsStreaming;

protected:
    // Driver interface methods
	virtual bool  DriverSetup(int* outNumSamplesPerCallback, double* outSampleRate);
	virtual bool  DriverStart();
	virtual bool  DriverStop();
  void          Callback( 
                  const void *input, void *output,
                  unsigned long frameCount, const VstTimeInfo* timeInfo );
    
public:
              SC_VSTAudioDriver(struct World *inWorld);
  	virtual  ~SC_VSTAudioDriver();

//    int PortAudioCallback( const void *input, void *output,
//            unsigned long frameCount, const PaStreamCallbackTimeInfo* timeInfo,
//            PaStreamCallbackFlags statusFlags );
};

inline SC_AudioDriver* SC_NewAudioDriver(struct World *inWorld)
{
  // This is called from "World* World_New(WorldOptions *inOptions)" in "SC_World.cpp"
  return new SC_VSTAudioDriver(inWorld); //This gets saved in inWorld->hw->mAudioDriver
}
#endif // SC_AUDIO_API == SC_AUDIO_API_INNERSC_VST

#endif
