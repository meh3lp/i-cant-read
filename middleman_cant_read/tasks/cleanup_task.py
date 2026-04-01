"""Celery task: OCR text cleanup via Ollama LLM."""

import json
import logging

import redis as _redis
from celery.exceptions import Ignore
from ollama import Client

import config
from tasks.celery_app import app

log = logging.getLogger(__name__)

_client = Client(host=config.OLLAMA_URL, timeout=30)


def _get_history(r: _redis.Redis, n: int) -> list[dict]:
    """Return the last *n* user/assistant exchanges from Redis."""
    raw = r.lrange(config.OLLAMA_HISTORY_KEY, -n * 2, -1)  # each exchange = 2 entries
    return [json.loads(entry) for entry in raw]


def _push_history(r: _redis.Redis, user_text: str, assistant_text: str, n: int) -> None:
    """Append an exchange and trim the list to the last *n* exchanges."""
    pipe = r.pipeline()
    pipe.rpush(
        config.OLLAMA_HISTORY_KEY,
        json.dumps({"role": "user", "content": user_text}),
        json.dumps({"role": "assistant", "content": assistant_text}),
    )
    pipe.ltrim(config.OLLAMA_HISTORY_KEY, -n * 2, -1)
    pipe.execute()


@app.task(name="tasks.clean_text")
def clean_text(prev_result: list) -> list:
    """Clean OCR text through Ollama.

    *prev_result* is ``[replica_dict, seq_num]`` where replica_dict is
    ``{"speaker": str, "text": str}``.
    Returns ``[replica_dict, seq_num]`` or marks SKIP + raises Ignore.
    """
    replica, seq_num = prev_result
    text = replica["text"]
    speaker = replica.get("speaker", "Narrator")

    log.info("cleaning: %s", text[:80])

    r = _redis.Redis.from_url(config.REDIS_URL, decode_responses=True)
    history_size = config.OLLAMA_CLEANUP_HISTORY_SIZE

    cleaned = text
    try:
        if history_size > 0:
            history = _get_history(r, history_size)
        else:
            history = []
        messages = [
            {"role": "system", "content": config.OLLAMA_CLEANUP_SYSTEM_PROMPT},
            *history,
            {"role": "user", "content": text},
        ]
        resp = _client.chat(
            model=config.OLLAMA_CLEANUP_MODEL,
            messages=messages,
            options={"num_predict": 2048},
            think=False,
            keep_alive=config.OLLAMA_KEEP_ALIVE,
        )
        result = resp.message.content.strip()
        if not result:
            log.warning("ollama returned empty — using original")
        else:
            log.info("cleaned:  %s", result[:80])
            cleaned = result

        _push_history(r, text, cleaned, history_size)
    except Exception:
        log.exception("ollama cleanup failed — using original text")

    if cleaned.strip().lower() == "failed recognition":
        log.info("cleanup: failed recognition for seq=%d", seq_num)
        r.hset(config.PLAYBACK_HASH_KEY, str(seq_num), "SKIP")
        raise Ignore()

    return [{"speaker": speaker, "text": cleaned}, seq_num]
