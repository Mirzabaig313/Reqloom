#!/usr/bin/env bash
# Smoke-test a representative slice of the GiGwala API via Reqloom CLI.
# Each line: actor type + chain depth + target op.
#
# Usage (from repo root):
#   ./tools/reqloom-smoke.sh
set -uo pipefail

BIN=./build/macos-debug/cli/reqloom
PROJECT=reqloom
ENV=local

run() {
    local label="$1"
    local op="$2"
    local out
    out=$("$BIN" run "$op" --project "$PROJECT" --env "$ENV" 2>&1)

    local result
    result=$(printf "%s\n" "$out" | grep -E "^Result:" | head -1 | awk '{print $2}')
    local steps
    steps=$(printf "%s\n" "$out" | grep -cE "  (OK|FAIL|SK|BLOCK|CANCEL)")
    local failed_step
    failed_step=$(printf "%s\n" "$out" | grep -E "FAIL " | head -1 | awk '{print $2}')
    local err_code
    err_code=$(printf "%s\n" "$out" | grep -E "FAIL " | head -1 | sed -E 's/.*err=([A-Z_]+).*/\1/')

    printf "  %-45s [%-50s] %-10s steps=%-2d" \
           "$label" "$op" "${result:-NORESULT}" "$steps"
    if [[ "${result:-}" != "SUCCEEDED" ]]; then
        printf "  fail=%s err=%s" "${failed_step:-?}" "${err_code:-?}"
    fi
    printf "\n"
}

echo "═══ Public endpoints (no auth) ═══"
run "industry list"                              "industry.list"
run "reference: skills"                          "reference.get_skills"
run "reference: roles"                           "reference.get_roles"

echo ""
echo "═══ Admin actor (email + password) ═══"
run "admin: list pending orgs"                   "admin_organization.list"
run "admin: KYC queue"                           "admin_workers.kyc_queue"

echo ""
echo "═══ Employer actor (email + password) ═══"
run "employer: get organization"                 "organization.get"
run "employer: list own jobs"                    "job.list_employer"
run "employer: marketplace workers"              "employer_workers.marketplace"

echo ""
echo "═══ Worker actor (phone + OTP chain) ═══"
run "worker: get profile"                        "worker_profile.get"
run "worker: list applications"                  "application.list_my_applications"
run "worker: feed jobs"                          "job.feed"

echo ""
echo "═══ Multi-step chains ═══"
run "employer creates job"                       "job.create"
run "worker applies to job"                      "application.apply"
