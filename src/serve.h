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
//   'P' a piece of a file of more than 4 MB, sent in pieces of 4 MB: its name, a zero byte, the piece's
//       bytes; the last piece comes as 'F', which keeps the whole (GEAR_FILE_MOST at most)
//   'F' a file for the next message: its name, a zero byte, its bytes (4 MB at most, or the last piece); kept in gear's data
//       folder under attachments/DATE/, and named in the message, "[attached TYPE: PATH]", as Panetone
//       names a file sent from Telegram
//   'C' {"to": the route, "action": one of a question's answers (from 'Z''s "actions")}: pressed, as
//       its Telegram button is; a refusal comes back as 'F'
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
//   'G' what is seen and muted: {"seen": WHO}, the conversation opened, its replies seen; or
//       {"muted": WHO, "on": true or false}; answered, to every client, with 'U'
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
//   'L' before each sentence's speech: {"of": the message being spoken (FNV-1a of its text), "start",
//       "end": the bytes of it the sentence was read from, "at": seconds into its speech where the
//       sentence starts}, for showing what is heard
//   'D' the reply has been spoken
//   'C' a question form's answers after one was pressed ('C' above): {"to", "action": the one pressed,
//       "actions" [{"id", "label"}] as they now are, the chosen marked, or none once the form is closed}
//   'Z' a message to show and not speak: {"seq", "who", "at", "text", "mine" when the user's, "again" when given
//       again after 'K', "quiet" for Panetone's own notice, "actions" [{"id", "label"}] for a question's
//       answers to press with 'C'}: a
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
//   'U' what is unseen and muted, the same on every client: {"unseen": {WHO: COUNT}, "muted": [WHO]},
//       on connecting and whenever it changes
//   'F' what went wrong, for the client to show
#pragma once

#include <stdbool.h>

// The largest file a client may send for a message ('P' and 'F'), and the pieces a larger than one
// frame is sent in (the app's FILE_MOST and FILE_PIECE are the same).
enum { GEAR_FILE_MOST = 100 * 1024 * 1024, GEAR_FILE_PIECE = 4 * 1024 * 1024 };

// Serves one client on standard input and output; with `hub`, stays running and serves whichever
// client connects to its socket ($XDG_RUNTIME_DIR/gear-hub.sock), the newest taking over, its log of
// messages giving each phone what it missed ('K').
int gear_serve(const char *models, int hear_threads, int say_threads, bool hub);
// `gear serve` with a hub running: standard input and output passed to the hub's socket until either
// end closes. -1 when no hub answers, for serving here instead.
int gear_serve_proxy(void);
