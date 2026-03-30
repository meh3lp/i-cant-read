"""Celery task: speech synthesis via Kokoro TTS."""

import logging

from tasks.celery_app import app
from gradio_client import Client
import config

log = logging.getLogger(__name__)

_client: Client | None = None

def _get_client() -> Client:
    global _client
    if _client is None:
        _client = Client(config.KOKORO_URL)
        log.info("Kokoro TTS gradio client connected")
    return _client


@app.task(name="tasks.run_kokoro_gradio")
def run_kokoro_gradio(prev_result: list) -> list:
    """Generate speech for the cleaned text.

    *prev_result* is ``[text, seq_num]`` from the cleanup task.
    Returns ``[wav_path, seq_num]``.
    """
    text, seq_num = prev_result

    # Preprocess text
    # 1) join into 1 line
    text = text.replace("\n", " ")
    # 2) replace ! with .
    text = text.replace("!", ".")

    client = _get_client()
    result = client.predict(
        voice_name=config.KOKORO_VOICE,
        text=text,
        format=config.KOKORO_FORMAT,
        speed=config.SPEED,
        api_name=config.KOKORO_API_NAME,
    )
    wav_path = result if isinstance(result, str) else result[0]

    log.info("tts: seq=%d → %s", seq_num, wav_path)

    return [wav_path, seq_num]
