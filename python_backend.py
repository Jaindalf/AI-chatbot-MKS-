import os
import io
import time
import json
import re
import random
from flask import Flask, request, Response, jsonify
from dotenv import load_dotenv
import requests
from gtts import gTTS
from pydub import AudioSegment
from faster_whisper import WhisperModel

# ==========================================================
# CONFIGURATION
# ==========================================================
GEMINI_API_KEY = "AIzaSyBnTRSp1VrFtjuIV5NqoZ7x-vjGWKB8nFs"
GEMINI_MODEL = "gemini-2.5-flash"
GEMINI_API_BASE_URL = "https://generativelanguage.googleapis.com/v1beta/models"
DEBUG_OUTPUT_DIR = "debug_audio_files"

app = Flask(__name__)

# ==========================================================
# INITIALIZE FASTERWHISPER MODEL
# ==========================================================
# "base" is lightweight and accurate enough for 6-second recordings
# Change to "small" or "medium" if you have a GPU
print("[INIT] Loading FasterWhisper model...")
stt_model = WhisperModel("base", device="cpu", compute_type="int8")
print("[INIT] FasterWhisper loaded successfully.")

# ==========================================================
# HELPERS
# ==========================================================
def clean_text_for_tts(text: str) -> str:
    """Clean up the text before TTS."""
    text = re.sub(r'[\*\#]', '', text)
    text = re.sub(r'\[.*?\]', '', text)
    text = re.sub(r'\s+', ' ', text).strip()
    return text

# ==========================================================
# STT WITH FASTERWHISPER
# ==========================================================
def transcribe_with_fasterwhisper(raw_pcm_data):
    """Transcribe raw PCM 16-bit mono audio using FasterWhisper."""
    try:
        # Convert raw PCM to temporary WAV file for Whisper
        temp_path = "temp_input.wav"
        audio = AudioSegment(
            data=raw_pcm_data,
            sample_width=2,
            frame_rate=16000,
            channels=1
        )
        audio.export(temp_path, format="wav")

        print("[STT] Running FasterWhisper transcription...")
        segments, info = stt_model.transcribe(temp_path, beam_size=5)
        text = " ".join([segment.text.strip() for segment in segments]).strip()
        os.remove(temp_path)

        print(f"[STT] Transcribed Text: {text}")
        return text
    except Exception as e:
        print(f"[STT ERROR] {e}")
        return None

# ==========================================================
# GEMINI TEXT GENERATION
# ==========================================================
def get_llm_response(prompt_text):
    """Send text query to Gemini and get a text response."""
    system_prompt = (
        "You are a friendly helpdesk assistant for JECRC University, "
        "located in Jaipur, Rajasthan. Use short, clear sentences."
    )

    payload = {
        "contents": [{
            "parts": [{"text": f"User query: {prompt_text}"}]
        }],
        "systemInstruction": {
            "parts": [{"text": system_prompt}]
        },
        "generationConfig": {
            "temperature": 0.7
        }
    }

    headers = {"Content-Type": "application/json"}
    url = f"{GEMINI_API_BASE_URL}/{GEMINI_MODEL}:generateContent?key={GEMINI_API_KEY}"

    try:
        print("[LLM] Sending query to Gemini...")
        response = requests.post(url, headers=headers, json=payload, timeout=20)
        response.raise_for_status()
        data = response.json()

        candidate = data.get("candidates", [{}])[0]
        part = candidate.get("content", {}).get("parts", [{}])[0]
        text_response = part.get("text", "").strip()

        print(f"[LLM] Gemini Response: {text_response}")
        return text_response or "I couldn’t generate a response."
    except Exception as e:
        print(f"[LLM ERROR] {e}")
        return "There was an issue reaching Gemini. Try again later."

# ==========================================================
# TTS WITH GTTS
# ==========================================================
def tts_generate_audio(text):
    """Convert text to 16-bit PCM WAV audio using gTTS."""
    try:
        cleaned_text = clean_text_for_tts(text)
        tts = gTTS(text=cleaned_text, lang="en")
        mp3_fp = io.BytesIO()
        tts.write_to_fp(mp3_fp)
        mp3_fp.seek(0)

        # Optional: save MP3 locally for debugging
        os.makedirs(DEBUG_OUTPUT_DIR, exist_ok=True)
        mp3_path = os.path.join(DEBUG_OUTPUT_DIR, "response_audio.mp3")
        with open(mp3_path, "wb") as f:
            f.write(mp3_fp.read())
        mp3_fp.seek(0)

        # Convert MP3 → WAV → raw PCM bytes
        audio = AudioSegment.from_file(mp3_fp, format="mp3")
        audio = audio.set_frame_rate(16000).set_channels(1).set_sample_width(2)
        raw = io.BytesIO()
        audio.export(raw, format="wav")
        raw.seek(44)  # strip WAV header
        final_pcm = raw.read()

        print(f"[TTS] Generated {len(final_pcm)} bytes of PCM audio.")
        return final_pcm
    except Exception as e:
        print(f"[TTS ERROR] {e}")
        return None

# ==========================================================
# MAIN PIPELINE
# ==========================================================
def process_voice_command(raw_pcm_data):
    """STT → Gemini → TTS"""
    text = transcribe_with_fasterwhisper(raw_pcm_data)
    if not text:
        return Response("STT_FAILED", status=500)

    reply = get_llm_response(text)
    audio = tts_generate_audio(reply)
    if not audio:
        return Response("TTS_FAILED", status=500)

    return Response(audio, mimetype="application/octet-stream")

# ==========================================================
# API ENDPOINT
# ==========================================================
@app.route("/voice_input", methods=["POST"])
def handle_voice_input():
    if request.mimetype == "application/octet-stream":
        audio_data = request.data
        if not audio_data:
            return jsonify({"error": "No audio data received"}), 400
        return process_voice_command(audio_data)
    return jsonify({"error": "Unsupported media type"}), 415

# ==========================================================
# MAIN ENTRY
# ==========================================================
if __name__ == "__main__":
    os.makedirs(DEBUG_OUTPUT_DIR, exist_ok=True)
    print(f"[SERVER] Debug audio saved in '{DEBUG_OUTPUT_DIR}'")
    print("[SERVER] Running at http://0.0.0.0:5002/voice_input")
    app.run(host="0.0.0.0", port=5002)
