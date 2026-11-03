"""Produce a REAL failure log from VaniScribe code (not a hand-written one) for testing AppAssist.

Runs the actual transcriber on a missing audio file and the actual translator on a missing model
folder, with ordinary logging around it, the way an application log would look.

usage: <VaniScribe venv python> make_vaniscribe_failure_log.py <VaniScribe project dir> <out.log>
"""
import logging
import sys

project, out = sys.argv[1:3]
sys.path.insert(0, project)

logging.basicConfig(filename=out, filemode="w", level=logging.INFO,
                    format="%(asctime)s %(levelname)-7s %(name)s: %(message)s")
log = logging.getLogger("vaniscribe")
log.info("VaniScribe starting, %d file(s) queued", 1)

from ytmp3.transcriber import TranscribeJob, TranscribeListener, TranscribeRequest  # noqa: E402


class LogListener(TranscribeListener):
    def log(self, message, level="info"):
        getattr(log, "error" if level == "error" else "info")(message)

    def finished(self, done, failed, cancelled, error):
        log.info("transcription finished: done=%s failed=%s cancelled=%s error=%s", done, failed, cancelled, error)


# 1. Transcribing an audio file that is not there (a real error path in the transcriber).
job = TranscribeJob(TranscribeRequest([r"C:\Recordings\Episode-07.mp3"], None, "te"), LogListener())
job.run()

# 2. Loading the translator from a folder that has no model (a real traceback from nllb.py).
from ytmp3.nllb import NllbTranslator  # noqa: E402

for attempt in range(3):  # retried, as an app might: the repeats should be grouped
    try:
        NllbTranslator(r"C:\VaniScribe\models\nllb-200-600m-int8-missing")
    except Exception:
        log.exception("English translation unavailable (attempt %d)", attempt + 1)
log.info("VaniScribe stopped")
