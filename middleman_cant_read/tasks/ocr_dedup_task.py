"""Celery task: deduplicate OCR results against recently seen text."""

import logging

import redis as _redis
from celery.exceptions import Ignore

import config
from tasks.celery_app import app
from tasks.utils.ocr_dedup import OCRDedup

log = logging.getLogger(__name__)

_redis_client: _redis.Redis | None = None
_dedup: OCRDedup | None = None


def _get_dedup() -> OCRDedup:
    """Lazy-init a module-level OCRDedup backed by Redis."""
    global _redis_client, _dedup
    if _dedup is None:
        _redis_client = _redis.Redis.from_url(config.REDIS_URL, decode_responses=True)
        _dedup = OCRDedup(redis_client=_redis_client)
    return _dedup


@app.task(name="tasks.dedup_ocr")
def dedup_ocr(prev_result: list) -> list:
    """Remove old / repeated text from an OCR result.

    *prev_result* is ``[text, seq_num]`` from :func:`ocr_frame`.
    Returns ``[deduped_text, seq_num]`` or marks SKIP + raises Ignore.
    """
    text, seq_num = prev_result

    dd = _get_dedup()
    result = dd.dedup(text)

    if result is None:
        log.info("dedup_ocr: rejected seq=%d text=%s", seq_num, text[:60])
        r = _redis.Redis.from_url(config.REDIS_URL, decode_responses=True)
        r.hset(config.PLAYBACK_HASH_KEY, str(seq_num), "SKIP")
        raise Ignore()

    log.info("dedup_ocr seq=%d: %s", seq_num, result[:80])
    return [result, seq_num]
