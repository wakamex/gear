# gear

gear is a voice front-end for coding agents. You speak to an agent such as [Claude Code](https://www.anthropic.com/claude-code) or [Codex](https://github.com/openai/codex), gear sends the message when you stop talking, reads the reply aloud and listens again, all on your own machine. This README records what works and the decisions behind it.

gear is a C program on [gesso](https://github.com/wakamex/gesso), which supplies the window, audio, echo cancellation and downloads, and [hearsay](https://github.com/wakamex/hearsay), which supplies speech detection, recognition and synthesis. It runs on Linux and Windows, on the GPU where one is usable and on the CPU otherwise; the Windows build is 50 MB with echo cancellation, most of it the GPU's compiled shaders, and 6.2 MB built for the CPU only.

## What works

The spoken loop runs end to end, from microphone to message and from reply text to the speakers:

- Listening: each 10 ms of microphone audio goes through gesso's `gs_voice` for echo cancellation, and [Silero VAD](https://github.com/snakers4/silero-vad) decides when speech starts and stops. Speech is cut into pieces at pauses of half a second, each piece keeping the 0.5 s before its start, and a recognizer thread transcribes each piece with Parakeet as it ends. The piece being spoken is also transcribed again every 0.25 s while it lasts, so its words show as they are said; the piece's own transcription replaces that draft, and only it goes through the checks below and into the message. Where Parakeet runs on a GPU, it writes the drafts itself, when no finished piece is waiting. On the CPU a draft already running would hold up the next piece's text by its own length (about 3 s for 10 s of speech with parakeet-ft-v1 on one thread), so the 110M model writes them instead, on a thread of its own, when `parakeet-tdt_ctc-110m-Q4_K_M.gguf` is in the models folder (it is not downloaded, since only a machine without a usable GPU needs it); without it there are no drafts. In the tests a 4.4 s sentence played in real time showed 4 drafts from the recognizer itself, and 15 from the 110M model on its own thread, the last close to the whole sentence either way. A piece whose loudest 100 ms stays under 0.0015 of full scale is not transcribed, since Parakeet writes words such as "Yeah" or "absolutely" into breathing; of the 119 kept pieces it found words in, the 9 under that level were all of that kind, and sentences peaked ten times higher (median). A piece Parakeet did write words into is then scored as speech or as another sound of the voice, since it also writes "Okay" into a throat clear, "um" into a cough and "oh" into a sigh, and a piece that scores under the cut is dropped. On speakers the score was not fitted on, the cut drops 2.0% of speech and lets through 22% of those sounds, against 70% for Silero's own score set to drop as much. When the speakers played anything during a piece, their sound over the same stretch is transcribed after it, and a piece of which at least three words in four are in theirs is dropped as a reply, video or call heard through the speakers; no piece's text reaches the message box before that check. Where the speakers cannot be recorded, what gear itself played stands in for their sound, so the check then covers gear's own replies. In the tests, a reply that reaches the microphone past the canceller stops itself, and the words heard of it ("Here is the" or "Here is the plan", from run to run) make a message without the check and none with it. Pieces add up to one message; once the user has stopped and every piece has its text, a countdown (3 s by default) runs, and talking again stops it.
- Speaking: a reply is synthesized by Kyutai's [Pocket TTS](https://github.com/kyutai-labs/pocket-tts) on a thread of its own and played as it is made, through a `gs_stream` in gesso's mix, in one of two of its voices, alba or mary; a click on the voice in the status bar switches to the other from the next sentence on, and the choice is remembered in `voice.txt` in the models folder. The two were picked by ear from Pocket's 27 English voices, all reading the same five agent replies. Gradium's [Phonon](https://huggingface.co/gradium/phonon-31m-en), built on Pocket TTS at half its size, got as many words across on 300 sentences from a week of agent replies (10.10% of words wrong against 10.50% for Pocket with alba, within noise, transcribed by parakeet-ft-v1 with gear's reading rules below), but every one of its 12 American voices stressed "record" as the noun in "the pipeline doesn't record", and alba took about 10% less time to read the 300. In the tests, mary's speech has a median pitch about 1.3 times alba's, and switching back gives alba's again. hearsay puts a comma before each piece Pocket generates, which keeps a short first word: without it Pocket dropped an opening "I" from 4.4% of replies and from 8.1% of the later pieces of long ones.
- Interrupting: when the user talks for 0.3 s during a reply, the reply stops. A shorter burst does not stop it: through loudspeakers a low vowel of the reply itself can reach the microphone in a way the echo canceller does not model, for about 0.15 s, and stopping at the first 50 ms of speech cut replies off a sentence in. In the tests, the reply stops 0.33 s after the user starts talking with headphones and 0.42 s after over loudspeakers with the echo canceller, and the player is silent 10 ms later.
- Echo: with `-Decho=true`, gesso's WebRTC echo canceller is given everything the speakers play, recorded with gesso's `gs_loopback` (WASAPI loopback on Windows, the default output's monitor on Linux), so neither a reply nor a podcast or call from another app is taken for the user talking. Where the speakers cannot be recorded (macOS, for now), it is given what gear plays. The status bar says which: "echo cancel: all audio" or "echo cancel: gear's audio". In the tests, where the microphone hears the reply at half its level 100 ms late as well as the user, the reply plays on until the user talks; without the canceller it cuts itself off 0.7 to 0.9 s in. The volume stops at 200% because of what the canceller does to the user's voice above it. Measured at 300% on a desktop PC with loudspeakers, the reply reached the microphone as loud as the user's voice at its loudest moments; the canceller took 33 to 37 dB of the reply out while the user was silent, and nothing of the reply became a message, but a sentence said over the reply ("tell me a different story but make it shorter") came out as "I'm sorry that maybe actually" and did not stop the reply. With a phrase of the user's voice mixed into that recording at 13 moments and the recording run through the canceller again, the phrase lost up to 14 dB and was transcribed word for word 7 times at 300%; with the speakers and their echo scaled down to 200% it lost about 3 dB at most (9 times), and to 100% the same (11 times). Without `-Decho`, use headphones.
- Debugging: with `GEAR_DEBUG` set, gear writes what the listener works with into `recordings` in its data folder: 16 kHz WAV files of three channels on one clock (the microphone as recorded, what the speakers played, which the echo canceller works against and which is silent when the speakers are not recorded, and the microphone as cleaned), 5 minutes a file, named for the local time of their first sample, the newest hour kept. A piece in `heard.jsonl` can be found in them to the sample. A reading stopped by talking also leaves its last 4 s of microphone audio beside the program.
- Agents: messages go to a coding agent through [Wakterm](https://github.com/wakamex/wakterm)'s Agent API, and the agent's final reply comes back to be read aloud. `wakterm cli agent catalog` lists the agents, `wakterm cli agent admit --return-final` gives one a message, with its text on standard input so it never shows in the process list, and `wakterm cli agent request get` returns the reply once the agent has finished. Wakterm knows each harness (Claude Code, Codex and the others it supports), so gear has no logic for any one agent. `--to` names an agent or, when no agent has that name, a tab by the title it shows, and gear looks it up again for each message, so a message reaches whatever harness is running there now, even after a restart. A relay thread sends messages in order and asks for each reply twice a second; a one-sentence answer from a Claude agent came back 3.1 s after sending.
- Reading replies: a reply is turned into what is worth hearing before it is spoken, by fixed rules. Code blocks and tables are named rather than read ("A code block of 6 lines.", "A table of 2 rows."), Markdown's marks are dropped, links are read as their text and bare addresses as their site ("a github dot com link"), paths as their file's name, snake_case as words, and headings and list items become sentences. Numbers and the symbols around them are written out as words, since the speech model reads digits unreliably (Pocket TTS read 139 as "39" and 0.34 as "through"): integers, decimals and versions, codes with leading zeros digit by digit, years in pairs, times, dates, percent, money in dollars and cents, units, and a hex hash as its first six characters. On the 300 sentences above, this cut the words Pocket TTS got wrong from 14.88% to 10.88%. The word "read" is written "red" where grammar says it is the past: after a form of be or have ("is read aloud", "has never read") and after he, she or it ("She read only the first page"), unless a verb such as let or did comes before. Phonon, gear's speech model at the time, said "reed" in three such sentences 16 times of 18, and "red" when it was written so 11 times of 12, judged by the vowel's first resonance against the same sentence written with "red" and with "reed". Elsewhere the word is left to the model, which said it correctly 30 times of 30 in five other sentences ("I read the whole file and found", "The agent read the file and stopped", "I will read"). Past 1,200 characters, about a minute and a quarter of speech, the reading stops at a sentence and says the rest is not read. `gear read` prints what would be said of a reply.
- Models: the window downloads any that are missing, showing its progress, and `gear fetch` does the same from the command line: five files, 638 MB in all, from Hugging Face at pinned commits, resuming a broken download and keeping a file only when its size and SHA-256 match the published ones.

| File | What it is | Size |
| --- | --- | ---: |
| `parakeet-ft-v1-Q4_K_M.gguf` | [parakeet-ft-v1](https://huggingface.co/freetranscribe/parakeet-ft-v1), a fine-tune of NVIDIA's Parakeet TDT 0.6B v3, speech recognition (CC-BY-4.0) | 485 MB |
| `pocket-tts-english_2026-09-Q8_0.gguf` | Kyutai Pocket TTS, English speech synthesis (CC-BY-4.0) | 138 MB |
| `alba.safetensors`, `mary.safetensors` | Kyutai's alba and mary voices for Pocket TTS (CC-BY-4.0) | 6 MB each |
| `silero-vad-v6.2-F32.gguf` | Silero VAD v6.2, telling speech from other sound (MIT) | 1.2 MB |

- Window: `gear window` draws the loop in the toolcore-native style: the state (LISTENING, HEARING, SENDING, WAITING, SPEAKING, MUTED or STOPPED), the target agent, the countdown's length with `-` and `+` beside it to change it a second at a time (0 to 10 s, where 0 sends a message as soon as its transcript is in, after the half-second pause that ends a piece; remembered), a volume slider for everything gear plays (0 to 200%, dragged, clicked or turned with the wheel over it, remembered), a microphone meter, the message with its countdown, under the messages sent before (a long message takes up to half the window and shows its newest words; the wheel scrolls back through it and the earlier messages, each under the time it was sent and its agent, the panel taking half the window meanwhile, and the box's title says how many are above), the reply as written with code and tables boxed as not read, and a status bar that turns rust with a fault. Space mutes: the microphone pauses and the pipeline hears silence instead, so a piece of speech under way ends and a counting message is still sent. Esc stops the reading or drops the message, Enter sends now, and `+` and `-` change the text size. `gear window --demo --shot F.png` draws a made-up exchange to a picture without a display.
- Messages sent: each is kept as a line of JSON in `messages.jsonl` in gear's data folder, with the time and the agent, and the newest 200 are shown again after a restart.
- Remembered target: the agent the window last reached with `--to`, and the host it was on, are kept in `target.txt` in the models folder, so starting gear with no options, from a shortcut or the taskbar, goes back to them; `--to` overrides them, and `--to none` forgets them.
- Agents on another machine: with `GEAR_WAKTERM_HOST` naming it, gear runs `wakterm` there through `ssh -T`, the message as a quoted argument, since the Windows ssh client never passes on the end of piped input. Each answer is read until it is a whole JSON object, because a session that admitted a prompt can stay open after the command has exited. From Windows, a message with quotes, `&`, `$` and `;` reached a Claude agent on Linux intact and its reply came back in 2.9 s. gear passes `--no-auto-start` to every `wakterm` call, so a client that cannot find the server fails rather than starting a second one.
- Updates: a published build is a test build, and says so in its window title and status bar with its build name. On its first run it asks whether to keep itself up to date, and until the answer is yes it never looks for updates; clicking the build's name in the status bar turns updating on or off. When on, it reads a manifest from the update server every minute with gesso's `gs_update`, naming the build and each file's name, SHA-256 and size, and checks the manifest's [Ed25519](https://ed25519.cr.yp.to/) signature against the publisher's key built into the program. Files of a newer build are downloaded, checked against their hashes and put in place of the old ones, which are renamed aside. gear restarts into the new build only when nothing is being heard, sent, waited for or read, there has been no key or click for two minutes, and the last update restart was at least 15 minutes ago; clicking "a new test build is ready" restarts at once. The new build comes back muted if gear was, does not take the keyboard, and says once in the status bar that it was updated. A build that keeps failing in its first minutes puts the one before it back and says so. The server is reached over plain HTTP; the signature, made with a key that never leaves the publisher's machine, is what keeps a changed file out. A build of one's own, without `-Dbuild`, never updates itself.
- Updating a machine at once: `tools/windows-update`, run after `tools/publish-update`, copies the published build to the Windows PC over SSH, ends the running gear and starts the new one in the signed-in user's session with deskrun, without waiting for gear to be idle.
- Test download: gear has no release builds yet, only test builds. [gear-test-windows.zip](https://updates.waka.mx/gear/gear-test-windows.zip) is the newest, built by `tools/publish-update` from each commit. It holds the program, its font, its LICENSE and THIRD-PARTY-LICENSES.txt with the notices of the code compiled into it. Unpacked into a folder of its own, it asks on first run whether to update itself, and if you say yes it downloads every new version and restarts when you've left it alone.

Not built yet: the reply's sentence being read shown bright, editing a misheard word in the message, changing the target from the window, the tray, and sending to a terminal pane that runs no agent.

## Roadmap

1. macOS: build and test on a Mac, where the speakers cannot be recorded for the echo canceller yet.

## Build and run

With [Zig](https://ziglang.org/) 0.17.0, which fetches gesso and hearsay at the commits pinned in `build.zig.zon`, and the [glslc](https://github.com/google/shaderc) shader compiler for the GPU build (Fedora's `glslc` package, or `-Dglslc=PATH`):

```sh
zig build -Doptimize=ReleaseFast             # zig-out/bin/gear, 51 MB on Linux, needing only libc
zig build -Doptimize=ReleaseFast -Decho=true # with echo cancellation, for loudspeakers
zig build -Doptimize=ReleaseFast -Dgpu=none  # CPU only, 6.7 MB, no glslc needed
gear fetch                                   # the models, into gear's folder in the user's data folder
gear agents [NAME]                           # the agents Wakterm can reach, or the one NAME reaches
gear listen --to AGENT                       # talk to AGENT; its replies are read aloud
gear send --to AGENT "Some message."         # one message, its reply printed
gear read - < reply.md                       # what would be read aloud of a reply
gear listen --speak-back                     # no agent: each message is printed and read back
gear window --to AGENT                       # the window; GEAR_WAKTERM_HOST=host reaches agents on another machine
```

On Windows gear.exe is a windowed program, so starting it from a shortcut or the taskbar opens no console; run from a terminal, the command-line modes print to it, though the prompt can come back before they finish (pipe the output, for example to `Out-String` in PowerShell, to wait for it). `--cpu` keeps the models on the CPU in a GPU build. `GEAR_MODELS` names another folder for the models. Three modes run the same pipeline on recordings, without a microphone or speakers:

```sh
gear hear speech.wav                         # messages as they would be sent, with their times
gear say "Some reply." reply.wav             # the reply as the player plays it
gear interrupt "Some reply." talking.wav out.wav  # a reply while talking.wav plays as the microphone
```

`zig build test` runs the tests. The listener and file tests always run; with `GEAR_MODELS` set to a folder holding the models, the end-to-end tests also transcribe `tests/utterance.wav` (LibriSpeech test-clean 1089-134686-0010, CC-BY-4.0) through the pipeline and check that talking two seconds into a reply stops it; built with `-Decho=true`, they also check the echo canceller with a simulated loudspeaker. With `GEAR_AGENT` naming an agent kept for tests, they send it a message through Wakterm and check its reply; without it they still check, against the real `wakterm` and an agent that does not exist, that a refused message is reported as refused.

## Decisions

### Window: one compact panel in toolcore-native

The window is to be drawn with gesso's `gs_ui` in the toolcore-native style: warm near-black, one monospace font, square panels, and colour and brightness only for what is happening now. It shows the current exchange only, since the agent's own session keeps the conversation.

```
┌──────────────────────────────────────────────────────────┐
│ ● LISTENING   → pockettts (claude)            ▁▃▅▂ mic   │
├─ MESSAGE ────────────────────────────────────────────────┤
│ fix the countdown so a long pause doesn't send, and      │
│ then run the tests…                                      │
│ ▰▰▰▰▰▰▰▱▱▱  sending in 2 s   enter sends · esc cancels   │
├─ REPLY ──────────────────────────────────────────────────┤
│ I found the bug. The countdown timer resets whenever…    │
│ ┌ code, not read ────────────────────────────────────┐   │
│ │ if (l->below < l->p.pause_frames) …                │   │
│ └────────────────────────────────────────────────────┘   │
│ Want me to commit it?                                    │
├──────────────────────────────────────────────────────────┤
│ echo cancel on · Parakeet 0.07 cpu · Pocket · 0.6 GB     │
└──────────────────────────────────────────────────────────┘
```

- State line: a dot and one word, LISTENING (green), HEARING, SENDING or SPEAKING (amber), MUTED or an error (rust); the target agent, which a click on it or Tab changes: a picker lists the agents Wakterm knows by the title their tab shows, the ones sent to lately first, and typing narrows them to those whose tab or name has what is typed; Enter takes the first, a click any, and Esc closes it. Messages go to the agent picked from then on, a start without `--to` comes back to it, and a reply the agent before still owes is read when it comes; and a microphone level meter, so the user can see they are heard before the words arrive.
- Message: the words appear as they are said, those of the piece still being spoken dim until its own transcription replaces them, and a piece waiting for its text shows as a dim ellipsis. The countdown is an amber bar draining under the text, with its keys printed beside it. Clicking into the text to fix a misheard word stops the countdown.
- Reply: the sentence being read is bright, what was read is dim, what is to come is muted. Code and tables the reading skips stay visible in a box marked as not read. Esc or talking stops the reading.
- Status bar: whether echo cancellation is on (or that headphones are needed), what is loaded, memory in use; a fault replaces it in rust.
- Keys: Space mutes the microphone, Esc cancels the message or stops the reading, Enter sends now, Tab changes the target, `+` and `-` change the font size.
- Tray: gear can wait in the tray with its icon in the state's colour, and the window can collapse to the state line and message panel as a strip at the screen's edge.

### Speech recognition: parakeet-ft-v1 Q4_K_M on transcribe.cpp

gear recognizes speech with parakeet-ft-v1, a fine-tune of NVIDIA's 0.6B Parakeet v3, running on [transcribe.cpp](https://github.com/handy-computer/transcribe.cpp), a C/C++ speech-to-text library built on the [ggml](https://github.com/ggml-org/ggml) tensor library. It uses the 4-bit Q4_K_M weights from [freetranscribe/parakeet-ft-v1-gguf](https://huggingface.co/freetranscribe/parakeet-ft-v1-gguf), a 485 MB file under CC-BY-4.0, on the GPU where one is usable. Each stretch of speech between pauses is transcribed once, when the pause comes, so recognition does no work while the user is silent.

parakeet-ft-v1 is the most accurate option measured, 0.6 to 1.1 points better than v3 with 95% intervals that exclude zero. On an RTX 3080 it transcribes 10 s of speech in 0.20 s, faster than the smallest model on the CPU. On the CPU alone it takes about 3 s on one thread, which mostly falls inside the 3-second countdown before a message is sent. Q4_K_M scores the same as Q8_0 on the sets below at two thirds of the size; Q8_0's transcripts match the original model's more closely word for word, without a measurable difference in error rate. Each option below was timed on the same idle machine, on the CPU, and scored on the same LibriSpeech utterances.

| Model and runtime | Download | Time for 10 s of speech, 1 thread (lower is better) | CPU-seconds per second of speech (lower is better) | Peak memory | Load time | Word error rate, test-clean | Word error rate, test-other |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Parakeet TDT-CTC 110M, Q4_K_M, transcribe.cpp | 90 MB | 0.65 s | 0.066 | 0.31 GB | 0.24 s | 2.70% | 5.23% |
| Parakeet TDT-CTC 110M, int8, [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx) | 108 MB | 0.42 s | 0.043 | 0.51 GB | 1.2 s | 2.39% | 5.23% |
| [parakeet-ft-v1](https://huggingface.co/freetranscribe/parakeet-ft-v1) (a long-form fine-tune of v3), Q4_K_M, transcribe.cpp (chosen) | 485 MB | 3.14 s | 0.306 | 1.08 GB | 1.2 s | 2.10% | 3.61% |
| Parakeet TDT 0.6B v3 (25 languages), int8, sherpa-onnx | 640 MB | 1.83 s | 0.185 | 1.38 GB | 3.1 s | 2.68% | 4.69% |
| [Parakeet Redux](https://huggingface.co/moondream/parakeet-redux) (v3 with ternary weights), transcribe.cpp | 159 MB | 2.76 s | 0.279 | 0.63 GB | 0.4 s | 2.43% | 4.52% |

The same 110M model runs on both transcribe.cpp and sherpa-onnx. sherpa-onnx transcribes it faster, at 0.04 CPU-seconds per second of speech instead of 0.07, and finishes 10 seconds of speech 0.23 s sooner. transcribe.cpp uses 0.2 GB less memory, loads five times faster, which matters when gear unloads models while idle, and ships as about 5 MB of code where sherpa-onnx needs 30 MB with [ONNX Runtime](https://onnxruntime.ai/). The extra time falls inside the 3-second countdown before a message is sent.

The [Parakeet TDT-CTC 110M](https://huggingface.co/nvidia/parakeet-tdt_ctc-110m) is the light option for a machine without a GPU: on the CPU it needs a fifth of parakeet-ft-v1's time and a third of its memory, at a word error rate within noise of v3's. gear runs any Parakeet GGUF in its models folder, such as the 110M's from [handy-computer/parakeet-tdt_ctc-110m-gguf](https://huggingface.co/handy-computer/parakeet-tdt_ctc-110m-gguf): `--hear-model FILE` picks one, a click on the model's name in the window's status bar switches to the next, and the choice is remembered.

Recognition for other languages is not decided. Parakeet TDT 0.6B v3 is the measured candidate; transcribe.cpp runs it as well, but only its sherpa-onnx build was timed.

#### How the speech recognition options were measured

Timing used 57 utterances (460 seconds) from [LibriSpeech](https://www.openslr.org/12) test-clean and test-other, transcribed one at a time with the model loaded once, on an idle Ryzen 7 3800XT (Zen 2 with AVX2, 8 cores) under Fedora 44 in WSL2. The time for 10 s of speech is the median over utterances of encoder plus decoder time, scaled to 10 seconds of audio. Peak memory is the process's peak resident set size; the sherpa-onnx rows ran from a Python script, which adds about 40 MB.

Accuracy used every tenth utterance of test-clean (262 utterances, 5,521 words) and test-other (294 utterances, 5,483 words), normalized with the Whisper English text normalizer that the [Open ASR Leaderboard](https://huggingface.co/spaces/hf-audio/open_asr_leaderboard) uses and scored with [jiwer](https://github.com/jitsi/jiwer). Against Parakeet v3, the chosen model differs by +0.02 percentage points on test-clean (95% interval -0.42 to +0.45) and +0.55 on test-other (-0.11 to +1.22), from a bootstrap that resamples speakers, since utterances from one speaker are not independent.

Versions: transcribe.cpp at commit `90cd63d`, a CPU build whose command-line program, with every model family it supports, is 5.2 MB; sherpa-onnx 1.13.8. The Redux row ran on [NairoDorian/transcribe.cpp](https://github.com/NairoDorian/transcribe.cpp) at `692350d`, a fork that adds the ternary weight type Redux needs.

LibriSpeech is read audiobook speech. How these models handle coding vocabulary, background noise, and a reply playing through the loudspeakers has not been measured yet.

### Models run on the GPU where one is usable

gear is built with ggml's [Vulkan](https://www.vulkan.org/) backend, so both models run on the GPU of any vendor whose driver supports Vulkan, and on the CPU where there is no GPU, no Vulkan driver, or `--cpu` was given. The GPU makes the larger, more accurate recognition models cheap: on an RTX 3080, parakeet-ft-v1 transcribes 10 s of speech in 0.20 s, three times faster than the 110M model on one CPU thread, and Pocket starts speaking 10 to 50 ms after a reply is queued instead of about 100 ms.

Times on a Windows 11 PC with a Ryzen 7 3800XT and an RTX 3080, recognition on one CPU thread and synthesis on two, the models loaded once:

| Work | GPU (RTX 3080) | CPU |
| --- | ---: | ---: |
| parakeet-ft-v1 Q4_K_M, time for 10 s of speech | 0.20 s | 2.97 s |
| parakeet-ft-v1 Q4_K_M, slowest of 57 utterances | 0.49 s | |
| Parakeet TDT-CTC 110M Q4_K_M, time for 10 s of speech | 0.10 s | 0.69 s |
| Pocket TTS, time to first audio | 0.01 to 0.05 s | 0.07 to 0.13 s |
| Pocket TTS, time for 6 s of speech | 0.14 s | 1.64 s |
| Load, parakeet-ft-v1 | 0.50 s | 0.34 s |

The GPU gives the same accuracy: on the 556 LibriSpeech utterances used for accuracy above, parakeet-ft-v1 scores 2.10% and 3.54% on the GPU against 2.10% and 3.61% on the CPU, and the 110M model 2.70% and 5.27% against 2.64% and 5.16%; the transcripts differ only where rounding tips a word.

The first run after a GPU driver is installed or updated compiles the shaders for that GPU: the first piece of speech then took 15 s and the first reply 2.9 s to start. The driver keeps what it compiled, and later runs start at the speeds above.

The cost is size: the compiled shaders add 44 MB to the program, which compresses to 8 MB with zstd. The Vulkan library is opened when gear starts rather than linked, so the same program starts on a machine without it. On Linux, gear on a machine without a GPU, and in a container without the Vulkan library, ran on the CPU with the same transcripts.

### Speech detection: Silero VAD at 0.5

The detector decides when the user has started talking. Its mistakes cost twice: a false start stops a reply being read, and the piece it cuts may be transcribed into words that were never said. gs_voice's detector, the small network WebRTC's gain control uses, took breathing and room noise for speech: of 247 pieces gear kept in daily use, 239 had no words in them. Silero VAD, run through hearsay on transcribe.cpp, rejects almost all of that and catches as many short words.

Each detector's scores for the same recordings were replayed through gear's listener, counting the recordings in which a piece of speech starts (lower is better for the first two columns, higher for the last two):

| Detector and rule to start a piece | Kept pieces with no words (240) | Background noise (408) | One-word recordings (4,074) | LibriSpeech sentences (556) |
| --- | ---: | ---: | ---: | ---: |
| gs_voice, 0.8 held for 30 ms (before) | 65.0% | 39.0% | 98.9% | 100% |
| gs_voice, 0.8 held for 100 ms | 7.9% | 32.6% | 95.4% | 100% |
| Silero, 0.5 held for two 32 ms windows (chosen) | 5.0% | 0.5% | 99.2% | 100% |
| Silero, 0.7 held for two 32 ms windows | 1.2% | 0.0% | 98.8% | 100% |

Holding gs_voice's start longer drops the breaths but also misses short words such as "up" and "stop", and takes a third of the background noise for speech whatever the rule. Silero at 0.7 rejects a little more, but it was slower to notice the user talking over a reply played through loudspeakers: 0.11 to 0.24 s against 0.11 to 0.17 s at 0.5. Silero costs about 0.06 ms of CPU for each 32 ms (transcribe.cpp's measurement on an Apple M4) and adds a 1.2 MB model; gs_voice still cancels the echo.

How it was measured: each recording was played with 0.5 s of silence before and 1 s after through a fresh detector, one score per 10 ms frame for gs_voice and per 32 ms window for Silero (the Python [silero-vad](https://pypi.org/project/silero-vad/) package, 6.2), and gear's listener was replayed over the scores. The kept pieces are the ones gs_voice had started on and Parakeet found no words in, so that column favours any other detector; the background noise is the 408 one-second clips of noise in the test set of Google's [Speech Commands](https://arxiv.org/abs/1804.03209) v0.02, whose one-word recordings of its ten command words (yes, no, up, down, left, right, on, off, stop, go) make the word column; the sentences are the LibriSpeech utterances used for accuracy above.

### Speech or another sound of the voice: a score from both detectors and the recognizer

A throat clear reached an agent as the message "Okay". The voice detector starts a piece for a throat clear, a cough, a sigh or a laugh as it does for a word, and Parakeet writes words into some of those pieces. Which signals tell them from speech? At a setting that drops 2% of speech, Silero's own score lets 70% of such pieces through, and a model over Silero's score, gs_voice's and Parakeet's raw scores lets 22% through. The comparison is on pieces cut by gear's own listener from public recordings, with each method scored on speakers it was not fitted on.

| Told apart by | Other sounds of the voice let through when 2% of speech is dropped |
| --- | ---: |
| Silero alone: the time its score spent at 0.95 or more | 70% |
| Logistic regression over both detectors and Parakeet's reported confidence | 49% |
| Logistic regression over all 17 inputs | 37% |
| Boosted trees over all 17 inputs, 100 trees of 15 leaves: what gear runs | 22% |

The speech is [Speech Commands](https://arxiv.org/abs/1804.03209)' test set of single words and 1,000 read sentences of [LibriSpeech](https://www.openslr.org/12) test-clean (both CC BY 4.0), 5,664 pieces with words. The other sounds are [VocalSound](https://github.com/YuanGongND/vocalsound) (CC BY-SA 4.0), 21,024 recordings of coughs, throat clearing, sighs, sneezes, sniffs and laughter, of which Parakeet wrote words into 4,968 pieces, such as "uh", "um", "oh", "yeah" and "ha ha". The trees are fitted with [LightGBM](https://lightgbm.readthedocs.io/) by `tools/fit-speech-model`, which writes them as a table in `src/speech_model.h`; `gear hear FILE --keep DIR` writes each piece's inputs to `DIR/heard.jsonl`, the rows the tool is fitted on.

By kind, the shipped model lets through 19% of throat clearing, 31% of coughs, 28% of sighs, 26% of sneezes, 23% of sniffs and 10% of laughter, and it drops 2.6% of single words and 0.1% of sentence pieces.

The inputs are the share of a piece's speech frames that Silero scored at 0.8, 0.9, 0.95 and 0.99 or more, with its peak and mean; the same for gs_voice's detector; and Parakeet's scores for the word pieces it wrote, taken before the softmax through hearsay: the chosen piece's score, its margin over writing nothing and over the next best piece, the log-sum-exp, its probability and the reported confidence. Of the trees' gain, 27% comes from Silero's share at 0.99 or more and 21% from Parakeet's margin over writing nothing, the two largest. Since the inputs overlap, leaving a group out says more: fitted without Parakeet's six scores the trees let 46% through, without Silero's six 31%, and without gs_voice's five 27%. The reported confidence alone, which is computed after the softmax, gave a throat clear's "Okay" 0.84 and spoken ones 0.76 to 0.93. No input counts frames or words. A first model had the time above each threshold and the number of word pieces among its inputs and was fitted on single words alone; it dropped every one of 44 pieces of six words or more from real use, having learned that a long sound is not a word.

A piece's score moves with where Silero's 32 ms windows fall on it. One recorded sentence scored from 0.17 to 0.75 when shifted by 0 to 25 ms, with the cut at 0.13. Sixteen commands and dictated sentences from real use, each shifted five ways, were kept in all 80 runs. The fitting recordings come from other microphones than the one gear listens to, so the figures above are for those recordings.

## Credits

[Earheart](https://github.com/cleanunicorn/earheart), an MIT-licensed dictation app by another author, is prior work for running local speech models in a lean native app.

## License

gear's own files are under the MIT license in [LICENSE](LICENSE), the icon in `assets/icon/` included.

`assets/ClankerMono-Regular.ttf`, the window's font, is Clanker Mono, built from the glyphs of NAVER Corporation's D2Coding, under the SIL Open Font License in [assets/ClankerMono-LICENSE.txt](assets/ClankerMono-LICENSE.txt).

`tests/utterance.wav` is utterance 1089-134686-0010 of LibriSpeech test-clean, by Vassil Panayotov, Guoguo Chen, Daniel Povey and Sanjeev Khudanpur ([LibriSpeech](https://www.openslr.org/12), 2015), under [CC-BY-4.0](https://creativecommons.org/licenses/by/4.0/). It was converted from FLAC to a 16-bit WAV at its original 16 kHz, without trimming.

The models gear downloads are not part of it: Parakeet and Pocket TTS with its voices are under CC-BY-4.0, and Kyutai's terms for Pocket TTS prohibit cloning a voice without consent and deceptive use; Silero VAD is under MIT.
