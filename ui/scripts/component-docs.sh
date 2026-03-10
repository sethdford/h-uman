#!/usr/bin/env bash
# Generates component API documentation from source
set -euo pipefail

cd "$(dirname "$0")/.."

OUTPUT="../docs/components.md"

echo "# Component API Reference" > "$OUTPUT"
echo "" >> "$OUTPUT"
echo "Auto-generated from \`ui/src/components/\`" >> "$OUTPUT"
echo "" >> "$OUTPUT"

for file in src/components/hu-*.ts; do
  [ -f "$file" ] || continue

  NAME=$(basename "$file" .ts)
  TAG="<${NAME}>"

  echo "## \`${TAG}\`" >> "$OUTPUT"
  echo "" >> "$OUTPUT"

  # Extract properties (@property and @state decorators)
  echo "### Properties" >> "$OUTPUT"
  echo "" >> "$OUTPUT"
  echo "| Property | Type | Default |" >> "$OUTPUT"
  echo "|----------|------|---------|" >> "$OUTPUT"

  (grep -E '@property|@state' "$file" 2>/dev/null || true) | while read -r line; do
    TYPE=$(echo "$line" | grep -oE "type: *[A-Za-z]+" | sed 's/type: *//') || TYPE=""
    [ -z "$TYPE" ] && TYPE="unknown"
    PROP=$(echo "$line" | sed -n 's/.*)  *\([a-zA-Z_][a-zA-Z0-9_]*\).*/\1/p')
    [ -n "$PROP" ] && echo "| \`$PROP\` | $TYPE | |" >> "$OUTPUT"
  done

  echo "" >> "$OUTPUT"

  # Extract events (CustomEvent)
  EVENTS=$(grep -oE "new CustomEvent\('[^']+'" "$file" 2>/dev/null | sed "s/new CustomEvent('//;s/'$//" | sort -u || true)
  if [ -n "$EVENTS" ]; then
    echo "### Events" >> "$OUTPUT"
    echo "" >> "$OUTPUT"
    echo "$EVENTS" | while read -r evt; do
      [ -n "$evt" ] && echo "- \`$evt\`" >> "$OUTPUT"
    done
    echo "" >> "$OUTPUT"
  fi

  # Extract slots
  SLOTS=$(grep -oE '<slot[^>]*name="[^"]*"[^>]*>|<slot[^>]*>' "$file" 2>/dev/null || true)
  if [ -n "$SLOTS" ]; then
    echo "### Slots" >> "$OUTPUT"
    echo "" >> "$OUTPUT"
    echo "$SLOTS" | while read -r slot; do
      SLOT_NAME=$(echo "$slot" | grep -oE 'name="[^"]*"' | sed 's/name="//;s/"//') || SLOT_NAME=""
      if [ -z "$SLOT_NAME" ]; then
        SLOT_NAME="default"
      fi
      echo "- \`$SLOT_NAME\`" >> "$OUTPUT"
    done
    echo "" >> "$OUTPUT"
  fi

  echo "---" >> "$OUTPUT"
  echo "" >> "$OUTPUT"
done

TOTAL=$(ls src/components/hu-*.ts 2>/dev/null | wc -l | tr -d ' ') || TOTAL=0
echo "" >> "$OUTPUT"
echo "_${TOTAL} components documented. Generated: $(date -u +%Y-%m-%dT%H:%M:%SZ)_" >> "$OUTPUT"

echo "Component docs written to $OUTPUT ($TOTAL components)"
