// gear for a client elsewhere, such as the Android app over SSH: framed messages on standard input and
// output, recognition, the agent and speech here. A frame is one byte of type, four of length (little
// endian) and that many bytes.
//
// From the client:
//   'A' audio being said: 16-bit little-endian samples, 16 kHz mono
//   'E' the end of what was said: it is transcribed and sent to the agent
//   'M' a message typed (UTF-8): sent to the agent as one said, and answered the same way
//   'X' stop: what is being spoken stops, and nothing more of it comes
//   'U' a message to speak (UTF-8), in place of what is being spoken: its speech comes as a reply's
//   'W' the same as 'U' without the words taught ('Y'): how the speech says it by itself
//   'J' a word, to ask how it is said: answered with 'J'
//   'D' a line for finding a fault (UTF-8), kept in phone-debug.log in gear's data folder
//   'Y' {"word", "say"}: the word taught to be said so from now on (an empty "say" forgets it), or ""
//       to ask for the list; answered with 'Y'
//   'L' the agents there are, answered with 'G'
//   'K' the number of the newest message the phone has (from 'K' below), sent on connecting: the
//       messages after it come again as 'Z', so none sent to a connection that had died is lost
//   'T' the agent to send to from now on, by name or tab title (UTF-8): what a conversation is named by
//   'V' the voice to speak in, "alba" or "mary"
//   'O' always-on, "1" or "0": while on, 'A' goes through gear's own listener, which finds the messages
//       (pauses, then the countdown) and sends them itself, and 'E' is not used
//   'N' always-on: send what has been said now, without waiting for the countdown
//   'B' the phone's app is behind other apps ("1") or in front ("0"): behind, new replies come as text
//       alone, and what is being spoken goes on
//   'Q' always-on, the phone's playback, before each 'A': "1 SECONDS" while it plays the reply, that
//       far into it, or "0". It lines what the phone played up with its microphone, for the echo
//       canceller and the check against the speakers' words, and tells the pipeline a reply is playing,
//       which talking over for 0.3 s stops ('I')
// To the client:
//   'H' ready: {"target": the agent's name or "", "tab": its tab's title, which names its conversation,
//       "voice": "alba"}
//   'M' the message sent, as transcribed
//   'R' the agent's reply, as written
//   'P' speech of the reply: 16-bit little-endian samples, 24 kHz mono
//   'L' before each sentence's speech: {"start", "end": the bytes of the reply or message being spoken
//       it was read from, "at": seconds into that speech where it starts}, for showing what is heard
//   'D' the reply has been spoken
//   'Z' a message to show and not speak: {"seq", "who", "at", "text", "mine" when the user's, "again" when given
//       again after 'K'}: a
//       reply from an agent sent to before, what Panetone posts to Mihai's own channels (the hub
//       follows `panetone watch`), as Telegram shows it, or one given again after the phone's 'K'
//   'K' the number of the message just sent ('M', 'R' or 'Z'), as the hub's log (hub-log.jsonl) has it
//   'I' always-on: the user talked over the reply, which stops: the phone drops what it has of it
//   'G' the agents: [{"name", "tab", "harness", "status"}]
//   'W' always-on, what is being said: {"text": the message so far, "draft": the piece being said,
//       "left": seconds before it is sent, or -1}
//   'J' {"word", "say": as taught or "", "dictionary": CMUdict's way respelled ("proh-tahn") or ""}
//   'Y' the words taught: [{"word", "say"}]
//   'S' what is happening, for the client to show ("hearing", "waiting for NAME")
//   'F' what went wrong, for the client to show
#pragma once

#include <stdbool.h>

// Serves one client on standard input and output; with `hub`, stays running and serves whichever
// client connects to its socket ($XDG_RUNTIME_DIR/gear-hub.sock), the newest taking over, its log of
// messages giving each phone what it missed ('K').
int gear_serve(const char *models, int hear_threads, int say_threads, bool hub);
// `gear serve` with a hub running: standard input and output passed to the hub's socket until either
// end closes. -1 when no hub answers, for serving here instead.
int gear_serve_proxy(void);
