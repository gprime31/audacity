/*!********************************************************************

  Audacity: A Digital Audio Editor

  @file AudioUnitInstance.cpp

  Dominic Mazzoni
  Leland Lucius

  Paul Licameli split from AudioUnitEffect.cpp

**********************************************************************/

#if USE_AUDIO_UNITS
#include "AudioUnitInstance.h"

#include <AudioToolbox/AudioUnitUtilities.h>
#include "AudacityException.h"
#include <wx/log.h>

AudioUnitInstance::AudioUnitInstance(const PerTrackEffect &effect,
   AudioComponent component, Parameters &parameters,
   const wxString &identifier,
   unsigned audioIns, unsigned audioOuts, bool useLatency
)  : PerTrackEffect::Instance{ effect }
   , AudioUnitWrapper{ component, &parameters }
   , mIdentifier{ identifier }
   , mBlockSize{ InitialBlockSize() }
   , mUseLatency{ useLatency }
{
   mAudioIns = audioIns;
   mAudioOuts = audioOuts;
   CreateAudioUnit();
}

size_t AudioUnitInstance::InitialBlockSize() const
{
   // Retrieve the desired number of frames per slice
   UInt32 blockSize{};
   if (GetFixedSizeProperty(
      kAudioUnitProperty_MaximumFramesPerSlice, blockSize))
      // Call failed?  Then supply a default:
      return 512;
   else
      return blockSize;
}

size_t AudioUnitInstance::SetBlockSize(size_t)
{
   // Ignore the argument!  Too-large block sizes won't work
   return mBlockSize;
}

size_t AudioUnitInstance::GetBlockSize() const
{
   return mBlockSize;
}

unsigned AudioUnitInstance::GetAudioInCount() const
{
   return mAudioIns;
}

unsigned AudioUnitInstance::GetAudioOutCount() const
{
   return mAudioOuts;
}

auto AudioUnitInstance::GetLatency(
   const EffectSettings &, double sampleRate) const -> SampleCount
{
   // Retrieve the latency (can be updated via an event)
   if (mUseLatency) {
      Float64 latency = 0.0;
      if (!GetFixedSizeProperty(kAudioUnitProperty_Latency, latency))
         return latency * sampleRate;
   }
   return 0;
}

#if 0
size_t AudioUnitInstance::GetTailSize() const
{
   // Retrieve the tail time
   Float64 tailTime = 0.0;
   if (!GetFixedSizeProperty(kAudioUnitProperty_TailTime, tailTime))
      return tailTime * mSampleRate;
   return 0;
}
#endif

bool AudioUnitInstance::ProcessInitialize(EffectSettings &settings,
   double sampleRate, ChannelNames chanMap)
{
   if (!StoreSettings(GetSettings(settings)))
      return false;

   mInputList =
      PackedArray::AllocateCount<AudioBufferList>(mAudioIns)(mAudioIns);
   mOutputList =
      PackedArray::AllocateCount<AudioBufferList>(mAudioOuts)(mAudioOuts);

   memset(&mTimeStamp, 0, sizeof(AudioTimeStamp));
   mTimeStamp.mSampleTime = 0; // This is a double-precision number that should
                               // accumulate the number of frames processed so far
   mTimeStamp.mFlags = kAudioTimeStampSampleTimeValid;

   mInitialization.reset();
   // Redo this with the correct sample rate, not the arbirary 44100 that the
   // effect used
   auto ins = mAudioIns;
   auto outs = mAudioOuts;
   if (!SetRateAndChannels(sampleRate, mIdentifier))
      return false;
   if (AudioUnitInitialize(mUnit.get())) {
      wxLogError("Couldn't initialize audio unit\n");
      return false;
   }
   if (ins != mAudioIns || outs != mAudioOuts) {
      // A change of channels with changing rate?  This is unexpected!
      ins = mAudioIns;
      outs = mAudioOuts;
      return false;
   }
   mInitialization.reset(mUnit.get());

   if (SetProperty(kAudioUnitProperty_SetRenderCallback,
      AudioUnitUtils::RenderCallback{ RenderCallback, this },
      kAudioUnitScope_Input)) {
      wxLogError("Setting input render callback failed.\n");
      return false;
   }

   if (AudioUnitReset(mUnit.get(), kAudioUnitScope_Global, 0))
      return false;

   if (!BypassEffect(false))
      return false;

   return true;
}

bool AudioUnitInstance::ProcessFinalize() noexcept
{
   mOutputList.reset();
   mInputList.reset();
   return true;
}

size_t AudioUnitInstance::ProcessBlock(EffectSettings &,
   const float *const *inBlock, float *const *outBlock, size_t blockLen)
{
   // mAudioIns and mAudioOuts don't change after plugin initialization,
   // so ProcessInitialize() made sufficient allocations
   assert(Count(mInputList) >= mAudioIns);
   for (size_t i = 0; i < mAudioIns; ++i)
      mInputList[i] = { 1, static_cast<UInt32>(sizeof(float) * blockLen),
         const_cast<float*>(inBlock[i]) };

   // See previous comment
   assert(Count(mOutputList) >= mAudioOuts);
   for (size_t i = 0; i < mAudioOuts; ++i)
      mOutputList[i] = { 1, static_cast<UInt32>(sizeof(float) * blockLen),
         outBlock[i] };

   AudioUnitRenderActionFlags flags = 0;
   OSStatus result;

   result = AudioUnitRender(mUnit.get(),
                            &flags,
                            &mTimeStamp,
                            0,
                            blockLen,
                            mOutputList.get());
   if (result != noErr) {
      wxLogError("Render failed: %d %4.4s\n",
         static_cast<int>(result), reinterpret_cast<char *>(&result));
      return 0;
   }

   mTimeStamp.mSampleTime += blockLen;
   return blockLen;
}

