/*
    ,--.                     ,--.     ,--.  ,--.
  ,-'  '-.,--.--.,--,--.,---.|  |,-.,-'  '-.`--' ,---. ,--,--,      Copyright 2024
  '-.  .-'|  .--' ,-.  | .--'|     /'-.  .-',--.| .-. ||      \   Tracktion Software
    |  |  |  |  \ '-'  \ `--.|  \  \  |  |  |  |' '-' '|  ||  |       Corporation
    `---' `--'   `--`--'`---'`--'`--' `---' `--' `---' `--''--'    www.tracktion.com

    Tracktion Engine uses a GPL/commercial licence - see LICENCE.md for details.
*/

namespace tracktion
{
inline namespace engine
{

    // this must be high enough for low freq sounds not to click
    static constexpr int minimumSamplesToPlayWhenStopping = 8;
    static constexpr int maximumSimultaneousNotes = 32;

    struct SamplerPlugin::SampledNote : public ReferenceCountedObject
    {
    public:
        SampledNote (int midiNote, int keyNote, float velocity, const AudioFile& file, double sampleRate, int sampleDelayFromBufferStart, const juce::AudioBuffer<float>& data, int lengthInSamples, float gainDb, float pan, bool openEnded_, float a, float d, float s, float r, int oi)
            : note (midiNote),
              offset (-sampleDelayFromBufferStart),
              audioData (data),
              openEnded (openEnded_),
              outputIndex (oi)
        {
            resampler[0].reset();
            resampler[1].reset();

            adsr.setSampleRate (sampleRate);
            adsr.setParameters ({ a, d, s, r });
            adsr.noteOn();

            const float volumeSliderPos = decibelsToVolumeFaderPosition (gainDb - (20.0f * (1.0f - velocity)));
            getGainsFromVolumeFaderPositionAndPan (volumeSliderPos, pan, getDefaultPanLaw(), gains[0], gains[1]);

            const double hz = juce::MidiMessage::getMidiNoteInHertz (midiNote);
            playbackRatio = hz / juce::MidiMessage::getMidiNoteInHertz (keyNote);
            playbackRatio *= file.getSampleRate() / sampleRate;
            samplesLeftToPlay = playbackRatio > 0 ? (1 + (int) (lengthInSamples / playbackRatio)) : 0;
        }

        void noteOff()
        {
            adsr.noteOff();
        }

        void addNextBlock (juce::AudioBuffer<float>& outBuffer, int startSamp, int numSamples)
        {
            jassert (! isFinished);

            if (offset < 0)
            {
                const int num = std::min (-offset, numSamples);
                startSamp += num;
                numSamples -= num;
                offset += num;
            }

            auto numSamps = std::min (numSamples, samplesLeftToPlay);

            if (numSamps > 0)
            {
                int numUsed = 0;

                for (int i = std::min (2, outBuffer.getNumChannels()); --i >= 0;)
                {
                    const int outChan = (outputIndex * 2 + i) % outBuffer.getNumChannels();

                    numUsed = resampler[i]
                                  .processAdding (playbackRatio,
                                                  audioData.getReadPointer (std::min (i, audioData.getNumChannels() - 1), offset),
                                                  outBuffer.getWritePointer (outChan, startSamp),
                                                  numSamps,
                                                  gains[i]);
                }

                // Apply ADSR envelope to the block we just added
                for (int i = 0; i < std::min (2, outBuffer.getNumChannels()); ++i)
                {
                    const int outChan = (outputIndex * 2 + i) % outBuffer.getNumChannels();
                    auto* d = outBuffer.getWritePointer (outChan, startSamp);

                    // This is slightly inefficient as we apply envelope after resampler, but simpler for now
                    juce::ADSR adsrCopy = adsr; // We need to process each channel with same envelope values
                    for (int s = 0; s < numSamps; ++s)
                    {
                        if (i == 0) // only advance adsr on first channel
                            d[s] *= adsr.getNextSample();
                        else
                            d[s] *= adsrCopy.getNextSample();
                    }
                }

                if (! adsr.isActive())
                    isFinished = true;

                offset += numUsed;
                samplesLeftToPlay -= numSamps;

                jassert (offset <= audioData.getNumSamples());
            }

            if (numSamples > numSamps && startFade > 0.0f)
            {
                startSamp += numSamps;
                numSamps = numSamples - numSamps;
                float endFade;

                if (numSamps > 100)
                {
                    endFade = 0.0f;
                    numSamps = 100;
                }
                else
                {
                    endFade = std::max (0.0f, startFade - numSamps * 0.01f);
                }

                const int numSampsNeeded = 2 + juce::roundToInt ((numSamps + 2) * playbackRatio);
                AudioScratchBuffer scratch (audioData.getNumChannels(), numSampsNeeded + 8);

                if (offset + numSampsNeeded < audioData.getNumSamples())
                {
                    for (int i = scratch.buffer.getNumChannels(); --i >= 0;)
                        scratch.buffer.copyFrom (i, 0, audioData, i, offset, numSampsNeeded);
                }
                else
                {
                    scratch.buffer.clear();
                }

                if (numSampsNeeded > 2)
                    AudioFadeCurve::applyCrossfadeSection (scratch.buffer, 0, numSampsNeeded - 2, AudioFadeCurve::linear, startFade, endFade);

                startFade = endFade;

                int numUsed = 0;

                for (int i = std::min (2, outBuffer.getNumChannels()); --i >= 0;)
                    numUsed = resampler[i].processAdding (playbackRatio,
                                                          scratch.buffer.getReadPointer (std::min (i, scratch.buffer.getNumChannels() - 1)),
                                                          outBuffer.getWritePointer (i, startSamp),
                                                          numSamps,
                                                          gains[i]);

                offset += numUsed;

                if (startFade <= 0.0f)
                    isFinished = true;
            }
        }

        juce::LagrangeInterpolator resampler[2];
        juce::ADSR adsr;
        int note, outputIndex;
        int offset, samplesLeftToPlay = 0;
        float gains[2];
        double playbackRatio = 1.0;
        const juce::AudioBuffer<float>& audioData;
        float lastVals[4] = { 0, 0, 0, 0 };
        float startFade = 1.0f;
        bool openEnded, isFinished = false;

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SampledNote)
    };

