#!/usr/bin/env bash
# Interactive Gmail OAuth setup for h-uman feed monitoring.
#
# Steps:
#   1. Go to https://console.cloud.google.com/apis/credentials
#   2. Create a project (or select existing)
#   3. Enable the Gmail API: https://console.cloud.google.com/apis/library/gmail.googleapis.com
#   4. Create OAuth 2.0 credentials (Desktop app type)
#   5. Run this script with the client_id and client_secret
#
# Usage:
#   ./scripts/setup_gmail_oauth.sh

set -euo pipefail

CONFIG="$HOME/.human/config.json"

echo "=== Gmail OAuth Setup ==="
echo ""
echo "Before running this, you need:"
echo "  1. A Google Cloud project with Gmail API enabled"
echo "  2. OAuth 2.0 credentials (Desktop app type)"
echo ""
echo "Quick links:"
echo "  - Console: https://console.cloud.google.com/apis/credentials"
echo "  - Enable Gmail API: https://console.cloud.google.com/apis/library/gmail.googleapis.com"
echo ""

read -rp "Client ID: " CLIENT_ID
read -rp "Client Secret: " CLIENT_SECRET

if [ -z "$CLIENT_ID" ] || [ -z "$CLIENT_SECRET" ]; then
    echo "Error: Both client_id and client_secret are required"
    exit 1
fi

REDIRECT_URI="urn:ietf:wg:oauth:2.0:oob"
SCOPE="https://www.googleapis.com/auth/gmail.readonly"
AUTH_URL="https://accounts.google.com/o/oauth2/v2/auth?client_id=${CLIENT_ID}&redirect_uri=${REDIRECT_URI}&response_type=code&scope=${SCOPE}&access_type=offline&prompt=consent"

echo ""
echo "Opening browser for Gmail authorization..."
echo "If browser doesn't open, visit this URL:"
echo ""
echo "  $AUTH_URL"
echo ""

open "$AUTH_URL" 2>/dev/null || true

read -rp "Paste the authorization code here: " AUTH_CODE

if [ -z "$AUTH_CODE" ]; then
    echo "Error: Authorization code is required"
    exit 1
fi

echo "Exchanging code for tokens..."

RESPONSE=$(curl -s -X POST "https://oauth2.googleapis.com/token" \
    -d "code=$AUTH_CODE" \
    -d "client_id=$CLIENT_ID" \
    -d "client_secret=$CLIENT_SECRET" \
    -d "redirect_uri=$REDIRECT_URI" \
    -d "grant_type=authorization_code")

REFRESH_TOKEN=$(echo "$RESPONSE" | python3 -c "import sys,json; print(json.load(sys.stdin).get('refresh_token',''))" 2>/dev/null)

if [ -z "$REFRESH_TOKEN" ]; then
    echo "Error: Failed to get refresh token. Response:"
    echo "$RESPONSE"
    exit 1
fi

echo ""
echo "Got refresh token. Updating config..."

python3 -c "
import json
with open('$CONFIG') as f:
    cfg = json.load(f)
cfg.setdefault('feeds', {}).setdefault('gmail', {})
cfg['feeds']['gmail']['client_id'] = '$CLIENT_ID'
cfg['feeds']['gmail']['client_secret'] = '$CLIENT_SECRET'
cfg['feeds']['gmail']['refresh_token'] = '$REFRESH_TOKEN'
with open('$CONFIG', 'w') as f:
    json.dump(cfg, f, indent=2)
    f.write('\n')
"

echo ""
echo "Gmail OAuth configured successfully!"
echo "Config updated at $CONFIG"
echo ""
echo "Test with: ./build/human feed poll --source gmail"
