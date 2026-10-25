
import os
from datetime import datetime, timezone
from typing import Any

import httpx
from dotenv import load_dotenv
from fastapi import FastAPI, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel, Field

load_dotenv()

app = FastAPI(title="VoltPilot AI Backend", version="1.0.0")

# Development setup. Restrict origins before public deployment.
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["GET", "POST"],
    allow_headers=["*"],
)

AGENT37_URL = os.getenv("AGENT37_INSTANCE_URL", "").rstrip("/")
AGENT37_KEY = os.getenv("AGENT37_API_KEY", "")
DEVICE_TOKEN = os.getenv("VOLTPILOT_DEVICE_TOKEN", "")

# Latest telemetry for the prototype.
# Use a database for persistence in a production version.
latest_telemetry: dict[str, Any] | None = None
last_received_at: datetime | None = None


class AskRequest(BaseModel):
    question: str = Field(min_length=1, max_length=1500)


def require_device_token(token: str | None):
    if DEVICE_TOKEN and token != DEVICE_TOKEN:
        raise HTTPException(status_code=401, detail="Invalid device token")


@app.get("/health")
def health():
    return {
        "status": "ok",
        "agent37_configured": bool(AGENT37_URL and AGENT37_KEY),
        "telemetry_received": latest_telemetry is not None,
    }


@app.post("/telemetry")
def receive_telemetry(
    payload: dict[str, Any],
    x_device_token: str | None = None,
):
    global latest_telemetry, last_received_at

    require_device_token(x_device_token)

    # Store the actual ESP32 payload without inventing readings.
    latest_telemetry = payload
    last_received_at = datetime.now(timezone.utc)

    return {
        "accepted": True,
        "received_at": last_received_at.isoformat(),
    }


@app.get("/api/live")
def get_live_telemetry():
    if latest_telemetry is None:
        return {
            "connected": False,
            "message": "Waiting for ESP32 telemetry",
            "data": None,
        }

    age_seconds = (
        datetime.now(timezone.utc) - last_received_at
    ).total_seconds()

    return {
        "connected": age_seconds <= 10,
        "age_seconds": round(age_seconds, 1),
        "received_at": last_received_at.isoformat(),
        "data": latest_telemetry,
    }


def extract_agent_text(result: dict[str, Any]) -> str:
    # OpenAI Responses-style output
    if isinstance(result.get("output_text"), str):
        return result["output_text"]

    pieces = []
    for item in result.get("output", []):
        for content in item.get("content", []):
            if content.get("type") in ("text", "output_text"):
                text = content.get("text")
                if text:
                    pieces.append(text)

    if pieces:
        return "\n".join(pieces)

    # Some compatible runtimes use Chat Completions format.
    choices = result.get("choices", [])
    if choices:
        return choices[0].get("message", {}).get("content", "")

    return "The agent returned a response in an unrecognised format."


@app.post("/api/ask")
async def ask_voltpilot(request: AskRequest):
    if latest_telemetry is None:
        raise HTTPException(
            status_code=409,
            detail="No ESP32 readings received yet. Connect the device first.",
        )

    if not AGENT37_URL or not AGENT37_KEY:
        raise HTTPException(
            status_code=503,
            detail="Configure the Agent37 instance URL and API key in .env",
        )

    age_seconds = (
        datetime.now(timezone.utc) - last_received_at
    ).total_seconds()

    if age_seconds > 30:
        raise HTTPException(
            status_code=409,
            detail="Telemetry is stale. Check the ESP32 connection first.",
        )

    context = {
        "telemetry_age_seconds": round(age_seconds, 1),
        "latest_sensor_payload": latest_telemetry,
    }

    prompt = f"""
You are VoltPilot's energy-analysis assistant.
Answer the user's question using the supplied hardware telemetry.

Rules:
- Use only values actually supplied in the telemetry.
- Clearly state when a sensor value or battery specification is missing.
- Never invent battery capacity, state of charge, or battery health.
- Estimate backup only when usable capacity and load power/current
  are sufficiently known; state assumptions and uncertainty.
- Voltage alone cannot establish battery health. Explain what further
  measurements are needed for a reliable health estimate.
- Do not claim that a relay, charger, or power source changed state
  unless the telemetry confirms it.
- Never instruct the user to bypass electrical protections.
- Be concise, clear, and practical.

Current telemetry JSON:
{context}

User question:
{request.question}
"""

    headers = {
        "Authorization": f"Bearer {AGENT37_KEY}",
        "Content-Type": "application/json",
    }

    # Agent37 instance Responses endpoint.
    url = f"{AGENT37_URL}/v1/responses"

    try:
        async with httpx.AsyncClient(timeout=30.0) as client:
            response = await client.post(
                url,
                headers=headers,
                json={"input": prompt},
            )

        if response.is_error:
            # Keep provider response details in server logs, not secrets.
            print("Agent37 HTTP error:", response.status_code,
                  response.text[:1000])
            raise HTTPException(
                status_code=502,
                detail=f"Agent37 request failed (HTTP {response.status_code})",
            )

        result = response.json()
        answer = extract_agent_text(result)

        return {
            "answer": answer,
            "telemetry_age_seconds": round(age_seconds, 1),
            "source": "Agent37",
        }

    except httpx.TimeoutException:
        raise HTTPException(
            status_code=504,
            detail="Agent37 timed out. Please retry.",
        )
    except httpx.RequestError:
        raise HTTPException(
            status_code=502,
            detail="Could not reach the Agent37 instance.",
        )