    //==============================================================================
    SamplerPlugin::SamplerPlugin (PluginCreationInfo info) : Plugin (info)
    {
        triggerAsyncUpdate();
    }

    SamplerPlugin::~SamplerPlugin()
    {
        notifyListenersOfDeletion();
    }

    const char* SamplerPlugin::xmlTypeName = "sampler";

    void SamplerPlugin::valueTreeChanged()
    {
        triggerAsyncUpdate();
        Plugin::valueTreeChanged();
    }

    void SamplerPlugin::handleAsyncUpdate()
    {
        juce::OwnedArray<SamplerSound> newSounds;

        auto numSounds = state.getNumChildren();

        for (int i = 0; i < numSounds; ++i)
        {
            auto v = getSound (i);

            if (v.hasType (IDs::SOUND))
            {
                auto s = new SamplerSound (*this,
                                           v[IDs::source].toString(),
                                           v[IDs::name],
                                           v[IDs::startTime],
                                           v[IDs::length],
                                           v[IDs::gainDb]);

                s->keyNote = juce::jlimit (0, 127, static_cast<int> (v[IDs::keyNote]));
                s->minNote = juce::jlimit (0, 127, static_cast<int> (v[IDs::minNote]));
                s->maxNote = juce::jlimit (0, 127, static_cast<int> (v[IDs::maxNote]));
                s->pan = juce::jlimit (-1.0f, 1.0f, static_cast<float> (v[IDs::pan]));
                s->openEnded = v[IDs::openEnded];
                s->reversed = v[IDs::isReversed];
                s->minVelocity = juce::jlimit (0, 127, static_cast<int> (v[IDs::minVelocity]));
                s->maxVelocity = juce::jlimit (0, 127, static_cast<int> (v[IDs::maxVelocity]));
                s->outputIndex = juce::jlimit (0, 64, static_cast<int> (v[IDs::busNum]));
                s->attack = juce::jlimit (0.001f, 10.0f, static_cast<float> (v.getProperty (IDs::ampAttack, 0.01f)));
                s->decay = juce::jlimit (0.001f, 10.0f, static_cast<float> (v.getProperty (IDs::ampDecay, 0.1f)));
                s->sustain = juce::jlimit (0.0f, 1.0f, static_cast<float> (v.getProperty (IDs::ampSustain, 1.0f)));
                s->release = juce::jlimit (0.001f, 10.0f, static_cast<float> (v.getProperty (IDs::ampRelease, 0.1f)));

                newSounds.add (s);
            }
        }

        for (auto newSound : newSounds)
        {
            for (auto s : soundList)
            {
                if (s->source == newSound->source
                    && s->startTime == newSound->startTime
                    && s->length == newSound->length)
                {
                    newSound->audioFile = s->audioFile;
                    newSound->fileStartSample = s->fileStartSample;
                    newSound->fileLengthSamples = s->fileLengthSamples;
                    newSound->audioData = s->audioData;
                }
            }
        }

        {
            const juce::ScopedLock sl (lock);
            allNotesOff();
            soundList.swapWith (newSounds);

            sourceMediaChanged();
        }

        newSounds.clear();
        changed();
    }

