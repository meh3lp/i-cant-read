import logging
import os
import requests

from tasks.celery_app import app
import config

log = logging.getLogger(__name__)

@app.task(name="tasks.run_kokoro_fastapi")
def run_kokoro_fastapi(prev_result: list) -> list:
    text, seq_num = prev_result

    response = requests.post(
        f"{config.KOKORO_URL}/v1/audio/speech",
        json={
            "input": text,
            "voice": config.KOKORO_VOICE,  # Equal weights
            "response_format": "wav",
            "speed": config.KOKORO_SPEED
        }
    )

    # make sure /dev/shm/cantread/tts exists
    os.makedirs(config.TTS_FILES_DIR, exist_ok=True)
    wav_path = f"{config.TTS_FILES_DIR}/kokoro_{seq_num}.wav"

    # with open(config.TTS_FILES_DIR + "/output.wav", "wb") as f:
    with open(wav_path, "wb") as f:
        f.write(response.content)

    return [wav_path, seq_num]
