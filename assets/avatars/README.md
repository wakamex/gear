# Characters

The phone app's four characters, each four frames of the same size and place: at rest, blinking, speaking and listening. They were generated on 2026-11-15 with Codex's image generation (model gpt-6.1-sol), run through AOP with the sealed profile, as one 2 by 2 character sheet each on a transparent background:

| Character | Run | How |
| --- | --- | --- |
| fox | 7236f20c-1858-42f0-a1fd-062e90d8f93f | from a prompt: a chibi fox with amber fur and headphones |
| girl | 81ec1002-1016-4437-9e34-9ca8ae65efc9 | from a prompt: a chibi girl with a dark bob, an amber hoodie and a headset |
| robot | 349c342c-90ea-4cd4-9fde-e4ded7a13f43 | from a prompt: a chibi robot with a dark visor face and amber eyes |
| tomato | bc2b2ec6-5dbf-48a6-8173-c18ea8fa1cf2 | an edit of a chibi sprite's sheet (run 090abec5-0edb-4c48-a6ca-7a9ac8840841) into a tomato |

Every prompt asked for the same character at the same size, position and framing in all four cells, so the cells serve as animation frames. Each sheet was cut into its cells, pixels under 40 of 255 opacity were cleared, all four cells were cropped to one box that holds all of them, padded square, scaled to 360 px, and reduced to 256 colours.

## Thinking frames

Each character also has four thinking frames, `NAME-think1.png` to `NAME-think4.png`: looking up to the left and to the right with a hand at the chin, eyes closed, and about to speak, played in turn while gear hears, sends or waits for the agent. They were generated on 2026-11-23 with the same model and profile, one run per character, each given the character's four existing frames as a 2 by 2 sheet (`--input`) and asked for a sheet of the same character in the same style, size, position and framing, with only the eyes, mouth and raised hand changed:

| Character | Run |
| --- | --- |
| fox | 77f99e6c-2398-44de-9fc1-2938d2607bd3 |
| girl | cbdced11-181c-4019-b37d-a8210725d686 |
| robot | eef634ff-cd78-4c5d-a324-4e3edda497ca |
| tomato | 99252fa7-efa1-45a1-ac71-75f7dad6409a |

Each sheet was cut into its cells and cleared under 40 of 255 opacity as before; since the new sheets are not at the old ones' scale, each cell was then scaled and placed where its opaque shape overlaps the character's resting frame most (95% for the robot, 97 to 98% for the others), at 360 px and 256 colours.