    void SamplerPlugin::initialise (const PluginInitialisationInfo&)
    {
        const juce::ScopedLock sl (lock);
        allNotesOff();
    }

    void SamplerPlugin::deinitialise()
    {
        allNotesOff();
    }

    //==============================================================================
    void SamplerPlugin::playNotes (const juce::BigInteger& keysDown)
    {
        const juce::ScopedLock sl (lock);

        if (highlightedNotes != keysDown)
        {
            for (int i = playingNotes.size(); --i >= 0;)
                if ((! keysDown[playingNotes.getUnchecked (i)->note])
                    && highlightedNotes[playingNotes.getUnchecked (i)->note]
                    && ! playingNotes.getUnchecked (i)->openEnded)
                    playingNotes.getUnchecked (i)->samplesLeftToPlay = minimumSamplesToPlayWhenStopping;

            for (int note = 128; --note >= 0;)
            {
                if (keysDown[note] && ! highlightedNotes[note])
                {
                    for (auto ss : soundList)
                    {
                        if (ss->minNote <= note
                            && ss->maxNote >= note
                            && ss->audioData.getNumSamples() > 0
                            && (! ss->audioFile.isNull())
                            && playingNotes.size() < maximumSimultaneousNotes)
                        {
                            playingNotes.add (new SampledNote (note,
                                                               ss->keyNote,
                                                               0.75f,
                                                               ss->audioFile,
                                                               sampleRate,
                                                               0,
                                                               ss->audioData,
                                                               ss->fileLengthSamples,
                                                               ss->gainDb,
                                                               ss->pan,
                                                               ss->openEnded,
                                                               ss->attack,
                                                               ss->decay,
                                                               ss->sustain,
                                                               ss->release,
                                                               ss->outputIndex));
                        }
                    }
                }
            }

            highlightedNotes = keysDown;
        }
    }

    int SamplerPlugin::getNumOutputChannelsGivenInputs (int numInputs)
    {
        (void) numInputs;
        // Return 16 stereo outputs (32 channels total) if possible, or at least 2
        return juce::jmax (2, 32);
    }

    void SamplerPlugin::allNotesOff()
    {
        const juce::ScopedLock sl (lock);
        playingNotes.clear();
        highlightedNotes.clear();
    }

