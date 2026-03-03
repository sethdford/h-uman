#!/usr/bin/env bash
# Generates an inventory of all icons in the project
set -euo pipefail

cd "$(dirname "$0")/.."

ICONS_FILE="src/icons.ts"
OUTPUT="../docs/icon-inventory.md"

if [ ! -f "$ICONS_FILE" ]; then
  echo "Error: $ICONS_FILE not found"
  exit 1
fi

echo "# Icon Inventory" > "$OUTPUT"
echo "" >> "$OUTPUT"
echo "Auto-generated from \`ui/src/icons.ts\`" >> "$OUTPUT"
echo "" >> "$OUTPUT"
echo "| Icon Name | Export | Usage |" >> "$OUTPUT"
echo "|-----------|--------|-------|" >> "$OUTPUT"

# Extract all exported icon names (keys of the icons object)
# Handle both "name:" and "name-with-dash":
grep -oE '^\s+["]?[a-zA-Z0-9-]+["]?\s*:' "$ICONS_FILE" | sed 's/[: "]*//g' | while read -r name; do
  [ -z "$name" ] && continue
  # Count usages: icons.name or icons["name"]
  RES=$(grep -rE "icons\.${name}\b|icons\[\"${name}\"\]" src/ --include="*.ts" 2>/dev/null) || RES=""
  if [ -z "$RES" ]; then COUNT=0; else COUNT=$(echo "$RES" | wc -l | tr -d ' '); fi
  if [[ "$name" == *-* ]]; then
    echo "| $name | \`icons[\"$name\"]\` | $COUNT references |" >> "$OUTPUT"
  else
    echo "| $name | \`icons.$name\` | $COUNT references |" >> "$OUTPUT"
  fi
done

TOTAL=$(grep -cE '^\s+["]?[a-zA-Z0-9-]+["]?\s*:' "$ICONS_FILE" 2>/dev/null || echo "0")
echo "" >> "$OUTPUT"
echo "**Total: $TOTAL icons**" >> "$OUTPUT"
echo "" >> "$OUTPUT"
echo "_Generated: $(date -u +%Y-%m-%dT%H:%M:%SZ)_" >> "$OUTPUT"

echo "Icon inventory written to $OUTPUT ($TOTAL icons)"
