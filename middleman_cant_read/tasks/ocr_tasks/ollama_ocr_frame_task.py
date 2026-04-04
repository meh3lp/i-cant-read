"""Celery task: OCR a screenshot via Ollama vision model (characters mode)."""

import json
import logging

import redis as _redis
from celery.exceptions import Ignore
from celery import group
from ollama import Client

import config
from tasks.celery_app import app

log = logging.getLogger(__name__)

_client = Client(host=config.OLLAMA_URL, timeout=60)

_REPLICAS_SCHEMA = {
    "type": "object",
    "properties": {
        "text_type": {
            "type": "string"
        },
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
    "required": ["replicas", "text_type"],
}


@app.task(name="tasks.run_ollama_ocr_frame")
def run_ollama_ocr_frame(prev_result: list) -> list:
    """OCR a base64-encoded PNG frame via Ollama vision.

    *prev_result* is ``[b64_image, seq_num]`` from initialize_frame_chain.
    Returns ``[replicas_list, seq_num]`` or marks SKIP + raises Ignore.
    Each replica is ``{"speaker": str, "text": str}``.
    """
    b64_image, seq_num = prev_result

    log.info("ocr_frame: processing seq=%d", seq_num)

    r = _redis.Redis.from_url(config.REDIS_URL, decode_responses=True)


    all_replicas = []

    # for _ in range(config.OCR_PASSES):
    #     try:
    #         data = _do_ocr(b64_image, seq_num)
    #     except Exception as e:
    #         log.error("ocr_frame: error in OCR pass for seq=%d: %s", seq_num, str(e))
    #         raise self.retry(exc=e)
    #     replicas = data.get("replicas", [])
    #     if replicas:
    #         all_replicas.extend(replicas)
    #     else:
    #         log.warning("ocr_frame: no replicas from Ollama for seq=%d", seq_num)
    #     if data.get("text_type", "").lower() == "book":
    #         break

    tasks = [run_ollama_ocr_single_pass.s([b64_image, seq_num]) for _ in range(config.OCR_PASSES)]
    results = group(tasks).apply().get(disable_sync_subtasks=False)
    for data in results:
        replicas = data.get("replicas", [])
        if replicas:
            all_replicas.extend(replicas)
        else:
            log.warning("ocr_frame: no replicas from Ollama for seq=%d in one pass", seq_num)

    if not all_replicas:
        log.info("ocr_frame: no replicas for seq=%d — SKIP", seq_num)
        r.hset(config.PLAYBACK_HASH_KEY, str(seq_num), "SKIP")
        raise Ignore()

    log.info("ocr_frame seq=%d: %d replicas", seq_num, len(all_replicas))
    return [all_replicas, seq_num]


@app.task(bind=True, name="tasks.run_ollama_ocr_single_pass", max_retries=3, default_retry_delay=5)
def run_ollama_ocr_single_pass(self, prev_result: list):
    """
    Celery task wrapper for _do_ocr to allow retries
    """
    b64_image, seq_num = prev_result
    log.info("ocr_frame (single pass): processing seq=%d", seq_num)
    try:
        data = _do_ocr(b64_image, seq_num)
    except Exception as e:
        log.error("ocr_frame (single pass): error in OCR for seq=%d: %s", seq_num, str(e))
        raise self.retry(exc=e)
    return data


def _do_ocr(b64_image, seq_num):
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
    log.info("Ollama raw response for seq=%d: %s", seq_num, raw.replace("\n", ""))
    if "''''" in raw:
        # Strip '''json ... ''' if present
        # raw = raw.split("'''", 1)[-1].rsplit("'''", 1)[0]
        raw = raw.strip("'''").strip("json").strip()
    if "```" in raw:
        # Strip ```json ... ``` if present
        raw = raw.strip("```").strip("json").strip()
    data = json.loads(raw)
    # AI returned wrong JSON root object
    if isinstance(data, list):
        # ```json[{"replicas": [{"speaker": "Narrator", "text": "You see, it's the time of the year again for the Titan popularity vote. The competition this time round is quite intense — Ol' Thannie's new album has got all the disciples in a zealous frenzy, and Mnestia's fabulous outfit at the fashion exhibition has garnered plenty of supporters."}]}]```
        if len(data) > 0:
            item = data[0]
            if isinstance(item, dict) and "replicas" in item:
                data = item
            elif isinstance(item, dict) and "speaker" in item and "text" in item:
                data = {"replicas": data}
            else:
                raise ValueError("Unexpected JSON structure: list without 'replicas' or replica dicts")

    replicas = data.get("replicas", [])

    # Filter out replicas with empty text
    replicas = [rep for rep in replicas if rep.get("text", "").strip()]
    data["replicas"] = replicas
    return data