    void SamplerPlugin::applyToBuffer (const PluginRenderContext& fc)
    {
        if (fc.destBuffer != nullptr)
        {
            SCOPED_REALTIME_CHECK

            const juce::ScopedLock sl (lock);

            clearChannels (*fc.destBuffer, 2, -1, fc.bufferStartSample, fc.bufferNumSamples);

            if (fc.bufferForMidiMessages != nullptr)
            {
                if (fc.bufferForMidiMessages->isAllNotesOff)
                {
                    playingNotes.clear();
                    highlightedNotes.clear();
                }

                for (auto& m : *fc.bufferForMidiMessages)
                {
                    if (m.isNoteOn())
                    {
                        const int note = m.getNoteNumber();
                        const int noteTimeSample = juce::roundToInt (m.getTimeStamp() * sampleRate);

                        for (auto playingNote : playingNotes)
                        {
                            if (playingNote->note == note && ! playingNote->openEnded)
                            {
                                playingNote->noteOff();
                                highlightedNotes.clearBit (note);
                            }
                        }

                        for (auto ss : soundList)
                        {
                            const int vel = juce::roundToInt (m.getVelocity());
                            if (ss->minNote <= note
                                && ss->maxNote >= note
                                && ss->minVelocity <= vel
                                && ss->maxVelocity >= vel
                                && ss->audioData.getNumSamples() > 0
                                && playingNotes.size() < maximumSimultaneousNotes)
                            {
                                highlightedNotes.setBit (note);

                                playingNotes.add (new SampledNote (note,
                                                                   ss->keyNote,
                                                                   m.getVelocity() / 127.0f,
                                                                   ss->audioFile,
                                                                   sampleRate,
                                                                   noteTimeSample,
                                                                   ss->audioData,
                                                                   ss->fileLengthSamples,
                                                                   ss->gainDb,
                                                                   ss->pan,
                                                                   ss->openEnded,
                                                                   ss->attack,
                                                                   ss->decay,
                                                                   ss->sustain,
                                                                   ss->release,
                                                                   ss->outputIndex));
                            }
                        }
                    }
                    else if (m.isNoteOff())
                    {
                        const int note = m.getNoteNumber();
                        const int noteTimeSample = juce::roundToInt (m.getTimeStamp() * sampleRate);

                        for (auto playingNote : playingNotes)
                        {
                            if (playingNote->note == note && ! playingNote->openEnded)
                            {
                                playingNote->samplesLeftToPlay = std::min (playingNote->samplesLeftToPlay,
                                                                           std::max (minimumSamplesToPlayWhenStopping,
                                                                                     noteTimeSample));

                                highlightedNotes.clearBit (note);
                            }
                        }
                    }
                    else if (m.isAllNotesOff() || m.isAllSoundOff())
                    {
                        playingNotes.clear();
                        highlightedNotes.clear();
                    }
                }
            }

            for (int i = playingNotes.size(); --i >= 0;)
            {
                auto sn = playingNotes.getUnchecked (i);

                sn->addNextBlock (*fc.destBuffer, fc.bufferStartSample, fc.bufferNumSamples);

                if (sn->isFinished)
                    playingNotes.remove (i);
            }
        }
    }

    //==============================================================================
    int SamplerPlugin::getNumSounds() const
    {
        return std::accumulate (state.begin(), state.end(), 0, [] (int total, auto v)
                                { return total + (v.hasType (IDs::SOUND) ? 1 : 0); });
    }

    juce::String SamplerPlugin::getSoundName (int index) const
    {
        return getSound (index)[IDs::name];
    }

    void SamplerPlugin::setSoundName (int index, const juce::String& n)
    {
        getSound (index).setProperty (IDs::name, n, getUndoManager());
    }

    bool SamplerPlugin::hasNameForMidiNoteNumber (int note, int, juce::String& noteName)
    {
        juce::String s;

        {
            const juce::ScopedLock sl (lock);

            for (auto ss : soundList)
            {
                if (ss->minNote <= note && ss->maxNote >= note)
                {
                    if (s.isNotEmpty())
                        s << " + " << ss->name;
                    else
                        s = ss->name;
                }
            }
        }

        noteName = s;
        return true;
    }

