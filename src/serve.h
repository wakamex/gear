// gear for a client elsewhere, such as the Android app over SSH: framed messages on standard input and
// output, recognition, the agent and speech here. A frame is one byte of type, four of length (little
// endian) and that many bytes.
//
// From the client:
//   'A' audio being said: 16-bit little-endian samples, 16 kHz mono
//   'E' the end of what was said: it is transcribed and sent to the agent
//   'X' stop: what is being spoken stops, and nothing more of it comes
//   'L' the agents there are, answered with 'G'
//   'T' the agent to send to from now on, by name or tab title (UTF-8)
//   'V' the voice to speak in, "alba" or "mary"
// To the client:
//   'H' ready: {"target": name or "", "voice": "alba"}
//   'M' the message sent, as transcribed
//   'R' the agent's reply, as written
//   'P' speech of the reply: 16-bit little-endian samples, 24 kHz mono
//   'D' the reply has been spoken
//   'G' the agents: [{"name", "tab", "harness", "status"}]
//   'S' what is happening, for the client to show ("hearing", "waiting for NAME")
//   'F' what went wrong, for the client to show
#pragma once

int gear_serve(const char *models, int hear_threads, int say_threads);