bool AudioUnitInstance::RealtimeInitialize(
   EffectSettings &settings, double sampleRate)
{
   return ProcessInitialize(settings, sampleRate, nullptr);
}

bool AudioUnitInstance::RealtimeAddProcessor(
   EffectSettings &settings, unsigned, float sampleRate)
{
   if (!mRecruited) {
      // Assign self to the first processor
      mRecruited = true;
      return true;
   }

   // Assign another instance with independent state to other processors
   auto &effect = static_cast<const PerTrackEffect&>(mProcessor);
   auto uProcessor = std::make_unique<AudioUnitInstance>(effect,
      mComponent, mParameters, mIdentifier,
      mAudioIns, mAudioOuts, mUseLatency);
   uProcessor->SetBlockSize(mBlockSize);
   if (!uProcessor->ProcessInitialize(settings, sampleRate, nullptr))
      return false;
   mSlaves.push_back(move(uProcessor));
   return true;
}

bool AudioUnitInstance::RealtimeFinalize(EffectSettings &) noexcept
{
return GuardedCall<bool>([&]{
   for (auto &pSlave : mSlaves)
      pSlave->ProcessFinalize();
   mSlaves.clear();
   mRecruited = false;
   return ProcessFinalize();
});
}

bool AudioUnitInstance::RealtimeSuspend()
{
   if (!BypassEffect(true))
      return false;
   for (auto &pSlave : mSlaves)
      if (!pSlave->BypassEffect(true))
         return false;
   return true;
}

bool AudioUnitInstance::RealtimeResume()
{
   if (!BypassEffect(false))
      return false;
   for (auto &pSlave: mSlaves)
      if (!pSlave->BypassEffect(false))
         return false;
   return true;
}

bool AudioUnitInstance::RealtimeProcessStart(EffectSettings &settings)
{
   auto &mySettings = GetSettings(settings);
   // Store only into the AudioUnit that was not also the source of the fetch
   // in the main thread.  Not only for efficiency, but also because controls
   // of at least one effect (AUGraphicEQ) are known to misbehave otherwise.
   auto storeSettings = [&](AudioUnitInstance &instance){
      if (&instance != mySettings.pSource)
         instance.StoreSettings(mySettings);
   };
   storeSettings(*this);
   for (auto &pSlave : mSlaves)
      storeSettings(*pSlave);
   return true;
}

size_t
AudioUnitInstance::RealtimeProcess(size_t group, EffectSettings &settings,
   const float *const *inbuf, float *const *outbuf, size_t numSamples)
{
   wxASSERT(numSamples <= mBlockSize);
   // Interpret the group number consistently with RealtimeAddProcessor
   if (!mRecruited)
      return 0;
   decltype(this) pSlave{};
   if (group == 0)
      pSlave = this;
   else if (--group < mSlaves.size())
      pSlave = mSlaves[group].get();
   if (pSlave)
      return pSlave->ProcessBlock(settings, inbuf, outbuf, numSamples);
   else
      return 0;
}

bool AudioUnitInstance::RealtimeProcessEnd(EffectSettings &) noexcept
{
   return true;
}

OSStatus AudioUnitInstance::Render(
   AudioUnitRenderActionFlags *inActionFlags,
   const AudioTimeStamp *inTimeStamp,
   UInt32 inBusNumber, UInt32 inNumFrames, AudioBufferList *ioData)
{
   size_t i = 0;
   auto size = std::min<size_t>(ioData->mNumberBuffers, Count(mInputList));
   for (; i < size; ++i)
      ioData->mBuffers[i].mData = mInputList[i].mData;
   // Some defensive code here just in case SDK requests from us an unexpectedly
   // large number of buffers:
   for (; i < ioData->mNumberBuffers; ++i)
      ioData->mBuffers[i].mData = nullptr;
   return 0;
}

// static
OSStatus AudioUnitInstance::RenderCallback(void *inRefCon,
   AudioUnitRenderActionFlags *inActionFlags,
   const AudioTimeStamp *inTimeStamp,
   UInt32 inBusNumber, UInt32 inNumFrames, AudioBufferList *ioData)
{
   return static_cast<AudioUnitInstance*>(inRefCon)->Render(inActionFlags,
      inTimeStamp, inBusNumber, inNumFrames, ioData);
}

void AudioUnitInstance::EventListener(const AudioUnitEvent *inEvent,
   AudioUnitParameterValue inParameterValue)
{
   // Handle property changes
   if (inEvent->mEventType == kAudioUnitEvent_PropertyChange) {
      // Handle latency changes
      if (inEvent->mArgument.mProperty.mPropertyID ==
          kAudioUnitProperty_Latency) {
         // Do what?
      }
      return;
   }
   
   if (inEvent->mEventType != kAudioUnitEvent_ParameterValueChange)
      return;

   // Only parameter changes at this point
   const auto parameterStorer = [inParameterValue,
      ID = inEvent->mArgument.mParameter.mParameterID
   ](AudioUnit pUnit){
      AudioUnitSetParameter(pUnit, ID,
         kAudioUnitScope_Global, 0, inParameterValue, 0);
   };

   // Save the parameter change in the instance, so it can be
   // fetched into Settings, used to initialize any new slave's state
   // This is like StoreSettings but for just one setting
   parameterStorer(GetAudioUnit());

   // Propagate the parameter
   for (auto &worker : mSlaves)
      parameterStorer(worker->GetAudioUnit());
}

bool AudioUnitInstance::BypassEffect(bool bypass)
{
   UInt32 value = (bypass ? 1 : 0);
   if (bypass && AudioUnitReset(mUnit.get(), kAudioUnitScope_Global, 0))
      return false;
   return !SetProperty(kAudioUnitProperty_BypassEffect, value);
}

#endif
