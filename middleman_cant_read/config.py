"""Centralized configuration for the owocr → F5-TTS → RVC pipeline."""

# ──────────────────────────────────────────────────────────
# ── OCR ───────────────────────────────────────────────────
# ──────────────────────────────────────────────────────────
# Choose OCR solution
# Available values:
# "owocr_send_frames", "owocr_receive_only" "ollama"
# > "owocr_send_frames" - capture frames in this app and send to owocr for OCR; requires owocr WS server running
# > "owocr_receive_only" - receive already-recognized text from owocr WS server, don't send frames; requires owocr WS server running and configured to send recognized text
OCR_PROVIDER = "ollama"
# What will be used to capture images that are passed to OCR_PROVIDER
# > Some OCR solutions (e.g. "owocr_receive_only") provide their own frame capturing,
# > when using one of those FRAME_CAPTURE_METHOD can be set to empty string to
# > let the OCR solution handle it.
# Required if OCR_PROVIDER is one of:
# > "ollama"
# > "owocr_send_frames"
# Ignored if OCR_PROVIDER is one of:
# > "owocr_receive_only"
# Can be empty if OCR_PROVIDER is one of:
# > "owocr_receive_only"
# Available values:
# "obs_plugin" (preferred), "obs_websocket"
FRAME_CAPTURE_METHOD = "obs_plugin"

# OCR Text processing params:
OLLAMA_TEXT_CLEANUP_ENABLED = False  # First use AI to fix misrecognized characters
                               # and check if text makes sense
OCR_DEDUP_ENABLED = True     # Then skip duplicate texts
TEXT_FILTER_ENABLED = True   # Then run text through filters


# ── owocr (OCR_PROVIDER in ("owocr_send_frames", "owocr_receive_only")) ────────────────────────────────────────────────────────────────────
OWOCR_WS_URL = "ws://localhost:7331"


# ── OBS Websocket (FRAME_CAPTURE_METHOD == "obs_websocket") ─────────────────────────────────────
OBS_HOST = "localhost"
OBS_PORT = 4455
OBS_PASSWORD = ""
OBS_SOURCE = ""  # empty = auto-detect current program scene
OBS_CAPTURE_FPS = 15
OBS_CAPTURE_DELAY_SECONDS = 2 # How many seconds to wait after OCR before another OCR can happen

# - Captured frame processing
VISION_STABILITY_FRAMES = 7       # consecutive stable frames before OCR
                                    # If dialogue changes fast and doesn't get picked up - decrease
                                    # If text gets picked up mid-animation - increase
VISION_STABILITY_THRESHOLD = 0.2  # max mean-abs-diff (0-255) to count as stable
                                    # If text gets picked up mid-animation - decrease
                                    # If text animation is done but text doesn't get picked up - increase
VISION_HASH_THRESHOLD = 7        # min dHash Hamming distance to consider "changed", lower = less change needed
                                    # If dialogue changes but doesn't get picked up - decrease
                                    # If same text gets picked up multiple times - increase
VISION_TEXT_LAP_THRESHOLD = 30.0  # Laplacian variance below this = no text
VISION_BLACK_THRESHOLD = 30        # pixel luma ≤ this = background, excluded from
                                   # diff/hash (0 = disabled, use ~10 with OBS Luma Key)
VISION_LUMA_NOISE_FLOOR = 30.0     # per-pixel diff below this is zeroed in luma mode,
                                   # suppresses compression / sensor noise (0 = disabled)
VISION_USE_LAP_CARVE = False
VISION_LAP_CARVE_EDGE_THRESHOLD = 12.0
VISION_LAP_DILATE_KERNEL = 9      # must be odd; covers ~/2 px around edge

# - Debug video stream captured frames to MPV
VISION_DEBUG_STREAM = False           # enabled via --debug-stream CLI flag
VISION_DEBUG_STREAM_WIDTH = 960       # target display width  (0 = native)
VISION_DEBUG_STREAM_HEIGHT = 540      # target display height (0 = native)


