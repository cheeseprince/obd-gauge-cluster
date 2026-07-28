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

echo "== merge strategy: SQUASH ONLY =="
# While the project bootstraps we want public history to stay one-commit-per-PR:
# easy to read, easy to revert, and no WIP/fixup commits leaking into main.
# Merge commits and rebase merges are disabled so the choice can't be made by
# accident in the UI. Squash title/body come from the PR itself (PR_TITLE/PR_BODY)
# rather than the branch's commit messages — concatenating those back in is exactly
# the noise squashing is meant to remove.
gh api -X PATCH "repos/$REPO" \
  -F allow_squash_merge=true \
  -F allow_merge_commit=false \
  -F allow_rebase_merge=false \
  -f squash_merge_commit_title=PR_TITLE \
  -f squash_merge_commit_message=PR_BODY \
  -F delete_branch_on_merge=true \
  --silent

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

echo "== CodeQL default setup: all three languages, security-extended =="
# Code scanning was previously invisible to this script: default setup is
# configured in repo SETTINGS and writes no workflow file, so a repo recreate
# would silently come back with no SAST at all — and nothing in the tree would
# show it was missing. Assert it here instead.
#
# security-extended over the default suite: the default suite found nothing on
# this codebase, extended immediately found an unpinned third-party action in
# the release job (the one that holds OTA_SIGNING_KEY). The extra rules earn
# their noise.
#
# NOTE: this PATCH is applied ASYNCHRONOUSLY — it returns a run_id and the
# config only reflects the change once that run completes. Re-reading
# query_suite immediately after will still say "default"; that is a race, not a
# failure. Give it a minute before verifying.
gh api -X PATCH "repos/$REPO/code-scanning/default-setup" \
  -f state=configured \
  -f query_suite=extended \
  -f 'languages[]=actions' -f 'languages[]=c-cpp' -f 'languages[]=python' \
  --silent || echo "  (CodeQL default setup unchanged or already matching)"

echo "== tag protection: release tags are immutable =="
# Branch protection covers main; nothing covered TAGS until now. Without this,
# anyone with push access can delete v0.1.2 and re-point it at another commit.
# That is not an OTA compromise — devices still enforce manifest.sig, so a
# swapped tag cannot ship firmware they will install — but it breaks the audit
# trail, and the SLSA provenance attests "built from tag X" while X is mutable.
# Immutable tags are what make that attestation mean anything.
#
# Idempotent: creating a ruleset that already exists returns 422, which is fine
# on a re-run of this script.
gh api -X POST "repos/$REPO/rulesets" --input - <<'JSON' --silent 2>/dev/null || echo "  (tag ruleset already exists — skipping)"
{
  "name": "Protect release tags",
  "target": "tag",
  "enforcement": "active",
  "conditions": { "ref_name": { "include": ["refs/tags/v*"], "exclude": [] } },
  "rules": [ { "type": "deletion" }, { "type": "non_fast_forward" } ]
}
JSON

echo "== done. Secret scanning + push protection default ON for public repos. =="
cat <<'MANUAL'

MANUAL steps (UI only — not settable via this script):
  1. Secrets → Actions → add  OTA_SIGNING_KEY  (paste ~/ota_signing_key.pem)
  2. Settings → Pages → Deploy from branch: gh-pages / (root)
  3. Settings → General → UNCHECK "Include this code in the GitHub Archive Program"
  4. Settings → Actions → General → Allow all actions (usually already on)
  5. NOTHING TO DO — secret_scanning_non_provider_patterns and
     secret_scanning_validity_checks read "disabled" and stay that way.
     Investigated 2026-07-27; do not chase them again:
       * VALIDITY CHECKS are plan-gated. GitHub docs: "only available to users
         with GitHub Team or GitHub Enterprise who enable the feature as part
         of GitHub Secret Protection." Not available on this account.
       * NON-PROVIDER PATTERNS: generic patterns such as rsa_private_key are
         ALREADY detected on free public repos via alerts. The toggle governs
         the paid expanded set.
     There is no UI for either (confirmed by Alan), and PATCHing them returns
     HTTP 200 while silently leaving them disabled — that silence is the plan
     limit, not a bad payload.
     The protection that actually matters is already in place: gitleaks runs as
     a REQUIRED status check and detects PEM private keys (verified against a
     throwaway EC key, 2026-07-27). An accidental commit of the OTA signing key
     fails CI and cannot be merged.
MANUAL
