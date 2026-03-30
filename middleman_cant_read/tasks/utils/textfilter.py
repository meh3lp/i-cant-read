"""Pre-LLM text filter: noise rejection, fuzzy dedup, and overlap extraction.

Sits between the owocr listener and the pipeline to reduce redundant or
garbled OCR text before it reaches the (expensive) Ollama cleanup step.

State (the sliding window of recent texts) is stored in Redis so that
multiple Celery workers can share the same dedup context.
"""

import json
import logging
import re
import time
from difflib import SequenceMatcher

import config

log = logging.getLogger(__name__)

# Sentence boundary regex — splits after . ! ? followed by whitespace
_SENT_RE = re.compile(r"(?<=[.!?])\s+")

# A token is "readable" if it's ≥3 ASCII letters, contains a vowel,
# and has sane casing (all-lower, all-upper, or Title Case — not "OmVETSE").
_VOWELS = set("aeiouAEIOU")
_SANE_CASE_RE = re.compile(
    r"^(?:[A-Z][a-z]+|[a-z]+|[A-Z]+)$"  # Title | lower | UPPER
)
# Common English words shorter than 3 characters — always count as readable.
_SHORT_WORDS = {
    "a", "i", "am", "an", "as", "at", "be", "by", "do", "go", "he", "if",
    "in", "is", "it", "me", "my", "no", "of", "on", "or", "ox", "so", "to",
    "up", "us", "we",
}


def _is_readable_token(tok: str) -> bool:
    """Return True if *tok* looks like a plausible English word."""
    # Strip trailing punctuation for the check
    core = tok.rstrip(".,;:!?'\"-")
    if core.lower() in _SHORT_WORDS:
        return True
    if len(core) < 3:
        return False
    if not core.isalpha():
        return False
    if not any(c in _VOWELS for c in core):
        return False
    if not _SANE_CASE_RE.match(core):
        return False
    return True


