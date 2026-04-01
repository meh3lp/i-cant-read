"""Celery task: pre-LLM text filtering (noise, fuzzy dedup, overlap)."""

import logging

import redis as _redis
from celery.exceptions import Ignore

import config
from tasks.celery_app import app
from tasks.utils import TextFilter

log = logging.getLogger(__name__)

# Celery runs in a different thread from ICantRead class,
# so tasks have their own redis clients
_redis_client: _redis.Redis | None = None
_filter: TextFilter | None = None


def _get_filter() -> TextFilter:
    """Lazy-init a module-level TextFilter backed by Redis."""
    global _redis_client, _filter
    if _filter is None:
        _redis_client = _redis.Redis.from_url(config.REDIS_URL, decode_responses=True)
        _filter = TextFilter(redis_client=_redis_client)
    return _filter


@app.task(name="tasks.filter_text")
def filter_text(prev_result: list) -> list:
    """Filter *text* through the noise / dedup / overlap pipeline.

    *prev_result* is ``[replica_dict, seq_num]`` where replica_dict is
    ``{"speaker": str, "text": str}``.
    Returns ``[replica_dict, seq_num]`` on success.
    Marks the playback hash as ``SKIP`` and raises :class:`Ignore` if rejected.
    """
    replica, seq_num = prev_result
    text = replica["text"]
    speaker = replica.get("speaker", "Narrator")

    tf = _get_filter()
    filtered = tf.filter(text)

    if filtered is None:
        log.info("filter: rejected seq=%d text=%s", seq_num, text[:60])
        r = _redis.Redis.from_url(config.REDIS_URL, decode_responses=True)
        r.hset(config.PLAYBACK_HASH_KEY, str(seq_num), "SKIP")
        raise Ignore()

    return [{"speaker": speaker, "text": filtered}, seq_num]