# ── OBS Plugin (FRAME_CAPTURE_METHOD == "obs_plugin") ─────────────────────────────────────
OBS_PLUGIN_GATE_SOCKET_PATH = '/tmp/i_cant_read_gate.sock'
OBS_PLUGIN_FRAME_PATH = '/dev/shm/i-cant-read.png'


# ── Ollama (OCR_PROVIDER == "ollama" or OLLAMA_TEXT_CLEANUP_ENABLED is True) ───────────────────────────────────────────────────────────────────
OLLAMA_URL = "http://localhost:11434"
OLLAMA_KEEP_ALIVE = False


# ── Ollama · vision OCR (OCR_PROVIDER == "ollama") ─────────────────────
OLLAMA_OCR_MODEL = "qwen3.5:0.8b"
OLLAMA_OCR_SYSTEM_PROMPT = (
    "You are an OCR agent. Your only job is to transcribe text visible in the image. "
    "Output plain text only — no markdown, no formatting, no explanations. "
    "Preserve the original wording exactly; do not paraphrase or summarize. "
    "If lines are wrapped due to layout constraints, join them into a single line. "
    "For Japanese text, omit furigana and output only the base characters. "
    "If text is partially cut off or obscured, transcribe only the clearly visible portion. "
    "If there is no readable text in the image, respond with exactly: [EMPTY]"
)
OLLAMA_OCR_HISTORY_SIZE = 0  # recent user/assistant exchanges to include


# ── Ollama · (OLLAMA_TEXT_CLEANUP_ENABLED is True) ────────────────────────────────────────────────────
OLLAMA_CLEANUP_MODEL = "qwen3.5:0.8b"
OLLAMA_CLEANUP_SYSTEM_PROMPT = '''
You clean up OCR text from a Visual Novel and reformat it for text-to-speech (TTS) output.

Apply these fixes in order:
1. If a part of the text was sent in a previous message, remove the repeated part and keep only the new text.
2. If the text does not make any sense or looks like an OCR error, reply with "Failed recognition"
3. If only some of the text is garbled, remove the garbled parts and keep the rest, if possible.
4. Fix OCR errors: restore missing spaces, fix merged words, correct scrambled word order.
5. Remove UI artifact words that don't belong to the dialogue: words like "Selection", "Skip", "Auto", "Log", or similar menu labels appearing at the start of the text.
6. Fix grammar and punctuation, but preserve ellipses (...).
7. Rewrite ALL-CAPS words in normal sentence case.
8. If the text begins with a speaker name followed by a colon or line break, reformat it as: Name says: <text>


Reply only with the corrected text. No explanations, no commentary.

Examples:

Input:  Grass WonderBy the way, Inari One... How do you liketo appreciatecherryblossoms?
Output: Grass Wonder says: By the way, Inari One... How do you like to appreciate cherry blossoms.

Input:  Special Week: THIS IS AMAZING! I can't believe it!
Output: Special Week says: This is amazing. I can't believe it.

Input:  Selection Well done! Haha, it's weird, I didn't really do allthat much physically, but I'm kindawiped out from the nerves.
Output: Well done. Haha, it's weird, I didn't really do all that much physically, but I'm kinda wiped out from the nerves.

Input: allain VII Playvusa s city uut 10
Output: Failed recognition

Input: כגו, Hayvusa mimmyits aliiviny mem.
Output: Failed recognition

Input: Divergent om כגו, Hayvusa mimmyits annuiny mem. Ah, Phagousa, if not for you, wherewould all these sea monsters andtroubles come from? Besides, thosetreasures were offerings sacrificed toyou before our journey!
Output: Ah, Phagousa, if not for you, where would all these sea monsters and troubles come from? Besides, those treasures were offerings sacrificed to you before our journey.
'''.strip()

OLLAMA_CLEANUP_HISTORY_SIZE = 5  # recent user/assistant exchanges to include


