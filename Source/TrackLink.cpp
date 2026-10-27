#include "TrackLink.h"
#include "ParamIDs.h"

const juce::Identifier TrackLink::trackNameTag { "trackName" };

namespace
{
    // The main's controls that linked tracks follow.
    constexpr std::array<const char*, 4> linkedParameters { ParamID::amount, ParamID::hpfDepth, ParamID::reverbDepth,
                                                            ParamID::delayDepth };
}

TrackLink::TrackLink (juce::AudioProcessorValueTreeState& state) : apvts (state)
{
    // Publish the main's controls the moment they change (from the UI, the
    // host's automation, or a state load), not only when it next processes.
    for (auto* id : linkedParameters)
        apvts.addParameterListener (id, this);

    // Added to a set that already has a main: join it as a TRACK, so dropping
    // PreDrop on a track is all it takes. A saved role still wins, since the
    // host restores the state right after construction.
    if (hub->hasMain())
    {
        auto* role = apvts.getParameter (ParamID::role);
        role->setValueNotifyingHost (role->convertTo0to1 ((float) (int) Role::track));
    }

    update();
}

TrackLink::~TrackLink()
{
    for (auto* id : linkedParameters)
        apvts.removeParameterListener (id, this);
    if (registered)
        hub->removeTrack (this);
    if (registeredReturn)
        hub->removeReturn (this);
    hub->releaseMain (this);
}

TrackLink::Role TrackLink::getRole() const noexcept
{
    return (Role) juce::jlimit (0, 2, juce::roundToInt (apvts.getRawParameterValue (ParamID::role)->load()));
}

void TrackLink::setRole (Role newRole)
{
    auto* p = apvts.getParameter (ParamID::role);
    p->setValueNotifyingHost (p->convertTo0to1 ((float) (int) newRole));
    update();
}

void TrackLink::parameterChanged (const juce::String&, float)
{
    publishMainState();   // atomic stores only: fine on whichever thread calls
}

void TrackLink::update()
{
    const auto role = getRole();

    if (const bool track = role == Role::track; track != registered)
    {
        if (track) hub->addTrack (this);
        else       hub->removeTrack (this);
        registered = track;
    }

    if (const bool ret = role == Role::returnTrack; ret != registeredReturn)
    {
        if (ret) hub->addReturn (this);
        else     hub->removeReturn (this);
        registeredReturn = ret;
    }

    if (role == Role::main)
        hub->claimMain (this);   // no-op while another instance is the main
    else
        hub->releaseMain (this);

    publishMainState();
}

void TrackLink::publishMainState() noexcept
{
    if (! isMain())
        return;

    auto& main = hub->mainState();
    const auto param = [this] (const char* id) { return apvts.getRawParameterValue (id)->load(); };
    main.amount.store (param (ParamID::amount));
    main.depths[(size_t) LinkHub::Effect::hpf].store    (param (ParamID::hpfDepth));
    main.depths[(size_t) LinkHub::Effect::reverb].store (param (ParamID::reverbDepth));
    main.depths[(size_t) LinkHub::Effect::delay].store  (param (ParamID::delayDepth));
    for (size_t i = 0; i < main.settings.size(); ++i)
        main.settings[i].store (param (EffectSettings::specs[i].id));
}

juce::String TrackLink::getTrackName() const
{
    if (const auto typed = apvts.state[trackNameTag].toString(); typed.isNotEmpty())
        return typed;

    {
        const juce::ScopedLock sl (lock);
        if (hostName.isNotEmpty())
            return hostName;
    }

    const int index = hub->indexOf (this);
    return index >= 0 ? "Track " + juce::String (index + 1) : juce::String ("Track");
}

void TrackLink::setTrackName (const juce::String& name)
{
    apvts.state.setProperty (trackNameTag, name.trim(), nullptr);
}

void TrackLink::setHostTrackName (const juce::String& name)
{
    const juce::ScopedLock sl (lock);
    hostName = name;
}

float TrackLink::getLinkLevel (LinkHub::Effect effect) const
{
    return apvts.getRawParameterValue (ParamID::trackLevel (effect))->load();
}

void TrackLink::setLinkLevel (LinkHub::Effect effect, float level)
{
    auto* p = apvts.getParameter (ParamID::trackLevel (effect));
    p->beginChangeGesture();
    p->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, level));
    p->endChangeGesture();
}
