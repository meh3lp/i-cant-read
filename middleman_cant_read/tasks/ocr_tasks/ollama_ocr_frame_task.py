"""Celery task: OCR a screenshot via Ollama vision model (characters mode)."""

import json
import logging

import redis as _redis
from celery.exceptions import Ignore
from ollama import Client

import config
from tasks.celery_app import app

log = logging.getLogger(__name__)

_client = Client(host=config.OLLAMA_URL, timeout=60)

_REPLICAS_SCHEMA = {
    "type": "object",
    "properties": {
        "replicas": {
            "type": "array",
            "items": {
                "type": "object",
                "properties": {
                    "speaker": {"type": "string"},
                    "text": {"type": "string"},
                },
                "required": ["speaker", "text"],
            },
        },
    },
    "required": ["replicas"],
}


@app.task(bind=True, name="tasks.run_ollama_ocr_frame", max_retries=3, default_retry_delay=5)
def run_ollama_ocr_frame(self, prev_result: list) -> list:
    """OCR a base64-encoded PNG frame via Ollama vision.

    *prev_result* is ``[b64_image, seq_num]`` from initialize_frame_chain.
    Returns ``[replicas_list, seq_num]`` or marks SKIP + raises Ignore.
    Each replica is ``{"speaker": str, "text": str}``.
    """
    b64_image, seq_num = prev_result

    log.info("ocr_frame: processing seq=%d", seq_num)

    r = _redis.Redis.from_url(config.REDIS_URL, decode_responses=True)

    try:
        messages = [
            {
                "role": "system",
                "content": config.OLLAMA_OCR_SYSTEM_PROMPT,
            },
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
            format=_REPLICAS_SCHEMA,
            keep_alive=config.OLLAMA_KEEP_ALIVE,
        )
        raw = resp.message.content.strip()
        log.debug("Ollama raw response for seq=%d: %s", seq_num, raw)
        if "''''" in raw:
            # Strip '''json ... ''' if present
            # raw = raw.split("'''", 1)[-1].rsplit("'''", 1)[0]
            raw = raw.strip("'''").strip("json").strip()
        if "```" in raw:
            # Strip ```json ... ``` if present
            raw = raw.strip("```").strip("json").strip()
        data = json.loads(raw)
        replicas = data.get("replicas", [])

    except Exception:
        log.exception("Ollama vision OCR failed for seq=%d", seq_num)
        try:
            raise self.retry()  # retry with the same image, in case of transient Ollama issues
        except self.MaxRetriesExceededError:
            log.error("Ollama OCR max retries exceeded for seq=%d", seq_num)
            replicas = []

    # Filter out replicas with empty text
    replicas = [rep for rep in replicas if rep.get("text", "").strip()]

    if not replicas:
        log.info("ocr_frame: no replicas for seq=%d — SKIP", seq_num)
        r.hset(config.PLAYBACK_HASH_KEY, str(seq_num), "SKIP")
        raise Ignore()

    log.info("ocr_frame seq=%d: %d replicas", seq_num, len(replicas))
    return [replicas, seq_num]