# ── OCR deduplication (OCR_DEDUP_ENABLED is True) ──────────────────────────
OCR_DEDUP_WINDOW_SIZE = 5              # number of recent OCR texts to keep
OCR_DEDUP_SIMILARITY_THRESHOLD = 0.95  # whole-text ratio above this → near-dup skip
OCR_DEDUP_SEGMENT_THRESHOLD = 0.85     # per-segment similarity (ratio or containment)
OCR_DEDUP_MIN_SEGMENT_LENGTH = 10      # ignore segments shorter than this (garbled / UI noise)


# ── Text filter (TEXT_FILTER_ENABLED is True) ───────────────────────────
TEXT_FILTER_WINDOW_SIZE = 10           # sliding window of recent texts
TEXT_FILTER_SIMILARITY_THRESHOLD = 0.85  # fuzzy dedup threshold
TEXT_FILTER_OVERLAP_THRESHOLD = 0.5    # trigger overlap extraction
TEXT_FILTER_MIN_LENGTH = 15            # reject texts shorter than this
TEXT_FILTER_UI_BLOCKLIST: list[str] = [
    "Divergent Universe",
    "You",
    "X",
]



# ──────────────────────────────────────────────────────────
# ─── TTS ──────────────────────────────────────────────────
# ──────────────────────────────────────────────────────────
TTS_PROVIDER = "kokoro_fastapi"
RVC_PROVIDER = "rvc_gradio" # set to empty string to disable RVC
TTS_FILES_DIR = "/dev/shm/cantread/tts"


# ── Kokoro TTS (TTS_PROVIDER == "kokoro_fastapi") ────────────────────────────────────────────
KOKORO_URL = "http://127.0.0.1:8880"
KOKORO_VOICE = "af_bella"
KOKORO_SPEED = 0.9


# ── RVC (RVC_PROVIDER == 'rvc_gradio') ───────────────────────────────────────────────────
RVC_URL = "http://localhost:7865"

RVC_MODEL = "CyreneAidenDawnHSR_e900_s122400.pth"
RVC_INDEX = (
    "/home/h3lp/Apps/tts/Retrieval-based-Voice-Conversion-WebUI"
    "/assets/weights/added_IVF256_Flat_nprobe_1_CyreneAidenDawnHSR_v2.index"
)
RVC_PROTECT_1 = 0
RVC_PROTECT_2 = 0

# Inference parameters
RVC_TRANSPOSE = 0
RVC_F0_METHOD = "rmvpe"


# ─── Player ─────────────────────────────────────────────────────────────────
PLAYER_ENABLED = True
PLAYER = 'mpv'
PLAYER_DELAY = 0.5  # seconds to wait before playing the next audio, to avoid overlap



# ──────────────────────────────────────────────────────────
# ─── Backend configuration ───────────────────────────────────────────────────────
# ──────────────────────────────────────────────────────────


# ── Redis / Celery ───────────────────────────────────────────────────────────
REDIS_URL = "redis://localhost:6379/0"

# Redis keys used by the pipeline
PLAYBACK_HASH_KEY = "cantread:playback:results"   # hash: seq_num → wav_path | "SKIP"
SEQ_COUNTER_KEY = "cantread:seq_counter"           # string: next sequence number to issue
TEXTFILTER_ZSET_KEY = "cantread:textfilter:window" # sorted set: text → timestamp
TEXTFILTER_LOCK_KEY = "cantread:textfilter:lock"   # distributed lock for filter state
OLLAMA_HISTORY_KEY = "cantread:ollama:history"      # list: recent cleanup exchanges (JSON)
OLLAMA_OCR_HISTORY_KEY = "cantread:ollama:ocr_history" # list: recent OCR exchanges (JSON)
OCR_DEDUP_LIST_KEY = "cantread:ocr_dedup:window"       # list: recent OCR texts for dedup
OCR_DEDUP_LOCK_KEY = "cantread:ocr_dedup:lock"          # distributed lock for dedup
