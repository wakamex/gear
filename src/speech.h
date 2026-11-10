// Telling speech from the other sounds a voice makes. The voice detector starts a piece for a throat
// clear, a cough, a sigh or a laugh as it does for a word, and the recognizer writes words into some of
// them ("Okay", "um", "oh", "ha ha"), which would go to the agent as a message. A piece the recognizer
// wrote words into is scored from what the two voice detectors and the recognizer made of it, and one
// that scores under the cut is dropped.
//
// The score is a model (src/speech_model.h, written by tools/fit-speech-model) fitted on pieces cut by
// gear's own listener from public recordings: single words (Speech Commands) and read sentences
// (LibriSpeech) as speech, and coughs, throat clearing, sighs, sneezes, sniffs and laughter (VocalSound)
// as the rest. No input grows with a piece's length, so a long sentence is not taken for a long sigh.
#pragma once
#include "hear.h"

enum { GEAR_SPEECH_INPUTS = 17 };

// The two voice detectors over one piece: Silero, whose score starts and ends pieces, and gs_voice's
// (WebRTC's). Zeroed when a piece starts, then given every frame of it, the frames that started it first.
typedef struct { int s50, s80, s90, s95, s99, r50, r90, r95; float speak, rpeak; double ssum, rsum; } gear_speech_heard;
void gear_speech_frame(gear_speech_heard *h, float silero, float second);
// The model's inputs for a piece, in the order tools/fit-speech-model names them: of the frames Silero
// scored 0.5 or more, the share at 0.95, 0.99, 0.9 and 0.8 or more, then its highest score and its mean
// over those frames; the recognizer's six scores; and for the second detector the frames at 0.5, 0.9 and
// 0.95 or more as shares of Silero's, its highest score and its mean over Silero's frames.
void gear_speech_inputs(const gear_speech_heard *h, const gear_hear_scores *scores, float in[GEAR_SPEECH_INPUTS]);
float gear_speech_score(const float in[GEAR_SPEECH_INPUTS]);  // how likely the piece is speech, 0 to 1
float gear_speech_cut(void);                                  // the score under which a piece is dropped
// The largest difference between this code's score and the fitting tool's, over the rows it kept for the check.
float gear_speech_checked(void);
