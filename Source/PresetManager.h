#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

class PreDropAudioProcessor;

/**
    PresetManager: PreDrop's presets, saved as .predrop files.

    A preset holds the sound: every effect's settings and depth, Mix and
    Output, the riser sample with its timing, and any VST3 reverb or delay with
    its own state. It leaves out what belongs to the moment or the session:
    Amount, the role (MAIN / TRACK / RETURN) and a linked track's levels.

    Presets live in one folder (on a Mac ~/Library/Audio/Presets/Zanders/PreDrop,
    elsewhere the user's app-data folder), listed by name. Any .predrop file can
    also be exported anywhere and imported back (which copies it into the
    folder). The name of the last one saved or loaded is kept with the project.

    Loading and saving are for the message thread (they may load plug-ins).
*/
class PresetManager
{
public:
    explicit PresetManager (PreDropAudioProcessor& processor);

    static constexpr const char* fileExtension = ".predrop";
    static const juce::Identifier presetTag;

    /** Where presets are kept unless setFolder() says otherwise. */
    static juce::File defaultFolder();

    void       setFolder (const juce::File& folder) { presetFolder = folder; }
    juce::File getFolder() const                    { return presetFolder; }

    /** The presets in the folder, sorted by name. */
    juce::Array<juce::File> getPresets() const;

    /** The preset last saved or loaded here, or empty. */
    juce::String getCurrentName() const;

    /** Whether a parameter is part of a preset's sound. */
    static bool isSoundParameter (const juce::String& parameterId);

    /** The sound as it is now, as a preset. */
    juce::ValueTree capture() const;

    /** Sets the sound from a preset; anything it doesn't mention returns to its
        default. False (changing nothing) if it isn't a preset. */
    bool apply (const juce::ValueTree& preset);

    // Each returns an error to show, or an empty string on success.

    /** Saves the sound into the folder under `name`, replacing one of that name. */
    juce::String save (const juce::String& name);

    /** Writes the sound to any file (given the .predrop extension). */
    juce::String exportTo (const juce::File& file) const;

    /** Loads a preset file. */
    juce::String load (const juce::File& file);

    /** Loads a preset file and keeps a copy in the folder. */
    juce::String importFrom (const juce::File& file);

    /** The file a preset of this name is saved as. */
    juce::File fileFor (const juce::String& name) const;

private:
    static juce::String write (const juce::ValueTree& preset, const juce::File& file);
    static juce::ValueTree read (const juce::File& file);
    void setCurrentName (const juce::String& name);

    PreDropAudioProcessor& processor;
    juce::File             presetFolder;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetManager)
};
