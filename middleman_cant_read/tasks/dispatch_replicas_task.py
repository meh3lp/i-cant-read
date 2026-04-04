"""Celery task: fan-out OCR replicas into individual text→TTS chains.

Uses string-based Celery signatures to avoid circular imports with
other task modules.
"""

import logging

import redis as _redis
from celery import chain

import config
from tasks.celery_app import app
from tasks.utils.history import PipelineHistory

log = logging.getLogger(__name__)

# Task-name → config-key mapping for TTS providers
_TTS_TASK_NAMES: dict[str, str] = {
    "kokoro_fastapi": "tasks.run_kokoro_fastapi",
    "applio": "tasks.run_applio_tts",
}

# Task-name → config-key mapping for RVC providers
_RVC_TASK_NAMES: dict[str, str] = {
    "rvc_gradio": "tasks.run_rvc_gradio",
    "applio": "tasks.run_applio_rvc",
}


def _next_seq(r: _redis.Redis) -> int:
    """Atomically claim the next sequence number from Redis."""
    return int(r.incr(config.SEQ_COUNTER_KEY)) - 1


@app.task(name="tasks.dispatch_replicas")
def dispatch_replicas(prev_result: list) -> None:
    """Dispatch a separate text→TTS chain for each replica.

    *prev_result* is ``[replicas_list, seq_num]`` where each replica is
    ``{"speaker": str, "text": str}``.  The *seq_num* from the OCR
    stage is unused — each replica gets its own seq via Redis INCR.
    """
    replicas, _ocr_seq = prev_result

    if not replicas:
        log.info("dispatch_replicas: no replicas to dispatch")
        return

    r = _redis.Redis.from_url(config.REDIS_URL, decode_responses=True)
    history = PipelineHistory(r)
    batch = PipelineHistory.next_batch(r)

    tts_task_name = _TTS_TASK_NAMES.get(config.TTS_PROVIDER)
    if tts_task_name is None:
        log.error("unsupported TTS provider: %s", config.TTS_PROVIDER)
        return

    rvc_task_name = None
    if config.RVC_PROVIDER and config.TTS_PROVIDER != "applio":
        # Applio TTS already includes RVC; skip standalone RVC
        rvc_task_name = _RVC_TASK_NAMES.get(config.RVC_PROVIDER)
        if rvc_task_name is None:
            log.error("unsupported RVC provider: %s", config.RVC_PROVIDER)
            return

    for replica in replicas:
        seq = _next_seq(r)
        speaker = replica.get("speaker", "Narrator")
        history.write_entry(seq, replica.get("text", ""), speaker, batch)
        replica_arg = (replica, seq)

        tasks = [app.signature("tasks.initialize_chain", args=(replica_arg,))]

        if config.OCR_DEDUP_ENABLED:
            tasks.append(app.signature("tasks.dedup_ocr"))

        if config.TEXT_FILTER_ENABLED:
            tasks.append(app.signature("tasks.filter_text"))

        if config.OLLAMA_TEXT_CLEANUP_ENABLED:
            tasks.append(app.signature("tasks.clean_text"))

        tasks.append(app.signature(tts_task_name))

        if rvc_task_name:
            tasks.append(app.signature(rvc_task_name))

        tasks.append(app.signature("tasks.enqueue_playback"))

        pipeline = chain(*tasks)
        pipeline.delay()
        log.info(
            "dispatch_replicas: dispatched seq=%d speaker=%s text=%s",
            seq, replica.get("speaker", "?"), replica.get("text", "")[:60],
        )