    AudioFile SamplerPlugin::getSoundFile (int index) const
    {
        const juce::ScopedLock sl (lock);

        if (auto s = soundList[index])
            return s->audioFile;

        return AudioFile (edit.engine);
    }

    juce::String SamplerPlugin::getSoundMedia (int index) const
    {
        const juce::ScopedLock sl (lock);

        if (auto s = soundList[index])
            return s->source;

        return {};
    }

    int SamplerPlugin::getKeyNote (int index) const { return getSound (index)[IDs::keyNote]; }
    int SamplerPlugin::getMinKey (int index) const { return getSound (index)[IDs::minNote]; }
    int SamplerPlugin::getMaxKey (int index) const { return getSound (index)[IDs::maxNote]; }
    float SamplerPlugin::getSoundGainDb (int index) const { return getSound (index)[IDs::gainDb]; }
    float SamplerPlugin::getSoundPan (int index) const { return getSound (index)[IDs::pan]; }
    double SamplerPlugin::getSoundStartTime (int index) const { return getSound (index)[IDs::startTime]; }
    bool SamplerPlugin::isSoundOpenEnded (int index) const { return getSound (index)[IDs::openEnded]; }
    bool SamplerPlugin::isSoundReversed (int index) const { return getSound (index)[IDs::isReversed]; }
    int SamplerPlugin::getSoundMinVelocity (int index) const { return getSound (index)[IDs::minVelocity]; }
    int SamplerPlugin::getSoundMaxVelocity (int index) const { return getSound (index)[IDs::maxVelocity]; }
    int SamplerPlugin::getSoundOutputIndex (int index) const { return getSound (index)[IDs::busNum]; }
    float SamplerPlugin::getSoundAttack (int index) const { return getSound (index).getProperty (IDs::ampAttack, 0.01f); }
    float SamplerPlugin::getSoundDecay (int index) const { return getSound (index).getProperty (IDs::ampDecay, 0.1f); }
    float SamplerPlugin::getSoundSustain (int index) const { return getSound (index).getProperty (IDs::ampSustain, 1.0f); }
    float SamplerPlugin::getSoundRelease (int index) const { return getSound (index).getProperty (IDs::ampRelease, 0.1f); }

    void SamplerPlugin::setSoundReversed (int index, bool r)
    {
        getSound (index).setProperty (IDs::isReversed, r, getUndoManager());
    }

    void SamplerPlugin::setSoundVelocityRange (int index, int minVel, int maxVel)
    {
        auto um = getUndoManager();
        auto v = getSound (index);
        v.setProperty (IDs::minVelocity, minVel, um);
        v.setProperty (IDs::maxVelocity, maxVel, um);
    }

    void SamplerPlugin::setSoundOutputIndex (int index, int oi)
    {
        getSound (index).setProperty (IDs::busNum, oi, getUndoManager());
    }

    void SamplerPlugin::setSoundAdsr (int index, float a, float d, float s, float r)
    {
        auto um = getUndoManager();
        auto v = getSound (index);
        v.setProperty (IDs::ampAttack, a, um);
        v.setProperty (IDs::ampDecay, d, um);
        v.setProperty (IDs::ampSustain, s, um);
        v.setProperty (IDs::ampRelease, r, um);
    }

    double SamplerPlugin::getSoundLength (int index) const
    {
        const double l = getSound (index)[IDs::length];

        if (l == 0.0)
        {
            const juce::ScopedLock sl (lock);

            if (auto s = soundList[index])
                return s->length;
        }

        return l;
    }

