# Roadmap

VoicePanel currently turns speech into text, processes completed sentences, inserts text into a remembered field, and plays or saves synthesized speech. Direct voice-message sending is postponed.

## Planned direction: voice messages

The existing capture, VAD, transcription, review and playback components could form the base of a separate voice-message workflow. The intended steps are:

1. Record a message and keep its audio together with its transcript.
2. Review the transcript and listen to the recording before sharing.
3. Choose whether to export the original recording or synthesize a new recording from reviewed text with a selected voice.
4. Select a destination and confirm each send action explicitly.

Future integrations could support direct audio transfer and voice messages in supported applications. They would need separate destination adapters, permission handling, delivery results and retry behavior.

## Conditions before implementation

- The user controls recording, review, destination and sending.
- A dictation shortcut never sends audio or submits a message automatically.
- Recordings remain local until the user explicitly chooses to share them.
- Cloned-voice workflows require a recording the user has the right to use.
- Integrations need their own security review and target-application tests.

This is a direction for future work, not a feature commitment. No timeline is set. The current release has no direct voice-message transmission or automatic message sending.
