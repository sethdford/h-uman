#!/usr/bin/env bash
# setup-from-voiceai.sh — Import voiceai credentials into Human config.
#
# Reads ~/Documents/voiceai/.env and maps available credentials to
# ~/.human/config.json channels/providers. Does NOT modify any repo files.
#
# Usage: bash scripts/setup-from-voiceai.sh [--dry-run]
#
set -euo pipefail

VOICEAI_ENV="$HOME/Documents/voiceai/.env"
HUMAN_CONFIG="$HOME/.human/config.json"
DRY_RUN=false

if [ "${1:-}" = "--dry-run" ]; then
    DRY_RUN=true
fi

if [ ! -f "$VOICEAI_ENV" ]; then
    echo "ERROR: voiceai .env not found at $VOICEAI_ENV"
    exit 1
fi

if [ ! -f "$HUMAN_CONFIG" ]; then
    echo "ERROR: Human config not found at $HUMAN_CONFIG"
    exit 1
fi

# Source voiceai .env (skip lines with spaces in values by reading key=value pairs)
while IFS='=' read -r key value; do
    key=$(echo "$key" | xargs 2>/dev/null || true)
    [[ -z "$key" || "$key" == \#* ]] && continue
    # Only export the keys we care about
    case "$key" in
        TWILIO_ACCOUNT_SID|TWILIO_AUTH_TOKEN|TWILIO_PHONE_NUMBER|\
        SENDGRID_API_KEY|SENDGRID_FROM_EMAIL|\
        TWITTER_CLIENT_ID|TWITTER_CLIENT_SECRET|\
        GEMINI_API_KEY|OPENAI_API_KEY|\
        DISCORD_WEBHOOK_URL|SLACK_WEBHOOK_URL|\
        GOOGLE_CALENDAR_CLIENT_ID|GOOGLE_CALENDAR_CLIENT_SECRET)
            export "$key=$value"
            ;;
    esac
done < "$VOICEAI_ENV"

echo "=== Human Channel Setup from voiceai ==="
echo ""

# Report what we found
found=0
missing=0

check() {
    local name="$1" var="$2"
    if [ -n "${!var:-}" ]; then
        echo "  ✓ $name"
        found=$((found + 1))
    else
        echo "  ✗ $name (missing $var)"
        missing=$((missing + 1))
    fi
}

echo "Providers:"
check "Gemini"  GEMINI_API_KEY
check "OpenAI"  OPENAI_API_KEY
echo ""

echo "Channels (fully wirable):"
check "Twilio (account_sid)"  TWILIO_ACCOUNT_SID
check "Twilio (auth_token)"   TWILIO_AUTH_TOKEN
check "Twilio (phone_number)" TWILIO_PHONE_NUMBER
check "Email/SendGrid (api_key)"     SENDGRID_API_KEY
check "Email/SendGrid (from_email)"  SENDGRID_FROM_EMAIL
echo ""

echo "Channels (partial — need OAuth tokens):"
check "Twitter (api_key)"     TWITTER_CLIENT_ID
check "Twitter (api_secret)"  TWITTER_CLIENT_SECRET
echo ""

echo "Channels (outbound-only webhook URLs — need Bot Tokens for full E2E):"
check "Discord (webhook_url)" DISCORD_WEBHOOK_URL
check "Slack (webhook_url)"   SLACK_WEBHOOK_URL
echo ""

echo "Found: $found credentials, Missing: $missing"
echo ""

if $DRY_RUN; then
    echo "[dry-run] Would update $HUMAN_CONFIG"
    exit 0
fi

# Back up existing config
cp "$HUMAN_CONFIG" "$HUMAN_CONFIG.bak"
echo "Backed up config to $HUMAN_CONFIG.bak"

# Build the updated config using python3 (jq alternative with no deps)
python3 << 'PYEOF'
import json
import os

config_path = os.path.expanduser("~/.human/config.json")
with open(config_path) as f:
    cfg = json.load(f)

# --- Providers ---
providers = cfg.get("providers", [])
provider_names = {p["name"] for p in providers}

gemini_key = os.environ.get("GEMINI_API_KEY", "")
openai_key = os.environ.get("OPENAI_API_KEY", "")

# Update or add Gemini provider
if gemini_key:
    for p in providers:
        if p["name"] == "gemini":
            p["api_key"] = gemini_key
            break
    else:
        providers.append({"name": "gemini", "api_key": gemini_key})

# Add OpenAI provider
if openai_key:
    for p in providers:
        if p["name"] == "openai":
            p["api_key"] = openai_key
            break
    else:
        providers.append({"name": "openai", "api_key": openai_key})

cfg["providers"] = providers

# --- Channels ---
channels = cfg.get("channels", {})

# Twilio
sid = os.environ.get("TWILIO_ACCOUNT_SID", "")
token = os.environ.get("TWILIO_AUTH_TOKEN", "")
phone = os.environ.get("TWILIO_PHONE_NUMBER", "")
if sid and token and phone:
    channels["twilio"] = {
        "account_sid": sid,
        "auth_token": token,
        "from_number": phone
    }

# Email via SendGrid SMTP relay
sg_key = os.environ.get("SENDGRID_API_KEY", "")
sg_from = os.environ.get("SENDGRID_FROM_EMAIL", "")
if sg_key and sg_from:
    channels["email"] = {
        "smtp_host": "smtp.sendgrid.net",
        "smtp_port": 587,
        "smtp_user": "apikey",
        "smtp_pass": sg_key,
        "from_address": sg_from
    }

# Twitter (partial — client creds only, needs OAuth access token for sending)
tw_key = os.environ.get("TWITTER_CLIENT_ID", "")
tw_secret = os.environ.get("TWITTER_CLIENT_SECRET", "")
if tw_key and tw_secret:
    channels["twitter"] = {
        "api_key": tw_key,
        "api_secret": tw_secret
    }

cfg["channels"] = channels

with open(config_path, "w") as f:
    json.dump(cfg, f, indent=2)
    f.write("\n")

print(f"Updated {config_path}")
PYEOF

echo ""
echo "=== Config updated ==="
echo ""
echo "Wired channels:"
echo "  • twilio    — full E2E (send SMS, receive webhooks)"
echo "  • email     — outbound via SendGrid SMTP relay"
echo "  • twitter   — partial (have client creds, need OAuth tokens)"
echo "  • imessage  — already configured (local AppleScript)"
echo "  • gmail     — already configured (OAuth)"
echo ""
echo "Not wired (need Bot Tokens, not webhook URLs):"
echo "  • discord   — need DISCORD_BOT_TOKEN + DISCORD_CHANNEL_ID"
echo "  • slack     — need SLACK_BOT_TOKEN (xoxb-...) + SLACK_CHANNEL_ID"
echo ""
echo "Not wired (no credentials in voiceai):"
echo "  • whatsapp  — need WhatsApp Business API creds"
echo "  • facebook  — need FB Page Token"
echo "  • tiktok    — need TikTok developer creds"
echo ""
echo "To start the gateway:  ./build-release/human gateway"
echo "To start with agent:   ./build-release/human gateway --with-agent"
