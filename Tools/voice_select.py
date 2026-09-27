"""Cuts every spoken phrase of NASA's historical recordings and of the radio series into the voice selection
(27.09.2026; the user: "Zumindest NASA und Radio sollten wir komplett nehmen").

    python Tools/voice_select.py

Reads `Tools/voice_transcripts.json` (Tools/voice_transcribe.py, faster-whisper large-v3, word timestamps) and
rewrites `Tools/voice_selection.json`, which `Tools/voice_pack.py` packs:
  * the 31 phrases chosen by hand in round "fx-psychedelia" stay first, as they were;
  * every Quiet, Please clip (already one phrase each, 2 to 6 s) becomes a phrase: from its first word to its
    last, category 1 (spoken);
  * NASA's speech (Apollo-Mercury, Discovery, Shuttle) is split at every pause of 0.45 s or more, and wherever a
    phrase would pass 6 s, category 0 (space).
Left out: stretches without words, words transcribed with a mean probability under 0.45 (the signals of Beeps and
Missions come back from the transcriber as words it made up -- they are with the Field track's NASA recordings instead, Tools/field_select.py),
phrases under 0.5 s, and any phrase whose words another phrase already says (the Apollo 11 landing is in the
archive twice).
"""
import json
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
TRANSCRIPTS = os.path.join(HERE, "voice_transcripts.json")
SELECTION = os.path.join(HERE, "voice_selection.json")
NASA_SPEECH = ("\\Apollo-Mercury\\", "\\Discovery\\", "\\Shuttle\\")
GAP, LONGEST, SHORTEST, MIN_PROB = 0.45, 6.0, 0.5, 0.45


def norm(text):
    return re.sub(r"[^a-z0-9 ]", "", text.lower()).split()


def phrase(file, words, category):
    return {"category": category, "file": file,
            "text": " ".join(w["w"] for w in words).strip(),
            "start": round(words[0]["start"], 3), "end": round(words[-1]["end"], 3),
            "last_word": round(words[-1]["start"], 3), "first_word_end": round(words[0]["end"], 3)}


def usable(words):
    if not words:
        return False
    if words[-1]["end"] - words[0]["start"] < SHORTEST:
        return False
    return sum(w["p"] for w in words) / len(words) >= MIN_PROB


def main():
    tr = json.load(open(TRANSCRIPTS, encoding="utf-8"))
    rows = [r for r in json.load(open(SELECTION, encoding="utf-8")) if r.get("hand", True)]
    rows = rows[:31]
    for r in rows:
        r["hand"] = True
    said = [" ".join(norm(r["text"])) for r in rows]

    def new(p):
        n = " ".join(norm(p["text"]))
        if not n or any(n in s or s in n for s in said):
            return False
        said.append(n)
        p["hand"] = False
        rows.append(p)
        return True

    added = {0: 0, 1: 0}
    for file, segs in tr.items():
        words = [w for s in segs for w in s["words"] if w["w"]]
        if file.startswith("Radio\\"):
            if usable(words) and new(phrase(file, words, 1)):
                added[1] += 1
            continue
        if not any(k in file for k in NASA_SPEECH):
            continue
        cur = []
        for w in words:
            if cur and (w["start"] - cur[-1]["end"] >= GAP or w["end"] - cur[0]["start"] > LONGEST):
                if usable(cur) and new(phrase(file, cur, 0)):
                    added[0] += 1
                cur = []
            cur.append(w)
        if usable(cur) and new(phrase(file, cur, 0)):
            added[0] += 1
    json.dump(rows, open(SELECTION, "w", encoding="utf-8"), indent=1, ensure_ascii=False)
    print("kept %d by hand, added %d from NASA and %d from the radio series: %d phrases"
          % (sum(1 for r in rows if r["hand"]), added[0], added[1], len(rows)))


if __name__ == "__main__":
    main()