    juce::String SamplerPlugin::addSound (const juce::String& source, const juce::String& name, double startTime, double length, float gainDb)
    {
        const int maxNumSamples = 64;

        if (getNumSounds() >= maxNumSamples)
            return TRANS ("Can't load any more samples");

        auto v = createValueTree (IDs::SOUND,
                                  IDs::source,
                                  source,
                                  IDs::name,
                                  name,
                                  IDs::startTime,
                                  startTime,
                                  IDs::length,
                                  length,
                                  IDs::keyNote,
                                  72,
                                  IDs::minNote,
                                  72 - 24,
                                  IDs::maxNote,
                                  72 + 24,
                                  IDs::gainDb,
                                  gainDb,
                                  IDs::pan,
                                  (double) 0,
                                  IDs::isReversed,
                                  false,
                                  IDs::minVelocity,
                                  0,
                                  IDs::maxVelocity,
                                  127,
                                  IDs::busNum,
                                  0,
                                  IDs::ampAttack,
                                  0.01,
                                  IDs::ampDecay,
                                  0.1,
                                  IDs::ampSustain,
                                  1.0,
                                  IDs::ampRelease,
                                  0.1);

        state.addChild (v, -1, getUndoManager());
        return {};
    }

    void SamplerPlugin::removeSound (int index)
    {
        state.removeChild (index, getUndoManager());

        const juce::ScopedLock sl (lock);
        playingNotes.clear();
        highlightedNotes.clear();
    }

    void SamplerPlugin::setSoundParams (int index, int keyNote, int minNote, int maxNote)
    {
        auto um = getUndoManager();

        auto v = getSound (index);
        v.setProperty (IDs::keyNote, juce::jlimit (0, 127, keyNote), um);
        v.setProperty (IDs::minNote, juce::jlimit (0, 127, std::min (minNote, maxNote)), um);
        v.setProperty (IDs::maxNote, juce::jlimit (0, 127, std::max (minNote, maxNote)), um);
    }

    void SamplerPlugin::setSoundGains (int index, float gainDb, float pan)
    {
        auto um = getUndoManager();

        auto v = getSound (index);
        v.setProperty (IDs::gainDb, juce::jlimit (-48.0f, 48.0f, gainDb), um);
        v.setProperty (IDs::pan, juce::jlimit (-1.0f, 1.0f, pan), um);
    }

    void SamplerPlugin::setSoundExcerpt (int index, double start, double length)
    {
        auto um = getUndoManager();

        auto v = getSound (index);
        v.setProperty (IDs::startTime, start, um);
        v.setProperty (IDs::length, length, um);
    }

    void SamplerPlugin::setSoundOpenEnded (int index, bool b)
    {
        auto um = getUndoManager();

        auto v = getSound (index);
        v.setProperty (IDs::openEnded, b, um);
    }

    void SamplerPlugin::setSoundMedia (int index, const juce::String& source)
    {
        auto v = getSound (index);
        v.setProperty (IDs::source, source, getUndoManager());
        triggerAsyncUpdate();
    }

    juce::ValueTree SamplerPlugin::getSound (int soundIndex) const
    {
        int index = 0;

        for (auto v : state)
            if (v.hasType (IDs::SOUND))
                if (index++ == soundIndex)
                    return v;

        return {};
    }

    //==============================================================================
    juce::Array<Exportable::ReferencedItem> SamplerPlugin::getReferencedItems()
    {
        juce::Array<ReferencedItem> results;

        // must be careful to generate this list in the right order..
        for (int i = 0; i < getNumSounds(); ++i)
        {
            auto v = getSound (i);

            Exportable::ReferencedItem ref;
            ref.itemID = ProjectItemID::fromProperty (v, IDs::source);
            ref.firstTimeUsed = v[IDs::startTime];
            ref.lengthUsed = v[IDs::length];
            results.add (ref);
        }

        return results;
    }

    void SamplerPlugin::reassignReferencedItem (const ReferencedItem& item, ProjectItemID newID, double newStartTime)
    {
        auto index = getReferencedItems().indexOf (item);

        if (index >= 0)
        {
            auto um = getUndoManager();

            auto v = getSound (index);
            v.setProperty (IDs::source, newID.toString(), um);
            v.setProperty (IDs::startTime, static_cast<double> (v[IDs::startTime]) - newStartTime, um);
        }
        else
        {
            jassertfalse;
        }
    }