class TextFilter:
    """Stateful filter that deduplicates and cleans incoming OCR text.

    Call :meth:`filter` for every incoming OCR string.  It returns the
    (possibly trimmed) text to keep, or *None* to skip the entry entirely.

    When *redis_client* is provided the sliding window is stored in a
    Redis sorted set so that state is shared across Celery workers.
    Otherwise an in-memory list is used (handy for tests).
    """

    def __init__(self, redis_client=None) -> None:
        self._max_win = getattr(config, "TEXT_FILTER_WINDOW_SIZE", 10)
        self._redis = redis_client

        # In-memory fallback when Redis is not available (tests, etc.)
        self._mem_window: list[tuple[float, str]] = []

    # ── window helpers ───────────────────────────────────────────────────

    def _zset_key(self) -> str:
        return getattr(config, "TEXTFILTER_ZSET_KEY", "cantread:textfilter:window")

    def _lock_key(self) -> str:
        return getattr(config, "TEXTFILTER_LOCK_KEY", "cantread:textfilter:lock")

    def _get_window(self) -> list[tuple[float, str]]:
        """Return list of ``(timestamp, text)`` from the sliding window."""
        if self._redis is None:
            return list(self._mem_window)

        # Redis sorted set: member = json-encoded text, score = timestamp
        raw = self._redis.zrange(self._zset_key(), 0, -1, withscores=True)
        return [(score, json.loads(member)) for member, score in raw]

    def _add_to_window(self, text: str) -> None:
        """Append *text* to the sliding window."""
        ts = time.time()

        if self._redis is None:
            self._mem_window.append((ts, text))
            if len(self._mem_window) > self._max_win:
                self._mem_window = self._mem_window[-self._max_win:]
            return

        pipe = self._redis.pipeline()
        pipe.zadd(self._zset_key(), {json.dumps(text): ts})
        # Trim to keep only the most recent entries
        pipe.zremrangebyrank(self._zset_key(), 0, -(self._max_win + 1))
        pipe.execute()

    # ── public API ───────────────────────────────────────────────────────

    def filter(self, text: str) -> str | None:
        """Return cleaned text or *None* if the text should be dropped."""
        if not getattr(config, "TEXT_FILTER_ENABLED", True):
            return text

        # When using Redis, wrap the whole filter in a lock so concurrent
        # workers don't make inconsistent dedup decisions.
        if self._redis is not None:
            import redis as _redis_mod
            lock = _redis_mod.lock.Lock(self._redis, self._lock_key(), timeout=5)
            with lock:
                return self._do_filter(text)
        else:
            return self._do_filter(text)

    def _do_filter(self, text: str) -> str | None:
        """Core filter logic (called inside the lock when Redis is used)."""
        # Stage A — noise / garbled rejection
        if self._is_noise(text):
            log.debug("filter: rejected as noise: %s", text[:60])
            return None

        # Stage B — fuzzy dedup against recent window
        if self._is_fuzzy_duplicate(text):
            log.debug("filter: rejected as fuzzy dup: %s", text[:60])
            return None

        # Stage C — overlap extraction (keep only new sentences)
        text = self._extract_new_content(text)
        if not text:
            log.debug("filter: nothing new after overlap removal")
            return None

        # Accept — add to window
        self._add_to_window(text)
        return text

    # ── Stage A: noise rejection ─────────────────────────────────────────

    def _is_noise(self, text: str) -> bool:
        min_len = getattr(config, "TEXT_FILTER_MIN_LENGTH", 15)
        blocklist: list[str] = getattr(config, "TEXT_FILTER_UI_BLOCKLIST", [])

        # Exact blocklist match (case-insensitive)
        stripped = text.strip()
        for entry in blocklist:
            if stripped.lower() == entry.lower():
                return True

        # Short text with low "readable" token ratio → likely garbled
        if len(stripped) < min_len:
            return True

        tokens = stripped.split()
        if not tokens:
            return True

        readable = sum(1 for t in tokens if _is_readable_token(t))
        ratio = readable / len(tokens)

        # For very short texts, require high readability
        if len(stripped) < 40 and ratio <= 0.5:
            return True

        # For any length, reject if almost nothing is readable
        if ratio < 0.3:
            return True

        return False

    # ── Stage B: fuzzy dedup ─────────────────────────────────────────────

    def _is_fuzzy_duplicate(self, text: str) -> bool:
        threshold = getattr(config, "TEXT_FILTER_SIMILARITY_THRESHOLD", 0.85)
        for _ts, prev in self._get_window():
            sim = SequenceMatcher(None, prev, text).ratio()
            if sim >= threshold:
                # Allow through if the new text is substantially longer
                # (it may contain genuinely new content appended)
                if len(text) > len(prev) * 1.3:
                    return False
                log.debug(
                    "filter: sim=%.2f with prev (len %d vs %d)",
                    sim, len(prev), len(text),
                )
                return True
        return False

    # ── Stage C: overlap extraction ──────────────────────────────────────

    def _extract_new_content(self, text: str) -> str | None:
        """If *text* overlaps heavily with the most recent window entry,
        return only the sentences that are new."""
        window = self._get_window()
        if not window:
            return text

        overlap_thresh = getattr(config, "TEXT_FILTER_OVERLAP_THRESHOLD", 0.5)
        sent_sim_thresh = 0.8  # per-sentence similarity for removal

        # Compare against the most recent entry only
        _ts, prev = window[-1]
        sim = SequenceMatcher(None, prev, text).ratio()

        if sim < overlap_thresh:
            return text  # not enough overlap to warrant extraction

        # Split both into sentences
        prev_sents = [s.strip() for s in _SENT_RE.split(prev) if s.strip()]
        new_sents = [s.strip() for s in _SENT_RE.split(text) if s.strip()]

        if not new_sents:
            return text

        # Keep sentences from *text* that don't closely match any in *prev*
        kept: list[str] = []
        for ns in new_sents:
            is_old = any(
                SequenceMatcher(None, ps, ns).ratio() >= sent_sim_thresh
                for ps in prev_sents
            )
            if not is_old:
                kept.append(ns)

        if not kept:
            return None

        result = " ".join(kept).strip()
        if not result:
            return None

        log.info("filter: extracted new content: %s", result[:80])
        return result
