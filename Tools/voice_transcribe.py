"""Word-level transcripts of the archive's speech folders, for the voice pack (27.09.2026).

    python Tools/voice_transcribe.py [archive folder] [--device cuda|cpu]

Writes `Tools/voice_transcripts.json`: per file its segments and words (text, start, end, probability), as
faster-whisper gives them. `Tools/voice_select.py` cuts the phrases out of it and `Tools/voice_pack.py` packs
them. Run where faster-whisper is installed (here: the WSL environment `transcribe`, on the GPU); the paths
in the output are relative to the archive, so it does not matter from which side the archive was reached.

The user's word (27.09.2026): "Zumindest NASA und Radio sollten wir komplett nehmen" -- every file of
NASA/Historical and Radio/Quiet-Please is transcribed; what is not speech comes back without words and
drops out in the selection.
"""
import json
import os
import sys

FOLDERS = ["NASA/Historical", "Radio/Quiet-Please"]
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "voice_transcripts.json")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    archive = args[0] if args else "/mnt/g/Tools/VRAudio/AmbientSynth/Library/Archive"
    device = "cuda" if "--device" not in sys.argv else sys.argv[sys.argv.index("--device") + 1]
    from faster_whisper import WhisperModel
    model = WhisperModel("large-v3", device=device, compute_type="float16" if device == "cuda" else "int8")
    out = {}
    for folder in FOLDERS:
        for root, _, files in os.walk(os.path.join(archive, folder)):
            for f in sorted(files):
                if not f.lower().endswith(".flac"):
                    continue
                path = os.path.join(root, f)
                rel = os.path.relpath(path, archive).replace("/", "\\")
                segs, _ = model.transcribe(path, language="en", word_timestamps=True, vad_filter=False,
                                           condition_on_previous_text=False)
                items = []
                for s in segs:
                    items.append({"start": s.start, "end": s.end, "text": s.text.strip(),
                                  "no_speech": s.no_speech_prob,
                                  "words": [{"w": w.word.strip(), "start": w.start, "end": w.end, "p": w.probability}
                                            for w in (s.words or [])]})
                out[rel] = items
                print("%-90s %d segments" % (rel[:90], len(items)), flush=True)
    json.dump(out, open(OUT, "w", encoding="utf-8"), indent=1)
    print("wrote", OUT, len(out), "files")


if __name__ == "__main__":
    main()
