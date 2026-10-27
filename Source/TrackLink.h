#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "LinkHub.h"

/**
    TrackLink: one PreDrop instance's place in the link (see LinkHub).

    Each instance is MAIN (normally on the master), TRACK or RETURN. With
    nothing linked the main processes the whole mix, as a single PreDrop always
    has. Once any TRACK or RETURN links, the main only adds the riser: every
    linked TRACK applies the effects at its own level per effect, and every
    RETURN adds reverb and delay, wet only, to whatever the tracks send it.
    Both follow the main's Amount and depths. A new instance joins as a TRACK
    when a main already exists.

    The role and the track levels are parameters of the processor's state, so
    they are saved and restored with it; this keeps the hub in step with them.
*/
class TrackLink : public LinkHub::LinkedTrack,
                  private juce::AudioProcessorValueTreeState::Listener
{
public:
    enum class Role { main = 0, track = 1, returnTrack = 2 };

    explicit TrackLink (juce::AudioProcessorValueTreeState& state);
    ~TrackLink() override;

    Role getRole() const noexcept;               // any thread
    void setRole (Role newRole);                 // message thread

    /** True for TRACK and RETURN: instances that follow the main. */
    bool followsMain() const noexcept { return getRole() != Role::main; }

    /** True while this instance is the main that linked tracks follow. */
    bool isMain() const noexcept { return hub->isMain (this); }

    LinkHub&       getHub() noexcept       { return *hub; }
    const LinkHub& getHub() const noexcept { return *hub; }

    /** This track's name in the main's list: the name typed into PreDrop, else
        the host's track name, else "Track N". */
    juce::String getTrackName() const;
    void setTrackName (const juce::String& name);   // empty: back to the host's name

    /** The host's name for this track; hosts may report it on any thread. */
    void setHostTrackName (const juce::String& name);

    /** Brings the hub in line with the role (registers, claims or releases
        the main slot). Message thread: after a role change or a state load. */
    void update();

    /** As the main, publishes Amount and the depths for the followers. Atomic
        stores only, so fine on any thread. */
    void publishMainState() noexcept;

    juce::String getLinkName() const override { return getTrackName(); }
    float getLinkLevel (LinkHub::Effect effect) const override;
    void  setLinkLevel (LinkHub::Effect effect, float level) override;

    /** State property holding a track name typed into PreDrop. */
    static const juce::Identifier trackNameTag;

private:
    void parameterChanged (const juce::String& parameterID, float newValue) override;

    juce::AudioProcessorValueTreeState& apvts;

    // One hub per process, shared by every PreDrop instance in it.
    juce::SharedResourcePointer<LinkHub> hub;

    juce::CriticalSection lock;                    // guards hostName
    juce::String          hostName;
    bool                  registered       = false;   // in the hub's track list
    bool                  registeredReturn = false;   // in the hub's return list

    JUCE_DECLARE_NON_COPYABLE (TrackLink)
};
