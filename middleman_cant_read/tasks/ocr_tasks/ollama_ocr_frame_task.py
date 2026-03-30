"""Celery task: OCR a screenshot via Ollama vision model."""

import json
import logging

import redis as _redis
from celery.exceptions import Ignore
from ollama import Client

import config
from tasks.celery_app import app

log = logging.getLogger(__name__)

_client = Client(host=config.OLLAMA_URL, timeout=60)


def _get_history(r: _redis.Redis, n: int) -> list[dict]:
    """Return the last *n* user/assistant OCR exchanges from Redis."""
    raw = r.lrange(config.OLLAMA_OCR_HISTORY_KEY, -n * 2, -1)
    return [json.loads(entry) for entry in raw]


def _push_history(r: _redis.Redis, assistant_text: str, n: int) -> None:
    """Append an OCR exchange and trim to the last *n* exchanges.

    User messages carry the image so we store a short placeholder instead;
    only the assistant reply (recognized text) matters for context.
    """
    pipe = r.pipeline()
    pipe.rpush(
        config.OLLAMA_OCR_HISTORY_KEY,
        json.dumps({"role": "user", "content": "OCR this image."}),
        json.dumps({"role": "assistant", "content": assistant_text}),
    )
    pipe.ltrim(config.OLLAMA_OCR_HISTORY_KEY, -n * 2, -1)
    pipe.execute()


@app.task(name="tasks.run_ollama_ocr_frame")
def run_ollama_ocr_frame(prev_result: list) -> list:
    """OCR a base64-encoded PNG frame via Ollama vision.

    *prev_result* is ``[b64_image, seq_num]`` from initialize_frame_chain.
    Returns ``[text, seq_num]`` or marks SKIP + raises Ignore.
    """
    b64_image, seq_num = prev_result

    log.info("ocr_frame: processing seq=%d", seq_num)

    r = _redis.Redis.from_url(config.REDIS_URL, decode_responses=True)
    history_size = config.OLLAMA_OCR_HISTORY_SIZE
    log.info("fetching last %d OCR history exchanges for context", history_size)

    try:
        if history_size > 0:
            history = _get_history(r, history_size)
        else:
            history = []
        log.info("fetched %d history exchanges", len(history) // 2)
        messages = [
            {
                "role": "system",
                "content": config.OLLAMA_OCR_SYSTEM_PROMPT,
            },
            *history,
            {
                "role": "user",
                "content": "OCR this image.",
                "images": [b64_image],
            },
        ]
        resp = _client.chat(
            model=config.OLLAMA_OCR_MODEL,
            messages=messages,
            options={"num_predict": 4096},
            think=False,
            keep_alive=config.OLLAMA_KEEP_ALIVE
        )
        text = resp.message.content.strip()
    except Exception:
        log.exception("Ollama vision OCR failed for seq=%d", seq_num)
        text = ""

    if not text:
        log.info("ocr_frame: empty result for seq=%d — SKIP", seq_num)
        r.hset(config.PLAYBACK_HASH_KEY, str(seq_num), "SKIP")
        raise Ignore()

    _push_history(r, text, history_size)
    log.info("ocr_frame seq=%d: %s", seq_num, text[:80])
    return [text, seq_num]