    void SamplerPlugin::sourceMediaChanged()
    {
        const juce::ScopedLock sl (lock);

        for (auto s : soundList)
            s->refreshFile();
    }

    void SamplerPlugin::restorePluginStateFromValueTree (const juce::ValueTree& v)
    {
        copyValueTree (state, v, getUndoManager());
    }

    //==============================================================================
    SamplerPlugin::SamplerSound::SamplerSound (SamplerPlugin& sf,
                                               const juce::String& source_,
                                               const juce::String& name_,
                                               const double startTime_,
                                               const double length_,
                                               const float gainDb_)
        : owner (sf),
          source (source_),
          name (name_),
          gainDb (juce::jlimit (-48.0f, 48.0f, gainDb_)),
          startTime (startTime_),
          length (length_),
          audioFile (owner.edit.engine, SourceFileReference::findFileFromString (owner.edit, source))
    {
        setExcerpt (startTime_, length_);

        keyNote = audioFile.getInfo().loopInfo.getRootNote();

        if (keyNote < 0)
            keyNote = 72;

        maxNote = keyNote + 24;
        minNote = keyNote - 24;
    }

    void SamplerPlugin::SamplerSound::setExcerpt (double startTime_, double length_)
    {
        CRASH_TRACER

        if (! audioFile.isValid())
        {
            audioFile = AudioFile (owner.edit.engine, SourceFileReference::findFileFromString (owner.edit, source));

#if JUCE_DEBUG
            if (! audioFile.isValid() && ProjectItemID (source).isValid())
                DBG ("Failed to find media: " << source);
#endif
        }

        if (audioFile.isValid())
        {
            const double minLength = 32.0 / audioFile.getSampleRate();

            startTime = juce::jlimit (0.0, audioFile.getLength() - minLength, startTime_);

            if (length_ > 0)
                length = juce::jlimit (minLength, audioFile.getLength() - startTime, length_);
            else
                length = audioFile.getLength();

            fileStartSample = juce::roundToInt (startTime * audioFile.getSampleRate());
            fileLengthSamples = juce::roundToInt (length * audioFile.getSampleRate());

            if (auto reader = owner.engine.getAudioFileManager().cache.createReader (audioFile))
            {
                audioData.setSize (audioFile.getNumChannels(), fileLengthSamples + 32);
                audioData.clear();

                auto audioDataChannelSet = juce::AudioChannelSet::canonicalChannelSet (audioFile.getNumChannels());
                auto channelsToUse = juce::AudioChannelSet::stereo();

                int total = fileLengthSamples;
                int offset = 0;

                while (total > 0)
                {
                    const int numThisTime = std::min (8192, total);
                    reader->setReadPosition (fileStartSample + offset);

                    if (! reader->readSamples (numThisTime, audioData, audioDataChannelSet, offset, channelsToUse, 2000))
                    {
                        jassertfalse;
                        break;
                    }

                    offset += numThisTime;
                    total -= numThisTime;
                }
            }
            else
            {
                audioData.clear();
            }

            // add a quick fade-in if needed..
            int fadeLen = 0;
            for (int i = audioData.getNumChannels(); --i >= 0;)
            {
                const float* d = audioData.getReadPointer (i);

                if (std::abs (*d) > 0.01f)
                    fadeLen = 30;
            }

            if (fadeLen > 0)
                AudioFadeCurve::applyCrossfadeSection (audioData, 0, fadeLen, AudioFadeCurve::concave, 0.0f, 1.0f);

            if (reversed)
            {
                for (int i = 0; i < audioData.getNumChannels(); ++i)
                {
                    auto* d = audioData.getWritePointer (i);
                    std::reverse (d, d + fileLengthSamples);
                }
            }
        }
        else
        {
            audioFile = AudioFile (owner.edit.engine);
        }
    }

    void SamplerPlugin::SamplerSound::refreshFile()
    {
        audioFile = AudioFile (owner.edit.engine);
        setExcerpt (startTime, length);
    }

} // namespace engine
} // namespace tracktion
