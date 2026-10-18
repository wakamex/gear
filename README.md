# gear

gear is a voice front-end for coding agents. You speak to an agent such as [Claude Code](https://www.anthropic.com/claude-code) or [Codex](https://github.com/openai/codex), gear sends the message when you stop talking, reads the reply aloud and listens again, all on your own machine. [BRIEF.md](BRIEF.md) holds the goals the project started from; this README records the decisions made since.

gear is a C program on [gesso](https://github.com/wakamex/gesso), which supplies the window, audio, the voice detector and downloads, and [hearsay](../hearsay), which supplies speech recognition and synthesis. It runs on Linux and Windows, on the GPU where one is usable and on the CPU otherwise; the Windows build is 50 MB with echo cancellation, most of it the GPU's compiled shaders, and 6.2 MB built for the CPU only.

## What works

The spoken loop runs end to end, from microphone to message and from reply text to the speakers:

- Listening: each 10 ms of microphone audio goes through gesso's `gs_voice`, whose voice detector decides when speech starts and stops. Speech is cut into pieces at pauses of half a second, each piece keeping the 0.3 s before its start, and a recognizer thread transcribes each piece with Parakeet as it ends. Pieces add up to one message; once the user has stopped and every piece has its text, a countdown (3 s by default) runs, and talking again stops it.
- Speaking: a reply is synthesized by Pocket TTS on a thread of its own and played as it is made, through a `gs_stream` in gesso's mix. The first sound plays 0.17 to 0.31 s after the reply is queued.
- Interrupting: when the user starts talking during a reply, the reply stops. In the tests, talking is detected 60 ms after the speech begins and the player is silent 10 ms later.
- Echo: with `-Decho=true`, gesso's WebRTC echo canceller is given what gear plays, so a reply through loudspeakers is not taken for the user talking. In the tests, where the microphone hears the reply at half its level 100 ms late as well as the user, the reply plays on until the user talks; without the canceller it cuts itself off 0.4 to 0.5 s in. Without `-Decho`, use headphones.
- Agents: messages go to a coding agent through [Wakterm](https://github.com/wakamex/wakterm)'s Agent API, and the agent's final reply comes back to be read aloud. `wakterm cli agent catalog` lists the agents, `wakterm cli agent admit --return-final` gives one a message, with its text on standard input so it never shows in the process list, and `wakterm cli agent request get` returns the reply once the agent has finished. Wakterm knows each harness (Claude Code, Codex and the others it supports), so gear has no logic for any one agent. `--to` names an agent or, when no agent has that name, a tab, and gear looks it up again for each message, so a message reaches whatever harness is running there now, even after a restart. A relay thread sends messages in order and asks for each reply twice a second; a one-sentence answer from a Claude agent came back 3.1 s after sending.
- Reading replies: a reply is turned into what is worth hearing before it is spoken, by fixed rules. Code blocks and tables are named rather than read ("A code block of 6 lines.", "A table of 2 rows."), Markdown's marks are dropped, links are read as their text and bare addresses as their site ("a github.com link"), paths as their file's name, snake_case as words, and headings and list items become sentences. Past 1,200 characters, about a minute and a quarter of speech, the reading stops at a sentence and says the rest is not read. `gear read` prints what would be said of a reply.
- Models: `gear fetch` downloads the three model files, 629 MB in all, from Hugging Face at pinned commits, resuming a broken download and keeping a file only when its size and SHA-256 match the published ones.

| File | What it is | Size |
| --- | --- | ---: |
| `parakeet-ft-v1-Q4_K_M.gguf` | [parakeet-ft-v1](https://huggingface.co/freetranscribe/parakeet-ft-v1), a fine-tune of NVIDIA's Parakeet TDT 0.6B v3, speech recognition (CC-BY-4.0) | 485 MB |
| `pocket-tts-english_2026-09-Q8_0.gguf` | Kyutai [Pocket TTS](https://github.com/kyutai-labs/pocket-tts), English speech synthesis (CC-BY-4.0) | 138 MB |
| `alba.safetensors` | Kyutai's alba voice for Pocket TTS (CC-BY-4.0) | 6 MB |

- Window: `gear window` draws the loop in the toolcore-native style: the state (LISTENING, HEARING, SENDING, WAITING, SPEAKING, MUTED or STOPPED), the target agent, a microphone meter, the message with its countdown, the reply as written with code and tables boxed as not read, and a status bar that turns rust with a fault. Space mutes, Esc stops the reading or drops the message, Enter sends now, and `+` and `-` change the text size. `gear window --demo --shot F.png` draws a made-up exchange to a picture without a display.
- Agents on another machine: with `GEAR_WAKTERM_HOST` naming it, gear runs `wakterm` there through `ssh -T`, the message as a quoted argument, since the Windows ssh client never passes on the end of piped input. Each answer is read until it is a whole JSON object, because a session that admitted a prompt can stay open after the command has exited. From Windows, a message with quotes, `&`, `$` and `;` reached a Claude agent on Linux intact and its reply came back in 2.9 s. gear passes `--no-auto-start` to every `wakterm` call, so a client that cannot find the server fails rather than starting a second one.
- Updates: a published build keeps itself up to date, as gsignal, the author's Signal client, does. Every minute it reads a manifest from the update server, naming the build and each file's name, SHA-256 and size, and checks the manifest's [Ed25519](https://ed25519.cr.yp.to/) signature against the publisher's key built into the program. When it names another build, the files that differ are downloaded and checked against their hashes, put in place of the old ones, which are renamed aside, and once gear has had nothing to hear, send, wait for or read for 10 s, the window closes and the new build starts in its place. The server is reached over plain HTTP; the signature, made with a key that never leaves the publisher's machine, is what keeps a changed file out. `tools/publish-update` builds the current commit for Windows and publishes it, with `gear-windows.zip` for a first install. A build of one's own, without `-Dbuild`, never updates itself.

Not built yet: the reply's sentence being read shown bright, editing a misheard word in the message, changing the target from the window, the tray, and sending to a terminal pane that runs no agent.

## Build and run

With [Zig](https://ziglang.org/) 0.17.0, beside a checkout of hearsay in `../hearsay`, and the [glslc](https://github.com/google/shaderc) shader compiler for the GPU build (Fedora's `glslc` package, or `-Dglslc=PATH`):

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

`--cpu` keeps the models on the CPU in a GPU build. `GEAR_MODELS` names another folder for the models. Three modes run the same pipeline on recordings, without a microphone or speakers:

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

- State line: a dot and one word, LISTENING (green), HEARING, SENDING or SPEAKING (amber), MUTED or an error (rust); the target agent, which a click or Tab changes; and a microphone level meter, so the user can see they are heard before the words arrive.
- Message: the words appear piece by piece, a piece still being transcribed as a dim ellipsis. The countdown is an amber bar draining under the text, with its keys printed beside it. Clicking into the text to fix a misheard word stops the countdown.
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

## Credits

[Earheart](https://github.com/cleanunicorn/earheart), an MIT-licensed dictation app by another author, is prior work for running local speech models in a lean native app.
