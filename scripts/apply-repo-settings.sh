#!/usr/bin/env bash
# Re-apply the GitHub repo settings (API-settable ones) after a recreate.
# The MANUAL, UI-only steps that this script cannot do are listed at the bottom.
#
# Usage:  gh auth status && bash scripts/apply-repo-settings.sh
set -euo pipefail
REPO="cheeseprince/obd-gauge-cluster"

echo "== description + topics =="
gh repo edit "$REPO" \
  --description "ESP32-S3 OBD-II gauge cluster — reads enhanced (Mode-22/UDS) PIDs your factory dash hides; auto-selects a vehicle profile by VIN; signed OTA." \
  --add-topic esp32 --add-topic esp32-s3 --add-topic obd-ii --add-topic elm327 \
  --add-topic lvgl --add-topic platformio --add-topic gauge-cluster \
  --add-topic vehicle-diagnostics --add-topic ota

echo "== Dependabot alerts + security updates =="
gh api -X PUT "repos/$REPO/vulnerability-alerts"
gh api -X PUT "repos/$REPO/automated-security-fixes"

echo "== branch protection on main (PR required + CI checks, no force-push/delete) =="
gh api -X PUT "repos/$REPO/branches/main/protection" --input - <<'JSON'
{
  "required_status_checks": {
    "strict": false,
    "contexts": [
      "Host tests (ubuntu-latest)",
      "Host tests (macos-latest)",
      "Device build (PlatformIO)",
      "Lint (ruff)",
      "Scan images/PDFs for sensitive metadata",
      "PII guard (no real VINs)",
      "Secret scan (gitleaks)"
    ]
  },
  "enforce_admins": false,
  "required_pull_request_reviews": { "required_approving_review_count": 0 },
  "restrictions": null,
  "allow_force_pushes": false,
  "allow_deletions": false
}
JSON

echo "== done. Secret scanning + push protection default ON for public repos. =="
cat <<'MANUAL'

MANUAL steps (UI only — not settable via this script):
  1. Secrets → Actions → add  OTA_SIGNING_KEY  (paste ~/ota_signing_key.pem)
  2. Settings → Pages → Deploy from branch: gh-pages / (root)
  3. Settings → General → UNCHECK "Include this code in the GitHub Archive Program"
  4. Settings → Actions → General → Allow all actions (usually already on)
MANUAL
